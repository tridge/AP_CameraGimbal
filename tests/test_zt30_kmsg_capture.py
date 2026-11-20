#!/usr/bin/env python3
"""Kernel-cursor fixture: preserve seq records, report overrun, bound output."""
from pathlib import Path
import secrets,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class KernelLogger(unittest.TestCase):
    def fixture(self,flags=(),existing=False):
        root=Path('/tmp/apcg-zt30.session.'+secrets.token_hex(3));root.mkdir()
        self.addCleanup(lambda:__import__('shutil').rmtree(root))
        device=root/'input';device.touch()
        source=root/'capture.c'
        # Only the host fixture substitutes filesystem policy; production has no
        # environment-controlled tmpfs bypass. All prefix/symlink guards remain.
        source.write_text((ROOT/'tools/zt30_kmsg_capture.c').read_text().replace('fs.f_type!=TMPFS_MAGIC','0').replace('!S_ISCHR(st.st_mode) || major(st.st_rdev)!=1 || minor(st.st_rdev)!=11','0'))
        binary=root/'capture'
        subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-DZT30_KMSG_DEVICE="'+str(device)+'"',str(source),'-o',str(binary)],check=True)
        mock=root/'mock.c';mock.write_text('''#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
ssize_t read(int fd,void *out,size_t size) {
 char path[128],name[256];snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
 ssize_t n=readlink(path,name,sizeof(name)-1);
 if(n>=0)name[n]=0;
 if(n<0 || strcmp(name,DEVICE))return ((ssize_t(*)(int,void*,size_t))dlsym(RTLD_NEXT,"read"))(fd,out,size);
 static int calls;calls++;
#ifdef PIPE_GAP
 if(calls==1) {errno=EPIPE;return -1;}
#endif
#ifdef DRAIN_TERM
 if(calls==4)raise(SIGTERM);
 if(calls>5) {errno=EAGAIN;return -1;}
#else
 if(calls>3) {errno=EAGAIN;return -1;}
#endif
 char data[256];n=snprintf(data,sizeof(data),"6,%d,%d,-;fixture message %d\\n",calls,100*calls,calls);
 if((size_t)n>size) {errno=EINVAL;return -1;}memcpy(out,data,n);return n;
}
''')
        lib=root/'mock.so';subprocess.run(['cc','-shared','-fPIC','-DDEVICE="'+str(device)+'"',*flags,str(mock),'-ldl','-o',str(lib)],check=True)
        output=root/'kmsg.bin'
        if existing:output.write_bytes(b'do not overwrite')
        result=subprocess.run([str(binary),str(root),'1'],env={'LD_PRELOAD':str(lib)},capture_output=True,text=True,timeout=3)
        return result,output.read_bytes()
    def test_record_bytes_and_bounded_finish(self):
        result,data=self.fixture();self.assertEqual(result.returncode,0,result.stderr)
        self.assertTrue(data.startswith(b'ZT30KMG1'));self.assertIn('records=3',result.stdout);self.assertIn('read_gaps=0',result.stdout);self.assertIn('caught_up=1 last_sequence=3',result.stdout)
        self.assertIn(b'6,1,100,-;fixture message 1',data)
    def test_overrun_is_failure_even_when_later_reads_work(self):
        result,data=self.fixture(['-DPIPE_GAP']);self.assertEqual(result.returncode,7);self.assertIn('read_gaps=1',result.stdout);self.assertIn(b'fixture message 2',data)
    def test_sigterm_drains_pending_messages_to_eagain(self):
        result,data=self.fixture(['-DDRAIN_TERM']);self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('records=5',result.stdout);self.assertIn('stopped=1 caught_up=1 last_sequence=5',result.stdout)
        self.assertIn(b'fixture message 5',data)
    def test_existing_output_is_not_overwritten(self):
        result,data=self.fixture(existing=True);self.assertEqual(result.returncode,3);self.assertEqual(data,b'do not overwrite');self.assertNotIn('READY',result.stdout)
if __name__=='__main__':unittest.main()
