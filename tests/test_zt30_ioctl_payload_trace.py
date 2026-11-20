#!/usr/bin/env python3
"""Test real interposition with fake ioctl, fixed harmless nodes and own stacks."""
from pathlib import Path
import os,re,runpy,secrets,shutil,struct,subprocess,unittest
ROOT=Path(__file__).resolve().parents[1]
FIXTURES=ROOT/'tests/fixtures/zt30'
def session():
 p=Path('/tmp/apcg-zt30.session.'+secrets.token_hex(3));p.mkdir();return p
class PayloadTrace(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.build=session();cls.app=cls.build/'probe';cls.mock=cls.build/'mock.so';cls.library=cls.build/'observer.so';cls.cap_library=cls.build/'cap.so'
  fstype=subprocess.check_output(['stat','-f','-c','%t',str(cls.build)],text=True).strip()
  common=['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror']
  subprocess.run(common+[str(ROOT/'tools/zt30_payload_trace_fixture.c'),'-lpthread','-o',str(cls.app)],check=True)
  subprocess.run(common+['-fPIC','-shared',str(ROOT/'tools/zt30_payload_trace_mock.c'),'-ldl','-o',str(cls.mock)],check=True)
  for lib,extra in [(cls.library,[]),(cls.cap_library,['-DZT30_PAYLOAD_LIMIT=1024'])]:
   subprocess.run(common+['-fPIC','-shared','-DZT30_PAYLOAD_HOST_FIXTURE','-DZT30_PAYLOAD_FIXTURE_DEVICES','-DZT30_IOCTL_VENDOR_EXE="'+str(cls.app)+'"','-DZT30_IOCTL_FIRST_DEVICE="/dev/null"','-DZT30_IOCTL_SENSOR_DEVICE="/dev/zero"','-DTMPFS_MAGIC=0x'+fstype,*extra,str(ROOT/'tools/zt30_ioctl_payload_trace.c'),'-ldl','-lpthread','-o',str(lib)],check=True)
 @classmethod
 def tearDownClass(cls):shutil.rmtree(cls.build)
 def fixture(self,mode='normal',cap=False,existing=None,inactive=False):
  d=session();self.addCleanup(lambda:shutil.rmtree(d))
  if existing:(d/existing).write_bytes(b'keep original')
  env=dict(os.environ,LD_PRELOAD=str(self.cap_library if cap else self.library)+':'+str(self.mock),APCG_IOCTL_TRACE=str(d/'ioctl.bin'),FIXTURE_MODE=mode,FIXTURE_SACRIFICIAL_FILE=str(d/'unrelated.bin'))
  result=subprocess.run(['/bin/true'] if inactive else [str(self.app)],env=env,capture_output=True,text=True,timeout=5)
  self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  files={p.name:p.read_bytes() for p in d.iterdir() if p.is_file()}
  return result,files
 def decoded(self,files):
  m=runpy.run_path(str(FIXTURES/'decode_ioctl_trace.py'));p=runpy.run_path(str(FIXTURES/'decode_ioctl_payload.py'))
  metadata=m['validate_counts'](m['decode'](files['ioctl.bin'],require_qualified_errno=True),files['ioctl-counts.bin'])
  return metadata,p['decode'](files['ioctl-payload.bin'],files['ioctl-payload-counts.bin'],metadata,fixture=True)
 def test_forwarding_threads_masks_raw_status_fork_and_totals(self):
  result,files=self.fixture();self.assertIn('PASS ',result.stdout)
  self.assertEqual(files['ioctl-payload.bin'][:8],b'ZT30PAY1');self.assertEqual((len(files['ioctl-payload.bin'])-8)//352,96)
  self.assertEqual(struct.unpack_from('<6I',files['ioctl-payload-counts.bin'],8)[:4],(96,0,0,0))
  metadata,payload=self.decoded(files);self.assertTrue(payload['complete']);self.assertEqual(metadata['open_count'],4)
  self.assertEqual(len({x['tid'] for x in metadata['pairs']}),5)
  child=int(re.search(r'child_pid=(\d+)',result.stdout).group(1));self.assertNotIn(child,{r['tid'] for r in metadata['records']})
  q=[r for r in payload['records'] if r['device']=='sensor'];self.assertEqual(len(q),32)
  self.assertTrue(all(r['capture_length']==(4 if r['phase']=='B' else 8) for r in q));self.assertTrue(all(r['raw_status']==0 for r in q))
  self.assertTrue(all(r['descriptor'][1]==0 for r in payload['records']))
 def test_short_and_faulting_reads_are_explicit_unavailable_and_calls_continue(self):
  for mode in ['short-read','fault-read']:
   result,files=self.fixture(mode);metadata,payload=self.decoded(files)
   self.assertTrue(metadata['complete']);self.assertTrue(payload['coverage_complete']);self.assertFalse(payload['complete']);self.assertEqual(payload['unavailable'],96)
 def test_invalid_descriptor_is_not_dereferenced(self):
  _,files=self.fixture('badptr');metadata,payload=self.decoded(files);self.assertTrue(metadata['complete']);self.assertEqual(payload['unavailable'],32)
 def test_nested_known_command_does_not_reenter_stack_reader(self):
  _,files=self.fixture('nested');_,payload=self.decoded(files);self.assertEqual(payload['unavailable'],1);self.assertTrue(any(r['copy_error']==35 for r in payload['records']))
 def test_cap_invalidates_capture_and_preserves_application(self):
  _,files=self.fixture(cap=True);self.assertEqual(files['ioctl-payload.bin'][:8],b'CAPLPAY1');self.assertLessEqual(len(files['ioctl-payload.bin']),1024);self.assertEqual(struct.unpack_from('<I',files['ioctl-payload-counts.bin'],16)[0],1)
 def test_short_and_repeatedly_interrupted_writes_fail_closed(self):
  for mode in ['short-write','interrupt-write']:
   _,files=self.fixture(mode);self.assertEqual(files['ioctl-payload.bin'][:8],b'FAILPAY1');self.assertGreater(struct.unpack_from('<I',files['ioctl-payload-counts.bin'],12)[0],0)
 def test_abrupt_exit_has_no_footer(self):
  _,files=self.fixture('abrupt');self.assertIn('ioctl-payload.bin',files);self.assertEqual(files['ioctl-payload-counts.bin'],b'')
 def test_existing_files_are_not_overwritten(self):
  for name in ['ioctl-payload.bin','ioctl-payload-counts.bin']:
   _,files=self.fixture(existing=name);self.assertEqual(files[name],b'keep original')
 def test_reused_writer_descriptors_cannot_modify_another_file(self):
  for mode in ['metadata-reuse','counts-reuse','writer-reuse','footer-reuse']:
   _,files=self.fixture(mode);self.assertEqual(files['unrelated.bin'],b'unrelated contents')
 def test_non_vendor_activation_creates_no_files(self):
  _,files=self.fixture(inactive=True);self.assertEqual(files,{})
if __name__=='__main__':unittest.main()
