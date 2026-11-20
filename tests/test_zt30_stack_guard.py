#!/usr/bin/env python3
"""Reject device/PFN/mixed mappings, malformed or unbounded proc metadata."""
import pathlib,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
SOURCE=r"""
#include "zt30_stack_guard.h"
#include <assert.h>
#include <stdlib.h>
static int parse(const char *text) {
 FILE *f=tmpfile();assert(f);assert(fwrite(text,1,strlen(text),f)==strlen(text));assert(!fflush(f));assert(!fseek(f,0,SEEK_SET));
 struct zt30_stack_guard g;errno=EINTR;int rc=zt30_stack_guard_fd(&g,fileno(f),0x80008000);assert(errno==EINTR);
 if(!rc) {assert(g.low==0x80000000 && g.high==0x80010000 && g.owner==getpid());assert(!zt30_stack_guard_check(&g,0x80008000,256));assert(zt30_stack_guard_check(&g,0x8000fffc,8));assert(zt30_stack_guard_check(&g,0x80008001,4));}
 else {assert(!g.low && !g.high && !g.owner);}
 fclose(f);return rc;
}
int main(int argc,char **argv) {
 assert(argc==2);int mode=atoi(argv[1]);
 if(mode==0) {
  assert(!parse("80000000-80010000 rw-p 00000000 00:00 0 [stack]\nSize: 64 kB\nVmFlags: rd wr mr mw me gd ac\n"));
  assert(!parse("80000000-80010000 rw-p 00000000 00:00 0\nVmFlags: rd wr mr mw me ac\n"));
  assert(!parse("80000000-80010000 rw-p 00000000 00:00 0 [stack:123]\nVmFlags: rd wr\n"));
 } else if(mode==1) {
  const char *bad[]={"rwxp 00000000 00:00 0","rw-s 00000000 00:00 0","rw-p 00001000 00:00 0","rw-p 00000000 00:01 0","rw-p 00000000 00:00 12","rw-p 00000000 00:00 0 /dev/zero","rw-p 00000000 00:00 0 [heap]"};
  for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {char buf[256];snprintf(buf,sizeof(buf),"80000000-80010000 %s\nVmFlags: rd wr\n",bad[i]);assert(parse(buf)==EPERM);}
  const char *flags[]={"rd wr io","rd wr pf","rd wr mm","rd","wr","rd wr malformed"};
  for(unsigned i=0;i<sizeof(flags)/sizeof(*flags);i++) {char buf[256];snprintf(buf,sizeof(buf),"80000000-80010000 rw-p 0 00:00 0\nVmFlags: %s\n",flags[i]);assert(parse(buf)==EPERM);}
 } else if(mode==2) {
  assert(parse("80000000-80010000 rw-p 0 00:00 0\nSize: 64 kB\n")==EPROTO);
  assert(parse("80000000-80010000 rw-p 0 00:00 0\n80020000-80030000 rw-p 0 00:00 0\nVmFlags: rd wr\n")==EPROTO);
  char longline[600];memset(longline,'a',sizeof(longline));longline[599]=0;assert(parse(longline)==E2BIG);
  assert(parse("80010000-80000000 rw-p 0 00:00 0\n")==EPROTO);
 } else if(mode==3) {
  size_t n=ZT30_SMAPS_LIMIT+4096;char *large=malloc(n+1);assert(large);for(size_t i=0;i<n;i++)large[i]=i%4==3?'\n':'a';large[n]=0;assert(parse(large)==E2BIG);free(large);
 }
 return 0;
}
"""
class Guard(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.directory=tempfile.TemporaryDirectory();d=pathlib.Path(cls.directory.name);source=d/'guard.c';source.write_text(SOURCE);cls.binary=d/'guard'
  subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-I',str(ROOT/'tools'),str(source),'-lpthread','-o',str(cls.binary)],check=True)
 @classmethod
 def tearDownClass(cls):cls.directory.cleanup()
 def run_case(self,case):subprocess.run([str(self.binary),str(case)],check=True,timeout=2)
 def test_anonymous_main_and_pthread_stack_metadata(self):self.run_case(0)
 def test_device_pfn_mixed_heap_permissions_and_alignment(self):self.run_case(1)
 def test_truncated_and_malformed_metadata(self):self.run_case(2)
 def test_total_read_bound(self):self.run_case(3)
if __name__=='__main__':unittest.main()
