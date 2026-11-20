#!/usr/bin/env python3
'''Real bundle staging with injected lifecycle failures and persistent recovery.'''
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import test_zt30_bundle as fixtures
from test_zt30_bundle import canonical, sha

class Update(unittest.TestCase):
    a = fixtures.Bundle.a
    old_manifest = fixtures.Bundle.old_manifest
    @classmethod
    def setUpClass(cls):
        fixtures.Bundle.setUpClass.__func__(cls)
        source=Path(cls.build.name)/'update.cpp'
        source.write_text(r'''#include "apcam/zt30_services.h"
#include <iostream>
struct Context {std::string root,old,mode;};
bool restart(void *p,const std::string &id,std::string &error) {
 auto &c=*static_cast<Context*>(p);
 FILE *f=fopen((c.root+"/calls").c_str(),"a");if(!f)return false;
 fprintf(f,"restart %s\n",id.c_str());fclose(f);
 if(id!=c.old && c.mode=="crash")_exit(77);
 if(id!=c.old && c.mode=="start-fail")return apcam_zt30::fail(error,"injected start failure");
 return true;
}
bool ready(void *p,const std::string &id,bool &thermal,std::string &error) {
 auto &c=*static_cast<Context*>(p);thermal=c.mode!="thermal-degraded";
 if((id!=c.old && c.mode=="ready-fail") || c.mode=="rollback-fail")return apcam_zt30::fail(error,"injected readiness failure");
 return true;
}
int main(int argc,char **argv) {
 if(argc!=5)return 2;
 Context c{argv[1],"",argv[4]};std::string error;
 if(!apcam_zt30::link_slot(c.root,"active",c.old,error))return 3;
 apcam_zt30::UpdateResult result;
 bool ok=apcam_zt30::update_application(c.root,argv[3],argv[2],{restart,ready,&c},result,error);
 std::cout<<"ok="<<ok<<" restored="<<result.restored<<" thermal="<<result.thermal<<" error="<<error<<"\n";
 return ok?0:1;
}''')
        cls.runner=Path(cls.build.name)/'update'
        subprocess.run(['c++','-std=gnu++17','-O2','-Wall','-Wextra','-Werror','-I'+str(Path(__file__).resolve().parents[1]/'include'),str(source),'-o',str(cls.runner)],check=True)
        source=Path(cls.build.name)/'activation-fault.c'
        source.write_text('''#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
int rename(const char *a,const char *b){
 int (*real)(const char*,const char*)=dlsym(RTLD_NEXT,"rename");size_t n=strlen(b);
 if(n>=7 && !strcmp(b+n-7,"/active")){errno=EIO;return -1;}return real(a,b);
}''')
        cls.activation_fault=Path(cls.build.name)/'activation-fault.so'
        subprocess.run(['cc','-shared','-fPIC',str(source),'-ldl','-o',str(cls.activation_fault)],check=True)
    @classmethod
    def tearDownClass(cls):cls.build.cleanup()
    def setUp(self):
        fixtures.Bundle.setUp(self)
        self.run=self.base/'run';self.run.mkdir(mode=0o755)
    tearDown=fixtures.Bundle.tearDown
    bundle=fixtures.Bundle.bundle
    call=fixtures.Bundle.call
    assert_preserved=fixtures.Bundle.assert_preserved
    active_old=fixtures.Bundle.active_old
    def command(self,*args):
        return [str(self.binary),'--root',str(self.root),'--runtime-root',str(self.runtime),'--run-dir',str(self.run),*map(str,args)]
    def update(self,mode='success',code=0,env=None):
        p=subprocess.run([str(self.runner),str(self.root),str(self.runtime),str(self.bundle()),mode],capture_output=True,text=True,timeout=5,env=env)
        self.assertEqual(p.returncode,code,p.stdout+p.stderr);self.assert_preserved();return p.stdout
    def trial(self):return json.loads((self.root/'trial.json').read_bytes())
    def rollback_path(self,t):
        identity='\n'.join(str(t[key]) for key in ('previous','candidate','boot','deadline'))
        return self.root/('trial.rollback.v1.'+sha(identity.encode()))
    def write_trial(self,t):
        # Simulate the production writer when injecting a boot/deadline change.
        old=self.rollback_path(self.trial())
        if old.exists():
            extension=self.rollback_path(t);extension.write_bytes(old.read_bytes());extension.chmod(0o600)
        (self.root/'trial.json').write_bytes(canonical(t));(self.root/'trial.json').chmod(0o600)
    def test_update_pending_until_both_real_services_are_verified(self):
        self.update();t=self.trial();self.assertEqual(t['state'],'ready')
        self.assertEqual(os.readlink(self.root/'active'),'slots/'+t['candidate'])
        self.assertEqual(os.readlink(self.root/'previous'),'slots/'+self.a)
        self.call('maintain');self.assertEqual(self.trial()['state'],'ready') # no actual app/web means no confirmation
        self.update(code=1);self.assertEqual(self.trial()['state'],'ready') # cannot replace a pending transaction
    def test_activation_failure_does_not_restart_the_previous_app(self):
        output=self.update(code=1,env=os.environ|{'LD_PRELOAD':str(self.activation_fault)})
        self.assertIn('restored=1',output);self.active_old()
        self.assertFalse((self.root/'calls').exists())
        self.assertEqual(self.trial()['state'],'rolled-back')
    def test_failure_before_ready_recovers_previous_ap(self):
        for mode in ('start-fail','ready-fail'):
            output=self.update(mode,1);self.assertIn('restored=1',output);self.assertIn('previous AP application restored',output)
            self.active_old();self.assertEqual(self.trial()['state'],'rolled-back')
    def test_rollback_failure_is_not_reported_as_success(self):
        output=self.update('rollback-fail',1);self.assertIn('restored=0',output)
        self.assertIn('previous AP application did not recover',output);self.active_old()
        self.assertEqual(self.trial()['state'],'recovering')
        self.call('maintain');self.assertEqual(self.trial()['state'],'rolled-back')
    def test_worker_death_after_activation_is_recovered_on_next_boot(self):
        self.update('crash',77);t=self.trial();self.assertEqual(t['state'],'starting')
        t['boot']='simulated-prior-boot';self.write_trial(t)
        self.call('maintain');self.active_old();self.assertEqual(self.trial()['state'],'rolled-back')
    def test_expired_trial_recovers_without_power_cycle(self):
        self.update();t=self.trial();t['deadline']=1;self.write_trial(t)
        self.call('maintain');self.active_old();self.assertEqual(self.trial()['state'],'rolled-back')
    def test_three_failed_web_launches_recover_previous(self):
        self.update()
        for count in range(1,4):self.call('note-web-launch');self.assertEqual(self.trial()['attempts'],count)
        self.call('note-web-launch');self.active_old();self.assertEqual(self.trial()['state'],'rolled-back')
    def test_compare_and_swap_preserves_operator_selection(self):
        self.update();t=self.trial();t['boot']='old-boot';self.write_trial(t)
        self.call('activate',self.a)
        self.call('maintain');self.active_old();self.assertEqual(self.trial()['state'],'abandoned')
    def test_thermal_degradation_does_not_fail_rgb_upgrade(self):
        self.assertIn('thermal=0',self.update('thermal-degraded'))
        self.assertEqual(self.trial()['state'],'ready')
    def test_failed_updates_preserve_the_older_manual_restore_target(self):
        older='d'*64
        for mode in ('start-fail','ready-fail','crash'):
            with self.subTest(mode=mode):
                previous=self.root/'previous'
                if previous.is_symlink():previous.unlink()
                previous.symlink_to('slots/'+older)
                self.update(mode,77 if mode=='crash' else 1)
                if mode=='crash':
                    trial=self.trial();trial['deadline']=1;self.write_trial(trial);self.call('maintain')
                self.assertEqual(os.readlink(previous),'slots/'+older)
                self.active_old()
                self.assertEqual(self.trial()['state'],'rolled-back')
    def test_failed_update_preserves_an_absent_previous_selection(self):
        self.update('ready-fail',1)
        self.assertFalse((self.root/'previous').is_symlink())
    def test_interrupted_before_activation_preserves_the_old_restore_target(self):
        self.update('crash',77)
        trial=self.trial();trial['rollback_previous']='e'*64
        self.write_trial(trial)
        self.call('activate',self.a)
        self.call('maintain')
        self.active_old();self.assertEqual(os.readlink(self.root/'previous'),'slots/'+'e'*64)
        self.assertEqual(self.trial()['state'],'rolled-back')
    def test_journal_remains_readable_by_strict_legacy_helpers(self):
        self.update()
        trial=self.trial()
        self.assertEqual(set(trial),{'previous','candidate','boot','deadline','attempts','state'})
        extension=self.rollback_path(trial)
        self.assertEqual(json.loads(extension.read_bytes()),{'schema':'apcg.zt30.rollback.v1','previous':''})
        self.assertEqual(extension.stat().st_mode&0o777,0o600)
        # An interrupted writer may leave metadata for the next epoch. The
        # current journal must retain its own rollback selection.
        next_trial=trial|{'deadline':trial['deadline']+1}
        self.rollback_path(next_trial).write_bytes(canonical({'schema':'apcg.zt30.rollback.v1','previous':'e'*64}))
        trial['deadline']=1;self.write_trial(trial);self.call('maintain')
        self.assertFalse((self.root/'previous').is_symlink())
    def test_unknown_rollback_metadata_version_is_rejected(self):
        self.update()
        self.rollback_path(self.trial()).write_bytes(canonical({'schema':'apcg.zt30.rollback.v2','previous':''}))
        self.call('maintain',ok=False)
    def test_malformed_trial_fails_closed(self):
        (self.root/'trial.json').write_bytes(b'{"state":"ready"}')
        self.update(code=1);self.active_old();self.call('maintain',ok=False)

if __name__=='__main__':unittest.main()
