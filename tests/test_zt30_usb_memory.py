#!/usr/bin/env python3
"""Private USB structure-copy method; no vendor process or USB/SDK access."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class Memory(unittest.TestCase):
 def test_private_guard_parser_scale_race_and_fixed_abi(self):
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory);binary=p/'probe'
   subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fstack-usage','-I'+str(ROOT/'tools'),str(ROOT/'tests/zt30_usb_memory_fixture.c'),'-lpthread','-o',str(binary)],check=True)
   result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=5)
   self.assertEqual(result.returncode,0,result.stderr+result.stdout)
   self.assertEqual(sum(l.startswith('SCALE ') for l in result.stdout.splitlines()),12)
   for marker in ['PARSER PASS','VENDOR_SCALE PASS','SMALL_STACK PASS','HIGH_POINTER PASS','FORK PASS','RACE PASS','FD_REUSE PASS','SMAPS_REUSE PASS','RESULT PASS']:self.assertIn(marker,result.stdout)
   reports=list(p.glob('*.su'));self.assertTrue(reports,'missing stack-use report');total=0
   for report in reports:
    for line in report.read_text().splitlines():
     fields=line.split('\t');self.assertLessEqual(int(fields[1]),8192,line);total+=int(fields[1])
   self.assertLessEqual(total,12288,'conservative sum of all fixture frames')
   print(result.stdout,end='')
if __name__=='__main__':unittest.main()
