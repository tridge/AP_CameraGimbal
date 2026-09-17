#!/usr/bin/env python3
"""MT11/A8/ZR10 gimbal-controller simulator using the private protocol over UDP."""

import argparse
import math
import os
import select
import signal
import socket
import struct
import sys
import time
from pathlib import Path

try:
    from .target_properties import TARGETS, transform
except ImportError:
    from target_properties import TARGETS, transform


MT11_CAMERA_CPU = 0x34
A8_CAMERA_CPU = 0x2C
GIMBAL_MCU = 0x2E
PRIVATE_LINK = 0x6B
TUNNEL_COMMAND = 0x16


def crc16(data):
    value = 0
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = ((value << 1) ^ 0x1021) & 0xFFFF if value & 0x8000 else (value << 1) & 0xFFFF
    return value


def crc8(data):
    value = 0
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = ((value >> 1) ^ 0x8C) if value & 1 else value >> 1
    return value


def mt11_private_frame(control, sequence, command, payload=b""):
    header = bytearray(12)
    header[0] = 0xAA
    header[1] = control
    header[2] = 3
    struct.pack_into("<H", header, 3, len(payload))
    header[5] = crc8(header[:5])
    struct.pack_into("<H", header, 6, sequence)
    header[8] = GIMBAL_MCU
    header[9] = MT11_CAMERA_CPU
    header[10] = PRIVATE_LINK
    header[11] = command
    result = bytes(header) + payload
    return result + struct.pack("<H", crc16(result))


def parse_mt11_private(data):
    if len(data) < 14 or data[0] != 0xAA or data[2] != 3 or crc8(data[:5]) != data[5]:
        raise ValueError("bad private header")
    payload_length = struct.unpack_from("<H", data, 3)[0]
    if len(data) != payload_length + 14 or crc16(data[:-2]) != struct.unpack_from("<H", data, len(data) - 2)[0]:
        raise ValueError("bad private length or CRC")
    sequence = struct.unpack_from("<H", data, 6)[0]
    return data[1], sequence, data[8], data[9], data[10], data[11], data[12:-2]


def a8_private_frame(control, sequence, command, payload=b""):
    if len(payload) > 255:
        raise ValueError("A8 private payload is too large")
    header = bytearray((
        0xAA, control, 0x02, len(payload), 0, sequence & 0xFF,
        sequence >> 8, GIMBAL_MCU, A8_CAMERA_CPU, PRIVATE_LINK, command,
    ))
    header[4] = crc8(header[:4])
    result = bytes(header) + payload
    return result + struct.pack("<H", crc16(result))


def parse_a8_private(data):
    if (len(data) < 13 or data[0] != 0xAA or data[2] != 0x02 or
            crc8(data[:4]) != data[4]):
        raise ValueError("bad A8 private header")
    payload_length = data[3]
    if (len(data) != payload_length + 13 or
            crc16(data[:-2]) != struct.unpack_from("<H", data, len(data) - 2)[0]):
        raise ValueError("bad A8 private length or CRC")
    sequence = struct.unpack_from("<H", data, 5)[0]
    return data[1], sequence, data[7], data[8], data[9], data[10], data[11:-2]


def siyi_frame(control, sequence, opcode, payload=b""):
    result = struct.pack("<BBBHHB", 0x55, 0x66, control, len(payload), sequence, opcode) + payload
    return result + struct.pack("<H", crc16(result))


def parse_siyi(data):
    if len(data) < 10 or data[:2] != b"\x55\x66":
        raise ValueError("bad SIYI header")
    payload_length = struct.unpack_from("<H", data, 3)[0]
    if len(data) != payload_length + 10 or crc16(data[:-2]) != struct.unpack_from("<H", data, len(data) - 2)[0]:
        raise ValueError("bad SIYI length or CRC")
    return data[2], struct.unpack_from("<H", data, 5)[0], data[7], data[8:-2]


def signed_byte(value):
    return value - 256 if value >= 128 else value


def clamp(value, minimum, maximum):
    return max(minimum, min(maximum, value))


