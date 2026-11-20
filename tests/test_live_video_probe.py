#!/usr/bin/env python3
import socket,struct,subprocess,tempfile,threading,time,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def box(kind,data=b'x'):return struct.pack('>I',len(data)+8)+kind+data
class Probe(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.temp=tempfile.TemporaryDirectory();p=Path(cls.temp.name)
  (p/'probe.cpp').write_text('#include "apcam/live_video_probe.h"\n#include <cstdlib>\nint main(int argc,char**argv){return argc==2 && apcam::live_video_probe(atoi(argv[1]),0,200)?0:1;}\n')
  cls.binary=p/'probe';subprocess.run(['c++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),str(p/'probe.cpp'),'-o',str(cls.binary)],check=True)
 @classmethod
 def tearDownClass(cls):cls.temp.cleanup()
 def probe(self,data,ok=False,stall=False):
  server=socket.socket();server.bind(('127.0.0.1',0));server.listen();port=server.getsockname()[1]
  def serve():
   with server:
    client,_=server.accept()
    with client:
     self.assertEqual(client.recv(1),b'\0')
     try:
      for i in range(0,len(data),3):client.sendall(data[i:i+3])
      if stall:time.sleep(2)
     except (BrokenPipeError,ConnectionResetError):pass
  t=threading.Thread(target=serve);t.start();start=time.monotonic()
  p=subprocess.run([str(self.binary),str(port)],timeout=3);elapsed=time.monotonic()-start;t.join(timeout=3)
  self.assertEqual(p.returncode==0,ok);return elapsed
 def test_two_complete_fragments_even_with_split_tcp_reads(self):
  self.probe(b'\0'+box(b'ftyp')+box(b'moov')+2*(box(b'moof')+box(b'mdat',b'frame')),True)
 def test_headers_only_or_one_frame_are_not_ready(self):
  for data in (box(b'ftyp')+box(b'moov'),box(b'moov')+box(b'moof')+box(b'mdat')):self.probe(b'\0'+data)
 def test_invalid_status_sizes_empty_or_unpaired_media(self):
  for data in (b'\1',b'\0'+struct.pack('>I',7)+b'moov',b'\0'+struct.pack('>I',4*1024*1024+1)+b'moov',
               b'\0'+box(b'moov')+2*box(b'mdat'),b'\0'+box(b'moov')+2*(box(b'moof')+box(b'mdat',b''))):self.probe(data)
 def test_deadline_bounds_stalled_connection(self):self.assertLess(self.probe(b'',stall=True),.8)
if __name__=='__main__':unittest.main()
