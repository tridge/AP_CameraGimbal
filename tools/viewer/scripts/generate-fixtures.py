#!/usr/bin/env python3
"""Generate small local files for visible-browser qualification; no flight files.

Needs numpy, PyAV (including add_data_stream), and ffmpeg.
All outputs are ignored generated fixtures; no camera build is required.
"""
import argparse
from fractions import Fraction
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess

import av
import numpy as np

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = Path(__file__).resolve().parents[1]/'tests/browser/fixtures'
VERSION = 7
UTC = 1787882912367000
UUID = '8d646b4e556f4a908b7c35e629510321'


def telemetry(frame=0):
    return dict(schema='apcg.telemetry.v1', pts90k=frame*18000, utc_us=UTC+frame*200000,
                position=dict(lat_e7=-353219444, lon_e7=1488380556, alt_amsl_m=1400,
                              alt_relative_m=600, age_ms=0),
                vehicle_attitude=dict(roll_rad=0, pitch_rad=0, yaw_rad=0, age_ms=0),
                gimbal_attitude=dict(roll_rad=0, pitch_rad=-np.pi/2, yaw_rad=0, age_ms=0),
                hfov_deg=60, heading_rad=0,
                named_values=dict(TMIN=dict(value=1.5, valid=True, age_ms=100),
                                  TMAX=dict(value=110.85, valid=True, age_ms=100)))


def thermal(path, *, calibrated=True, utc_known=True, mixed_calibration=False):
    spec=importlib.util.spec_from_file_location('thermal_archive_fixture', ROOT/'tools/thermal_to_video.py')
    archive=importlib.util.module_from_spec(spec);spec.loader.exec_module(archive)
    codec=av.CodecContext.create('ffv1','w')
    codec.width,codec.height,codec.pix_fmt=640,512,'gray16le'
    codec.time_base=Fraction(1,1000);codec.gop_size=1;codec.thread_count=2
    codec.flags|=av.codec.context.Flags.global_header
    codec.options={'level':'3','coder':'1','slicecrc':'1'};codec.open()
    yy,xx=np.mgrid[:512,:640]
    with path.open('wb') as out:
        out.write(archive.matroska_header(codec.extradata,200000000,2400))
        for number in range(12):
            pixels=(17536+xx*2+yy+number*64).astype('<u2')
            pixels[255:258,319+number*2:322+number*2]=24576+number*64
            data=pixels.tobytes()
            frame=av.VideoFrame.from_ndarray(pixels,format='gray16le')
            frame.pts=number*200;frame.time_base=codec.time_base
            packets=codec.encode(frame)
            assert len(packets)==1 and packets[0].is_keyframe
            meta=dict(schema='apcg.thermal.v1',frame_id=number+1,width=640,height=512,
                      pixel_format='gray16le',bits_per_sample=16,temperature_scale_k=1/64,
                      temperature_offset_k=0,rotation_deg=180,hfov_deg=24.2,
                      minimum_c=int(pixels.min())/64-273.15,maximum_c=int(pixels.max())/64-273.15,
                      original_filename=f'fixture-{number}.bin',archive_frame_count=12,
                      source_sha256=hashlib.sha256(data).hexdigest(),telemetry=telemetry(number))
            if not calibrated or (mixed_calibration and number>0):
                meta.pop('temperature_scale_k');meta.pop('temperature_offset_k')
            if not utc_known:
                meta['telemetry']['utc_us']=None
            out.write(archive.matroska_frame(bytes(packets[0]),meta,number*200))
        assert not codec.encode(None)


SIZES={'Q':8,'B':1,'H':2,'I':4,'f':4,'L':4,'e':4,'Z':64}


def fmt(number,name,format_,columns):
    length=3+sum(SIZES[c] for c in format_)
    return struct.pack('<BBBBB4s16s64s',0xa3,0x95,128,number,length,name.encode(),format_.encode(),columns.encode())


def data(number,format_,values):
    out=bytearray([0xa3,0x95,number])
    for c,value in zip(format_,values):
        if c=='L':out.extend(struct.pack('<i',round(value*1e7)))
        elif c=='e':out.extend(struct.pack('<i',round(value*100)))
        elif c=='Z':out.extend(str(value).encode()[:64].ljust(64,b'\0'))
        else:out.extend(struct.pack('<'+c,value))
    return out