class Gimbal:
    def __init__(self, mounting_direction, backend="mt11"):
        self.backend = backend
        self.properties = TARGETS[backend]
        self.mounting_direction = mounting_direction
        self.yaw = -38.0 if backend != "mt11" else 0.0
        self.pitch = -20.0
        self.roll = 0.0
        self.yaw_rate = 0.0
        self.pitch_rate = 0.0
        self.commanded_rates = (0.0, 0.0)
        self.target = None
        self.mode = 0
        self.zoom = 1.0
        self.sequence = 1
        self.last_update = time.monotonic()
        self.started = self.last_update

    def update(self):
        now = time.monotonic()
        elapsed = min(now - self.last_update, 0.25)
        self.last_update = now
        if self.target is not None:
            target_yaw, target_pitch = self.target
            maximum_step = self.properties["gimbal_rate_max"] * elapsed
            yaw_error = target_yaw - self.yaw
            if self.properties["gimbal_yaw_continuous"]:
                yaw_error = self.wrap_angle(yaw_error)
            pitch_error = target_pitch - self.pitch
            yaw_step = clamp(yaw_error, -maximum_step, maximum_step)
            pitch_step = clamp(pitch_error, -maximum_step, maximum_step)
            self.yaw = self.constrain_yaw(self.yaw + yaw_step)
            self.pitch = clamp(self.pitch + pitch_step, self.properties["gimbal_pitch_min"], self.properties["gimbal_pitch_max"])
            self.yaw_rate = yaw_step / elapsed if elapsed > 0 else 0.0
            self.pitch_rate = pitch_step / elapsed if elapsed > 0 else 0.0
            if abs(yaw_error) < 0.1 and abs(pitch_error) < 0.1:
                self.target = None
                self.yaw_rate = 0.0
                self.pitch_rate = 0.0
                self.commanded_rates = (0.0, 0.0)
        else:
            tau = self.properties.get("sim_rate_time_constant", 0)
            steps, rates = [], []
            for actual, demand in zip((self.pitch_rate, self.yaw_rate), self.commanded_rates):
                if tau > 0:
                    decay = math.exp(-elapsed / tau)
                    steps.append(demand * elapsed + (actual - demand) * tau * (1 - decay))
                    rates.append(demand + (actual - demand) * decay)
                else:
                    steps.append(demand * elapsed)
                    rates.append(demand)
            self.pitch_rate, self.yaw_rate = rates
            self.yaw = self.constrain_yaw(self.yaw + steps[1])
            self.pitch = clamp(self.pitch + steps[0], self.properties["gimbal_pitch_min"], self.properties["gimbal_pitch_max"])

    def vendor_rate_response(self, axis, command):
        """Map a quantised SDK command to physical motion, not desired rate.

        Optional target curves contain signed (command, deg/s) pairs. Signed
        knots allow different positive/negative response. Unmeasured commands
        extrapolate the nearest segment; calibration notes document coverage.
        """
        command = clamp(command, -100, 100)
        if command == 0:
            return 0.0
        curve = self.properties.get("sim_" + axis + "_rate_curve")
        if curve:
            if len(curve) < 4 or len(curve) % 2 or any(
                    curve[i] >= curve[i + 2] for i in range(0, len(curve) - 2, 2)):
                raise ValueError("invalid " + axis + " rate response curve")
            segment = len(curve) - 4
            for i in range(0, len(curve) - 2, 2):
                if command <= curve[i + 2]:
                    segment = i
                    break
            x1, y1, x2, y2 = curve[segment:segment + 4]
            rate = y1 + (command - x1) * (y2 - y1) / (x2 - x1)
        else:
            rate = command * self.properties["vendor_" + axis + "_rate_full_scale"] / 100
        return rate * self.properties.get("sim_" + axis + "_rate_gain", 1.0)

    def constrain_yaw(self, value):
        if self.properties["gimbal_yaw_continuous"]:
            return self.wrap_angle(value)
        return clamp(value,self.properties["gimbal_yaw_min"],self.properties["gimbal_yaw_max"])

    @staticmethod
    def wrap_angle(value):
        return (value + 180.0) % 360.0 - 180.0

    def wire_pose(self, channel="feedback", rates=False):
        vector = (0, self.pitch_rate, self.yaw_rate) if rates else (self.roll, self.pitch, self.yaw)
        return transform(self.properties, channel, self.mounting_direction == 2,
                         vector, rates=rates, inverse=True)

    def wire_pitch(self):
        return self.wire_pose()[1]

    def wire_pitch_rate(self):
        return self.wire_pose(rates=True)[1]

    def attitude(self):
        roll, pitch, yaw = self.wire_pose()
        roll_rate, pitch_rate, yaw_rate = self.wire_pose(rates=True)
        return struct.pack("<hhhhhh", *[round(v * 10) for v in
                           (yaw, pitch, roll, yaw_rate, pitch_rate, roll_rate)])

    def private_attitude(self):
        roll, pitch, yaw = self.wire_pose("private_feedback")
        return struct.pack("<hhh", round((yaw % 360) * 10), round(pitch * 10), round(roll * 10))

    def handle_siyi(self, packet):
        _control, sequence, opcode, payload = parse_siyi(packet)
        reply = None
        if opcode == 0x01:
            reply = bytes((0, 4, 9, 1, 0, 0, 0, 4, 9, 0, 0, 0))
        elif opcode == 0x02:
            # AP_Mount_Siyi recognises the MT11's ZT30-compatible command set
            # from the standard SIYI 7A hardware prefix.
            reply = {"mt11": b"7A-MT11-SITL", "a8": b"73-A8-SITL", "zr10": b"6B-ZR10-SITL"}[self.backend]
        elif opcode == 0x07 and len(payload) == 2:
            self.update()
            self.target = None
            # Rate command 0x07 is positive right on both controllers even
            # though MT11 attitude report 0x0d is positive left.
            _, pitch_rate, yaw_rate = transform(
                self.properties, "rate_command", self.mounting_direction == 2,
                (0, self.vendor_rate_response("pitch", signed_byte(payload[1])),
                 self.vendor_rate_response("yaw", signed_byte(payload[0]))), rates=True, inverse=True)
            self.commanded_rates = (pitch_rate, yaw_rate)
            if not self.properties.get("sim_rate_time_constant", 0):
                self.pitch_rate, self.yaw_rate = self.commanded_rates
        elif opcode == 0x08 and len(payload) == 1:
            if payload[0] == 1:
                self.target = (0.0, 0.0)
            elif payload[0] == 2:
                self.target = (self.yaw, self.properties["gimbal_pitch_min"])
            elif payload[0] == 3:
                self.target = (self.yaw, self.properties["gimbal_pitch_max"])
            reply = b"\x01"
        elif opcode == 0x0C and len(payload) == 1 and 3 <= payload[0] <= 5:
            self.mode = payload[0] - 3
            reply = bytes((self.mode,))
            opcode = 0x19
        elif opcode == 0x0D and not payload:
            reply = self.attitude()
        elif opcode == 0x0E and len(payload) >= 4:
            yaw, pitch = struct.unpack_from("<hh", payload)
            self.update()
            _, pitch, yaw = transform(self.properties, "angle_command",
                                       self.mounting_direction == 2, (0, pitch / 10, yaw / 10), inverse=True)
            self.target = (clamp(yaw, self.properties["gimbal_yaw_min"], self.properties["gimbal_yaw_max"]),
                           clamp(pitch, self.properties["gimbal_pitch_min"], self.properties["gimbal_pitch_max"]))
            reply = self.attitude()[:6]
        elif opcode == 0x15 and not payload:
            distance_dm = round(125 + 20 * math.sin((time.monotonic() - self.started) * 0.7))
            reply = struct.pack("<H", distance_dm)
        elif opcode == 0x17 and not payload:
            reply = struct.pack("<ii", 1491234567, -351234567)
        elif opcode == 0x18 and not payload:
            reply = bytes((int(self.zoom), round((self.zoom % 1) * 10)))
        elif opcode == 0x16 and not payload:
            reply = bytes((int(self.properties["zoom_max"]), 0))
        elif opcode == 0x19 and not payload:
            reply = bytes((self.mode,))
        elif opcode in (0x22, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x32, 0x3E, 0x71):
            reply = b"\x01"
        if reply is None:
            return None
        return siyi_frame(2, sequence, opcode, reply)

    def handle_private(self, data):
        if self.properties['vendor_protocol'] == 2:
            if len(data) != 40 or data[:2] != b"\xa9\x5b" or crc16(data):
                raise ValueError("bad XFRobot MCU frame")
            if not data[7] & 0x40:
                roll, pitch, yaw = (struct.unpack_from('<h', data, offset)[0] / 100 for offset in (5, 8, 11))
                _, pitch, yaw = transform(self.properties, 'angle_command', self.mounting_direction == 2,
                                          (roll, pitch, yaw), inverse=True)
                self.target = (yaw, pitch)
            return self.z1_attitude()

        if self.backend != "mt11":
            parsed = parse_a8_private(data)
            camera_cpu = A8_CAMERA_CPU
        else:
            parsed = parse_mt11_private(data)
            camera_cpu = MT11_CAMERA_CPU
        _control, _sequence, source, destination, link, command, payload = parsed
        if source != camera_cpu or destination != GIMBAL_MCU or link != PRIVATE_LINK:
            return None
        reply = None
        if self.backend == "zr10" and command in (0x04, 0x05, 0x06, 0x07, 0x37):
            if command == 0x37 and len(payload) == 2:
                self.zoom = clamp(payload[0] + payload[1] * .1, 1, 30)
            elif command == 0x04 and len(payload) == 1:
                self.zoom = clamp(self.zoom + signed_byte(payload[0]), 1, 30)
            elif command in (0x06, 0x07):
                self.handle_siyi(siyi_frame(1, _sequence, {0x06: 0x07, 0x07: 0x08}[command], payload))
            return None
        if command == 0x17:
            reply = bytes((self.mounting_direction,))
        elif self.backend != "mt11" and command == 0x15:
            if payload:
                self.mode = payload[0]
            reply = bytes((self.mode,))
        elif command == TUNNEL_COMMAND:
            reply = self.handle_siyi(payload)
        if reply is None:
            return None
        sequence = self.sequence
        self.sequence = (self.sequence + 1) & 0xFFFF
        if self.backend != "mt11":
            return a8_private_frame(0x0A, sequence, command, reply)
        return mt11_private_frame(0x0A, sequence, command, reply)

    def z1_attitude(self):
        reply = bytearray(24)
        reply[:2] = b"\xb5\x9a"
        roll, pitch, yaw = self.wire_pose()
        for offset, value in ((12, roll), (14, pitch), (16, yaw), (18, pitch), (20, roll), (22, yaw)):
            struct.pack_into('<h', reply, offset, round(value * 100))
        return bytes(reply) + struct.pack('>H', crc16(reply))

    def stream_attitude(self):
        if self.properties['vendor_protocol'] == 2:
            return self.z1_attitude()

        if self.backend == "mt11":
            return None
        sequence = self.sequence
        self.sequence = (self.sequence + 1) & 0xFFFF
        return a8_private_frame(0x0A, sequence, 0x50,
                                self.private_attitude())


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=37261)
    parser.add_argument("--backend", choices=tuple(TARGETS), default="mt11")
    parser.add_argument("--orientation", choices=("upright", "inverted"), default="upright")
    parser.add_argument("--ready-file", type=Path,
                        help="create this file after the UDP socket is bound")
    return parser.parse_args()


