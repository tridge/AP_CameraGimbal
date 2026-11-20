#!/usr/bin/env python3
"""Headed stock-Firefox real thermal/terrain qualification using W3C WebDriver.

Requires geckodriver and an existing dedicated Xephyr display. Recordings are
selected with a local file input; neither script nor viewer uploads their bytes.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--thermal', type=Path, required=True)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--driver', default='geckodriver')
    parser.add_argument('--firefox', default=shutil.which('firefox'))
    parser.add_argument('--display', default=':99')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--navigation', action='store_true', help='Also test Follow and physical map clicks')
    parser.add_argument('--frame', type=int, default=8315)
    parser.add_argument('--x', type=int, default=337)
    parser.add_argument('--y', type=int, default=373)
    args = parser.parse_args()
    if not re.fullmatch(r':[1-9]\d*(?:\.\d+)?', args.display):
        parser.error('Use a dedicated Xephyr display, never the operator display :0')
    args.output.mkdir(parents=True, exist_ok=True)
    for path in (args.thermal, args.log):
        if not path.is_file():
            parser.error(f'File does not exist: {path}')
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    session = None
    with tempfile.TemporaryDirectory(prefix='viewer-firefox-') as profile_root:
        runtime = Path(profile_root, 'runtime')
        runtime.mkdir(mode=0o700)
        environment = dict(os.environ, DISPLAY=args.display,
                           MOZ_DISABLE_CONTENT_SANDBOX='1', MOZ_DISABLE_RDD_SANDBOX='1',
                           LIBGL_ALWAYS_SOFTWARE='1', XDG_RUNTIME_DIR=str(runtime))
        with (args.output / 'geckodriver.log').open('w') as driver_log:
            driver = subprocess.Popen([args.driver, '--host', '127.0.0.1', '--port', str(port),
                                       '--profile-root', profile_root], env=environment,
                                      stdout=driver_log, stderr=subprocess.STDOUT)
            def call(method, path, body=None):
                data = json.dumps(body).encode() if body is not None else None
                req = urllib.request.Request(f'http://127.0.0.1:{port}{path}', data=data,
                                             method=method, headers={'Content-Type': 'application/json'})
                with opener.open(req, timeout=180) as response:
                    value = json.load(response)['value']
                if isinstance(value, dict) and 'error' in value and 'stacktrace' in value:
                    raise RuntimeError(value)
                return value

            def command(method, path, body=None):
                return call(method, f'/session/{session}{path}', body)

            def js(source, values=None):
                return command('POST', '/execute/sync', {'script': source, 'args': values or []})

            def wait(expression, timeout=150):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if js('return ' + expression):
                        return
                    time.sleep(.25)
                raise TimeoutError(expression)

            def select_file(selector, path):
                element = command('POST', '/element', {'using': 'css selector', 'value': selector})
                key = element['element-6066-11e4-a52e-4f735466cecf']
                command('POST', f'/element/{key}/value', {'text': str(path.resolve())})

            try:
                for _ in range(100):
                    try:
                        call('GET', '/status')
                        break
                    except OSError:
                        if driver.poll() is not None:
                            raise RuntimeError('geckodriver exited before startup')
                        time.sleep(.1)
                result = call('POST', '/session', {'capabilities': {'alwaysMatch': {
                    'browserName': 'firefox', 'moz:firefoxOptions': {
                        'binary': str(Path(args.firefox).resolve()), 'args': ['-no-remote'],
                        'prefs': {'webgl.force-enabled': True, 'gfx.webrender.software': True,
                                  'browser.shell.checkDefaultBrowser': False}}}}})
                session = result['sessionId']
                assert result['capabilities']['moz:headless'] is False
                command('POST', '/window/rect', {'width': 1500, 'height': 1100})
                command('POST', '/url', {'url': args.url})
                wait('!!window.fireViewer?.map?.viewer?.scene?.globe')
                assert not js('return !!document.querySelector("#open-dem,#dem-file,#ground-altitude,#clear-dem")')
                select_file('#video-file', args.thermal)
                wait("window.fireViewer?.state?.decoded?.kind==='gray16'")
                wait('window.fireViewer.state.indexed')
                select_file('#log-file', args.log)
                wait('!!window.fireViewer.state.log')
                js('window.fireViewer.showFrame(arguments[0]);', [args.frame])
                wait(f'window.fireViewer.state.number==={args.frame} && !window.fireViewer.state.decoding')
                js('for(const [id,v] of [["pixel-x",arguments[0]],["pixel-y",arguments[1]]]){const e=document.getElementById(id);e.value=v;e.dispatchEvent(new Event("input",{bubbles:true}));}', [args.x, args.y])
                point = f'window.fireViewer.elevation.ground(window.fireViewer.pose,{args.x},{args.y},window.fireViewer.state.decoded.width,window.fireViewer.state.decoded.height)'
                wait(point + '!==null', 90)
                wait('document.getElementById("hover-reading").textContent.includes("(approx.)")')
                ground = js('return ' + point)
                js('window.fireViewer.markFinding();')
                wait('window.fireViewer.state.findings.length>0 && !document.getElementById("mark-finding").disabled')
                finding = js('return window.fireViewer.state.findings.at(-1)')
                assert finding['frameNumber'] == args.frame and finding['ground'] is not None
                for mode in ('3d', '2d'):
                    js('document.getElementById(arguments[0]).click();', ['map-' + mode])
                    time.sleep(1)
                    other = js('return ' + point)
                    assert abs(other['lat'] - ground['lat']) < 1e-10
                    assert abs(other['lon'] - ground['lon']) < 1e-10
                    (args.output / f'firefox-{mode}.png').write_bytes(base64.b64decode(command('GET', '/screenshot')))
                navigation = []
                zoom = []
                if args.navigation:
                    assert js('return document.getElementById("map-follow").checked && window.fireViewer.map.follow')
                    js('const e=document.getElementById("map-follow");e.checked=false;e.dispatchEvent(new Event("change",{bubbles:true}));')
                    for mode in ('2d', '3d'):
                        js('document.getElementById(arguments[0]).click();window.fireViewer.map.centerCurrent();', ['map-'+mode])
                        time.sleep(1)
                        measure_zoom = '''const m=window.fireViewer.map,c=m.viewer.camera,s=m.viewer.scene;
                            if(c.frustum.right!==undefined)return Math.max(c.frustum.right-c.frustum.left,c.frustum.top-c.frustum.bottom);
                            const q={x:s.canvas.clientWidth/2,y:s.canvas.clientHeight/2},ray=c.getPickRay(q);
                            const p=s.globe.pick(ray,s)||c.pickEllipsoid(q,s.globe.ellipsoid);
                            return p?Math.hypot(c.positionWC.x-p.x,c.positionWC.y-p.y,c.positionWC.z-p.z):c.positionCartographic.height;'''
                        initial_zoom = js(measure_zoom)
                        js('document.getElementById("map-zoom-in").click();')
                        button_in = js(measure_zoom)
                        assert button_in < initial_zoom
                        js('document.getElementById("map-zoom-out").click();document.getElementById("map").focus();')
                        button_out = js(measure_zoom)
                        assert button_out > button_in
                        for key in ('=', '-'):
                            command('POST', '/actions', {'actions':[{'type':'key','id':'keyboard','actions':[
                                {'type':'keyDown','value':key},{'type':'keyUp','value':key}]}]})
                            if key == '=': key_in = js(measure_zoom)
                            else: key_out = js(measure_zoom)
                        assert key_in < button_out and key_out > key_in
                        zoom.append({'mode':mode,'before':initial_zoom,'buttonIn':button_in,'buttonOut':button_out,'keyIn':key_in,'keyOut':key_out})
                        # Track depth picking must use the framebuffer rendered
                        # with the new camera, rather than the pre-zoom depth.
                        command('POST', '/execute/async', {'script': '''
                            const done=arguments[arguments.length-1],s=window.fireViewer.map.viewer.scene;
                            const remove=s.postRender.addEventListener(()=>{remove();done(true);});
                            s.requestRender();''', 'args': []})
                        target = js("""
                            const {map,state}=window.fireViewer,scene=map.viewer.scene;
                            const candidates=map.videoTrack.filter(p=>p.pts-state.frame.pts>5 && p.pts-state.frame.pts<25);
                            const rect=scene.canvas.getBoundingClientRect();
                            for(const p of candidates){
                              const q=scene.cartesianToCanvasCoordinates(p.world);
                              if(!q||q.x<20||q.y<20||q.x>rect.width-20||q.y>rect.height-20)continue;
                              const x=Math.round(rect.left+q.x),y=Math.round(rect.top+q.y)+2;
                              const hit=map.pickVideoTrack({x:x-rect.left,y:y-rect.top});
                              if(!hit)continue;
                              let index=0;
                              for(let i=1;i<state.frames.length;i++)if(Math.abs(state.frames[i].pts-hit.pts)<Math.abs(state.frames[index].pts-hit.pts))index=i;
                              return {x,y,index,pts:hit.pts};
                            }
                            return null;
                            """)
                        assert target, 'No visible seekable video track point in '+mode
                        command('POST', '/actions', {'actions': [{'type':'pointer','id':'mouse','parameters':{'pointerType':'mouse'},'actions':[
                            {'type':'pointerMove','duration':0,'x':target['x'],'y':target['y'],'origin':'viewport'},
                            {'type':'pointerDown','button':0},{'type':'pointerUp','button':0}]}]})
                        wait(f'window.fireViewer.state.number==={target["index"]} && !window.fireViewer.state.decoding')
                        assert js('return document.getElementById("play").textContent') == 'Play'
                        navigation.append({'mode':mode, 'selectedFrame':target['index'], 'selectedPts':target['pts']})
                        (args.output / f'firefox-navigation-{mode}.png').write_bytes(base64.b64decode(command('GET', '/screenshot')))
                        js('window.fireViewer.showFrame(arguments[0]);',[args.frame])
                        wait(f'window.fireViewer.state.number==={args.frame} && !window.fireViewer.state.decoding')
                    js('const e=document.getElementById("map-follow");e.checked=true;e.dispatchEvent(new Event("change",{bubbles:true}));')
                    centre=js('const m=window.fireViewer.map,s=m.viewer.scene,p=m.currentPosition,q=s.cartesianToCanvasCoordinates(m.position(p.lat,p.lon,p.altMsl));return {x:q.x,y:q.y,w:s.canvas.clientWidth,h:s.canvas.clientHeight};')
                    assert abs(centre['x']-centre['w']/2)<1 and abs(centre['y']-centre['h']/2)<1, centre
                cors_probe = command('POST', '/execute/async', {'script': """
                    const done=arguments[arguments.length-1];
                    const terrain=new URL(window.fireViewer.map.configuration.terrainUrl,location.href);
                    if(terrain.origin===location.origin){done({crossOrigin:false});return;}
                    fetch(new URL('layer.json',terrain),{headers:{'Content-Type':'application/json'}})
                      .then(async response=>({crossOrigin:true,status:response.status,format:(await response.json()).format}))
                      .then(done).catch(error=>done({error:String(error)}));
                    """, 'args': []})
                if cors_probe.get('crossOrigin'):
                    assert cors_probe.get('status') == 200 and cors_probe.get('format') == 'quantized-mesh-1.0', cors_probe
                assert 'error' not in cors_probe, cors_probe
                report = {'browser': result['capabilities'], 'ground': ground, 'finding': finding,
                          'cors': cors_probe, 'navigation': navigation, 'zoom': zoom,
                          'hover': js('return document.getElementById("hover-reading").textContent'),
                          'missionItems': js('return window.fireViewer.state.log.mission?.items?.length')}
                (args.output / 'qualification.json').write_text(json.dumps(report, indent=2) + '\n')
                print(json.dumps({'browser': result['capabilities']['browserVersion'],
                                  'ground': ground, 'missionItems': report['missionItems']}, indent=2))
            finally:
                if session:
                    try:
                        command('DELETE', '')
                    except OSError:
                        pass
                driver.terminate()
                try:
                    driver.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    driver.kill()
                    driver.wait()


if __name__ == '__main__':
    main()
