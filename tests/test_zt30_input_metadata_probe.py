#!/usr/bin/env python3
"""Inert default and activation fences; host tests never load camera SDK."""
import os,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Probe(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory(prefix='zt30-input-probe-host-');cls.binary=Path(cls.tmp.name)/'probe'
  subprocess.run([os.environ.get('CXX','g++'),'-std=c++11','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tools/zt30_input_metadata_probe.cpp'),str(ROOT/'camera_app/src/backends/zt30/input_metadata.cpp'),'-ldl','-o',str(cls.binary)],check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def test_default_layout(self):
  r=subprocess.run([str(self.binary)],capture_output=True,check=True)
  self.assertIn(b'no-SDK-access',r.stdout);self.assertEqual(r.stderr,b'')
 def test_inspect_requires_exact_ram_executable_directory(self):
  r=subprocess.run([str(self.binary),'--inspect'],capture_output=True)
  self.assertEqual(r.returncode,3);self.assertIn(b'RAM qualification directory required',r.stderr)
 def test_unknown_and_extra_arguments(self):
  for args in [['--wrong'],['--inspect','extra']]:
   self.assertEqual(subprocess.run([str(self.binary),*args],capture_output=True).returncode,2)
if __name__=='__main__':unittest.main()
