#!/usr/bin/env python3
"""ZT30 dual-use container, SD command ordering and bootstrap layout checks."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import zt30_firmware as firmware
import build_zt30_firmware as builder
from test_zt30_dev_package import payload


class Firmware(unittest.TestCase):
    def test_container_offsets_checksums_and_stock_trailer(self):
        app = b'APCZT301'+b'fixture-app-payload'
        image = firmware.pack(payload(), app)
        report = firmware.inspect(image)
        self.assertEqual(image[report['bundle_offset']:report['bundle_offset']+report['bundle_bytes']], app)
        self.assertEqual(image[-24:], b'12345678'+image[:16])
        self.assertEqual(report['payload_bytes'], len(payload()))
        for at in (0, 100, firmware.SCRIPT_BYTES+8192, report['bundle_offset']+12, len(image)-25, len(image)-1):
            damaged = bytearray(image); damaged[at] ^= 1
            with self.subTest(at=at), self.assertRaises(ValueError): firmware.inspect(damaged)
        for damaged in (image[:-1], image+b'x', image[:16384], b'x'*len(image)):
            with self.assertRaises(ValueError): firmware.inspect(damaged)

    def test_sd_run_chain_never_writes_after_failed_load_or_checksum(self):
        # Interpret actual setenv/run lines with U-Boot's multi-variable run
        # semantics: a failed command stops the list before subsequent writes.
        script = firmware.script_for(payload()).decode()
        env = {}; chain = None
        for line in script.splitlines():
            if not line or line.startswith(('#', '%')): continue
            args = shlex.split(line)
            if args[0] == 'setenv': env[args[1]] = shlex.split(args[2])
            else:
                self.assertEqual(args[0], 'run'); self.assertIsNone(chain); chain = args[1:]
        self.assertEqual(chain[-1], 'bootcmd')
        self.assertEqual(env['apcg_load'][-2:], [hex(len(payload())), '0x4000'])
        self.assertEqual(int(env['apcg_expected'][2], 16), zlib.crc32(payload()) & 0xffffffff)
        self.assertEqual(env['apcg_crc'][0], 'crc32')
        self.assertEqual(env['apcg_compare'], ['cmp.l', '0x20f00000', '0x20f00004', '1'])
        for failure in chain[:-1]:
            executed = []
            for command in chain:
                executed.append(command)
                if command == failure: break
            if failure != 'apcg_write': self.assertNotIn('apcg_write', executed)
            self.assertNotIn('bootcmd', executed)
        for forbidden in ('nand erase', 'ubi create', 'saveenv', 'setenv bootargs', 'ubi write 0x21000000 rootfs'):
            self.assertNotIn(forbidden, script)

    def test_bootstrap_is_slot_based_and_contains_no_captured_unit_state(self):
        files = {'camera-app': b'camera', 'zt30-web': b'web', 'encoder-sdk.so': b'sdk',
                 'apcam-upload': b'worker', 'upload-ca.pem': b'ca'}
        for name in ('live.js', 'status-actions.html', 'parameters-form.html'): files['webroot/'+name] = b'asset'
        manifest = dict(schema='apcg.zt30.app.v1', board='SIYI_ZT30', launcher=1, version='test',
                        runtime=[dict(name='libc.so.6', path='/lib/libc.so.6', size=7,
                                      sha256=hashlib.sha256(b'runtime').hexdigest())],
                        files=[dict(path=n, mode=0o644 if n.startswith('webroot/') or n.endswith('.pem') else 0o755,
                                               size=len(b), sha256=hashlib.sha256(b).hexdigest())
                                           for n, b in sorted(files.items())])
        text = json.dumps(manifest, separators=(',', ':')).encode()
        data = b'APCZT301'+struct.pack('<I', len(text))+text+b''.join(files[n] for n in sorted(files))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); customer = root/'customer'; customer.mkdir()
            (root/'lib').mkdir(); (root/'lib/libc.so.6').write_bytes(b'runtime')
            bundle = root/'app'; bundle.write_bytes(data)
            binary = root/'binary'; binary.write_bytes(b'bootstrap')
            args = argparse.Namespace(bundle=bundle, slot_tool=binary, dropbear=binary, dropbearkey=binary,
                                      runtime_root=root, readelf='readelf')
            with patch.object(builder, 'check') as checker: slot = builder.populate(customer, args)
            self.assertIn('apcam-upload', [c.args[0].name for c in checker.call_args_list])
            orphan = dict(manifest, files=[f for f in manifest['files'] if f['path'] != 'upload-ca.pem'])
            text2 = json.dumps(orphan, separators=(',', ':')).encode()
            data2 = b'APCZT301'+struct.pack('<I', len(text2))+text2+b''.join(files[n] for n in sorted(files) if n != 'upload-ca.pem')
            other = root/'other'; other.mkdir()
            with patch.object(builder, 'check'), self.assertRaisesRegex(ValueError, 'native upload'):
                builder.populate(other, args, data2)
            app = customer/'camera-app'
            self.assertEqual((app/'active').readlink().as_posix(), 'slots/'+slot)
            for n in files: self.assertEqual((app/'slots'/slot/n).read_bytes(), files[n])
            self.assertEqual((app/'webroot').readlink().as_posix(), 'active/webroot')
            self.assertEqual((app/'web.pass.default').stat().st_mode & 0o777, 0o600)
            self.assertTrue((customer/'.sys_upgrade_finish_flag').is_file())
            self.assertEqual({p.name for p in customer.iterdir()}, {'demo.sh', 'camera-app', '.sys_upgrade_finish_flag'})
            self.assertFalse((app/'camera.ini').exists())
            self.assertFalse((app/'auth').exists())


if __name__ == '__main__': unittest.main()
