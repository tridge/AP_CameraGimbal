#!/usr/bin/env python3
"""Flat snapshot error, pointer and call-phase invariants without device APIs."""
from pathlib import Path
import subprocess,tempfile,unittest,re,json
ROOT=Path(__file__).resolve().parents[1]
class Payload(unittest.TestCase):
 def test_guarded_snapshots_and_forwarding_contract(self):
  with tempfile.TemporaryDirectory() as d:
   b=Path(d)/'probe';subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-DZT30_PAYLOAD_HOST_FIXTURE',str(ROOT/'tools/zt30_payload_probe.c'),'-lpthread','-o',str(b)],check=True)
   r=subprocess.run([str(b)],capture_output=True,text=True,timeout=3);self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertIn('RESULT PASS cases=11',r.stdout);self.assertEqual(sum(x.startswith('CASE ') for x in r.stdout.splitlines()),11)
 def test_registry_matches_audited_artifact(self):
  proof=ROOT/'tests/fixtures/zt30/registry.json'
  entries=json.loads(proof.read_text())['entries'];text=(ROOT/'tools/zt30_ioctl_registry.h').read_text()
  rows=re.findall(r'\{0x([0-9a-f]+)U,(\d+),(\d+),(\d+),(\d+),"([^"]+)"\}',text)
  mapping={'sys':0,'sensor':1,'vif':2,'isp':3,'scl':4,'venc':5}
  expected=[(e['request'],str(e['length']),str(e['pre_read_length']),str(mapping[e['device']]),str(int(e['post_snapshot'])),e['function']) for e in entries]
  self.assertEqual(rows,expected)
if __name__=='__main__' :unittest.main()
