#!/usr/bin/env python3
"""Shared recorder: FAT-only IDR rollover, telemetry continuity and large offsets."""
from pathlib import Path
import json, os, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]
BINARY = ROOT/'camera_app/tests/test_mp4_rollover'
def run(*args, **kw):
    return subprocess.run(list(map(str,args)),check=True,capture_output=True,text=True,**kw)
with tempfile.TemporaryDirectory(prefix='mp4-rollover-') as temp:
    root=Path(temp); fixture=root/'input.h264'
    run('ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=320x240:rate=10','-t','12',
        '-c:v','libx264','-x264-params','aud=1:bframes=0:slices=4:keyint=10:min-keyint=10:scenecut=0',
        '-pix_fmt','yuv420p','-f','h264',fixture)
    env=dict(os.environ,CAMERA_APP_RECORD_SEGMENT_BYTES='65536')
    for name,fs in [('fat',0x4d44),('exfat',0x2011bab0),('ext4',0xef53),('fuse',0x65735546)]:
        path=root/name;path.mkdir();video=path/'video.mp4'
        result=run(BINARY,fixture,video,fs,'normal',env=env)
        expected=int(result.stdout.strip().split('=')[1]); assert expected==120
        files=sorted(path.glob('*.mp4'))
        assert (len(files)>1)==(name=='fat'), (name,files)
        rows=[]
        for file in files:
            probe=json.loads(run('ffprobe','-v','error','-select_streams','v:0','-show_frames',
                '-show_entries','frame=key_frame,width,height','-of','json',file).stdout)['frames']
            assert probe[0]['key_frame']==1
            assert all((p['width'],p['height'])==(320,240) for p in probe)
            assert not run('ffmpeg','-v','error','-i',file,'-f','null','-').stderr
            meta=file.with_suffix('.jsonl')
            run('python3',ROOT/'tools/video_telemetry.py',file,'--output',meta)
            records=[json.loads(s) for s in meta.read_text().splitlines()]
            assert len(records)==len(probe)
            assert [r['pts90k'] for r in records]==[i*9000 for i in range(len(records))]
            assert all(r['vehicle_attitude'] and r['gimbal_attitude'] and abs(r['hfov_deg']-54.7)<.001 for r in records)
            rows+=records
        assert len(rows)==expected
        assert [r['position']['lat_e7'] for r in rows]==[-353632610+i for i in range(expected)]
        print(f'PASS {name}: {len(files)} files, {len(rows)} frames, decoder headers and continuous telemetry')
    for name,fs in [('fat',0x4d44),('exfat',0x2011bab0),('ext4',0xef53),('fuse',0x65735546)]:
        path=root/('misb-'+name);path.mkdir();video=path/'video.ts'
        result=run(BINARY,fixture,video,fs,'normal','misb',env=env)
        expected=int(result.stdout.strip().split('=')[1]); assert expected==120
        files=sorted(path.glob('*.ts'))
        assert (len(files)>1)==(name=='fat'), (name,files)
        count=0
        for file in files:
            frames=json.loads(run('ffprobe','-v','error','-select_streams','v:0','-show_frames',
                '-show_entries','frame=key_frame,width,height','-of','json',file).stdout)['frames']
            assert frames[0]['key_frame']==1
            assert all((p['width'],p['height'])==(320,240) for p in frames)
            assert not run('ffmpeg','-v','error','-i',file,'-map','0:v:0','-f','null','-').stderr
            packets=json.loads(run('ffprobe','-v','error','-show_packets',
                '-show_entries','packet=codec_type,pts','-of','json',file).stdout)['packets']
            vpts=[p['pts'] for p in packets if p['codec_type']=='video']
            kpts=[p['pts'] for p in packets if p['codec_type']=='data']
            assert vpts==kpts==list(range(0,len(frames)*9000,9000))
            count+=len(frames)
        assert count==expected
        print(f'PASS MISB {name}: {len(files)} files, {count} frames, cached decoder headers and synchronous KLV')
    collision=root/'collision';collision.mkdir()
    existing=collision/'video_part0002.mp4';existing.write_bytes(b'preserve existing recording')
    run(BINARY,fixture,collision/'video.mp4',0x4d44,'collision',env=env)
    assert existing.read_bytes()==b'preserve existing recording'
    existing_ts=collision/'video_part0002.ts';existing_ts.write_bytes(b'preserve MISB recording')
    run(BINARY,fixture,collision/'video.ts',0x4d44,'collision','misb',env=env)
    assert existing_ts.read_bytes()==b'preserve MISB recording'
    run(BINARY,fixture,root/'fs-error.mp4',-1,'normal',env=env)
    assert not (root/'fs-error.mp4').exists()
    print('PASS collision and filesystem detection errors preserve recordings')
    if '--large' in __import__('sys').argv:
        env.pop('CAMERA_APP_RECORD_SEGMENT_BYTES')
        for name,fs,mode in [('fat-large',0x4d44,'large'),('exfat-large',0x2011bab0,'large'),('fat-no-idr',0x4d44,'no-idr')]:
            path=root/name;path.mkdir()
            run(BINARY,fixture,path/'video.mp4',fs,mode,env=env)
            sizes=[p.stat().st_size for p in sorted(path.glob('*.mp4'))]
            if name=='fat-large':
                assert len(sizes)==2 and (4*1024**3-64*1024**2)<=sizes[0]<2**32
            elif name=='exfat-large': assert len(sizes)==1 and sizes[0]>2**32
            else: assert len(sizes)==1 and sizes[0]<2**32
            print(f'PASS {name}: sparse file sizes {sizes}')
