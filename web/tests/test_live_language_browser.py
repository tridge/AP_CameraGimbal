"""Exercise real translated page scripts in Chrome without camera movement."""
import html
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def browser_check(page, script, setup, checks, name):
    browser = shutil.which('google-chrome') or shutil.which('chromium')
    if not browser:
        print('SKIP live/language browser regression: Chrome not installed')
        return
    with tempfile.TemporaryDirectory(prefix='camera-live-language-', ignore_cleanup_errors=True) as temp:
        root = Path(temp)
        document = re.sub(r'<script\b[^>]*>.*?</script>', '', page.decode(), flags=re.S)
        document = re.sub(r'<link\b[^>]*>|<img\b[^>]*>', '', document)
        result = '''<script>
        (async () => {
          const out = document.createElement('pre'); out.id='browser-test-result'; document.body.append(out);
          const assert = (ok, message) => { if (!ok) throw new Error(message); };
          const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
          const wait = async predicate => { for (let i=0;i<400;i++) { if(predicate()) return; await sleep(20); } throw new Error('condition timed out'); };
          try { CHECKS; out.textContent='PASS NAME'; }
          catch(error) { out.textContent='FAIL NAME: '+error.stack; }
        })();</script>'''.replace('CHECKS', checks).replace('NAME', name)
        document = document.replace('</body>', '<script>'+setup+'</script><script data-csrf="test">'+
                                    script.decode()+'</script>'+result+'</body>')
        fixture = root/'test.html'; fixture.write_text(document)
        if os.environ.get('WEB_BROWSER_EVIDENCE'):
            evidence = Path(os.environ['WEB_BROWSER_EVIDENCE'])
            evidence.mkdir(parents=True, exist_ok=True)
            (evidence/(name.replace(' ', '-')+'.html')).write_text(document)
        run = subprocess.run([browser, '--headless', '--no-sandbox', '--disable-gpu',
            '--disable-dev-shm-usage', '--no-first-run', '--disable-background-networking',
            '--disable-component-update', '--disable-extensions', '--virtual-time-budget=20000',
            '--user-data-dir='+str(root/'profile'), '--dump-dom', fixture.as_uri()],
            capture_output=True, timeout=35)
        match = re.search(r'<pre id="browser-test-result">(.*?)</pre>', run.stdout.decode(), re.S)
        assert run.returncode == 0 and match, run.stderr.decode()[-2000:]
        message = html.unescape(match[1])
        assert message.startswith('PASS '), message or 'Browser check did not finish'
        print(message)


LIVE_SETUP = r"""
const fixture = {zoom:10.3, calls:[], failCommand:false, failRenew:false, deferPoll:false, releasePoll:null};
HTMLMediaElement.prototype.load = function() {};
HTMLMediaElement.prototype.play = function() { return Promise.resolve(); };
window.fetch = async (url, options={}) => {
  if (url==='/live/attitude.json') {
    const snapshot={roll_deg:0,pitch_deg:-45,yaw_deg:0,roll_rate_dps:0,pitch_rate_dps:0,yaw_rate_dps:0,zoom:fixture.zoom};
    if (fixture.deferPoll) {
      fixture.deferPoll=false;
      await new Promise(resolve=>{fixture.releasePoll=resolve;});
    }
    return {ok:true,json:async()=>snapshot};
  }
  if (url!=='/live/control') throw new Error('Unexpected request '+url);
  const action=options.body.get('action'); fixture.calls.push([action,options.body.get('value')]);
  if (action==='acquire') return {ok:true,text:async()=> '0123456789abcdef0123456789abcdef'};
  if (action==='renew' && fixture.failRenew) return {ok:false,status:400,text:async()=> 'expired'};
  if (action==='zoom') {
    if (fixture.failCommand) { fixture.failCommand=false; return {ok:false,status:400,text:async()=> 'rejected'}; }
    await new Promise((resolve,reject) => {
      const timer=setTimeout(resolve,2000);
      options.signal.addEventListener('abort',()=>{clearTimeout(timer);reject(new DOMException('aborted','AbortError'));},{once:true});
    });
    fixture.zoom=Number(options.body.get('value'));
  }
  return {ok:true,text:async()=> 'OK'};
};
"""

