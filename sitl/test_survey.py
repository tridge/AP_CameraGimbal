#!/usr/bin/env python3
"""Moving-aircraft survey, distinct raw bursts, stream/SD identity and stop tests."""
import argparse
import bisect
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
from types import SimpleNamespace
import subprocess
import sys
import threading
import time

from test_mavlink_parameters import survey_status_path, port, stop, wait_ready, connect
from pymavlink import mavutil
from pymavlink.quaternion import Quaternion

ROOT = Path(__file__).resolve().parents[1]
M = mavutil.mavlink


def run(output, speed, duration, mavproxy, terrain_dropout=False, lens="thermal", count=0, xml_mode=False, terrain_root=None, terrain_variation=False, replay=None, pattern="both", gentle_turn=False, no_stream=False, navigation=False, aim="rows"):
    sys.path.insert(0,str(mavproxy))
    spec=importlib.util.spec_from_file_location("survey_coverage_under_test",
        mavproxy/'MAVProxy/modules/mavproxy_camera/survey.py')
    survey_module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(survey_module)
    coverage=survey_module.SurveyCoverage(SimpleNamespace(mpstate=SimpleNamespace(map=None)))
    output.mkdir(parents=True, exist_ok=True)
    cfg = output / 'camera.ini'
    cfg.write_text(f'[mavlink]\nsystem_id=42\n[survey]\nlens={lens}\nburst=2\nburst_ms=80\ndwell_ms=500\n')
    track = json.loads(replay.read_text()) if replay else None
    track_times = [p['t'] for p in track] if track else []
    if track:
        cfg.write_text('[mavlink]\nsystem_id=42\n[survey]\nburst=2\nburst_ms=100\ndwell_ms=400\nsettle_ms=100\nfore_pct=25\naft_pct=25\n')
    with cfg.open('a') as f: f.write(f'pattern={pattern}\naim={aim}\n')
    gp, cp, raw = port(), port(), port()
    env = dict(os.environ, CAMERA_APP_CONFIG=str(cfg), CAMERA_APP_BACKEND='mt11',
               CAMERA_APP_SITL_PHOTO=str(ROOT/'camera_app/tests/photo.jpg'),
               CAMERA_APP_UART=f'udp://127.0.0.1:{gp}', CAMERA_APP_PORT=str(port()),
               CAMERA_APP_MAVLINK_TCP_PORT=str(cp), CAMERA_APP_MAVLINK_UDP_PORT='0',
               CAMERA_APP_RTSP_PORT=str(port()), CAMERA_APP_RAW_THERMAL_PORT=str(raw),
               CAMERA_APP_CAPTURE_ROOT=str(output/'capture'), CAMERA_APP_RECORD_ROOT=str(output/'record'),
               CAMERA_APP_READY_PATH=str(output/'camera.ready'))
    if terrain_root:
        env['CAMERA_APP_TERRAIN_ROOT'] = str(terrain_root.resolve())
    for key in ('CAMERA_APP_SITL_TERRAIN', 'CAMERA_APP_SITL_VIDEO1', 'CAMERA_APP_SITL_VIDEO2'):
        env.pop(key, None)
    for ready in output.glob('*.ready'):
        ready.unlink()
    captures, frames, acks, intervals, capabilities, mode_acks = [], {}, [], set(), [], []
    path_sources = {}
    path_states = {}
    camera = gimbal = link = None
    finished = threading.Event()
    errors = []

    def stream():
        sys.path.insert(0, str(mavproxy))
        from MAVProxy.modules.mavproxy_camera.thermal_stream import ThermalReader
        try:
            reader = ThermalReader(f'http://127.0.0.1:{raw}/thermal.mkv')
            try:
                for pixels, metadata in reader.frames():
                    if 'survey' in metadata:
                        frames[metadata['survey']['image_index']] = hashlib.sha256(pixels.astype('<u2').tobytes()).hexdigest()
                    if finished.is_set():
                        break
            finally:
                reader.close()
        except Exception as ex:
            errors.append(str(ex))

    with (output/'camera.log').open('w') as log:
        try:
            gimbal = subprocess.Popen([sys.executable, str(ROOT/'sitl/gimbal_sim.py'), '--port', str(gp),
                '--ready-file', str(output/'gimbal.ready')], stdout=log, stderr=subprocess.STDOUT)
            wait_ready(output/'gimbal.ready', gimbal)
            camera = subprocess.Popen([str(ROOT/'build/sitl/camera-app')], env=env, stdout=log, stderr=subprocess.STDOUT)
            wait_ready(output/'camera.ready', camera)
            link = connect(f'tcp:127.0.0.1:{cp}')
            thread = None
            if not no_stream:
                thread = threading.Thread(target=stream, daemon=True)
                thread.start()
            start = time.monotonic()
            dropout_count = None
            owned = False
            started = False
            last_heartbeat = last_terrain = 0
            pause_at = duration-5
            stopped_count = None
            while time.monotonic()-start < duration:
                elapsed = time.monotonic()-start
                lat = (-35.48 if terrain_root else -35.2785018) + math.degrees(speed*elapsed/6378137)
                lon = 148.9 if terrain_root else 148.9534632
                vn, ve, vd, yaw, roll, pitch = speed, 0, 0, 0, 0, 0
                altitude = 2100 if terrain_root else 984
                ground = 584 + (80*math.sin(elapsed*.05) if terrain_variation else 0)
                if gentle_turn:
                    # Start surveying northbound, then finish a gentle 5.4°
                    # turn at 0.54°/s, well below both existing turn thresholds.
                    rate=math.radians(.54)
                    turning=max(0,min(10,elapsed-10))
                    yaw=rate*turning
                    after=max(0,elapsed-20)
                    north=speed*(min(elapsed,10)+math.sin(yaw)/rate+after*math.cos(yaw))
                    east=speed*((1-math.cos(yaw))/rate+after*math.sin(yaw))
                    lat=-35.2785018+math.degrees(north/6378137)
                    lon=148.9534632+math.degrees(east/(6378137*math.cos(math.radians(-35.2785018))))
                    vn,ve=speed*math.cos(yaw),speed*math.sin(yaw)
                if track:
                    p = track[min(bisect.bisect_left(track_times, elapsed), len(track)-1)]
                    lat, lon, altitude, ground = p['lat'], p['lon'], p['alt'], p['ground']
                    vn, ve, vd, yaw, roll, pitch = (p[k] for k in ('vn', 've', 'vd', 'yaw', 'roll', 'pitch'))
                if navigation:
                    # The aircraft flies 80m east of a northbound mission leg.
                    # Recover the intended line, then exercise telemetry loss,
                    # loiter rejection, a new parallel leg and leaving AUTO.
                    east_offset = 80
                    if elapsed >= 75:
                        yaw = math.radians(20)
                        vn,ve = speed*math.cos(yaw),speed*math.sin(yaw)
                        east_offset += ve*(elapsed-75)
                        lat -= math.degrees((speed-vn)*(elapsed-75)/6378137)
                    lon += math.degrees(east_offset/(6378137*math.cos(math.radians(lat))))
                    nav_seq = 4 if elapsed < 50 else 6
                    nav_command = M.MAV_CMD_NAV_LOITER_UNLIM if 35 <= elapsed < 50 else M.MAV_CMD_NAV_WAYPOINT
                    nav_auto = elapsed < 65 or elapsed >= 75
                    line_east = 150 if elapsed >= 50 else 0
                    east_error = east_offset-line_east
                    north_distance = 5000-speed*elapsed
                    bearing = math.degrees(math.atan2(-east_error,north_distance))
                    if not 20 <= elapsed < 35:
                        link.mav.srcSystem, link.mav.srcComponent = 42, 1
                        link.mav.nav_controller_output_send(0,0,0,int(bearing),round(math.hypot(north_distance,east_error)),0,0,-east_error)
                link.mav.srcSystem, link.mav.srcComponent = 42, 1
                if elapsed-last_heartbeat > .5:
                    # Plane AUTO uses GUIDED and STABILIZE, never AUTO_ENABLED.
                    auto_flag = M.MAV_MODE_FLAG_GUIDED_ENABLED|M.MAV_MODE_FLAG_STABILIZE_ENABLED if navigation and nav_auto else 0
                    link.mav.heartbeat_send(M.MAV_TYPE_FIXED_WING, M.MAV_AUTOPILOT_ARDUPILOTMEGA,
                        128|auto_flag|M.MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, 10 if auto_flag else 0, 4)
                    if navigation:
                        link.mav.mission_current_send(nav_seq)
                        state_path = survey_status_path(cfg)
                        if state_path.exists():
                            path_status = json.loads(state_path.read_text())
                            path_sources[elapsed] = path_status['path_source']
                            path_states[elapsed] = path_status['state']
                    link.mav.gimbal_manager_status_send(int(elapsed*1000), 0, 154, 42 if owned else 0, 154 if owned else 0, 0, 0)
                    last_heartbeat = elapsed
                link.mav.global_position_int_send(int(elapsed*1000), round(lat*1e7), round(lon*1e7),
                                                   round(altitude*1000), round((altitude-ground)*1000), round(vn*100), round(ve*100), round(vd*100), round(math.degrees(yaw)%360*100))
                link.mav.autopilot_state_for_gimbal_device_send(42, 154, round(time.monotonic()*1e6),
                    Quaternion([roll, pitch, yaw]).q, 0, vn, ve, vd, 0, 0, 0, M.MAV_LANDED_STATE_IN_AIR)
                if elapsed-last_terrain > .5 and not (terrain_dropout and 30 < elapsed < 37):
                    link.mav.terrain_report_send(round(lat*1e7), round(lon*1e7), 100, ground, altitude-ground, 0, 10)
                    last_terrain = elapsed
                if not started and elapsed > 1:
                    link.mav.srcSystem, link.mav.srcComponent = 255, 190
                    link.mav.command_long_send(42,100,M.MAV_CMD_REQUEST_MESSAGE,0,M.MAVLINK_MSG_ID_CAMERA_INFORMATION,0,0,0,0,0,0)
                    if xml_mode:
                        link.mav.param_ext_set_send(42,100,b'CAM_MODE',(2).to_bytes(4,'little'),M.MAV_PARAM_EXT_TYPE_INT32)
                    else:
                        link.mav.command_long_send(42, 100, M.MAV_CMD_SET_CAMERA_MODE, 0, 0, 2, 0, 0, 0, 0, 0)
                    if count:
                        link.mav.command_long_send(42,100,M.MAV_CMD_IMAGE_START_CAPTURE,0,0,0,count,0,0,0,0)
                    started = True
                if elapsed > pause_at and stopped_count is None:
                    link.mav.srcSystem, link.mav.srcComponent = 255, 190
                    link.mav.command_long_send(42, 100, M.MAV_CMD_IMAGE_STOP_CAPTURE, 0, 0, 0, 0, 0, 0, 0, 0)
                    stopped_count = len(captures)
                if terrain_dropout and not terrain_root and 34 < elapsed < 37:
                    if dropout_count is None:
                        dropout_count = len(captures)
                    assert len(captures) == dropout_count, 'captures continued with stale terrain'
                    status = json.loads(survey_status_path(cfg).read_text())
                    assert 'terrain' in status['state'], status
                until = time.monotonic()+.05
                while time.monotonic() < until:
                    msg = link.recv_match(blocking=True, timeout=.005)
                    if msg is None:
                        continue
                    coverage.packet(msg)
                    if msg.get_type() == 'COMMAND_LONG':
                        if msg.command == M.MAV_CMD_DO_GIMBAL_MANAGER_CONFIGURE:
                            if msg.param1 == -2:
                                owned = True
                            elif msg.param1 == -3:
                                owned = False
                        if msg.command == M.MAV_CMD_SET_MESSAGE_INTERVAL:
                            intervals.add(int(msg.param1))
                    elif msg.get_type() == 'MISSION_REQUEST_INT' and navigation:
                        link.mav.srcSystem, link.mav.srcComponent = 42, 1
                        link.mav.mission_item_int_send(msg.get_srcSystem(),msg.get_srcComponent(),msg.seq,
                            M.MAV_FRAME_GLOBAL_RELATIVE_ALT_INT,nav_command,1,1,0,0,0,0,
                            round((-35.2785018+math.degrees(5000/6378137))*1e7),round(148.9534632*1e7),400)
                    elif msg.get_type() == 'CAMERA_IMAGE_CAPTURED':
                        captures.append(msg.to_dict())
                    elif msg.get_type() == 'CAMERA_INFORMATION':
                        capabilities.append(msg.flags)
                    elif msg.get_type() == 'PARAM_EXT_ACK':
                        mode_acks.append(msg)
                    elif msg.get_type() == 'COMMAND_ACK':
                        acks.append(msg.to_dict())
            if terrain_dropout and not terrain_root:
                assert dropout_count is not None
            finished.set()
            if thread:
                thread.join(timeout=3)
            (output/'captures.json').write_text(json.dumps(captures, indent=2))
            capture_glob='*.bin.json' if lens=='thermal' else '*.jpg.json'
            records = [json.loads(p.read_text()) for p in (output/'capture').rglob(capture_glob)]
            assert not errors, errors
            assert len(coverage.images)==sum(c['capture_result']==1 for c in captures), 'captured footprint pairing failed'
            assert M.MAVLINK_MSG_ID_TERRAIN_REPORT in intervals, intervals
            assert any(f & M.CAMERA_CAP_FLAGS_HAS_IMAGE_SURVEY_MODE for f in capabilities), capabilities
            if xml_mode:
                assert any(m.param_id=='CAM_MODE' and m.param_result==M.PARAM_ACK_ACCEPTED for m in mode_acks), mode_acks
            else:
                assert any(a['command'] == M.MAV_CMD_SET_CAMERA_MODE and a['result'] == M.MAV_RESULT_ACCEPTED for a in acks), acks
            assert len(records) >= (count or 9), (len(records), survey_status_path(cfg).read_text())
            assert len(captures) <= stopped_count+1, 'capture continued after stop'
            assert len({r['capture_monotonic_ms'] for r in records}) == len(records), 'duplicate frames'
            if count:
                assert len(records)==count, (len(records),count)
            if not terrain_dropout and not count and not navigation and aim=='rows':
                cycles=sorted({r['cycle'] for r in records})
                assert cycles == list(range(cycles[0],cycles[-1]+1)), ('skipped ground rows',cycles)
                views={}
                for r in records:
                    views.setdefault((r['leg'],r['row'],r['column']),set()).add(r['slot'] if pattern=='fore_only' else r['slot']//3)
                if pattern=='both':
                    assert any(len(v)==3 for v in views.values()), 'no target captured from front, middle and rear'
                elif pattern in ('port','starboard'):
                    assert any(len(v)==3 for v in views.values()), 'no side target captured from front, abeam and rear'
                elif pattern=='fore_aft':
                    assert any(v=={0,2} for v in views.values()), 'no front/rear revisit'
                elif pattern=='fore_wide':
                    sweeps={}
                    for r in records:
                        sweeps.setdefault((r['leg'],r['cycle'],r['row']),set()).add(r['column'])
                    assert any(c=={-1,0,1} for c in sweeps.values()), 'no left/centre/right sweep on one row'
                elif pattern=='fore_only':
                    assert any(v=={9,10,11} for v in views.values()), 'no far/middle/near revisit'
            allowed={'both':set(range(9)), 'left_right':{5,3}, 'fore_aft':{1,7}, 'fore_only':{9,10,11}, 'fore_wide':{0,1,2}, 'port':{0,5,6}, 'starboard':{2,3,8}}[pattern]
            assert {r['slot'] for r in records} == allowed, ('unexpected views',pattern)
            assert all(r['positions']==len(allowed) and 0<=r['position']<len(allowed) for r in records)
            assert all(r['pattern']=={'both':0,'left_right':1,'fore_aft':2,'fore_only':3,'fore_wide':4,'port':5,'starboard':6}[pattern] for r in records)
            if aim=='ahead' and not replay and not gentle_turn and not navigation:
                # Straight north: each frame's target must sit its configured
                # distance ahead of (or behind) the aircraft at capture.
                fore=aft=.5
                errors_m=[]
                for r in records:
                    agl=r['alt_amsl_m']-r['ground_amsl_m']
                    s=r['slot']
                    expect=(12-s)*fore*agl if s>=9 else fore*agl if s<3 else 0 if s<6 else -aft*agl
                    errors_m.append(abs((r['target'][0]-r['lat'])*111319.5-expect))
                errors_m.sort()
                median=errors_m[len(errors_m)//2]
                print(f'Ahead aiming: median along-track error {median:.1f}m, worst {errors_m[-1]:.1f}m')
                assert median<max(5,.1*agl), ('ahead target not at configured distance at capture',median)
            if pattern=='fore_only' and aim=='rows':
                assert all(r['row']>r['cycle'] and r['column']==0 for r in records), 'invalid forward grid'
                if not replay and not gentle_turn and not navigation:
                    assert all(r['target'][0]>r['lat'] and abs(r['target'][1]-r['lon'])<1e-6 for r in records), 'target passed behind aircraft'
                    assert all(abs(Quaternion(c['q']).euler[2])<math.radians(1) for c in captures if c['capture_result']==1), 'fore-only yawed away from flight path'
            bursts={}
            for r in records:
                key=(r['leg'],r['cycle'],r['slot'])
                assert bursts.setdefault(key,r['target'])==r['target'], 'burst target moved'
            if gentle_turn:
                assert len({r['leg'] for r in records})>1, 'grid failed to realign after gentle turn'
                cross=[]
                for r in records:
                    t=r['capture_monotonic_ms']*.001-start
                    heading=math.radians(.54)*max(0,min(10,t-10))
                    north=(r['target'][0]-r['lat'])*111319.5
                    east=(r['target'][1]-r['lon'])*111319.5*math.cos(math.radians(r['lat']))
                    cross.append(abs(-north*math.sin(heading)+east*math.cos(heading)))
                assert max(cross)<50, ('target drifted outside flight swath',max(cross))
                print(f'Gentle turn: max target cross-track {max(cross):.1f}m; burst targets fixed')
            if navigation:
                assert M.MAVLINK_MSG_ID_NAV_CONTROLLER_OUTPUT in intervals
                assert M.MAVLINK_MSG_ID_MISSION_CURRENT in intervals
                for lo,hi,source,east in ((8,19,'navigation',0),(26,34,'ground track',80),
                        (41,49,'ground track',80),(57,64,'navigation',150),(71,74,'ground track',80)):
                    observed = [v for t,v in path_sources.items() if lo<t<hi]
                    assert observed and all(v==source for v in observed), (lo,source,observed)
                    selected = [r for r in records if lo<r['capture_monotonic_ms']*.001-start<hi]
                    assert selected, ('no captures in navigation phase',lo)
                    errors_m = [(r['target'][1]-148.9534632)*111319.5*math.cos(math.radians(r['lat']))-east for r in selected]
                    assert max(abs(e) for e in errors_m)<20, (lo,errors_m)
                observed = [v for t,v in path_states.items() if 82<t<89]
                assert observed and all(v=='waiting for straight flight' for v in observed), observed
                assert not any(82<r['capture_monotonic_ms']*.001-start<89 for r in records), 'captured during off-course departure'
                print('PASS navigation centreline, stale fallback, in-place loiter edit, parallel leg change, AUTO exit and off-course suspension')
            status=json.loads(survey_status_path(cfg).read_text())
            assert status['positions']==len(allowed), status

            if terrain_root or terrain_variation:
                assert len({r['leg'] for r in records})==1, 'terrain caused a grid restart'
            if terrain_root:
                assert all(abs(r['target'][2]-584)>5 for r in records), 'FC report used instead of uploaded terrain'
            for p in (output/'capture').rglob(capture_glob):
                record = json.loads(p.read_text())
                assert record['footprint'] is not None, record
                if lens!='thermal':
                    assert Path(str(p)[:-5]).read_bytes().startswith(b'\xff\xd8')
                    continue
                pixels = Path(str(p)[:-5]).read_bytes()
                assert len(pixels) == 640*512*2, 'incomplete raw thermal image'
                if not no_stream:
                    assert record['image_index'] in frames, ('survey frame missing from stream', record['image_index'])
                    assert hashlib.sha256(pixels).hexdigest() == frames[record['image_index']]
            validation = 'SD capture without a stream client' if no_stream else 'identical stream/SD pixels'
            print(f'PASS {speed} m/s {lens} {pattern} aim={aim}: {len(records)} unique survey frames; stop, count, terrain subscription; ' + (validation if lens=='thermal' else 'selected-lens JPEGs'))
        finally:
            finished.set()
            if link:
                link.close()
            stop(camera)
            stop(gimbal)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/survey-test')
    parser.add_argument('--speed', type=float, default=25)
    parser.add_argument('--duration', type=float, default=80)
    parser.add_argument('--mavproxy', type=Path, required=True)
    parser.add_argument("--terrain-dropout", action="store_true")
    parser.add_argument("--lens", choices=("thermal","wide","zoom"), default="thermal")
    parser.add_argument("--count", type=int, default=0)
    parser.add_argument("--xml-mode", action="store_true")
    parser.add_argument('--terrain-root', type=Path)
    parser.add_argument('--terrain-variation', action='store_true')
    parser.add_argument('--replay', type=Path)
    parser.add_argument('--pattern', choices=('both','left_right','fore_aft','fore_only','fore_wide','port','starboard'), default='both')
    parser.add_argument('--aim', choices=('rows','ahead'), default='rows')
    parser.add_argument('--gentle-turn', action='store_true', help='regress grid drift after a gentle 5.4-degree turn')
    parser.add_argument('--no-stream', action='store_true', help='verify SD capture with no thermal stream consumer')
    parser.add_argument('--navigation', action='store_true', help='exercise waypoint-path telemetry and fallbacks for 95 seconds')
    args = parser.parse_args()
    if args.gentle_turn and (args.replay or args.terrain_root or args.pattern not in ('fore_only','fore_aft')):
        parser.error('--gentle-turn requires fore_only/fore_aft and no replay/uploaded terrain')
    if args.navigation and (args.pattern!='fore_only' or args.duration<95 or args.count or args.gentle_turn or args.replay or args.terrain_root or args.terrain_dropout):
        parser.error('--navigation requires fore_only, duration >=95 and no count/turn/replay/terrain overrides')
    run(args.output, args.speed, args.duration, args.mavproxy, args.terrain_dropout, args.lens, args.count, args.xml_mode, args.terrain_root, args.terrain_variation, args.replay, args.pattern, args.gentle_turn, args.no_stream, args.navigation, args.aim)
