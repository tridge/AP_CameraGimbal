from pathlib import Path
import os,secrets,struct,subprocess,tempfile,unittest,shutil
ROOT=Path(__file__).resolve().parents[1]
BASE=struct.Struct('<6Q3I3i3I44s');EXT=struct.Struct('<I8s34sBBi3I')
class Setup(unittest.TestCase):
 def fixture(self,mode='normal'):
  with tempfile.TemporaryDirectory(prefix='zt30-setup-host-',dir='/tmp') as td:
   p=Path(td)/('apcg-zt30.session.'+secrets.token_hex(3));p.mkdir()
   # Constructor requires canonical /tmp prefix, not a nested directory.
   canonical=Path('/tmp')/p.name;p.rename(canonical);p=canonical
   try:
    host_fs=subprocess.check_output(['stat','-f','-c','%t',str(p)],text=True).strip()
    probe=p/'probe';obs=p/'observer.so';real=p/'real.so'
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tests/zt30_usb_setup_fixture.c'),'-lpthread','-o',str(probe)],check=True)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared',str(ROOT/'tests/zt30_usb_setup_mock.c'),'-o',str(real)],check=True)
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared','-fstack-usage','-DTMPFS_MAGIC=0x'+host_fs,'-DZT30_IOCTL_VENDOR_EXE="'+str(probe)+'"','-DZT30_USB_DEVICE="/dev/null"',str(ROOT/'tools/zt30_usb_setup_trace.c'),'-ldl','-lpthread','-o',str(obs)],check=True)
    result=subprocess.run([str(probe),mode],env=dict(os.environ,LD_PRELOAD=str(obs)+':'+str(real),APCG_USB_SETUP_TRACE=str(p/'usb_setup.bin')),capture_output=True,text=True,timeout=8)
    return result,(p/'usb-setup.bin').read_bytes(),(p/'usb_setup.bin').read_bytes(),(p/'usb_setup-counts.bin').read_bytes()
   finally:shutil.rmtree(p)
 def rows(self,data):
  self.assertEqual(data[:8],b'ZT30UVS1');self.assertEqual(data[-64:-56],b'ENDUVS1!');self.assertEqual((len(data)-72)%192,0)
  footer=struct.unpack('<16I',data[-64:]);self.assertEqual(footer[4],(len(data)-72)//192);self.assertEqual(footer[5:8],(0,0,1));self.assertEqual(footer[12:],(0,0,0,0))
  rows=[(BASE.unpack_from(data,i),EXT.unpack_from(data,i+128)) for i in range(8,len(data)-64,192)]
  return rows,footer
 def test_claim_gate_alignment_out_only_and_open_generation(self):
  for mode in ['normal','reopen']:
   p,b,s,c=self.fixture(mode);self.assertEqual(p.returncode,0,p.stderr);rows,f=self.rows(b);self.assertEqual(len(rows),2)
   self.assertTrue(all(a[13]==0 and e[1][0:2]==b'\x21\x01' and e[3:6]==(1,34,0) for a,e in rows),rows)
   self.assertEqual([e[1][3] for a,e in rows],[1,2]);self.assertTrue(all(a[9]==f[8] and e[0]==f[9] and e[0]>0 for a,e in rows))
   self.assertLessEqual(f[10],100000000);self.assertEqual(s[:8],b'ZT30UV00');self.assertEqual(c[:8],b'ZT30UVC2')
 def test_in_unavailable_and_attempt_cap(self):
  for mode in ['in','invalid','main','small','cap']:
   p,b,s,c=self.fixture(mode);self.assertEqual(p.returncode,0,p.stderr);rows,f=self.rows(b);self.assertLessEqual(f[2],64);self.assertLessEqual(f[3],16);self.assertLessEqual(len(rows),80)
   if mode=='in':self.assertTrue(all(e[3:6]==(1,0,0) and not any(e[2]) for a,e in rows))
   if mode in ['invalid','main','small']:self.assertTrue(all(not e[3] and not e[4] and not any(e[1]) and not any(e[2]) for a,e in rows))
   if mode=='cap':self.assertTrue(f[2]==64 or f[10]>=100000000)
if __name__=='__main__':unittest.main()
