#!/usr/bin/env python3
"""Vendor launcher rejects malformed groups/programs before changing credentials."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class VendorExec(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='zt30-exec-test-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'exec'
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', str(ROOT/'tools/zt30_vendor_exec.c'), '-o', str(cls.binary)], check=True)
    def run_exec(self,*args):
        return subprocess.run([str(self.binary),*args],capture_output=True,text=True,timeout=3)
    def test_invalid_groups(self):
        for groups in ['-1','+1','oops','0; touch x','4294967296','0 '*65]:
            with self.subTest(groups=groups):
                self.assertEqual(self.run_exec('--check-groups',groups).returncode,2)
    def test_wrong_program(self):
        self.assertEqual(self.run_exec('', '/bin/true').returncode,2)
    def test_keeps_current_groups_without_privileges(self):
        groups=os.getgroups()
        result=self.run_exec('--check-groups',' '.join(map(str,groups)))
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn(f'group_count={len(groups)}',result.stdout)
if __name__=='__main__': unittest.main()
