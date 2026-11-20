#!/usr/bin/env python3
"""Loader and guarded getter ABI failure tests using fake SDK libraries."""
from pathlib import Path
import subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]

class Probe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='zt30-probe-test-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root=Path(cls.temp.name)
        cls.binary=cls.root/'probe'
        subprocess.run(['c++','-std=gnu++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-I'+str(ROOT),str(ROOT/'tools/zt30_abi_probe.cpp'),'-ldl','-o',str(cls.binary)],check=True)
    def sdk(self,mode):
        folder=self.root/mode;folder.mkdir(exist_ok=True)
        source=folder/'fake.c'
        source.write_text('''#include <stdint.h>
#include <string.h>
#include <stdlib.h>
void MI_SYS_Init(void) { abort(); }
void MI_SYS_Exit(void) { abort(); }
int MI_SYS_GetVersion(unsigned short chip, char *p) {
    if (chip) abort();
#ifdef BAD_RETURN
    return -22;
#endif
    memset(p,0,128); strcpy(p,"mock SDK");
#ifdef OVERFLOW
    memset(p+128,0,8);
#endif
    return 0;
}
#ifndef MISSING_PTS
int MI_SYS_GetCurPts(unsigned short chip, uint64_t *p) { if(chip) abort();*p=1234;return 0; }
#endif
''')
        flags={'overflow':['-DOVERFLOW'],'failure':['-DBAD_RETURN'],'missing':['-DMISSING_PTS']}.get(mode,[])
        subprocess.run(['cc','-shared','-fPIC',*flags,str(source),'-o',str(folder/'libmi_sys.so')],check=True)
        subprocess.run(['cc','-shared','-fPIC',str(source),'-o',str(folder/'libcam_os_wrapper.so')],check=True)
        return folder
    def run_probe(self,mode):
        return subprocess.run([str(self.binary),'--query-sys',str(self.sdk(mode))],capture_output=True,text=True,timeout=5)
    def test_expected_getters(self):
        r=self.run_probe('good');self.assertEqual(r.returncode,0,r.stderr)
        self.assertIn('MI_SYS_GetCurPts=1234',r.stdout)
    def test_version_bounds(self):
        r=self.run_probe('overflow');self.assertEqual(r.returncode,5,r.stderr)
        self.assertIn('guard corrupted',r.stderr)
    def test_sdk_failure(self):
        self.assertEqual(self.run_probe('failure').returncode,6)
    def test_missing_getter(self):
        self.assertEqual(self.run_probe('missing').returncode,4)
    def test_load_failure(self):
        r=subprocess.run([str(self.binary),'--query-sys',str(self.root/'absent')],capture_output=True,text=True)
        self.assertEqual(r.returncode,3)

if __name__=='__main__':unittest.main()
