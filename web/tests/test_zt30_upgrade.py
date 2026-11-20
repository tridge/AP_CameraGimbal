#!/usr/bin/env python3
'''HTTP upload orchestration with real validation and injected lifecycle callbacks.
No ARM candidate executes on the host; hardware testing covers real services.
'''
import base64,copy,fcntl,http.client,json,os,re,shutil,socket,struct,subprocess,sys,tempfile,time,threading
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tests'))
from test_zt30_bundle import Bundle,canonical,sha

with tempfile.TemporaryDirectory(prefix='zt30-http-upgrade-') as directory:
 base=Path(directory);app=base/'app';app.mkdir(mode=0o755);run=base/'run';run.mkdir(mode=0o755)
 runtime=base/'runtime';(runtime/'lib').mkdir(parents=True)
 library=b'runtime-http-fixture';(runtime/'lib/libc.so.6').write_bytes(library)
 for name in ('zt30-slot','app-init.sh','app-supervisor.sh','web-supervisor.sh','start-dropbear.sh','dropbear'):
  (app/name).write_bytes(b'fixture');(app/name).chmod(0o755)
 (app/'web.pass').write_bytes(b'test-password\n');(app/'web.pass').chmod(0o600)
 config=(ROOT/'packaging/zt30/camera.ini.dev').read_bytes();(app/'camera.ini').write_bytes(config)
 identity=b'identity-http-fixture';(app/'auth').mkdir(mode=0o700);(app/'auth/key').write_bytes(identity)
 # Parser fixtures are intentionally not runnable; actual activation is exercised
 # with real ELF on the camera. All staging checks and HTTP policies remain real.
 from test_zt30_bundle import arm
 data={'camera-app':arm(),'encoder-sdk.so':arm(True),'zt30-web':arm()}
 for path in (ROOT/'web/webroot').rglob('*'):
  if path.is_file():data['webroot/'+path.relative_to(ROOT/'web/webroot').as_posix()]=path.read_bytes()
 old_manifest=canonical({'schema':'apcg.zt30.slot-development.v1','board':'SIYI_ZT30','label':'old'})
 old=sha(old_manifest);(app/'slots').mkdir(mode=0o755);old_dir=app/'slots'/old;old_dir.mkdir(mode=0o755)
 for name,payload in data.items():
  path=old_dir/name;path.parent.mkdir(parents=True,exist_ok=True)
  path.write_bytes(payload);path.chmod(0o644 if name.startswith('webroot/') else 0o755)
 for path in old_dir.rglob('*'):
  if path.is_dir():path.chmod(0o755)
 (old_dir/'manifest.json').write_bytes(old_manifest);(old_dir/'manifest.json').chmod(0o644)
 (app/'active').symlink_to('slots/'+old)
 def bundle(version='http-test',override=None):
  m={'schema':'apcg.zt30.app.v1','board':'SIYI_ZT30','launcher':1,'version':version,
     'runtime':[{'name':'libc.so.6','path':'/lib/libc.so.6','size':len(library),'sha256':sha(library)}],
     'files':[{'path':name,'mode':0o644 if name.startswith('webroot/') else 0o755,'size':len(payload),'sha256':sha(payload)} for name,payload in sorted(data.items())]}
  if override:override(m)
  text=canonical(m);return b'APCZT301'+struct.pack('<I',len(text))+text+b''.join(data[f['path']] for f in m['files']),sha(text)
 wrapper=base/'host.cpp'
 wrapper.write_text(r'''#define main zt30_web_main
#include "''' + str(ROOT/'web/mt11-web.cpp') + r'''"
#undef main
static std::string selected_version() {
 std::string path,error;return apcam_zt30::resolve(APP_DIR,path,error)?zt30_bundle_version(path):"";
}
static bool fake_restart(void *,const std::string &,std::string &error) {
 if(selected_version()=="fail-start")return apcam_zt30::fail(error,"injected candidate start failure");
 return true;
}
static bool fake_ready(void *,const std::string &,bool &thermal,std::string &error) {
 thermal=false;
 if(selected_version()=="fail-ready")return apcam_zt30::fail(error,"injected candidate RGB failure");
 return true;
}
// Keep the response/cleanup gap observable on fast hosts too. Production
// intentionally retains the update lock until after sending the response.
extern "C" int __real_close(int fd);
extern "C" int __wrap_close(int fd) {
 char name[64]{},target[4096]{};
 snprintf(name,sizeof(name),"/proc/self/fd/%d",fd);
 ssize_t size=readlink(name,target,sizeof(target)-1);
 if(size>0 && !strcmp(target,UPGRADE_LOCK_PATH))usleep(50000);
 return __real_close(fd);
}
int main(int argc,char **argv) {
 if(argc==2 && !strcmp(argv[1],"--restart-lock-test")) {
  char error[256]{};int lock=lock_restart(error,sizeof(error));
  if(lock<0){fprintf(stderr,"%s",error);return 1;}close(lock);return 0;
 }
 zt30_upgrade_services={fake_restart,fake_ready,nullptr};return zt30_web_main(argc,argv);
}''')
 paths={'APP_DIR':app,'MEDIA_ROOT':base/'mnt','RUNTIME_DIR':run,'CAMERA_READY_PATH':run/'ready',
        'UPGRADE_LOCK_PATH':run/'upload.lock','PASSWORD_PATH':app/'web.pass','REPLACEMENT_CONFIG_PATH':app/'camera.ini',
        'REPLACEMENT_CONFIG_BACKUP_PATH':app/'camera.ini.bak','USER_LOCK_PATH':run/'users.lock',
        'APP_REQUEST_PATH':run/'request','APP_STARTED_PATH':run/'started','APP_REQUEST_LOCK_PATH':run/'request.lock',
        'REPLACEMENT_CAMERA_PATH':app/'camera-app','WEB_PATH':app/'zt30-web','WEBROOT_PATH':old_dir/'webroot',
        'ZT30_RUNTIME_ROOT':runtime,'SESSION_PATH':run/'sessions','AUTHORIZED_KEYS_PATH':app/'auth/key',
        'REPLACEMENT_LOG_PATH':run/'camera.log','REPLACEMENT_LOG_OLD_PATH':run/'camera.old'}
 binary=base/'host'
 subprocess.run(['c++','-std=gnu++17','-O2','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers',
                 '-DZT30_UPLOAD_DURATION_MS=1000','-DAPCAM_TARGET=APCAM_TARGET_ZT30','-DZT30_WEB_NATIVE','-DCAMERA_APP_ZT30_DEV','-DMT11_WEB_TEST',
                 '-I'+str(ROOT/'include'),*[f'-D{name}="{path}"' for name,path in paths.items()],
                 str(wrapper),str(ROOT/'camera_app/src/config.cpp'),'-Wl,--wrap=close','-lm','-o',str(binary)],check=True)
 # Ordinary restarts tolerate short supervisor maintenance transactions;
 # uploads retain the immediate conflict policy tested through HTTP below.
 with (run/'camera-app.restart.lock').open('w') as restart_lock:
  fcntl.flock(restart_lock,fcntl.LOCK_EX)
  release=threading.Timer(.15,lambda:fcntl.flock(restart_lock,fcntl.LOCK_UN));release.start()
  started=time.monotonic()
  result=subprocess.run([str(binary),'--restart-lock-test'],capture_output=True,timeout=3)
  release.join();assert result.returncode==0,result.stderr
  assert .1<time.monotonic()-started<1.2
  fcntl.flock(restart_lock,fcntl.LOCK_EX)
  started=time.monotonic()
  result=subprocess.run([str(binary),'--restart-lock-test'],capture_output=True,timeout=3)
  assert result.returncode==1 and 1.4<time.monotonic()-started<2.5
 with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
 log=(base/'web.log').open('w');process=subprocess.Popen([str(binary),'-p',str(port)],stdout=log,stderr=log)
 auth='Basic '+base64.b64encode(b'admin:test-password').decode()
 def wait_upload_idle():
  deadline=time.monotonic()+5
  while time.monotonic()<deadline:
   try:
    with (run/'upload.lock').open('r+') as lock:
     fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    return
   except FileNotFoundError:return
   except BlockingIOError:time.sleep(.01)
  raise AssertionError('Previous HTTP update worker did not release its upload lock')
 def request(path,body=None,headers=None,authenticated=True):
  c=http.client.HTTPConnection('127.0.0.1',port,timeout=5)
  h={'Authorization':auth} if authenticated else {}
  h.update(headers or {});c.request('POST' if body is not None else 'GET',path,body,h)
  response=c.getresponse();status=response.status;payload=response.read();c.close()
  # The worker sends its reply before closing the transaction locks, keeping
  # the supervisor from reloading web services mid-response. Payload receipt
  # alone is not a cleanup barrier. Never retry requests or mask real 409s.
  if path in ('/upgrade','/upgrade/rollback') and status!=409:wait_upload_idle()
  return status,payload
 def preserved():
  assert (app/'camera.ini').read_bytes()==config
  assert (app/'web.pass').read_bytes()==b'test-password\n'
  assert (app/'auth/key').read_bytes()==identity
 def upload(data,csrf,**extra):
  h={'Content-Type':'application/octet-stream','X-CSRF-Token':csrf,'X-Firmware-Name':'ZT30_AP_http.apbundle'};h.update(extra)
  return request('/upgrade',data,h)
 try:
  until=time.monotonic()+8
  while time.monotonic()<until:
   try:status,page=request('/');break
   except (ConnectionError,OSError):time.sleep(.05)
  assert status==200,page
  pending='Checking the new application and web interface…'.encode()
  assert pending not in page
  # A stale session must not trigger a browser Basic-auth prompt while the
  # upgrade monitor polls either endpoint.
  for path in ('/upgrade-status','/upgrade-result'):
   c=http.client.HTTPConnection('127.0.0.1',port,timeout=5)
   c.request('GET',path,headers={'Cookie':'session=expired'})
   response=c.getresponse();assert response.status==401
   assert response.getheader('WWW-Authenticate') is None
   response.read();c.close()
  assert b'id=firmware-upload' in page and b'id=bundle-selected' in page,page
  csrf=re.search(rb'name=csrf value="([0-9a-f]+)"',page)[1].decode()
  payload,id=bundle()
  assert request('/upgrade',payload,authenticated=False)[0]==401
  assert upload(payload,'bad')[0]==403
  assert upload(payload,csrf,**{'X-Firmware-Name':'ZT30_FW_bad.bin'})[0]==400
  assert upload(payload,csrf,**{'Content-Type':'text/plain'})[0]==415
  for bad in (payload[:-1],payload[:-1]+bytes([payload[-1]^1]),bundle(override=lambda m:m.update(board='SIYI_MT11'))[0]):
   status,message=upload(bad,csrf);assert status==400,(status,message);assert os.readlink(app/'active')=='slots/'+old;preserved()
  with (run/'upload.lock').open('w') as lock:
   fcntl.flock(lock,fcntl.LOCK_EX);assert upload(payload,csrf)[0]==409
  with (run/'camera-app.restart.lock').open('w') as lock:
   fcntl.flock(lock,fcntl.LOCK_EX);assert upload(payload,csrf)[0]==409
  # An interrupted worker's private tmpfs upload is reaped under the upload lock.
  orphan=run/'firmware-upload.ABC123';orphan.write_bytes(b'interrupted');orphan.chmod(0o600)
  assert upload(payload[:-1],csrf)[0]==400;assert not orphan.exists()
  # A sender that never finishes is bounded by the total upload deadline, rather
  # than extending the per-recv timeout indefinitely.
  with socket.create_connection(('127.0.0.1',port),timeout=4) as client:
   headers=(f'POST /upgrade HTTP/1.1\r\nHost: localhost\r\nAuthorization: {auth}\r\nContent-Type: application/octet-stream\r\nX-CSRF-Token: {csrf}\r\nX-Firmware-Name: ZT30_AP_slow.apbundle\r\nContent-Length: 100000\r\n\r\n').encode()
   started=time.monotonic();client.sendall(headers+b'x')
   reply=b''
   while True:
    block=client.recv(4096)
    if not block:break
    reply+=block
   assert b'400 Bad Request' in reply,reply
   assert time.monotonic()-started<3.5
  assert not list(run.glob('firmware-upload.*'));preserved()
  for version in ('fail-start','fail-ready'):
   status,message=upload(bundle(version)[0],csrf);assert status==500,(status,message)
   assert b'previous AP application restored' in message,message;assert os.readlink(app/'active')=='slots/'+old
   assert json.loads((app/'trial.json').read_bytes())['state']=='rolled-back';preserved()
  status,message=upload(payload,csrf);assert status==201,(status,message)
  answer=json.loads(message);assert answer=={'slot':id,'thermal':False},answer
  assert os.readlink(app/'active')=='slots/'+id
  status,result=request('/upgrade-result');assert status==200
  result=json.loads(result);assert result['state']=='ready' and result['candidate']==id,result
  status,page=request('/');assert b'http-test' in page and b'id=application-rollback' in page
  assert pending in page
  # Model independent supervisor confirmation; callback tests cover recovery,
  # hardware checks the actual listener/app/gates and confirmation.
  trial=json.loads((app/'trial.json').read_bytes());trial['state']='confirmed';(app/'trial.json').write_bytes(canonical(trial))
  status,result=request('/upgrade-result');assert json.loads(result)['state']=='confirmed'
  status,page=request('/');assert pending not in page
  status,bad_csrf=request('/upgrade/rollback',b'csrf=bad',{'Content-Type':'application/x-www-form-urlencoded'});assert status!=201 and os.readlink(app/'active')=='slots/'+id,(status,bad_csrf)
  status,result=request('/upgrade/rollback',('csrf='+csrf).encode(),{'Content-Type':'application/x-www-form-urlencoded'})
  assert status==201,(status,result);assert json.loads(result)['slot']==old
  assert os.readlink(app/'active')=='slots/'+old;preserved()
  assert not list(run.glob('firmware-upload.*'))
  print('PASS native ZT30 HTTP authentication/CSRF/name/type/locks, validation, failed-start/RGB rollback, pending result, version status and explicit AP rollback')
 finally:
  process.terminate();process.wait(timeout=5);log.close()
