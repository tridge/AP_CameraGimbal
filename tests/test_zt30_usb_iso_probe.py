#!/usr/bin/env python3
"""Default/CLI RAM activation fences, without peripheral access."""
import os,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Probe(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory(prefix='zt30-iso-probe-host-');cls.binary=Path(cls.tmp.name)/'probe'
  subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tools/zt30_usb_iso_probe.cpp'),str(ROOT/'camera_app/src/backends/zt30/usb_iso.cpp'),str(ROOT/'camera_app/src/backends/zt30/uvc_frame.cpp'),'-o',str(cls.binary)],check=True)
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def test_default_layout(self):
  r=subprocess.run([str(self.binary)],capture_output=True,check=True)
  self.assertIn(b'no-USB-access',r.stdout);self.assertEqual(r.stderr,b'')
 def test_capture_requires_exact_ram_executable_directory(self):
  r=subprocess.run([str(self.binary),'--capture'],capture_output=True)
  self.assertEqual(r.returncode,3);self.assertIn(b'RAM qualification directory required',r.stderr)
 def test_unknown_and_extra_arguments(self):
  for args in [['--wrong'],['--capture','extra']]:
   r=subprocess.run([str(self.binary),*args],capture_output=True)
   self.assertEqual(r.returncode,2)
if __name__=='__main__':unittest.main()
