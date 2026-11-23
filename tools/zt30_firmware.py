#!/usr/bin/env python3
"""ZT30 SD container: U-Boot script, customer UBIFS, application bundle, index.

The stock 24-byte trailer remains at EOF. The SD script loads only UBIFS;
the AP web updater verifies the whole container and stages only the bundle.
"""
import hashlib
import json
import struct
import zlib

from build_zt30_dev_package import SCRIPT_BYTES, CUSTOMER_BYTES, check_ubifs

MAGIC = b'APCZTF01'
INDEX = struct.Struct('<8sQQ32s32s')
LIMIT = 112 * 1024 * 1024


def script_for(payload):
    size = len(payload)
    crc = zlib.crc32(payload) & 0xffffffff
    # The captured U-Boot has the four-argument memory CRC command (not -v),
    # mw, cmp, setenv and multi-variable run. run stops at the first failure;
    # no separate write command is allowed after that run. Never saveenv.
    return ('\n# AP CameraGimbal ZT30 customer update\n'
            f'# payload_sha256 {hashlib.sha256(payload).hexdigest()}\n'
            f"setenv apcg_load 'fatload mmc 0 0x21000000 $(SdUpgradeImage) 0x{size:x} 0x4000'\n"
            "setenv apcg_base 'base 0'\n"
            f"setenv apcg_expected 'mw.l 0x20f00000 0x{crc:08x} 1'\n"
            f"setenv apcg_crc 'crc32 0x21000000 0x{size:x} 0x20f00004'\n"
            "setenv apcg_compare 'cmp.l 0x20f00000 0x20f00004 1'\n"
            "setenv apcg_partition 'ubi part ubi0'\n"
            "setenv apcg_volume 'ubi check customer'\n"
            f"setenv apcg_write 'ubi write 0x21000000 customer 0x{size:x}'\n"
            'run apcg_load apcg_base apcg_expected apcg_crc apcg_compare apcg_partition apcg_volume apcg_write bootcmd\n'
            '% <- this is end of script symbol\n').encode('ascii')


def pack(payload, bundle):
    check_ubifs(payload)
    if not bundle.startswith(b'APCZT301') or not 12 < len(bundle) <= 32 * 1024 * 1024:
        raise ValueError('invalid embedded application bundle')
    header = script_for(payload).ljust(SCRIPT_BYTES, b'\0')
    if len(header) != SCRIPT_BYTES:
        raise ValueError('SD script exceeds header')
    index = INDEX.pack(MAGIC, len(payload), len(bundle), hashlib.sha256(header + payload).digest(),
                       hashlib.sha256(bundle).digest())
    image = header + payload + bundle + index + b'12345678' + header[:16]
    inspect(image)
    return image


def inspect(image):
    if not SCRIPT_BYTES + INDEX.size + 36 < len(image) <= LIMIT:
        raise ValueError('invalid ZT30 firmware size')
    header = image[:SCRIPT_BYTES]
    if image[-24:] != b'12345678' + header[:16]:
        raise ValueError('invalid stock trailer')
    magic, size, bundle_size, payload_hash, bundle_hash = INDEX.unpack(image[-24-INDEX.size:-24])
    if magic != MAGIC or not 0 < size <= CUSTOMER_BYTES or not 12 < bundle_size <= 32 * 1024 * 1024:
        raise ValueError('invalid firmware index')
    offset = SCRIPT_BYTES + size
    if offset + bundle_size + INDEX.size + 24 != len(image):
        raise ValueError('invalid firmware offsets')
    payload, bundle = image[SCRIPT_BYTES:offset], image[offset:offset+bundle_size]
    if hashlib.sha256(image[:offset]).digest() != payload_hash or hashlib.sha256(bundle).digest() != bundle_hash:
        raise ValueError('firmware checksum mismatch')
    check_ubifs(payload)
    if header != script_for(payload).ljust(SCRIPT_BYTES, b'\0') or not bundle.startswith(b'APCZT301'):
        raise ValueError('invalid SD script or application bundle')
    return dict(format='apcg.zt30.firmware.v1', payload_bytes=size, bundle_offset=offset,
                bundle_bytes=bundle_size, bytes=len(image), sha256=hashlib.sha256(image).hexdigest())


if __name__ == '__main__':
    import argparse
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    print(json.dumps(inspect(parser.parse_args().image.read_bytes()), indent=2))
