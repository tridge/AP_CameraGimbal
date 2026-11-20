#!/usr/bin/env python3
"""Exercise the legacy safe tar extraction path independently of host Python."""
import io
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from safe_tar import safe_extract


def make_tar(entries):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode='w') as archive:
        for name, kind, data, mode, linkname in entries:
            member = tarfile.TarInfo(name)
            member.mode = mode
            member.uid = member.gid = 12345
            member.uname = member.gname = 'archive-owner'
            if kind == 'file':
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
            else:
                member.type = {
                    'symlink': tarfile.SYMTYPE, 'hardlink': tarfile.LNKTYPE,
                    'fifo': tarfile.FIFOTYPE, 'chardev': tarfile.CHRTYPE,
                    'dir': tarfile.DIRTYPE,
                }[kind]
                member.linkname = linkname or ''
                if kind == 'chardev':
                    member.devmajor, member.devminor = 1, 3
                archive.addfile(member)
    return output.getvalue()


class SafeTar(unittest.TestCase):
    def extract_legacy(self, data, destination):
        with mock.patch.object(tarfile, 'data_filter', None, create=True), \
                mock.patch.object(tarfile.TarFile, 'chown', side_effect=AssertionError('archive chown')):
            safe_extract(io.BytesIO(data), destination)

    def test_hardened_modes_and_relative_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'out'
            self.extract_legacy(make_tar([
                ('.', 'dir', b'', 0o777, None),
                ('tool/readonly/', 'dir', b'', 0o555, None),
                ('tool/readonly/child', 'file', b'child', 0o644, None),
                ('tool/bin/compiler', 'file', b'compiler', 0o4777, None),
                ('tool/bin/nonexec', 'file', b'data', 0o055, None),
                ('tool/link', 'symlink', b'', 0o777, 'bin/compiler'),
            ]), root)
            self.assertEqual((root / 'tool/bin/compiler').read_bytes(), b'compiler')
            self.assertEqual((root / 'tool/bin/compiler').stat().st_mode & 0o7777, 0o755)
            self.assertEqual((root / 'tool/bin/compiler').stat().st_uid,
                             getattr(os, 'getuid', lambda: 0)())
            self.assertEqual((root / 'tool/bin/compiler').stat().st_gid,
                             getattr(os, 'getgid', lambda: 0)())
            self.assertEqual((root / 'tool/bin/nonexec').stat().st_mode & 0o777, 0o644)
            self.assertEqual((root / 'tool/readonly/child').read_bytes(), b'child')
            self.assertEqual(os.readlink(root / 'tool/link'), 'bin/compiler')

    def test_existing_symlink_cannot_be_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'out'
            archive = make_tar([
                ('original', 'file', b'keep', 0o644, None),
                ('link', 'symlink', b'', 0o777, 'original'),
                ('link', 'file', b'overwrite', 0o644, None),
            ])
            with self.assertRaises(RuntimeError):
                self.extract_legacy(archive, root)
            self.assertEqual((root / 'original').read_bytes(), b'keep')

    def test_chained_symlink_escape_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'out'
            archive = make_tar([
                ('NAME/', 'dir', b'', 0o777, None),
                ('NAME/up', 'symlink', b'', 0o777, '..'),
                ('NAME/up/up2', 'symlink', b'', 0o777, '..'),
                ('NAME/up/up2/pwned', 'file', b'bad', 0o666, None),
            ])
            with self.assertRaises(RuntimeError):
                self.extract_legacy(archive, root)
            self.assertFalse((Path(directory) / 'pwned').exists())

    def test_rejects_hostile_members(self):
        hostile = [
            ('/absolute', 'file', b'x', 0o644, None),
            ('../traversal', 'file', b'x', 0o644, None),
            ('escape', 'symlink', b'', 0o777, '../../outside'),
            ('hardlink', 'hardlink', b'', 0o644, '../outside'),
            ('pipe', 'fifo', b'', 0o644, None),
            ('device', 'chardev', b'', 0o644, None),
        ]
        for entry in hostile:
            with self.subTest(name=entry[0]), tempfile.TemporaryDirectory() as directory:
                with self.assertRaises(RuntimeError):
                    self.extract_legacy(make_tar([entry]), Path(directory) / 'out')


if __name__ == '__main__':
    unittest.main()
