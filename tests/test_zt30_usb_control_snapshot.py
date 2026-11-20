from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class Control(unittest.TestCase):
 def test_private_allowlist_alignment_and_mapping_guards(self):
  with tempfile.TemporaryDirectory() as directory:
   out=Path(directory)/'probe'
   subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fstack-usage','-I'+str(ROOT/'tools'),str(ROOT/'tests/zt30_usb_control_fixture.c'),'-o',str(out)],check=True)
   p=subprocess.run([str(out)],capture_output=True,text=True,timeout=5);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
   for marker in ['ALIGNMENT PASS','ALLOWLIST PASS','PREFILTER PASS','GUARDS PASS','LIFETIME PASS','READER_REUSE PASS','PASS private UVC']:self.assertIn(marker,p.stdout)
   reports=list(Path(directory).glob('*.su'));self.assertTrue(reports);sizes=[int(l.split('\t')[1]) for s in reports for l in s.read_text().splitlines()];self.assertLessEqual(max(sizes),8192);self.assertLessEqual(sum(sizes),12288)
   print(p.stdout,end='')
if __name__=='__main__':unittest.main()