def log(path):
    rows=[fmt(129,'CMD','QHHHffffLLfB','TimeUS,CTot,CNum,CId,Prm1,Prm2,Prm3,Prm4,Lat,Lng,Alt,Frame'),
          fmt(130,'MSG','QZ','TimeUS,Message'),fmt(131,'FNCE','QBBBLLBf','TimeUS,Tot,Seq,Type,Lat,Lng,Count,Radius'),
          fmt(132,'POS','QLLfff','TimeUS,Lat,Lng,Alt,RelHomeAlt,RelOriginAlt'),
          fmt(133,'ORGN','QBLLe','TimeUS,Type,Lat,Lng,Alt'),
          fmt(134,'GPS','QBBIHBLLe','TimeUS,I,Status,GMS,GWk,NSats,Lat,Lng,Alt')]
    rows+=[data(133,'QBLLe',[1,1,-35.3219,148.838,800]),data(130,'QZ',[2,'New mission'])]
    for seq,frame in enumerate([0,3,6,10]):
        rows.append(data(129,'QHHHffffLLfB',[1000000+seq,4,seq,16,0,0,0,0,
                                        -35.3219+seq*.0002,148.838+seq*.0002,1100 if frame==0 else 80,frame]))
    rows.append(data(130,'QZ',[2000000,'New fence']))
    for seq in range(8):
        kind=98 if seq<3 else 97 if seq<6 else 92 if seq==6 else 93
        rows.append(data(131,'QBBBLLBf',[2000010+seq,8,seq,kind,
                                       -35.322+(seq%3)*.0003,148.838+(seq%2)*.0003,
                                       3 if seq<6 else 1,60 if seq==6 else 20 if seq==7 else 0]))
    rows.append(data(134,'QBBIHBLLe',[3000000,0,3,100000,2433,10,-35.3219,148.838,1400]))
    for i in range(10):
        rows.append(data(132,'QLLfff',[3000000+i*200000,-35.3219+i*.00001,148.838,1400,600,600]))
    path.write_bytes(b''.join(rows))


def reference(path, output):
    """Independent PyAV raw-frame checks; never copy the source into fixtures."""
    identity = dict(path=str(path.resolve()), size=path.stat().st_size,
                    mtime_ns=path.stat().st_mtime_ns)
    if output.is_file():
        prior=json.loads(output.read_text())
        if prior.get('source')==identity:
            return
    with av.open(str(path)) as container:
        stream=container.streams.video[0]
        count=sum(1 for packet in container.demux(stream) if packet.size)
    wanted={0,count//2,count-1};frames=[]
    with av.open(str(path)) as container:
        stream=container.streams.video[0]
        for number,packet in enumerate(p for p in container.demux(stream) if p.size):
            if number not in wanted:
                continue
            decoded=packet.decode()
            if len(decoded)!=1:
                raise RuntimeError('Reference expects independently decodable FFV1 frames')
            pixels=decoded[0].to_ndarray(format='gray16le').astype('<u2',copy=False)
            frames.append(dict(number=number,pts=float(packet.pts*packet.time_base),
                               raw_sha256=hashlib.sha256(pixels.tobytes()).hexdigest(),
                               sample=dict(x=20,y=30,value=int(pixels[30,20])),
                               width=pixels.shape[1],height=pixels.shape[0]))
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(dict(source=identity,count=count,frames=frames),indent=2)+'\n')


