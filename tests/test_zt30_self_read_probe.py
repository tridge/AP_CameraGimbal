#!/usr/bin/env python3
"""Run the meaningful guarded copy/error/race fixture, no device APIs."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class SelfRead(unittest.TestCase):
 def test_own_memory_errors_short_reads_and_unmap_race(self):
  with tempfile.TemporaryDirectory() as d:
   binary=Path(d)/'probe'
   subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tools/zt30_self_read_probe.c'),'-lpthread','-o',str(binary)],check=True)
   result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=4)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr);self.assertIn('cases=10',result.stdout);self.assertIn('CASE short_page PASS',result.stdout);self.assertIn('CASE concurrent_unmap PASS',result.stdout);self.assertIn('CASE stack_guard PASS main_and_pthread',result.stdout)
 def test_no_configuration_or_external_process_selection(self):
  with tempfile.TemporaryDirectory() as d:
   binary=Path(d)/'probe';subprocess.run(['cc','-std=gnu11','-O2',str(ROOT/'tools/zt30_self_read_probe.c'),'-lpthread','-o',str(binary)],check=True)
   result=subprocess.run([str(binary),'unexpected'],capture_output=True,timeout=2);self.assertEqual(result.returncode,2);self.assertEqual(result.stdout,b'')
if __name__=='__main__':unittest.main()
