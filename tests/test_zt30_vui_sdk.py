#!/usr/bin/env python3
"""Host-only SDK symbol-pair loader test. Stub addresses are never called."""
import os,re,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class VuiLoader(unittest.TestCase):
 def test_pair_is_published_only_if_both_symbols_exist(self):
  header=ROOT/'camera_app/src/backends/zt30/mi/native_sdk.h'
  source=header.read_text()
  names=set(re.findall(r'symbol\([^,]+,"[^"]+","(MI_[^"]+)"\)',source))
  names.update(re.findall(r'LOAD\([^,]+,"(MI_[^"]+)"\)',source))
  self.assertIn('MI_VENC_GetH264Vui',names);self.assertIn('MI_VENC_SetH264Vui',names)
  with tempfile.TemporaryDirectory() as d:
   directory=Path(d)
   probe=directory/'probe.cpp'
   probe.write_text('#include "'+str(header)+'"\n#include <cassert>\nint main(int argc,char **argv) { assert(argc==4);ca_zt30::NativeSdk sdk; assert(!sdk.resolve_h264_timing());assert(sdk.open(argv[1]) && sdk.resolve_encoder());bool ok=sdk.resolve_h264_timing();assert(ok==(argv[2][0]==\'1\'));assert(bool(sdk.encoder.get_h264_vui)==ok && bool(sdk.encoder.set_h264_vui)==ok);ok=sdk.resolve_h264_getter();assert(ok==(argv[3][0]==\'1\'));assert(bool(sdk.encoder.get_h264_vui)==ok && !sdk.encoder.set_h264_vui);sdk.close();assert(!sdk.encoder.get_h264_vui && !sdk.encoder.set_h264_vui);}\n')
   compiler=os.environ.get('CXX','c++')
   subprocess.run([compiler,'-std=c++11','-Wall','-Wextra','-Werror',str(probe),'-ldl','-o',str(directory/'probe')],check=True)
   for missing in [None,'MI_VENC_GetH264Vui','MI_VENC_SetH264Vui']:
    with self.subTest(missing=missing):
     library=directory/((missing or 'complete')+'.so');stub=directory/'stub.cpp'
     stub.write_text(''.join('extern "C" int '+name+'() { return 0; }\n' for name in sorted(names) if name!=missing))
     subprocess.run([compiler,'-shared','-fPIC',str(stub),'-o',str(library)],check=True)
     proc=subprocess.run([str(directory/'probe'),str(library),'0' if missing else '1','0' if missing=='MI_VENC_GetH264Vui' else '1'],capture_output=True)
     self.assertEqual(proc.returncode,0,proc.stderr.decode())
     if missing:self.assertIn(missing,proc.stderr.decode())
if __name__=='__main__':unittest.main()
