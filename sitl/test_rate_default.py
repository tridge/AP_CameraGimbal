#!/usr/bin/env python3
"""Factory Rate tracking closes MT11 small errors and follows sub-dead-zone motion."""
import argparse
import math
import os
from pathlib import Path
import subprocess
import tempfile
import time

from test_gimbal_angle_hold import MCU
from test_mavlink_parameters import connect, port, stop, wait_ready, receive, M

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT/'build/sitl')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mt11-rate-default-') as temp:
        root = Path(temp); config = root/'camera.ini'; ready = root/'ready'
        config.write_text('[mavlink]\nsystem_id=42\n')  # factory Rate
        mcu = MCU('mt11', 1)
        with mcu.lock:
            mcu.gimbal.yaw, mcu.gimbal.pitch = 3, -42
        tcp = port()
        env = dict(os.environ, CAMERA_APP_CONFIG=str(config), CAMERA_APP_BACKEND='mt11',
            CAMERA_APP_UART=f'udp://127.0.0.1:{mcu.socket.getsockname()[1]}',
            CAMERA_APP_PORT=str(port()), CAMERA_APP_RTSP_PORT=str(port()),
            CAMERA_APP_MAVLINK_TCP_PORT=str(tcp), CAMERA_APP_MAVLINK_UDP_PORT='0',
            CAMERA_APP_READY_PATH=str(ready), CAMERA_APP_RECORD_ROOT=str(root/'record'),
            CAMERA_APP_LOG_ROOT=str(root/'logs'), CAMERA_APP_CAPTURE_ROOT=str(root/'capture'))
        for key in ('CAMERA_APP_SITL_TERRAIN','CAMERA_APP_SITL_VIDEO1','CAMERA_APP_SITL_VIDEO2'):
            env.pop(key, None)
        link = app = None
        with (root/'camera.log').open('w') as log:
            try:
                app = subprocess.Popen([str(args.build.resolve()/'camera-app')], env=env,
                    stdout=log, stderr=subprocess.STDOUT)
                wait_ready(ready, app)
                link = connect(f'tcp:127.0.0.1:{tcp}')
                link.mav.srcSystem, link.mav.srcComponent = 42, 1
                def feed(elapsed, yaw=0, rate=0):
                    link.mav.heartbeat_send(M.MAV_TYPE_FIXED_WING, M.MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, M.MAV_STATE_ACTIVE)
                    link.mav.global_position_int_send(round(elapsed*1000), -350000000, 1490000000,
                        600000, 100000, 0, 0, 0, 0)
                    link.mav.attitude_send(round(elapsed*1000), 0, 0, math.radians(yaw), 0, 0, math.radians(rate))
                feed(0); time.sleep(.2)
                target_lat = -35 + math.degrees(100 / 6378137)
                link.mav.command_int_send(42, 154, M.MAV_FRAME_GLOBAL, M.MAV_CMD_DO_SET_ROI_LOCATION,
                    0, 0, 0, 0, 0, 0, round(target_lat*1e7), 1490000000, 500)
                ack = receive(link, 'COMMAND_ACK', lambda m: m.command == M.MAV_CMD_DO_SET_ROI_LOCATION)
                assert ack.result == M.MAV_RESULT_ACCEPTED, ack
                errors=[]; stationary=[]; start=time.monotonic()
                for frame in range(360):
                    time.sleep(max(0, start+frame*.05-time.monotonic()))
                    elapsed=time.monotonic()-start
                    yaw=.5*min(max(elapsed-8,0),5)-.5*max(elapsed-13,0)
                    feed(elapsed,yaw,0 if elapsed<8 else .5 if elapsed<13 else -.5)
                    with mcu.lock:
                        mcu.gimbal.update()
                        error=max(abs(math.remainder(mcu.gimbal.yaw+yaw,360)), abs(mcu.gimbal.pitch+45))
                        if 5<elapsed<8:stationary.append(error)
                        if elapsed>9:errors.append(error)
                    while link.recv_match(blocking=False) is not None: pass
                assert stationary and max(map(abs,stationary)) < .6, stationary
                assert errors and max(map(abs,errors)) < .9, errors
                print('PASS factory Rate: cold ROI closes 3-degree errors on both axes; 0.5deg/s tracking and reversal peak %.3fdeg' % max(map(abs,errors)),flush=True)
                # A fresh vehicle stream must not mask stale gimbal feedback.
                with mcu.lock: mcu.drop_feedback=True
                start=time.monotonic(); was_moving=False
                while time.monotonic()-start<1.2:
                    t=time.monotonic()-start
                    feed(18+t, 10*t, 10); time.sleep(.05)
                    with mcu.lock:
                        was_moving |= any(abs(v)>0 for v in mcu.gimbal.commanded_rates)
                assert was_moving, 'stale-stop test never commanded motion'
                with mcu.lock:
                    assert mcu.gimbal.commanded_rates == (0,0), mcu.gimbal.commanded_rates
                    mcu.drop_feedback=False
                # Stale vehicle data stops both axes even after pose recovers.
                time.sleep(2)
                with mcu.lock:
                    assert mcu.gimbal.commanded_rates == (0,0), mcu.gimbal.commanded_rates
                print('PASS pulse state stops on stale gimbal and vehicle telemetry',flush=True)
            except Exception:
                log.flush();print((root/'camera.log').read_text());raise
            finally:
                if link:link.close()
                stop(app);mcu.close()


if __name__ == '__main__':main()
