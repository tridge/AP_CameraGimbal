#!/usr/bin/env python3
import argparse, importlib.util, json, struct, tarfile, tempfile, unittest, zlib
from unittest.mock import patch
from pathlib import Path
p=Path(__file__).resolve().parents[1]/'tools/build_zt30_dev_package.py'
spec=importlib.util.spec_from_file_location('package',p);package=importlib.util.module_from_spec(spec);spec.loader.exec_module(package)
def payload():
    data=bytearray(b'\xff'*(package.LEB_BYTES*3))
    data[:4096]=bytes(4096)
    struct.pack_into('<I',data,0,package.MAGIC);struct.pack_into('<I',data,16,4096);data[20]=6
    struct.pack_into('<IIII',data,32,package.MIN_IO,package.LEB_BYTES,3,package.CUSTOMER_FS_MAX_LEBS)
    struct.pack_into('<I',data,4,zlib.crc32(data[8:4096])^0xffffffff)
    return bytes(data)
class PackageTests(unittest.TestCase):
    def test_built_package_includes_external_web_assets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);baseline=root/'customer';baseline.mkdir()
            (baseline/'demo.sh').write_text('#!/bin/sh\necho platform\nchmod a+x /customer/siyi_app_359\n/vendor/app\n')
            binary=root/'binary';binary.write_bytes(b'fixture')
            args=argparse.Namespace(output=root/'firmware.candidate',customer_root=baseline,
                camera_app=binary,web_app=binary,sdk=binary,dropbear=binary,dropbearkey=binary,slot_tool=binary,
                authorized_keys=None,overlay_output=root/'overlay.tar.gz',mkfs='fixture-mkfs')
            def mkfs(cmd,check):
                Path(cmd[cmd.index('-o')+1]).write_bytes(payload())
            with patch.object(package.subprocess,'run',side_effect=mkfs),patch('builtins.print'):
                package.build(args)
            with tarfile.open(args.overlay_output) as archive:
                self.assertTrue(all(member.uid==0 and member.gid==0 for member in archive))
                for asset in (package.ROOT/'web/webroot').rglob('*'):
                    if asset.is_file():
                        path='camera-app/webroot/'+str(asset.relative_to(package.ROOT/'web/webroot'))
                        self.assertEqual(archive.extractfile(path).read(),asset.read_bytes())
                        self.assertEqual(archive.getmember(path).mode,0o644)
            manifest=json.loads(args.overlay_output.with_suffix('.gz.json').read_text())
            self.assertIn('camera-app/webroot/status-actions.html',manifest['files'])

    def test_roundtrip_and_scope(self):
        report=package.inspect(package.pack(payload()))
        self.assertFalse(report['installable']);self.assertEqual(report['write_scope'],['ubi0:customer'])
        script=package.script_for(payload()).decode()
        for forbidden in ('nand erase','ubi create','saveenv','setenv bootargs','miservice'):
            self.assertNotIn(forbidden,script)
    def test_corruption_after_superblock(self):
        image=bytearray(package.pack(payload()));image[package.SCRIPT_BYTES+8192]^=1
        with self.assertRaisesRegex(ValueError,'checksum'):package.inspect(image)
    def test_trailer(self):
        image=bytearray(package.pack(payload()));image[-1]^=1
        with self.assertRaisesRegex(ValueError,'trailer'):package.inspect(image)
    def test_truncation(self):
        with self.assertRaises(ValueError):package.inspect(package.pack(payload())[:-4096])
    def test_extra_write_command(self):
        image=bytearray(package.pack(payload()));at=image.index(b'ubi check customer')
        image[at:at+18]=b'nand erase rootfs '
        with self.assertRaises(ValueError):package.inspect(image)
    def test_wrong_geometry(self):
        data=bytearray(payload());struct.pack_into('<I',data,44,700)
        struct.pack_into('<I',data,4,zlib.crc32(data[8:4096])^0xffffffff)
        with self.assertRaisesRegex(ValueError,'geometry'):package.pack(data)
    def test_payload_exceeds_filesystem(self):
        with self.assertRaisesRegex(ValueError,'length exceeds filesystem'):
            package.pack(payload()+b'\xff'*package.LEB_BYTES)
    def test_wrong_superblock_crc(self):
        data=bytearray(payload());data[100]^=1
        with self.assertRaisesRegex(ValueError,'CRC'):package.pack(data)
if __name__=='__main__':unittest.main()