def misb_fixture(source, destination):
    """Mux real H.264 with an independent ST0601 golden vector using FFmpeg.

    The vector is also checked against authoritative decoded values in the JS
    telemetry tests. Keep this independent of the optional camera MISB writer so
    the viewer and camera-format changes can be built and reviewed separately.
    """
    golden=bytes.fromhex(
        '060e2b34020b01010e0103010100000069020800065f1ab5f649f10b044d543131'
        '0c0e47656f64657469632057475338340d04cdd211ec0e0469f59be40f021311'
        '0502071c06020000070200005a04000000005b04000000001002226b1102138f'
        '1204000000001304eaaaaaa114040000000041010801020fc2')
    with av.open(str(source)) as capture, av.open(str(destination), 'w', format='mpegts') as output:
        video=output.add_stream_from_template(capture.streams.video[0])
        metadata=output.add_data_stream('klv')
        metadata.time_base=Fraction(1,90000)
        for number,packet in enumerate(p for p in capture.demux(video=0) if p.size):
            packet.stream=video
            packet.time_base=Fraction(1,90000)
            packet.pts=packet.dts=number*9000
            packet.duration=9000
            output.mux(packet)
            record=bytearray(golden)
            record[19:27]=struct.pack('>Q',UTC+number*100000)
            checksum=sum(value*(256 if i%2==0 else 1) for i,value in enumerate(record[:-2]))&65535
            record[-2:]=struct.pack('>H',checksum)
            klv=av.Packet(record)
            klv.stream=metadata
            klv.time_base=Fraction(1,90000)
            klv.pts=klv.dts=number*9000
            output.mux(klv)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,default=OUTPUT)
    parser.add_argument('--force',action='store_true')
    parser.add_argument('--reference',type=Path,help='Generate ignored independent first/middle/last thermal reference')
    args=parser.parse_args();out=args.output
    if args.reference:
        reference(args.reference,out/'real-reference.json');return
    manifest=out/'manifest.json'
    if not args.force and manifest.is_file() and json.loads(manifest.read_text()).get('generator_version')==VERSION and all((out/name).is_file() for name in ['thermal.mkv','mission.bin','rgb.mp4','misb.ts','bad.mkv','uncalibrated.mkv','unknown-time.mkv','partial.mkv','mixed-calibration.mkv','trimmed.mp4','mid-gop.ts','rgb.mkv']):
        print('Browser fixtures already generated:',out);return
    out.mkdir(parents=True,exist_ok=True)
    thermal(out/'thermal.mkv');log(out/'mission.bin')
    thermal(out/'uncalibrated.mkv',calibrated=False)
    thermal(out/'unknown-time.mkv',utc_known=False)
    thermal(out/'mixed-calibration.mkv',mixed_calibration=True)
    # Complete raw frames remain reviewable, but the trailing Cluster ID has no
    # EBML size: indexing must stop with a visible error, not pretend completion.
    (out/'partial.mkv').write_bytes((out/'thermal.mkv').read_bytes()+bytes.fromhex('1f43b675'))
    text=json.dumps(telemetry(),separators=(',',':')).replace(',',r'\,').replace(':',r'\:')
    bsf="h264_metadata=sei_user_data='"+UUID+'+'+text+"'"
    subprocess.run(['ffmpeg','-y','-v','error','-f','lavfi','-i','testsrc2=size=320x240:rate=10',
                    '-t','2','-c:v','libx264','-threads','1','-bf','0','-g','10','-pix_fmt','yuv420p',
                    '-bsf:v',bsf,str(out/'rgb.mp4')],check=True)
    subprocess.run(['ffmpeg','-y','-v','error','-f','lavfi','-i','testsrc2=size=320x240:rate=10',
                    '-t','2','-c:v','libx264','-threads','1','-x264-params','aud=1:bframes=0:keyint=10:min-keyint=10:scenecut=0',
                    '-pix_fmt','yuv420p','-f','h264',str(out/'source.h264')],check=True)
    misb_fixture(out/'source.h264', out/'misb.ts')
    subprocess.run(['ffmpeg','-y','-v','error','-ss','0.7','-i',str(out/'rgb.mp4'),
                    '-t','1','-c','copy',str(out/'trimmed.mp4')],check=True)
    subprocess.run(['ffmpeg','-y','-v','error','-i',str(out/'rgb.mp4'),
                    '-c','copy',str(out/'rgb.mkv')],check=True)
    with av.open(str(out/'misb.ts')) as capture:
        video_pid = capture.streams.video[0].id
    # Preserve PSI/KLV, removing just the first video PES to emulate joining a
    # live stream mid-GOP. A later IDR still provides a decodable remainder.
    packets = (out/'misb.ts').read_bytes()
    starts = 0; kept = []
    for at in range(0, len(packets), 188):
        packet = packets[at:at+188]
        pid = ((packet[1]&31)<<8) | packet[2]
        if pid == video_pid:
            if packet[1]&64: starts += 1
            if starts == 1: continue
        kept.append(packet)
    (out/'mid-gop.ts').write_bytes(b''.join(kept))
    (out/'bad.mkv').write_bytes(b'Not a Matroska recording\0\x1f\x43\xb6\x75')
    expected=dict(generator_version=VERSION,thermal=dict(file='thermal.mkv',width=640,height=512,frames=12,
                  utc_us=UTC,frame_interval_s=.2,background_formula='17536+2*x+y+64*frame_index',
                  peak_xy=[320,256],peak_raw_first=24576,scale_k=1/64,offset_k=0,metadata_rotation=180),
                  rgb=dict(file='rgb.mp4',frames=20),misb=dict(file='misb.ts'),
                  log=dict(file='mission.bin',mission_items=4,fences=4,mission_frames=[0,3,6,10]))
    manifest.write_text(json.dumps(expected,indent=2)+'\n')
    print('Generated browser fixtures:',out)


if __name__=='__main__':main()
