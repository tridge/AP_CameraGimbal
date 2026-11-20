#!/usr/bin/env python3
'''Execute native application-update browser confirmation with a DOM/XHR stub.'''
import json,subprocess
from pathlib import Path
script=(Path(__file__).resolve().parents[1]/'webroot/upgrade.js').read_text()
harness=r'''
const vm=require('vm'),assert=require('assert');
const source=JSON.parse(require('fs').readFileSync(0,'utf8')).script;
for(const state of ['confirmed','rolled-back','abandoned','login']) {
 let change,reloaded=0,redirected='',alerts=[],timers=[],polls=0;
 const slot='b'.repeat(64),old='a'.repeat(64),epoch='c'.repeat(64);
 const nodes={'firmware-upload':{},'firmware':{files:[{name:'ZT30_AP_test.apbundle',size:1024}],addEventListener:(t,f)=>change=f},
 'select-firmware':{addEventListener:()=>{}},'firmware-progress':{},'firmware-status':{}};
 const L={applicationUpdate:'true',prefix:'ZT30_AP_',suffix:'.apbundle',maxSize:'33554432',confirmUpload:'Install %s?',
 uploaded:'Checking application',back:'Ready',appFailed:'Rolled back',closed:'Closed'};
 const context={L,Date,document:{currentScript:{dataset:{csrf:old}},getElementById:id=>nodes[id]},
 confirm:()=>true,alert:s=>alerts.push(s),setTimeout:f=>timers.push(f),window:{location:{reload:()=>reloaded++,replace:p=>redirected=p}},
 XMLHttpRequest:function(){this.upload={};this.open=(m,p)=>{this.method=m;this.path=p};this.setRequestHeader=()=>{};
 this.send=()=>{
  if(this.method==='POST'){this.status=201;this.responseText=JSON.stringify({slot,thermal:true});this.onload();return;}
  assert(this.path.startsWith('/upgrade-result?'));polls++;
  if(polls===1){this.status=200;this.responseText=JSON.stringify({candidate:slot,state:'ready',csrf:epoch});}
  else if(state==='login'){this.status=401;this.responseText='';}
  else {this.status=200;this.responseText=JSON.stringify({candidate:slot,state,csrf:epoch});}
  this.onload();
 };}};
 vm.runInNewContext(source,context);change();
 assert.equal(reloaded,0,'must not finish a pending trial');assert.equal(polls,1);
 for(let i=0;i<10 && timers.length;i++)timers.shift()();
 if(state==='login'){assert.equal(redirected,'/login');assert.equal(reloaded,0);}
 else {assert.equal(reloaded,1);assert.equal(polls,2);if(state!=='confirmed')assert(alerts.includes('Rolled back'));}
}
'''
subprocess.run(['node','-e',harness],input=json.dumps({'script':script}),text=True,check=True)
print('PASS native application monitor waits for exact candidate confirmation, handles rollback/abandonment and login expiry')
