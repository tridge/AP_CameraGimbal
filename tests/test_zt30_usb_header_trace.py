#!/usr/bin/env python3
"""Two-slot header observer with private synthetic URBs and fake next ioctl."""
from pathlib import Path
import os,secrets,struct,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
RECORD=struct.Struct('<6Q3I3i3I44s')
class Headers(unittest.TestCase):
 def fixture(self,mode='normal',existing=False,inactive=False):
  with tempfile.TemporaryDirectory(prefix='apcg-zt30.session.',dir='/tmp') as directory:
   p=Path(directory);assert len(p.name.rsplit('.',1)[1])==8
   # Qualified constructor accepts exactly6 chars; Python's mktemp uses8.
   renamed=p.with_name("apcg-zt30.session."+secrets.token_hex(3));p.rename(renamed);p=renamed
   try:
    host_fs=subprocess.check_output(['stat','-f','-c','%t',str(p)],text=True).strip()
    probe=p/'probe';real=p/'real.so';obs=p/'observer.so'
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tests/zt30_usb_header_fixture.c'),'-lpthread','-o',str(probe)],check=True)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared',str(ROOT/'tests/zt30_usb_header_mock.c'),'-o',str(real)],check=True)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fPIC','-fstack-usage','-shared','-DTMPFS_MAGIC=0x'+host_fs,'-DZT30_IOCTL_VENDOR_EXE="'+str(probe)+'"','-DZT30_USB_DEVICE="/dev/null"',str(ROOT/'tools/zt30_usb_header_trace.c'),'-ldl','-lpthread','-o',str(obs)],check=True)
    if existing:(p/'usb-headers.bin').write_bytes(b'preserve')
    env=dict(os.environ,LD_PRELOAD=str(obs)+':'+str(real),APCG_USB_HEADER_TRACE=str(p/'usb_header.bin'))
    result=subprocess.run([str(probe) if not inactive else '/bin/true',mode],cwd=p,env=env,capture_output=True,text=True,timeout=8)
    read=lambda name:(p/name).read_bytes() if (p/name).exists() else b''
    data=read('usb-headers.bin');scalar=read('usb_header.bin');counts=read('usb_header-counts.bin');reports=[x.read_text() for x in p.glob('*.su')]
    return result,data,scalar,counts,reports
   finally:
    import shutil;shutil.rmtree(p)
 def decode(self,data):
  self.assertEqual(data[:8],b'ZT30UHD1');self.assertEqual(data[-32:-24],b'ENDUHD1!')
  footer=struct.unpack('<8I',data[-32:]);self.assertEqual(footer[2:],(1,1,2,0,0,0));self.assertEqual(len(data),296)
  return [RECORD.unpack_from(data,8+i*128) for i in range(2)]
 def test_two_slots_and_fixed_headers_errno_and_stack(self):
  result,data,scalar,counts,reports=self.fixture();self.assertEqual(result.returncode,0,result.stderr)
  rows=self.decode(data);self.assertEqual(sorted(r[12] for r in rows),[0,1]);self.assertEqual([r[13] for r in rows],[0,0])
  self.assertEqual([r[15][:2] for r in rows],[b'\x02\x00',b'\x00\x81'])
  self.assertTrue(all(r[14]>=32768 and r[10]==123 and r[11]==0 for r in rows))
  self.assertEqual(scalar[:8],b'ZT30UH00');self.assertEqual(counts[:8],b'ZT30UHC1')
  self.assertEqual(struct.unpack_from('<6I',counts,8)[2:],(0,0,0,0))
  self.assertTrue(reports)
  lines=''.join(reports).splitlines();frames={l.split('\t')[0].rsplit(':',1)[-1]:int(l.split('\t')[1]) for l in lines}
  self.assertIn('usb_header_stack',frames);self.assertIn('usb_header_heavy',frames);self.assertLessEqual(frames['usb_header_stack'],512);self.assertLessEqual(max(frames.values()),8192)
 def test_unavailable_and_fork_exclusion(self):
  for mode,error in [('small',7),('main',95),('invalid',None),('fork',0),('concurrent','busy')]:
   with self.subTest(mode=mode):
    result,data,*_=self.fixture(mode);self.assertEqual(result.returncode,0,result.stderr);rows=self.decode(data)
    if error=='busy':self.assertTrue(all(r[13] in [0,35] for r in rows) and any(r[13]==0 for r in rows),rows)
    elif error is not None:self.assertTrue(all(r[13]==error for r in rows),rows)
    else:self.assertTrue(all(r[13] for r in rows))
    if mode not in ['fork','concurrent']:self.assertTrue(all(not any(r[15]) for r in rows))
 def test_pinned_header_output_existing_and_inactive(self):
  result,data,*_=self.fixture(existing=True);self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(data,b'preserve')
  result,data,*_=self.fixture(inactive=True);self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(data,b'')
  result,data,*_=self.fixture('reuse');self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(data,b'ZT30UHD1')
if __name__=='__main__':unittest.main()