def main():
    args = parse_args()
    if not 0 <= args.port <= 65535:
        raise SystemExit("port must be in 0..65535 (zero selects a free port)")
    stop = False

    def request_stop(_signum, _frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.host, args.port))
    bound_host, bound_port = sock.getsockname()
    gimbal = Gimbal(2 if args.orientation == "inverted" else 1, args.backend)
    if args.ready_file is not None:
        args.ready_file.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.ready_file.with_name(
            args.ready_file.name + f".{os.getpid()}.tmp")
        try:
            temporary.write_text(f"{bound_host}:{bound_port}\n", encoding="ascii")
            temporary.replace(args.ready_file)
        finally:
            temporary.unlink(missing_ok=True)
    print(f"gimbal-sim listening on udp://{bound_host}:{bound_port} "
          f"backend={args.backend} orientation={args.orientation}", flush=True)
    peer = None
    next_stream = time.monotonic() + 0.1
    try:
        while not stop:
            readable, _, _ = select.select((sock,), (), (), 0.05)
            gimbal.update()
            if readable:
                try:
                    data, peer = sock.recvfrom(8192)
                except (ConnectionResetError, ConnectionRefusedError):
                    # Windows reports ICMP port-unreachable on recvfrom when
                    # the camera restarts. Wait for its new UART connection.
                    peer = None
                    continue
                try:
                    reply = gimbal.handle_private(data)
                except (ValueError, struct.error) as error:
                    print(f"gimbal-sim rejected {len(data)} bytes from {peer}: {error}", file=sys.stderr, flush=True)
                else:
                    if reply is not None:
                        sock.sendto(reply, peer)
            now = time.monotonic()
            if peer is not None and args.backend != "mt11" and now >= next_stream:
                sock.sendto(gimbal.stream_attitude(), peer)
                next_stream = now + 0.1
    finally:
        sock.close()
        if args.ready_file is not None:
            args.ready_file.unlink(missing_ok=True)
    print("gimbal-sim stopped", flush=True)


if __name__ == "__main__":
    main()
