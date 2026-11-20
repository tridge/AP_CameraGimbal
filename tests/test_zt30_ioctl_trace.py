#!/usr/bin/env python3
"""Prove ioctl passthrough/errno and bounded metadata without pointer reads."""
from pathlib import Path
import secrets,subprocess,struct,unittest,shutil
ROOT=Path(__file__).resolve().parents[1]
class IoctlTrace(unittest.TestCase):
    def fixture(self,count=2,existing=False,collision=False,inactive=False,distinct=False,counts_existing=False,abrupt=False,zero=False,negative=False,usb=False,missing=False,reuse=False,reuse_counts=False):
        root=Path('/tmp/apcg-zt30.session.'+secrets.token_hex(3));root.mkdir();self.addCleanup(lambda:shutil.rmtree(root))
        app=root/'app';source=root/'app.c'
        source.write_text('''#ifndef EXPECTED_RC
#define EXPECTED_RC 123
#define EXPECTED_ERR 16
#endif
#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int output_descriptor(const char *wanted) {
 DIR *dir=opendir("/proc/self/fd");if(!dir)return -1;
 struct dirent *entry;int found=-1;
 while((entry=readdir(dir))) {
  char *end=0;long n=strtol(entry->d_name,&end,10);if(!*entry->d_name || *end)continue;
  char name[64],target[4096];snprintf(name,sizeof(name),"/proc/self/fd/%ld",n);
  ssize_t size=readlink(name,target,sizeof(target)-1);if(size<0)continue;target[size]=0;
  if(!strcmp(target,wanted)){found=(int)n;break;}
 }
 closedir(dir);return found;
}
int main(void) {
 int fd=open("/dev/null",O_RDWR);if(fd<0)return 2;
 int other=open("/dev/zero",O_RDWR);if(other<0)return 6;
#ifdef REUSE_OUTPUT
 int output=output_descriptor(getenv("TRACE_REUSE_PATH"));if(output<0)return 22;
 int sink=open("output-sink",O_RDWR|O_CREAT|O_TRUNC,0600);if(sink<0)return 20;
 if(write(sink,"keep original sink",18)!=18 || dup2(sink,output)!=output)return 21;close(sink);
#endif
 for(unsigned i=0;i<COUNT;i++) {
  unsigned long command=0x6900U+(i&255U)+((unsigned long)((i>>8)&3U)<<30);
#ifdef COLLISION
 command=0x6901U+((i&1U)<<8)+(((i>>1)&1U)<<16);
#endif
#ifdef DISTINCT
 command=0x690000U+i;
#endif
  errno=EINTR;int rc=ioctl(fd,command,(void*)(uintptr_t)0xdeadbeefU);
  if(rc!=EXPECTED_RC || errno!=EXPECTED_ERR)return 3;
 }
#ifdef ZERO
 errno=EINTR;if(ioctl(fd,0,(void*)(uintptr_t)0xdeadbeefU)!=EXPECTED_RC || errno!=EXPECTED_ERR)return 8;
#endif
 /* Optional argument omitted; the observer must not va_arg-read it. */
 errno=EINTR;if(ioctl(fd,0x69ff)!=EXPECTED_RC || errno!=EXPECTED_ERR)return 4;
 errno=EINTR;if(ioctl(other,0x69ff)!=EXPECTED_RC || errno!=EXPECTED_ERR)return 7;close(other);
#ifdef ABRUPT
 _exit(0);
#endif
 close(fd);puts("PASSTHROUGH PASS");return 0;
}
''')
        subprocess.run(['cc','-O2','-DCOUNT='+str(count),*(['-DREUSE_OUTPUT'] if reuse or reuse_counts else []),*(['-DCOLLISION'] if collision else []),*(['-DDISTINCT'] if distinct else []),*(['-DABRUPT'] if abrupt else []),*(['-DZERO'] if zero else []),*(['-DEXPECTED_RC=-1','-DEXPECTED_ERR=9'] if negative else []),str(source),'-o',str(app)],check=True)
        mock=root/'mock.c';mock.write_text('''#ifndef EXPECTED_RC
#define EXPECTED_RC 123
#define EXPECTED_ERR 16
#endif
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
int ioctl(int fd,unsigned long command,...) {
 static unsigned calls;unsigned i=calls++;
 if(fd<0)return -900;
 if(i<COUNT) {
  unsigned long expected=0x6900U+(i&255U)+((unsigned long)((i>>8)&3U)<<30);
#ifdef COLLISION
 expected=0x6901U+((i&1U)<<8)+(((i>>1)&1U)<<16);
#endif
#ifdef DISTINCT
 expected=0x690000U+i;
#endif
  va_list args;va_start(args,command);void *p=va_arg(args,void*);va_end(args);
  if(command!=expected || (uintptr_t)p!=0xdeadbeefU)return -901;
 } else {
#ifdef ZERO
  if(i==COUNT) {
   va_list args;va_start(args,command);void *p=va_arg(args,void*);va_end(args);
   if(command || (uintptr_t)p!=0xdeadbeefU)return -903;
  } else
#endif
  if(command!=0x69ff)return -902;
 }
 errno=EXPECTED_ERR;return EXPECTED_RC;
}
''');real=root/'real.so';subprocess.run(['cc','-shared','-fPIC','-DCOUNT='+str(count),*(['-DCOLLISION'] if collision else []),*(['-DDISTINCT'] if distinct else []),*(['-DZERO'] if zero else []),*(['-DEXPECTED_RC=-1','-DEXPECTED_ERR=9'] if negative else []),str(mock),'-o',str(real)],check=True)
        observer=root/'observer.c';observer.write_text((ROOT/'tools/zt30_ioctl_trace.c').read_text().replace('fs.f_type!=TMPFS_MAGIC','0'))
        (root/'zt30_trace_output.h').write_text((ROOT/'tools/zt30_trace_output.h').read_text().replace('fs.f_type!=TMPFS_MAGIC','0'))
        lib=root/'observer.so';subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared','-DZT30_IOCTL_VENDOR_EXE="'+(str(root/'different') if inactive else str(app))+'"','-DZT30_IOCTL_FIRST_DEVICE="/dev/null"',*(['-DZT30_USB_TRACE','-DZT30_USB_DEVICE="'+('/dev/does-not-exist' if missing else '/dev/null')+'"'] if usb else []),str(observer),'-ldl','-o',str(lib)],check=True)
        output=root/('usb.bin' if usb else 'ioctl.bin');count_file=root/('usb-counts.bin' if usb else 'ioctl-counts.bin')
        if existing:output.write_bytes(b'do not overwrite')
        if counts_existing:count_file.write_bytes(b'keep original counts')
        result=subprocess.run([str(app)],env={'TRACE_REUSE_PATH':str(count_file if reuse_counts else output),'LD_PRELOAD':str(lib)+':'+str(real),('APCG_USB_TRACE' if usb else 'APCG_IOCTL_TRACE'):str(output)},capture_output=True,text=True,timeout=10,cwd=root)
        if reuse or reuse_counts:self.assertEqual((root/'output-sink').read_bytes(),b'keep original sink')
        return result,output.read_bytes() if output.exists() else b'',count_file.read_bytes() if count_file.exists() else b''
    def test_plain_tracer_pins_metadata_and_count_outputs(self):
        for options in ({'reuse':True},{'reuse_counts':True}):
            result,_,_=self.fixture(**options)
            self.assertEqual(result.returncode,0,result.stderr)
    def test_return_errno_and_opaque_invalid_pointer(self):
        result,data,counts=self.fixture();self.assertEqual(result.returncode,0,result.stderr);self.assertIn('PASSTHROUGH PASS',result.stdout)
        self.assertEqual(data[:8],b'ZT30IOT0');self.assertEqual((len(data)-8)%64,0)
        types=[data[i+45] for i in range(8,len(data),64)]
        self.assertIn(ord('O'),types);self.assertEqual(types.count(ord('B')),3);self.assertEqual(types.count(ord('A')),3);self.assertEqual(counts[:8],b'ZT30IOC1')
    def test_log_cap_does_not_change_application_results(self):
        result,data,counts=self.fixture(22000);self.assertEqual(result.returncode,0,result.stderr)
        self.assertLessEqual(len(data),2*1024*1024);self.assertEqual((len(data)-8)%64,0)
        self.assertEqual(data[-64+45],ord('L'))
    def test_existing_output_not_overwritten_and_app_unaffected(self):
        result,data,counts=self.fixture(existing=True);self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(data,b'do not overwrite')
    def test_full_request_keys_and_exit_drop_totals(self):
        result,data,counts=self.fixture(80,collision=True);self.assertEqual(result.returncode,0,result.stderr)
        rows=[struct.unpack_from('<8I',counts,i) for i in range(32,len(counts),32)]
        self.assertEqual(len(rows),5);self.assertEqual(sum(row[2] for row in rows),81)
        self.assertEqual(sum(row[3] for row in rows),65);self.assertEqual(sum(row[4] for row in rows),16)
        self.assertEqual({row[1] for row in rows},{0x6901,0x6a01,0x16901,0x16a01,0x69ff})
    def test_non_vendor_executable_produces_no_logs(self):
        result,data,counts=self.fixture(inactive=True);self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(data,b'');self.assertEqual(counts,b'')
    def test_key_table_saturation_is_reported_and_calls_continue(self):
        result,data,counts=self.fixture(1100,distinct=True);self.assertEqual(result.returncode,0,result.stderr)
        header=struct.unpack_from('<8I',counts);self.assertEqual(header[4],77)
        self.assertEqual((len(counts)-32)//32,1024)
    def test_existing_counts_file_leaves_application_working(self):
        result,data,counts=self.fixture(counts_existing=True);self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(data,b'');self.assertEqual(counts,b'keep original counts')
    def test_abrupt_exit_has_no_snapshot_and_cannot_qualify(self):
        result,data,counts=self.fixture(abrupt=True);self.assertEqual(result.returncode,0,result.stderr)
        self.assertTrue(data.startswith(b'ZT30IOT0'));self.assertEqual(counts,b'')
    def test_zero_request_has_its_own_count_and_pairs(self):
        result,data,counts=self.fixture(zero=True);self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(struct.unpack_from('<8I',counts)[4],0)
        rows=[struct.unpack_from('<8I',counts,i) for i in range(32,len(counts),32)]
        self.assertEqual([r[2:5] for r in rows if r[1]==0],[(1,1,0)])
        self.assertEqual(sum(r[2] for r in rows),4)
    def test_errno_is_only_reported_for_minus_one_without_changing_caller_errno(self):
        for negative in [False,True]:
            result,data,counts=self.fixture(negative=negative);self.assertEqual(result.returncode,0,result.stderr)
            returns=[struct.unpack_from('<i',data,i+28)[0] for i in range(8,len(data),64) if data[i+45]==ord('A')]
            self.assertEqual(returns,[9 if negative else 0]*3)
if __name__=='__main__':unittest.main()
