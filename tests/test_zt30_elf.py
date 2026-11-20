#!/usr/bin/env python3
"""ZT30 loader qualification contracts with controlled readelf/runtime fixtures."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('zt30_elf', ROOT / 'tools/check_zt30_elf.py')
elf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(elf)
HEADER = """Class: ELF32
Data: 2's complement, little endian
Machine: ARM
Flags: 0x5000400, Version5 EABI, hard-float ABI
Tag_CPU_arch: v7
Tag_CPU_arch_profile: Application
Tag_ABI_VFP_args: VFP registers
"""
INTERP = '[Requesting program interpreter: /lib/ld-linux-armhf.so.3]\n'


def image(needed=(), requirements=None, exports=(), executable=False):
    result = HEADER + (INTERP if executable else '')
    result += ''.join(f'(NEEDED) Shared library: [{name}]\n' for name in needed)
    if exports:
        result += 'Version definition section\n'
        result += ''.join(f'Name: {name}\n' for name in exports)
    if requirements:
        result += 'Version needs section\n'
        for name, versions in requirements.items():
            result += f'File: {name}\n'
            result += ''.join(f'Name: {version}\n' for version in versions)
    return result


class ElfContract(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='zt30-elf-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.binary = self.root / 'probe'
        self.data = {self.binary: image(executable=True)}
        self.mock = patch.object(elf, 'elf_info', side_effect=lambda path, _: self.data[Path(path)])
        self.mock.start()
        self.addCleanup(self.mock.stop)

    def library(self, name, data=None, directory='/lib'):
        path = self.root / directory.lstrip('/') / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        self.data[path] = data or image()
        return path

    def check(self, **kwargs):
        return elf.check(self.binary, runtime_root=self.root, **kwargs)

    def test_shared_library_contract(self):
        self.data[self.binary] = image() + 'Type: DYN (Shared object file)\n'
        self.assertIsNone(self.check(shared_library=True)['interpreter'])
        with self.assertRaisesRegex(ValueError, 'incompatible ARM ELF'):
            self.check()
        self.data[self.binary] += INTERP
        with self.assertRaisesRegex(ValueError, 'must not have an interpreter'):
            self.check(shared_library=True)
        self.data[self.binary] = image() + 'Type: EXEC\n'
        with self.assertRaisesRegex(ValueError, 'shared library type'):
            self.check(shared_library=True)

    def test_accept_application(self):
        self.assertTrue(self.check()['runtime_checked'])

    def test_reject_wrong_architecture_and_abi(self):
        for before, after in [('ELF32', 'ELF64'), ('Machine: ARM', 'Machine: X86-64'),
                              ('little endian', 'big endian'), ('hard-float ABI', 'soft-float ABI'),
                              ('v7', 'v6'), ('Application', 'Microcontroller'),
                              ('VFP registers', 'Base AAPCS'), (INTERP, '')]:
            with self.subTest(after=after):
                self.data[self.binary] = image(executable=True).replace(before, after)
                with self.assertRaisesRegex(ValueError, 'incompatible ARM ELF'):
                    self.check()

    def test_reject_version_ceilings_and_private(self):
        for version in ['GLIBC_2.31', 'GLIBCXX_3.4.27', 'CXXABI_1.3.13', 'GCC_8.0.0', 'GLIBC_PRIVATE']:
            with self.subTest(version=version):
                self.data[self.binary] = image(requirements={'libc.so.6': [version]}, executable=True)
                with self.assertRaisesRegex(ValueError, 'ceiling|private runtime'):
                    self.check()

    def test_missing_library(self):
        self.data[self.binary] = image(needed=['missing.so'], executable=True)
        with self.assertRaisesRegex(ValueError, 'runtime library missing'):
            self.check()

    def test_missing_direct_version(self):
        self.data[self.binary] = image(needed=['libc.so.6'], requirements={'libc.so.6': ['GLIBC_2.4']}, executable=True)
        self.library('libc.so.6', image(exports=['GLIBC_2.3']))
        with self.assertRaisesRegex(ValueError, 'missing runtime versions'):
            self.check()

    def test_late_transitive_version_requirement(self):
        # Stack visits b first, then a introduces a new requirement on b.
        self.data[self.binary] = image(needed=['a.so', 'b.so'], executable=True)
        self.library('b.so', image(exports=['B_1']))
        self.library('a.so', image(needed=['b.so'], requirements={'b.so': ['B_2']}))
        with self.assertRaisesRegex(ValueError, 'missing transitive runtime versions'):
            self.check()

    def test_transitive_cycle_and_internal_private_version(self):
        self.data[self.binary] = image(needed=['a.so'], executable=True)
        self.library('a.so', image(needed=['b.so'], requirements={'b.so': ['GLIBC_PRIVATE']}))
        self.library('b.so', image(needed=['a.so'], exports=['GLIBC_PRIVATE']))
        self.assertEqual(len(self.check()['runtime_dependencies']), 2)

    def test_dlopen_libraries_are_qualified(self):
        self.library('plugin.so', image(needed=['dependency.so']))
        with self.assertRaisesRegex(ValueError, 'runtime library missing'):
            self.check(load_libraries=['plugin.so'])
        self.library('dependency.so')
        self.assertEqual(len(self.check(load_libraries=['plugin.so'])['runtime_dependencies']), 2)
        with self.assertRaisesRegex(ValueError, 'requires runtime-root'):
            elf.check(self.binary, load_libraries=['plugin.so'])

    def test_unqualified_version_library_alias_is_rejected(self):
        self.data[self.binary] = image(needed=['real.so'], requirements={'alias.so': ['A_1']}, executable=True)
        self.library('real.so', image(exports=['A_1']))
        with self.assertRaisesRegex(ValueError, 'unqualified version-needs library aliases'):
            self.check()

    def test_runtime_architecture(self):
        self.data[self.binary] = image(needed=['a.so'], executable=True)
        self.library('a.so', image().replace('Machine: ARM', 'Machine: X86-64'))
        with self.assertRaisesRegex(ValueError, 'incompatible runtime ELF'):
            self.check()

    def test_absolute_parent_and_relative_file_symlinks(self):
        path = self.library('real.so', directory='/config/lib')
        (self.root / 'config/lib/link.so').symlink_to('real.so')
        (self.root / 'lib').symlink_to('/config/lib')
        self.data[self.binary] = image(needed=['link.so'], executable=True)
        self.assertEqual(self.check()['runtime_dependencies'][0]['path'], str(path))

    def test_symlink_escape_and_loop(self):
        (self.root / 'lib').symlink_to('../../outside')
        with self.assertRaisesRegex(ValueError, 'escapes captured root'):
            elf.runtime_library(self.root, 'a.so', ['/lib'])
        (self.root / 'lib').unlink()
        (self.root / 'lib').symlink_to('/loop')
        (self.root / 'loop').symlink_to('/lib')
        with self.assertRaisesRegex(ValueError, 'symlink loop'):
            elf.runtime_library(self.root, 'a.so', ['/lib'])

    def test_explicit_library_path_precedence(self):
        self.data[self.binary] = image(needed=['a.so'], executable=True)
        self.library('a.so', image(exports=['vendor']), '/config/lib')
        expected = self.library('a.so')
        self.assertEqual(self.check()['runtime_dependencies'][0]['path'], str(expected))
        result = self.check(library_path=('/config/lib', '/lib'))
        self.assertIn('/config/lib/a.so', result['runtime_dependencies'][0]['path'])

    def test_reject_unqualified_loader_paths(self):
        self.data[self.binary] = image(executable=True) + '(RUNPATH) Library runpath: [/config/lib]'
        with self.assertRaisesRegex(ValueError, 'RPATH/RUNPATH'):
            self.check()
        self.data[self.binary] = image(needed=['a.so'], executable=True)
        self.library('a.so', image() + '(RPATH) Library rpath: [/config/lib]')
        with self.assertRaisesRegex(ValueError, 'RPATH/RUNPATH'):
            self.check()
        for directory in ['relative', '$ORIGIN', '']:
            with self.subTest(directory=directory):
                with self.assertRaisesRegex(ValueError, 'unsupported device library path'):
                    elf.runtime_library(self.root, 'a.so', [directory])


if __name__ == '__main__':
    unittest.main()
