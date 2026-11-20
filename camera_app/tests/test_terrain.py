#!/usr/bin/env python3
"""Known sloping grid, AP block boundaries, CRC/bitmap rejection and cache reload."""
import binascii
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time

M = .011131884502145034

def block(lat, lon, x, y, base=100, bitmap=(1<<56)-1):
    spacing = 30
    b = bytearray(2048)
    dlat = int(x*24*spacing/M)
    glat = lat*10000000+dlat
    glon = lon*10000000+int(y*28*spacing/M/math.cos(math.radians(lat+dlat/2e7)))
    struct.pack_into('<QiiHHH', b, 0, bitmap, glat, glon, 0, 1, spacing)
    struct.pack_into('<896h', b, 22, *(base+2*(x*24+i)+3*(y*28+j) for i in range(28) for j in range(32)))
    struct.pack_into('<HHhbB', b, 1814, x, y, lon, lat, 1)
    struct.pack_into('<H', b, 16, binascii.crc_hqx(b[:1821],0))
    return b


def install(root, name, base, corrupt=False, bitmap=(1<<56)-1):
    name = 'dataset-' + name
    d = root/name; d.mkdir()
    for lat, lon, filename in [(0,0,'N00E000.DAT'),(-1,-1,'S01W001.DAT')]:
        stride = int((111318.84502145034*math.cos(math.radians(lat))+2*30*32)/(30*28))
        with (d/filename).open('wb') as f:
            for x in range(2):
                for y in range(2):
                    b=block(lat,lon,x,y,base,bitmap)
                    if corrupt: b[22]^=1
                    f.seek((x*stride+y)*2048);f.write(b)
    tmp=root/'new'; tmp.write_text(name+'\n'); tmp.replace(root/'current')

with tempfile.TemporaryDirectory() as tmp:
    root=Path(tmp); install(root,'first',100)
    proc=subprocess.Popen([sys.argv[1],str(root)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    def query(x,y,lat=0,lon=0):
        # AP computes east offset using the mean latitude of degree origin
        # and query. Quantise to MAVLink's 1e-7 degree position precision.
        la=round((lat+x*30/(M*1e7))*1e7)/1e7
        lo=round((lon+y*30/(M*1e7*math.cos(math.radians((lat+la)/2))))*1e7)/1e7
        proc.stdin.write(f'{la} {lo}\n');proc.stdin.flush()
        state,height=proc.stdout.readline().split()
        return int(state),float(height)
    try:
        for lat,lon in [(0,0),(-1,-1)]:
            for x,y in [(2.5,3.25),(23.99,27.99),(24.01,28.01),(30.5,32.25)]:
                state,h=query(x,y,lat,lon)
                assert state==2 and abs(h-(100+2*x+3*y))<.02,(state,h,x,y,lat,lon)
        install(root,'replacement',300);time.sleep(1.2)
        state,h=query(2.5,3.25); assert state==2 and abs(h-314.75)<.02,(state,h)
        install(root,'bad',400,corrupt=True);time.sleep(1.2)
        assert query(2.5,3.25)[0]==0,'bad CRC accepted'
        install(root,'partial',500,bitmap=1);time.sleep(1.2)
        assert query(2.5,3.25)[0]==0,'missing neighbour bitmap accepted'
        assert query(2.5,2.5)[0]==2
        assert query(2.5,2.5,40,50)[0]==0,'missing tile accepted'
    finally:
        proc.stdin.close();proc.wait(timeout=5)
print('PASS terrain interpolation, hemispheres, block boundaries, CRC, bitmap, missing tiles and live cache invalidation')
