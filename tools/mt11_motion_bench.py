#!/usr/bin/env python3
"""Upright MT11 bench travel, rate and settling measurements via raw SIYI.

Moves real hardware on a stationary base. Upright mounting only.
Pause/disconnect external gimbal controllers first. Records
all angle feedback; restores tracking/logging parameters and the starting pose.
Rate trials stay within a bounded region, independently of commanded duration.
"""
import argparse
import csv
import json
import math
from pathlib import Path
import signal
import struct
import time

from mt11_rate_sweep import Vendor, parameter
from pymavlink import mavutil
from target_properties import transform


class Bench:
    def __init__(self, args):
        self.args = args
        self.vendor = Vendor(args.host, args.vendor_port)
        self.link = mavutil.mavlink_connection(f'tcp:{args.host}:{args.mavlink_port}',
                                             source_system=255, source_component=190)
        self.original = {}
        self.initial = None
        self.trial = 0
        self.trials = []
        self.out = args.output.open('w', newline='')
        self.writer = csv.writer(self.out)
        self.writer.writerow(('trial','kind','axis','command','phase','monotonic_s','elapsed_s',
                              'yaw','pitch','roll','gyro_yaw','gyro_pitch','gyro_roll','rtt_s'))

    def sample(self, kind, axis, command, phase, start):
        t0 = time.monotonic()
        pose = self.vendor.attitude()
        now = time.monotonic()
        self.writer.writerow((self.trial,kind,axis,command,phase,now,now-start,*pose,now-t0))
        return pose

    def hold(self, duration, kind, axis, command, phase, start):
        end = time.monotonic()+duration
        poses = []
        while time.monotonic()<end:
            poses.append(self.sample(kind,axis,command,phase,start))
            time.sleep(.018)
        self.out.flush()
        return poses

    def angle(self, yaw, pitch):
        _, p, y = transform(self.vendor.properties,'angle_command',False,(0,pitch,yaw))
        self.vendor.send(14,struct.pack('<hh',round(y*10),round(p*10)))

    def step(self, axis, value, duration=3.0, kind='angle'):
        self.trial += 1
        initial = self.vendor.attitude()
        yaw = value if axis=='yaw' else 0
        pitch = value if axis=='pitch' else -45
        start = time.monotonic()
        self.angle(yaw,pitch)
        poses = self.hold(duration,kind,axis,value,'move',start)
        final = poses[-1]
        result=dict(trial=self.trial,kind=kind,axis=axis,command=value,start=start,
                    initial=initial,final=final)
        self.trials.append(result)
        print(f'{kind} {axis} {value:+g}: yaw={final[0]:.1f}, pitch={final[1]:.1f}, roll={final[2]:.1f}',flush=True)
        return final

    def center(self):
        self.vendor.center(-45)

    def travel(self):
        # At the first rejected incremental step, stop extending that direction.
        for targets in ((-45,-70,-85,-90,-95,-100,-110,-120,-130,-135),
                        (0,15,25,30,35,40,45)):
            for target in targets:
                final = self.step('pitch',target,2.5,'travel')
                if abs(final[1]-target)>3 or abs(final[2])>15:
                    self.vendor.stop()
                    print('Further angle travel stopped: target not reached or Euler branch changed.',flush=True)
                    break
            self.step('pitch',-45,3,'return')
        self.boundaries()

    def boundaries(self):
        # Check whether angle-command boundaries are also enforced in rate mode.
        for initial,command,bound in [(-80,-10,-93),(15,10,28)]:
            self.step('pitch',initial,3,'prepare')
            self.trial += 1
            start=time.monotonic(); previous=[]
            while time.monotonic()-start<2.0:
                self.vendor.send(7,struct.pack('<bb',0,command))
                pose=self.sample('boundary_rate','pitch',command,'run',start)
                previous.append((time.monotonic(),pose[1]))
                if (command<0 and pose[1]<bound) or (command>0 and pose[1]>bound) or abs(pose[2])>15:break
                recent=[v for t,v in previous if time.monotonic()-t<.4]
                if time.monotonic()-start>1.2 and len(recent)>8 and max(recent)-min(recent)<.15:break
                time.sleep(.018)
            self.vendor.stop()
            self.hold(1.5,'boundary_rate','pitch',0,'stop',time.monotonic())
            print(f'Rate boundary {command:+d}: {self.vendor.attitude()[:3]}',flush=True)
            self.step('pitch',-45,3,'return')

    def rate_trials(self):
        for axis in ('pitch','yaw'):
            for magnitude in self.args.commands:
                for sign in ([1] if magnitude==0 else [1,-1]):
                    command=sign*magnitude
                    self.center(); self.trial+=1
                    initial=self.vendor.attitude(); index=0 if axis=='yaw' else 1
                    start=time.monotonic()
                    payload=struct.pack('<bb',command if axis=='yaw' else 0,command if axis=='pitch' else 0)
                    while time.monotonic()-start<1.4:
                        self.vendor.send(7,payload)
                        pose=self.sample('rate',axis,command,'run',start)
                        if abs(math.remainder(pose[index]-initial[index],360))>=25 or not -82<pose[1]<10:break
                        time.sleep(.018)
                    self.vendor.stop();stopped=time.monotonic()
                    self.hold(1.5,'rate',axis,0,'stop',stopped)
                    final=self.vendor.attitude()
                    self.trials.append(dict(trial=self.trial,kind='rate',axis=axis,command=command,
                                            start=start,stop=stopped,initial=initial,final=final))
                    print(f'rate {axis} {command:+d}: run {stopped-start:.3f}s; final {final[:2]}',flush=True)

    def steps(self):
        for repeat in range(self.args.repeats):
            for axis,targets in [('pitch',[-40,-30,-60,-75,-45,-15,-45]),
                                 ('yaw',[5,-5,15,-15,30,-30,60,-60,90,-90,0])]:
                self.center()
                for value in targets:self.step(axis,value,4.0)

    def run(self):
        try:
            # Read state before enabling test commands. No parameter writes if
            # communication or the initial attitude query fails.
            self.initial=self.vendor.attitude()
            for name in ('MAV_POS_TARGET','LOG_DISARMED'):
                self.original[name]=parameter(self.link,self.args.system,name)
            self.args.output.with_suffix('.original.json').write_text(json.dumps(
                dict(pose=self.initial,parameters=self.original),indent=2))
            parameter(self.link,self.args.system,'MAV_POS_TARGET',0)
            parameter(self.link,self.args.system,'LOG_DISARMED',1)
            if self.args.mode=='travel':self.travel()
            elif self.args.mode=='boundaries':self.boundaries()
            elif self.args.mode=='rates':self.rate_trials()
            else:self.steps()
        finally:
            for _ in range(3):
                try:self.vendor.stop()
                except OSError:pass
                time.sleep(.05)
            try:
                if self.initial is not None:
                    self.angle(self.initial[0],self.initial[1])
                    self.hold(4,'restore','both',0,'restore',time.monotonic())
                    print('Restored pose:',self.vendor.attitude()[:3],flush=True)
            finally:
                for name,value in self.original.items():
                    try:parameter(self.link,self.args.system,name,value)
                    except Exception as e:print(f'RESTORE FAILED {name}={value}: {e}',flush=True)
                self.args.output.with_suffix('.trials.json').write_text(json.dumps(self.trials,indent=2))
                self.out.close();self.vendor.socket.close();self.link.close()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host',required=True)
    p.add_argument('--vendor-port',type=int,default=37260)
    p.add_argument('--mavlink-port',type=int,default=16001)
    p.add_argument('--system',type=int,default=1)
    p.add_argument('--mode',choices=('travel','boundaries','rates','steps'),required=True)
    p.add_argument('--commands',default='0,1,3,4,5,6,7,10,20,40,60,80,100')
    p.add_argument('--repeats',type=int,default=2)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();args.commands=[int(v) for v in args.commands.split(',')]
    if any(v<0 or v>100 for v in args.commands) or not 1<=args.repeats<=5:p.error('invalid command range or repeats')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    signal.signal(signal.SIGTERM,lambda signum,frame: (_ for _ in ()).throw(KeyboardInterrupt()))
    Bench(args).run()

if __name__=='__main__':main()