LIVE_CHECKS = r"""
const zoom=document.getElementById('live-zoom'), value=document.getElementById('live-zoom-value'), enable=document.getElementById('manual-enable');
await wait(()=>value.textContent==='10.3x');
assert(Number(zoom.value)===10,'actual quantized zoom clamps only the slider');
enable.checked=true; enable.dispatchEvent(new Event('change'));
await wait(()=>enable.checked && !enable.disabled && !zoom.disabled);
const setZoom=n=>{zoom.value=n;zoom.dispatchEvent(new Event('input'));zoom.dispatchEvent(new Event('change'));};
setZoom(9);
await sleep(1700);
assert(enable.checked,'slow lens movement released manual control');
assert(value.textContent==='9.0x','polling overwrote pending slider change');
setZoom(7);setZoom(5);
await wait(()=>fixture.zoom===5);
await sleep(350);
assert(fixture.calls.filter(c=>c[0]==='zoom').map(c=>c[1]).join(',')==='9.0,5.0','zoom changes were not coalesced');
// A poll begun during a move must stay invalid after that move completes.
fixture.deferPoll=true;setZoom(6);
await wait(()=>fixture.releasePoll);
await wait(()=>fixture.zoom===6);await sleep(100);
fixture.releasePoll();await sleep(20);
assert(value.textContent==='6.0x','late poll overwrote completed zoom: '+value.textContent);
fixture.failCommand=true;setZoom(8);
await sleep(1100);
assert(enable.checked,'a command rejection released a valid lease');
fixture.failRenew=true;
await wait(()=>!enable.checked);
assert(zoom.disabled,'an expired lease did not disable controls');
const stream=document.getElementById('live-stream');
stream.value='1';stream.dispatchEvent(new Event('change'));
assert(sessionStorage.getItem('camera.live.stream')==='1','stream preference was not stored independently of labels');
const rate=document.getElementById('live-rate');rate.value='23';rate.dispatchEvent(new Event('input'));
assert(sessionStorage.getItem('camera.live.rate')==='23','manual rate preference was not stored');
"""


LOGIN_CHECKS = r"""
const select=document.getElementById('lang-login'), previous=select.value;
const username=document.getElementById('username'), password=document.getElementById('password');
username.value='engineer';password.value='unsaved-test-password';
let confirmations=0;
window.confirm=message=>{++confirmations;assert(message===select.dataset.unsaved && message.length>10,'localized login confirmation missing');return false;};
select.value=[...select.options].find(option=>option.value!==previous).value;
select.dispatchEvent(new Event('change'));
assert(confirmations===1 && select.value===previous,'login language change ignored cancellation');
assert(username.value==='engineer' && password.value==='unsaved-test-password','login credentials discarded');
assert(sessionStorage.length===0 && localStorage.length===0,'login credentials copied into browser storage');
"""


LANGUAGE_CHECKS = r"""
const nav=document.getElementById('lang-nav'), previous=nav.value, field=document.getElementById('brightness'), original=field.value;
const other=[...nav.options].find(option=>option.value!==previous).value;
let submitted=0, confirms=0, approved=false;
nav.form.submit=()=>{++submitted;};
window.confirm=message=>{++confirms;assert(message===nav.dataset.unsaved && message.length>10,'localized confirmation missing');return approved;};
window.location.hash='network';
field.value='63';
nav.value=other;nav.dispatchEvent(new Event('change'));
assert(submitted===0 && confirms===1 && nav.value===previous && field.value==='63','cancel discarded edits or changed language');
approved=true;nav.value=other;nav.dispatchEvent(new Event('change'));
assert(submitted===1 && nav.form.elements.namedItem('next').value==='/parameters#network','language change lost parameter tab');
field.value=original;nav.dispatchEvent(new Event('change'));
assert(submitted===2 && confirms===2,'clean form triggered an unnecessary confirmation');
assert(nav.form.elements.namedItem('lang').value===other,'translated label changed submitted language code');
"""


def check_live_browser(page, script, language):
    browser_check(page, script, LIVE_SETUP, LIVE_CHECKS, 'live control '+language)


def check_language_browser(page, script, language):
    browser_check(page, script, '', LANGUAGE_CHECKS, 'language navigation '+language)


def check_login_language_browser(page, script, language):
    browser_check(page, script, '', LOGIN_CHECKS, 'login language '+language)
