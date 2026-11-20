"""Regress the ISP/algorithm circular-loader failure without camera hardware."""
import os,re,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Loader(unittest.TestCase):
 def test_group_resolves_cycle_before_constructors(self):
  header=(ROOT/'camera_app/src/backends/zt30/mi/native_sdk.h').read_text()
  names=re.findall(r'symbol\(api\.\w+,"[^"]+","([^"]+)"\)',header)
  self.assertEqual(len(names),46)
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory)
   # A imports B, B imports A. A's constructor actually calls B. Sequential
   # RTLD_NOW(A) fails; one DT_NEEDED scope must resolve both before its ctor.
   stubs='\n'.join('int '+n+'(void) {return 0;}' for n in names if n!='MI_SYS_Init')
   (p/'a.c').write_text('extern int beta(void); int alpha(void){return 1;} static int ready; __attribute__((constructor)) static void init(void){ready=beta();} int MI_SYS_Init(unsigned short chip){return chip?0:ready;}\n'+stubs)
   (p/'b.c').write_text('extern int alpha(void); int beta(void){return alpha()+6;}')
   (p/'group.c').write_text('const unsigned group_abi=1;')
   for lib in ['a','b']:
    subprocess.run(['cc','-fPIC','-shared',str(p/(lib+'.c')),'-Wl,-soname,lib'+lib+'.so','-o',str(p/('lib'+lib+'.so'))],check=True,capture_output=True)
   subprocess.run(['cc','-fPIC','-shared',str(p/'group.c'),'-L'+directory,'-Wl,--no-as-needed','-la','-lb','-o',str(p/'group.so')],check=True,capture_output=True)
   (p/'test.cpp').write_text('#include "camera_app/src/backends/zt30/mi/native_sdk.h"\n#include <cassert>\nint main(int argc,char** argv){assert(argc==3);ca_zt30::NativeSdk sdk;assert(!sdk.api.sys_init);void* bad=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL);assert(!bad);assert(sdk.open(argv[2]));assert(sdk.api.sys_init(0)==7);assert(!sdk.open(argv[2]));sdk.close();assert(!sdk.api.sys_init);assert(sdk.open(argv[2]));assert(sdk.api.sys_init(0)==7);sdk.close();}\n')
   subprocess.run(['c++','-std=c++11','-Wall','-Wextra','-Werror','-I'+str(ROOT),str(p/'test.cpp'),'-ldl','-o',str(p/'test')],check=True,capture_output=True)
   subprocess.run([str(p/'test'),str(p/'liba.so'),str(p/'group.so')],env={**os.environ,'LD_LIBRARY_PATH':directory},check=True,capture_output=True)
if __name__=='__main__':unittest.main()
