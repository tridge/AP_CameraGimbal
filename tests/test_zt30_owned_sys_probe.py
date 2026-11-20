#!/usr/bin/env python3
"""Prove Init/getter/Exit order and cleanup with controlled non-hardware libraries."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class OwnedSys(unittest.TestCase):
    def run_fixture(self,flags=()):
        temp=tempfile.TemporaryDirectory(prefix='zt30-owned-sys-');self.addCleanup(temp.cleanup)
        root=Path(temp.name);events=root/'events'
        source=root/'sys.c'
        source.write_text('''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int active;
static void event(const char *s) { FILE *f=fopen(EVENT_FILE,"a"); if(!f)abort();fprintf(f,"%s\\n",s);fclose(f); }
int MI_SYS_Init(unsigned short chip) { if(chip || active)abort();event("init");
#ifdef INIT_FAIL
 return -99;
#else
 active=1;return 0;
#endif
}
#ifndef MISSING_EXIT
int MI_SYS_Exit(unsigned short chip) { if(chip || !active)abort();event("exit");active=0;return 0; }
#endif
int MI_SYS_GetVersion(unsigned short chip,char *p) { if(chip || !active)abort();event("version");memset(p,0,128);strcpy(p,"mock initialized SDK");return 0; }
int MI_SYS_GetCurPts(unsigned short chip,uint64_t *p) { if(chip || !active)abort();event("pts");
#ifdef PTS_FAIL
 return -42;
#endif
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);*p=(uint64_t)t.tv_sec*1000000+(uint64_t)t.tv_nsec/1000;
#ifdef PTS_OVERFLOW
 memset((char*)p+8,0,8);
#endif
#ifdef PTS_FLAT
 *p=1;
#endif
#ifdef PTS_DOUBLE_RATE
 *p*=2;
#endif
 return 0;
}
''')
        subprocess.run(['cc','-shared','-fPIC','-DEVENT_FILE="'+str(events)+'"',*flags,str(source),'-o',str(root/'libmi_sys.so')],check=True)
        cam=root/'cam.c';cam.write_text('int cam_placeholder;\n')
        subprocess.run(['cc','-shared','-fPIC',str(cam),'-o',str(root/'libcam_os_wrapper.so')],check=True)
        binary=root/'probe'
        subprocess.run(['c++','-std=gnu++17','-O2','-Wall','-Wextra','-Werror','-DZT30_SYS_LIBRARY_DIRECTORY="'+str(root)+'"',str(ROOT/'tools/zt30_owned_sys_probe.cpp'),'-ldl','-o',str(binary)],check=True)
        result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=4)
        return result,events.read_text().splitlines() if events.exists() else []
    def test_initialized_order_and_pts_rate(self):
        result,events=self.run_fixture()
        self.assertEqual(result.returncode,0,result.stderr);self.assertIn('RESULT PASS',result.stdout)
        self.assertEqual(events,['init','version']+['pts']*10+['exit'])
    def test_init_failure_does_not_call_exit(self):
        result,events=self.run_fixture(['-DINIT_FAIL'])
        self.assertEqual(result.returncode,6);self.assertEqual(events,['init'])
    def test_getter_error_or_overwrite_unwinds_once(self):
        for flag in ['PTS_FAIL','PTS_OVERFLOW']:
            with self.subTest(flag=flag):
                result,events=self.run_fixture(['-D'+flag])
                self.assertEqual(result.returncode,7,result.stderr)
                self.assertEqual(events,['init','version','pts','exit'])
    def test_timestamp_stalls_and_wrong_scale_fail(self):
        for flag in ['PTS_FLAT','PTS_DOUBLE_RATE']:
            with self.subTest(flag=flag):
                result,events=self.run_fixture(['-D'+flag])
                self.assertEqual(result.returncode,8,result.stderr)
                self.assertEqual(events[0],'init');self.assertEqual(events[-1],'exit');self.assertEqual(events.count('exit'),1)
    def test_missing_exit_never_initializes(self):
        result,events=self.run_fixture(['-DMISSING_EXIT'])
        self.assertEqual(result.returncode,4,result.stderr);self.assertFalse(events)
if __name__=='__main__':unittest.main()
