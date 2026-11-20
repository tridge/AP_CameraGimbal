#!/usr/bin/env python3
'''Exercise the real native stager, including interrupted and failed writes.

Tiny ARM headers are parser fixtures, not runnable or ABI-qualified applications.
Production bundles additionally undergo the existing full host ELF checker.
'''
import argparse
import copy
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_zt30_app_bundle as builder


def sha(data): return hashlib.sha256(data).hexdigest()
def canonical(manifest): return json.dumps(manifest, separators=(',', ':')).encode()
def arm(shared=False):
    ident = b'\x7fELF\x01\x01\x01' + bytes(9)
    interp = b'/lib/ld-linux-armhf.so.3\0'
    header = struct.pack('<16sHHIIIIIHHHHHH', ident, 3 if shared else 2, 40, 1, 0, 52, 0, 0x05000400,
                         52, 32, 1, 40, 0, 0)
    ph = struct.pack('<IIIIIIII', 1 if shared else 3, 84, 0, 0, len(interp), len(interp), 4, 1)
    return header + ph + interp


class Bundle(unittest.TestCase):
    old_manifest = canonical({'schema':'apcg.zt30.slot-development.v1','board':'SIYI_ZT30','label':'old'})
    a = sha(old_manifest)
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='zt30-bundle-build-')
        cls.binary = Path(cls.build.name) / 'zt30-slot'
        subprocess.run(['c++', '-std=gnu++17', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'include'),
                        str(ROOT/'tools/zt30_slots.cpp'), '-o', str(cls.binary)], check=True)
        source = Path(cls.build.name) / 'fault.c'
        source.write_text(r'''#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>
int fsync(int fd) {
 static unsigned calls;
 int (*real_sync)(int) = dlsym(RTLD_NEXT, "fsync");
 ++calls;
 const char *fault = getenv("ZTEST_FAULT");
 if(fault && calls==1 && !strcmp(fault,"write")) {errno=EIO;return -1;}
 if(fault && calls==1 && !strcmp(fault,"pause")) {
  const char *marker=getenv("ZTEST_MARKER");int f=open(marker,O_CREAT|O_WRONLY,0600);if(f>=0)close(f);
  for(;;)pause();
 }
 if(fault && !strcmp(fault,"publication")) {
  char link[64],path[4096];snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
  ssize_t n=readlink(link,path,sizeof(path)-1);
  if(n>0) {path[n]=0; if(n>=6 && !strcmp(path+n-6,"/slots")){errno=EIO;return -1;}}
 }
 return real_sync(fd);
}
int statvfs(const char *path, struct statvfs *st) {
 int (*real_stat)(const char *, struct statvfs *)=dlsym(RTLD_NEXT,"statvfs");
 int r=real_stat(path,st);const char *fault=getenv("ZTEST_FAULT");
 if(!r && fault && !strcmp(fault,"space"))st->f_bavail=1;
 return r;
}''')
        cls.shim = Path(cls.build.name) / 'fault.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', str(source), '-ldl', '-o', str(cls.shim)], check=True)
        source = Path(cls.build.name) / 'hash.cpp'
        source.write_text('#include "apcam/sha256.h"\n#include <iostream>\nint main(){apcam::SHA256 h;char b[13];while(std::cin){std::cin.read(b,sizeof(b));h.update(b,size_t(std::cin.gcount()));}std::cout<<h.finish();}\n')
        cls.hashbin = Path(cls.build.name) / 'hash'
        subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+str(ROOT/'include'), str(source), '-o', str(cls.hashbin)], check=True)
    @classmethod
    def tearDownClass(cls): cls.build.cleanup()
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='zt30-bundle-')
        self.base = Path(self.temp.name)
        self.root = self.base/'app'; self.root.mkdir(mode=0o755)
        self.runtime = self.base/'runtime'; (self.runtime/'lib').mkdir(parents=True)
        runtime = b'qualified-runtime-fixture'
        (self.runtime/'lib/libc.so.6').write_bytes(runtime)
        for name in ('zt30-slot','app-init.sh','app-supervisor.sh','web-supervisor.sh','start-dropbear.sh','dropbear'):
            (self.root/name).write_text('stable '+name); (self.root/name).chmod(0o755)
        (self.root/'camera.ini').write_bytes(b'persisted user settings\n')
        (self.root/'web.pass').write_bytes(b'authentication fixture\n')
        (self.root/'auth').mkdir(mode=0o700)
        (self.root/'auth/key').write_bytes(b'identity fixture\n')
        self.payload = {'camera-app': arm(), 'zt30-web': arm(), 'encoder-sdk.so': arm(True),
                        'webroot/live.js': b'live fixture', 'webroot/status-actions.html': b'status fixture',
                        'webroot/parameters-form.html': b'parameters fixture',
                        'webroot/licenses/dlib.txt': b'license fixture'}
        self.manifest = {'schema':'apcg.zt30.app.v1','board':'SIYI_ZT30','launcher':1,'version':'test-1',
                         'runtime':[{'name':'libc.so.6','path':'/lib/libc.so.6','size':len(runtime),'sha256':sha(runtime)}],
                         'files':[{'path':p,'mode':0o644 if p.startswith('webroot/') else 0o755,
                                   'size':len(b),'sha256':sha(b)} for p,b in sorted(self.payload.items())]}
        (self.root/'slots').mkdir(mode=0o755)
        old = self.root/'slots'/(self.a); old.mkdir(mode=0o755)
        (old/'webroot').mkdir(mode=0o755)
        (old/'manifest.json').write_bytes(self.old_manifest)
        (old/'manifest.json').chmod(0o644)
        for p,b in self.payload.items():
            f=old/p; f.parent.mkdir(parents=True, exist_ok=True,mode=0o755); f.write_bytes(b)
            f.chmod(0o644 if p.startswith('webroot/') else 0o755)
        (self.root/'active').symlink_to('slots/'+self.a)
        self.protected = {p:(self.root/p).read_bytes() for p in ('camera.ini','web.pass','auth/key')}
    def tearDown(self): self.temp.cleanup()
    def bundle(self, manifest=None, payload=None, raw=None):
        m=copy.deepcopy(self.manifest if manifest is None else manifest)
        text=canonical(m) if raw is None else raw
        p=self.base/'update.apbundle'
        p.write_bytes(b'APCZT301'+struct.pack('<I',len(text))+text+
                      b''.join((self.payload if payload is None else payload)[f['path']] for f in m['files']))
        return p
    def command(self,*args):
        return [str(self.binary),'--root',str(self.root),'--runtime-root',str(self.runtime),*map(str,args)]
    def call(self,*args,ok=True,fault=None,error=None):
        env=os.environ.copy()
        if fault: env.update(LD_PRELOAD=str(self.shim), ZTEST_FAULT=fault)
        p=subprocess.run(self.command(*args),capture_output=True,text=True,env=env,timeout=5)
        self.assertEqual(p.returncode==0,ok,p.stderr)
        if error is not None:self.assertIn(error,p.stderr)
        self.assert_preserved()
        return p.stdout.strip()
    def assert_preserved(self):
        for p,b in self.protected.items(): self.assertEqual((self.root/p).read_bytes(),b)
    def active_old(self): self.assertEqual(os.readlink(self.root/'active'),'slots/'+self.a)
    def test_sha256_known_vectors_and_block_boundaries(self):
        for b in (b'',b'abc',b'a'*55,b'a'*56,b'a'*64,b'a'*65,b'a'*1000000,bytes(range(256))*999):
            p=subprocess.run([str(self.hashbin)],input=b,capture_output=True,check=True)
            self.assertEqual(p.stdout.decode(),sha(b))
    def test_stage_activate_idempotent_and_explicit_rollback(self):
        bundle=self.bundle(); id=sha(canonical(self.manifest))
        self.assertEqual(self.call('stage',bundle),id); self.active_old()
        slot=self.root/'slots'/id
        self.assertEqual((slot/'manifest.json').read_bytes(),canonical(self.manifest))
        for f in self.manifest['files']:
            self.assertEqual((slot/f['path']).read_bytes(),self.payload[f['path']])
            self.assertEqual((slot/f['path']).stat().st_mode&0o7777,f['mode'])
        self.assertEqual(self.call('stage',bundle),id)
        self.call('activate',id);self.assertEqual(os.readlink(self.root/'active'),'slots/'+id)
        self.call('activate',self.a);self.active_old();self.assertTrue(slot.exists())
        self.assertFalse(list((self.root/'slots').glob('.stage.*')))
    def test_damaged_existing_slot_never_repaired_or_accepted(self):
        bundle=self.bundle();id=self.call('stage',bundle);slot=self.root/'slots'/id
        (slot/'webroot/live.js').write_bytes(b'damaged')
        self.call('stage',bundle,ok=False);self.active_old()
        self.assertEqual((slot/'webroot/live.js').read_bytes(),b'damaged')
    def test_wrong_identity_schema_version_and_unknown_fields(self):
        for field,value in (('board','SIYI_MT11'),('schema','apcg.zt30.app.v2'),('launcher',2),('version','../bad')):
            m=copy.deepcopy(self.manifest);m[field]=value
            self.call('stage',self.bundle(m),ok=False);self.active_old()
        m=copy.deepcopy(self.manifest);m['unknown']=True
        self.call('stage',self.bundle(m),ok=False)
    def test_paths_modes_duplicates_order_and_missing_files(self):
        for bad in ('../camera.ini','/customer/camera.ini','webroot/../web.pass','webroot/.hidden.js',
                    'webroot/x//evil.js','app-init.sh','webroot/live.js/extra'):
            m=copy.deepcopy(self.manifest)
            m['files'].append({'path':bad,'mode':0o644,'size':7,'sha256':sha(b'fixture')})
            m['files'].sort(key=lambda f:f['path'])
            data=self.payload|{bad:b'fixture'}
            self.call('stage',self.bundle(m,data),ok=False,error='Disallowed bundle path');self.active_old()
        for mode in (0o777,0o4755,0o644):
            m=copy.deepcopy(self.manifest);m['files'][0]['mode']=mode
            self.call('stage',self.bundle(m),ok=False)
        m=copy.deepcopy(self.manifest);m['files'].append(m['files'][-1].copy())
        self.call('stage',self.bundle(m),ok=False)
        m=copy.deepcopy(self.manifest);m['files'].reverse();self.call('stage',self.bundle(m),ok=False)
        m=copy.deepcopy(self.manifest);m['files']=m['files'][1:];self.call('stage',self.bundle(m),ok=False)
    def test_truncation_trailing_bytes_corruption_lengths_and_json(self):
        original=self.bundle().read_bytes()
        for data in (original[:11],original[:-1],original+b'extra',b'OTHER001'+original[8:],
                     original[:8]+struct.pack('<I',65537)+original[12:],
                     original[:-1]+bytes([original[-1]^1])):
            b=self.base/'bad.apbundle';b.write_bytes(data);self.call('stage',b,ok=False);self.active_old()
        m=copy.deepcopy(self.manifest);m['files'][0]['size']=2**63
        self.call('stage',self.bundle(m),ok=False)
        text=canonical(self.manifest)
        for raw in (text+b'\n',text.replace(b'"launcher":1',b'"launcher":01'),
                    text.replace(b'"version":"test-1"',b'"version":"test\\u002d1"'),
                    text.replace(b'"board":"SIYI_ZT30"',b'"board":"SIYI_ZT30","board":"SIYI_ZT30"')):
            self.call('stage',self.bundle(raw=raw),ok=False)
    def test_directory_budget_is_bounded_before_staging(self):
        payload=self.payload.copy()
        for i in range(70):payload[f'webroot/branch{i}/one/two/asset.js']=b'asset'
        m=copy.deepcopy(self.manifest)
        m['files']=[{'path':name,'mode':0o644 if name.startswith('webroot/') else 0o755,
                     'size':len(data),'sha256':sha(data)} for name,data in sorted(payload.items())]
        self.call('stage',self.bundle(m,payload),ok=False,error='Too many bundle tree entries');self.active_old()
        self.assertFalse(list((self.root/'slots').glob('.stage.*')))
    def test_exact_tree_entry_boundary_stages_activates_and_repeats(self):
        payload=self.payload.copy()
        for i in range(61):payload[f'webroot/branch{i}/one/two/asset.js']=b'asset'
        payload.update({'webroot/extra1.js':b'one','webroot/extra2.js':b'two'})
        m=copy.deepcopy(self.manifest)
        def indexed():return [{'path':name,'mode':0o644 if name.startswith('webroot/') else 0o755,
                               'size':len(data),'sha256':sha(data)} for name,data in sorted(payload.items())]
        m['files']=indexed();b=self.bundle(m,payload);id=self.call('stage',b)
        self.call('activate',id);self.assertEqual(self.call('stage',b),id)
        payload['webroot/entry257.js']=b'one over';m['files']=indexed()
        self.call('stage',self.bundle(m,payload),ok=False,error='Too many bundle tree entries')
        self.assertEqual(os.readlink(self.root/'active'),'slots/'+id)
        self.assertFalse(list((self.root/'slots').glob('.stage.*')))
    def test_bad_arm_elf_even_with_updated_checksum(self):
        for offset,value in ((4,2),(5,2),(18,62),(37,0),(84,ord('X'))):
            data=bytearray(self.payload['camera-app']);data[offset]=value
            payload=self.payload|{'camera-app':bytes(data)}
            m=copy.deepcopy(self.manifest);m['files'][0]['sha256']=sha(data)
            self.call('stage',self.bundle(m,payload),ok=False,error='Invalid basic ARM hard-float ELF ABI');self.active_old()
            self.assertFalse(list((self.root/'slots').glob('.stage.*')))
    def test_activation_rechecks_hashes_identity_modes_and_extra_entries(self):
        b=self.bundle();id=self.call('stage',b);slot=self.root/'slots'/id
        original=(slot/'webroot/live.js').read_bytes()
        (slot/'webroot/live.js').write_bytes(b'corruption');self.call('activate',id,ok=False);self.active_old()
        (slot/'webroot/live.js').write_bytes(original)
        (slot/'webroot/extra.js').write_bytes(b'extra');self.call('activate',id,ok=False);self.active_old()
        (slot/'webroot/extra.js').unlink()
        (slot/'camera-app').chmod(0o700);self.call('activate',id,ok=False);self.active_old()
        (slot/'camera-app').chmod(0o755)
        (slot/'manifest.json').write_bytes(canonical(self.manifest)+b' ');self.call('activate',id,ok=False);self.active_old()
        (slot/'manifest.json').unlink();self.call('activate',id,ok=False);self.active_old()
    def test_extra_empty_directory_and_runtime_change_block_activation(self):
        b=self.bundle();id=self.call('stage',b);slot=self.root/'slots'/id
        (slot/'webroot/extra').mkdir(mode=0o755);self.call('activate',id,ok=False);self.active_old()
        (slot/'webroot/extra').rmdir()
        (self.runtime/'lib/libc.so.6').write_bytes(b'changed-runtime')
        self.call('activate',id,ok=False);self.active_old()
        self.call('activate',self.a) # explicit trusted migration rollback is unaffected
    def test_development_manifest_identity_is_not_inferred_from_substrings(self):
        old=self.root/'slots'/self.a
        (old/'manifest.json').write_bytes(self.old_manifest+b' ')
        self.call('activate',self.a,ok=False);self.active_old()
        for text in (
            self.old_manifest.replace(b'"schema":',b'"schema":"apcg.zt30.slot-development.v1","schema":'),
            self.old_manifest.replace(b'slot-development',b'slot-development\\u0000'),
            self.old_manifest.replace(b'SIYI_ZT30',b'SIYI_MT11')):
            id=sha(text);destination=self.root/'slots'/id
            import shutil
            shutil.copytree(old,destination)
            (destination/'manifest.json').write_bytes(text)
            self.call('activate',id,ok=False);self.active_old()
    def test_bundle_symlink_and_fifo_fail_without_blocking(self):
        b=self.bundle();link=self.base/'symlink.apbundle';link.symlink_to(b)
        self.call('stage',link,ok=False);self.active_old()
        fifo=self.base/'fifo.apbundle';os.mkfifo(fifo)
        self.call('stage',fifo,ok=False);self.active_old()
    def test_staging_can_repair_invalid_active_selection(self):
        (self.root/'active').unlink();(self.root/'active').symlink_to('slots/'+'b'*64)
        id=self.call('stage',self.bundle());self.call('activate',id)
        self.assertEqual(os.readlink(self.root/'active'),'slots/'+id)
    def test_runtime_mismatch_path_and_duplicate(self):
        (self.runtime/'lib/libc.so.6').write_bytes(b'other')
        self.call('stage',self.bundle(),ok=False);self.active_old()
        m=copy.deepcopy(self.manifest);m['runtime'][0]['path']='/etc/passwd'
        self.call('stage',self.bundle(m),ok=False)
        m=copy.deepcopy(self.manifest);m['runtime'].append(m['runtime'][0].copy())
        self.call('stage',self.bundle(m),ok=False)
    def test_writer_lock_and_bootstrap_dependency(self):
        b=self.bundle()
        with (self.root/'.slot-update.lock').open('w') as lock:
            (self.root/'.slot-update.lock').chmod(0o600)
            fcntl.flock(lock,fcntl.LOCK_EX);self.call('stage',b,ok=False)
        (self.root/'start-dropbear.sh').unlink();self.call('stage',b,ok=False);self.active_old()
    def test_space_and_staged_fsync_failure(self):
        b=self.bundle()
        for fault in ('space','write'):
            self.call('stage',b,ok=False,fault=fault);self.active_old()
            self.assertFalse(list((self.root/'slots').glob('.stage.*')))
    def test_interrupted_staging_reaped_under_writer_lock(self):
        b=self.bundle();marker=self.base/'paused'
        env=os.environ|{'LD_PRELOAD':str(self.shim),'ZTEST_FAULT':'pause','ZTEST_MARKER':str(marker)}
        p=subprocess.Popen(self.command('stage',b),env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            until=time.monotonic()+5
            while not marker.exists() and time.monotonic()<until: time.sleep(.01)
            self.assertTrue(marker.exists());self.active_old()
            self.assertTrue(list((self.root/'slots').glob('.stage.*')))
        finally:
            p.kill();p.communicate(timeout=5)
        self.call('stage',b);self.active_old()
        self.assertFalse(list((self.root/'slots').glob('.stage.*')))
    def test_failed_parent_sync_leaves_only_complete_retained_payload(self):
        b=self.bundle();self.call('stage',b,ok=False,fault='publication',error=sha(canonical(self.manifest)));self.active_old()
        id=sha(canonical(self.manifest));self.assertTrue((self.root/'slots'/id/'manifest.json').is_file())
        self.assertEqual(self.call('stage',b),id);self.active_old()
    def test_orphan_symlink_is_not_traversed(self):
        outside=self.base/'outside';outside.mkdir();(outside/'preserve').write_text('safe')
        stage=self.root/'slots/.stage.ABC123';stage.mkdir(mode=0o700)
        (stage/'escape').symlink_to(outside,target_is_directory=True)
        self.call('stage',self.bundle());self.assertEqual((outside/'preserve').read_text(),'safe')
        stage.symlink_to(outside,target_is_directory=True)
        self.call('stage',self.bundle(),ok=False);self.assertTrue((outside/'preserve').exists())
    def test_builder_is_reproducible_and_runs_full_checker(self):
        inputs=self.base/'inputs';inputs.mkdir();assets=inputs/'webroot';assets.mkdir()
        for name,data in self.payload.items():
            path=inputs/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
        args=argparse.Namespace(camera_app=inputs/'camera-app',web_app=inputs/'zt30-web',sdk=inputs/'encoder-sdk.so',
                                webroot=assets,runtime_root=self.runtime,readelf='readelf',version='test-1',output=self.base/'builder.apbundle')
        def checked(path,*args,**kwargs):
            return {'runtime_dependencies':[{'name':'libc.so.6','path':str(self.runtime/'lib/libc.so.6')}]}
        with patch.object(builder,'check',side_effect=checked) as checker:
            first=builder.build(args);data=args.output.read_bytes();second=builder.build(args)
            self.assertEqual(first,second);self.assertEqual(args.output.read_bytes(),data)
            self.assertEqual(checker.call_count,6)
            self.assertEqual(self.call('stage',args.output),first['slot'])
        with patch.object(builder,'check',side_effect=ValueError('ABI incompatible')):
            with self.assertRaisesRegex(ValueError,'ABI incompatible'):builder.build(args)
        self.assertEqual(args.output.read_bytes(),data) # rejected build cannot damage previous output
        (assets/'escape.js').symlink_to(inputs/'camera-app')
        with patch.object(builder,'check',side_effect=checked):
            with self.assertRaisesRegex(ValueError,'symlink'):builder.build(args)
        (assets/'escape.js').unlink()
        for i in range(70):
            path=assets/f'branch{i}/one/two/asset.js';path.parent.mkdir(parents=True);path.write_bytes(b'asset')
        with patch.object(builder,'check',side_effect=checked):
            with self.assertRaisesRegex(ValueError,'too many application tree entries'):builder.build(args)


if __name__ == '__main__': unittest.main()
