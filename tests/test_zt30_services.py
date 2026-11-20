#!/usr/bin/env python3
'''Positive confirmation with real managed dummy processes and network streams.'''
import json,os,shutil,socket,struct,subprocess,tempfile,threading,time,unittest
from pathlib import Path
import test_zt30_bundle as fixtures
ROOT=Path(__file__).resolve().parents[1]
class Services(unittest.TestCase):
 a=fixtures.Bundle.a;old_manifest=fixtures.Bundle.old_manifest
 setUp=fixtures.Bundle.setUp;tearDown=fixtures.Bundle.tearDown
 bundle=fixtures.Bundle.bundle;call=fixtures.Bundle.call;command=fixtures.Bundle.command
 assert_preserved=fixtures.Bundle.assert_preserved;active_old=fixtures.Bundle.active_old
 @classmethod
 def setUpClass(cls):
  fixtures.Bundle.setUpClass.__func__(cls)
  p=Path(cls.build.name)
  (p/'web.cpp').write_text(r'''#include <cstdlib>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
int main(int argc,char**argv){
 if(argc!=3)return 2;int s=socket(AF_INET,SOCK_STREAM,0),one=1;
 setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
 sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(atoi(argv[2]));a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 if(bind(s,(sockaddr*)&a,sizeof(a)) || listen(s,8))return 3;
 signal(SIGPIPE,SIG_IGN);
 while(true){int c=accept(s,nullptr,nullptr);if(c<0)continue;char b[4096];read(c,b,sizeof(b));
 const char r[]="HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";write(c,r,sizeof(r)-1);close(c);}
}''')
  (p/'app.cpp').write_text('#include <unistd.h>\nint main(){for(;;)pause();}\n')
  cls.app=p/'camera-app';subprocess.run(['c++',str(p/'app.cpp'),'-o',str(cls.app)],check=True)
  cls.web=p/'zt30-web'
  subprocess.run(['c++','-O2',str(p/'web.cpp'),'-o',str(cls.web)],check=True)
  (p/'maintain.cpp').write_text(r'''#include "apcam/zt30_services.h"
#include <cstdio>
int main(int argc,char**argv){if(argc!=5)return 2;std::string error;
 bool ok=apcam_zt30::maintain_trial(argv[1],argv[2],atoi(argv[3]),false,error,atoi(argv[4]));
 if(!ok)fprintf(stderr,"%s\n",error.c_str());
 return ok?0:1;}''')
  cls.maintain=p/'maintain'
  subprocess.run(['c++','-O2','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),str(p/'maintain.cpp'),'-o',str(cls.maintain)],check=True)
 @classmethod
 def tearDownClass(cls):cls.build.cleanup()
 def exercise(self,old_boot=False,media=True):
  import hashlib
  text=fixtures.canonical({'schema':'apcg.zt30.slot-development.v1','board':'SIYI_ZT30','label':'new'})
  candidate=hashlib.sha256(text).hexdigest();slot=self.root/'slots'/candidate
  shutil.copytree(self.root/'slots'/self.a,slot)
  (slot/'manifest.json').write_bytes(text);(slot/'manifest.json').chmod(0o644)
  shutil.copy2(self.app,slot/'camera-app');shutil.copy2(self.web,slot/'zt30-web')
  (slot/'camera-app').chmod(0o755);(slot/'zt30-web').chmod(0o755)
  self.call('activate',candidate)
  video=socket.socket();video.bind(('127.0.0.1',0));video.listen();video.settimeout(5);live_port=video.getsockname()[1]
  with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
  app=subprocess.Popen([str(slot/'camera-app')],env=os.environ|{'CAMERA_APP_CONFIG':str(self.root/'camera.ini')})
  web=subprocess.Popen([str(slot/'zt30-web'),'-p',str(port)])
  def serve():
   with video:
    try:c,_=video.accept()
    except socket.timeout:return
    with c:
     c.recv(1)
     def box(name):return struct.pack('>I',9)+name+b'x'
     c.sendall(b'\0'+box(b'moov')+2*(box(b'moof')+box(b'mdat')) if media else b'\1')
  thread=threading.Thread(target=serve);thread.start()
  trial={'previous':self.a,'candidate':candidate,'boot':'old-boot' if old_boot else Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
         'deadline':1 if media else 2**52,'attempts':1,'state':'ready'}
  (self.root/'trial.json').write_bytes(fixtures.canonical(trial));(self.root/'trial.json').chmod(0o600)
  try:
   until=time.monotonic()+5
   while time.monotonic()<until:
    p=subprocess.run(self.command('web-listener',str(port)),capture_output=True,text=True)
    if p.returncode==0:break
    time.sleep(.02)
   self.assertEqual(p.stdout.strip(),str(web.pid));self.assertIsNone(app.poll())
   subprocess.run([str(self.maintain),str(self.root),str(self.runtime),str(port),str(live_port)],check=True,timeout=8)
   state=json.loads((self.root/'trial.json').read_bytes())['state']
   if old_boot:
    self.assertEqual(state,'rolled-back');self.active_old();self.assertEqual(app.wait(timeout=3),-15)
   elif media:
    self.assertEqual(state,'confirmed');self.assertEqual(os.readlink(self.root/'active'),'slots/'+candidate)
    self.assertIsNone(app.poll());self.assertIsNone(web.poll())
   else:self.assertEqual(state,'ready')
   self.assert_preserved()
  finally:
   for process in (app,web):
    if process.poll() is None:process.terminate()
    process.wait(timeout=3)
   thread.join(timeout=6)
 def test_live_candidate_confirms_even_at_deadline(self):self.exercise()
 def test_unconfirmed_previous_boot_rolls_back_even_if_healthy(self):self.exercise(old_boot=True)
 def test_no_rgb_does_not_confirm(self):self.exercise(media=False)
if __name__=='__main__':unittest.main()
