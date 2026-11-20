#!/usr/bin/env python3
"""Exercise UART observation on a PTY without MI, camera devices or injected traffic."""
import errno
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import tempfile
import tty
import unittest
ROOT=Path(__file__).resolve().parents[1]
HEADER=struct.Struct('<QIiIB3x')
class UartTrace(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='apcg-zt30.session.')
        self.addCleanup(self.temp.cleanup)
        # Production path grammar deliberately requires six alphanumeric suffix bytes.
        self.session=Path('/tmp')/('apcg-zt30.session.'+os.urandom(3).hex())
        self.session.mkdir()
        self.addCleanup(lambda: __import__('shutil').rmtree(self.session))
        self.master,self.slave=pty.openpty()
        self.addCleanup(os.close,self.master);self.addCleanup(os.close,self.slave)
        tty.setraw(self.slave)
        self.library=Path(self.temp.name)/'trace.so'
        host_fs=subprocess.check_output(['stat','-f','-c','%t',str(self.session)],text=True).strip()
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fPIC','-shared','-DTMPFS_MAGIC=0x'+host_fs,'-DZT30_TRACE_HOST_TEST','-DZT30_TRACE_PROGRAM="'+str(Path(self.temp.name)/'fixture')+'"','-DZT30_TRACE_UART="'+os.ttyname(self.slave)+'"',str(ROOT/'tools/zt30_uart_trace.c'),'-ldl','-lpthread','-o',str(self.library)],check=True)
        self.program=Path(self.temp.name)/'fixture'
        source=Path(self.temp.name)/'fixture.c'
        source.write_text('''#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
int main(int argc, char **argv) {
 (void)argc;
 int fd=open(argv[1],O_RDWR|O_NOCTTY|O_NONBLOCK);
 char buf[64]; ssize_t n;
 if(fd<0)return 2;
 errno=EDOM;
 if(write(fd,"\\x55\\x66\\x00\\xff",4)!=4 || errno!=EDOM)return 3;
 if(strcmp(argv[2],"cap")==0) {
  memset(buf,0x42,sizeof(buf));
  for(int i=0;i<40000;i++) {
   while(write(fd,buf,sizeof(buf))<0) { if(errno!=EAGAIN)return 4; usleep(100); }
  }
 } else {
  if(strcmp(argv[2],"reuse")==0) {
   int output=output_descriptor(getenv("APCG_UART_TRACE"));if(output<0)return 10;
   int sink=open(argv[3],O_RDWR|O_CREAT|O_TRUNC,0600);
   if(sink<0 || write(sink,"untouched",9)!=9 || dup2(sink,output)!=output)return 9;
   close(sink);
  }
  for(int i=0;i<20000;i++) {
   n=read(fd,buf,3);
   if(n>0) { if(n!=3 || memcmp(buf,"\\x01\\x00\\xfe",3))return 5;break; }
   if(errno!=EAGAIN)return 6;
   usleep(100);
  }
  errno=0;
  if(read(-1,buf,1)!=-1 || errno!=EBADF)return 7;
  if(strcmp(argv[2],"reuse")==0)return 0;
  int regular=open(argv[3],O_CREAT|O_WRONLY,0600);
  if(regular<0 || write(regular,"other",5)!=5)return 8;
  close(regular);
 }
 close(fd);
 return 0;
}
''')
        subprocess.run(['cc','-Wall','-Wextra','-Werror',str(source),'-o',str(self.program)],check=True)
        self.trace=self.session/'uart.bin'
    def launch(self,mode='normal'):
        return subprocess.Popen([str(self.program),os.ttyname(self.slave),mode,str(self.session/'other')],env=dict(os.environ,LD_PRELOAD=str(self.library),APCG_UART_TRACE=str(self.trace)),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    def decode(self):
        raw=self.trace.read_bytes();self.assertEqual(raw[:8],b'ZT30URT1')
        offset=8;records=[]
        while offset<len(raw):
            ns,tid,fd,length,direction=HEADER.unpack_from(raw,offset);offset+=HEADER.size
            data=raw[offset:offset+length];self.assertEqual(len(data),length);offset+=length
            records.append((chr(direction),data,ns,tid,fd))
        return raw,records
    def test_bytes_errno_and_only_uart(self):
        process=self.launch()
        self.addCleanup(lambda: process.poll() is None and process.kill())
        self.assertTrue(select.select([self.master],[],[],3)[0])
        self.assertEqual(os.read(self.master,4),b'\x55\x66\x00\xff')
        os.write(self.master,b'\x01\x00\xfe')
        _,err=process.communicate(timeout=5)
        self.assertEqual(process.returncode,0,err)
        _,records=self.decode()
        self.assertEqual([(d,data) for d,data,*_ in records],[('W',b'\x55\x66\x00\xff'),('R',b'\x01\x00\xfe'),('E',b'')])
        self.assertTrue(all(ns>0 and tid>0 for d,_,ns,tid,_ in records if d in 'RW'))
        self.assertEqual((self.session/'other').read_bytes(),b'other')
    def test_limit_marker_without_stopping_app(self):
        process=self.launch('cap');self.addCleanup(lambda:process.poll() is None and process.kill())
        count=0
        while process.poll() is None:
            if select.select([self.master],[],[],0.01)[0]:count+=len(os.read(self.master,65536))
        _,err=process.communicate(timeout=3)
        self.assertEqual(process.returncode,0,err)
        raw,records=self.decode()
        self.assertLessEqual(len(raw),2*1024*1024)
        self.assertEqual(records[-1][0],'L')
        self.assertGreater(count,2*1024*1024)
    def test_reused_output_cannot_modify_an_unrelated_file(self):
        process=self.launch('reuse');self.addCleanup(lambda:process.poll() is None and process.kill())
        self.assertTrue(select.select([self.master],[],[],3)[0]);os.read(self.master,4)
        os.write(self.master,b'\x01\x00\xfe')
        _,err=process.communicate(timeout=5)
        self.assertEqual(process.returncode,0,err)
        self.assertEqual((self.session/'other').read_bytes(),b'untouched')
    def test_existing_output_is_not_overwritten(self):
        self.trace.write_bytes(b'sentinel')
        process=self.launch();self.addCleanup(lambda:process.poll() is None and process.kill())
        self.assertTrue(select.select([self.master],[],[],3)[0]);os.read(self.master,4)
        os.write(self.master,b'\x01\x00\xfe')
        _,err=process.communicate(timeout=5)
        self.assertEqual(process.returncode,0,err)
        self.assertEqual(self.trace.read_bytes(),b'sentinel')
if __name__=='__main__':unittest.main()
