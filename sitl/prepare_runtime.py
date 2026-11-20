#!/usr/bin/env python3
"""Create the writable filesystem tree used by AP_CameraGimbal SITL."""

import argparse
import os
import pathlib
import shutil
import struct


def write_if_missing(path, data, mode=0o644):
    if path.exists():
        return
    path.write_bytes(data)
    path.chmod(mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runtime")
    parser.add_argument("camera_config")
    parser.add_argument("--reset-parameters", action="store_true")
    args = parser.parse_args()

    root = pathlib.Path(args.runtime).resolve()
    for relative in ("app/bin", "app/dropbear", "mnt/DCIM/capture",
                     "mnt/DCIM/record", "run", "runtime-auth"):
        (root / relative).mkdir(parents=True, exist_ok=True)
    camera_config = root / "app/camera.ini"
    if args.reset_parameters and camera_config.exists():
        shutil.copy2(camera_config, camera_config.with_suffix('.ini.reset.bak'))
    if args.reset_parameters or not camera_config.exists():
        temporary = camera_config.with_suffix('.ini.new')
        shutil.copyfile(args.camera_config, temporary)
        # Seed each launcher's camera identity and MAVLink ports once. Saved
        # web settings continue to take effect on subsequent launches.
        instance = os.environ.get('CAMERA_GIMBAL_SITL_INSTANCE')
        if instance:
            try:
                from .launcher_config import initial_config
            except ImportError:
                from launcher_config import initial_config
            temporary.write_text(initial_config(temporary.read_text(), int(instance)))
        os.replace(temporary, camera_config)
    write_if_missing(root / "app/config.ini", b"# vendor configuration is unavailable in SITL\n")
    write_if_missing(root / "app/web.pass", b"ardupilot\n", 0o600)
    write_if_missing(root / "app/app_selection.txt", b"replacement\n")
    write_if_missing(root / "app/dropbear/authorized_keys", b"", 0o600)
    write_if_missing(root / "runtime-auth/authorized_keys", b"", 0o600)

    # Three SS928 sensor registers corresponding to approximately 52 C.
    sensor = bytearray(4096)
    for offset, raw in ((0x008, 546), (0x108, 548), (0x208, 544)):
        struct.pack_into("<I", sensor, offset, raw)
    (root / "run/soc-temperature").write_bytes(sensor)


if __name__ == "__main__":
    main()
