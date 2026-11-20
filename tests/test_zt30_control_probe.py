#!/usr/bin/env python3
"""Check read-only UART requests, interleaving, independent MCU sequences and unwind."""
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import threading
import time
import unittest
ROOT=Path(__file__).resolve().parents[1]
def crc8(data):
    v=0
    for b in data:
        v^=b
        for _ in range(8):v=(v>>1)^0x8c if v&1 else v>>1
    return v
def crc16(data):
    v=0
    for b in data:
        v^=b<<8
        for _ in range(8):v=((v<<1)^0x1021 if v&0x8000 else v<<1)&65535
    return v
def sdk(op,payload):
    f=b'\x55\x66\x02'+struct.pack('<HHB',len(payload),901,op)+payload
    return f+struct.pack('<H',crc16(f))
def link(op,payload,flags=10):
    f=bytes([170,flags,2,len(payload)]);f+=bytes([crc8(f)])+struct.pack('<H',543)+bytes([46,44,107,op])+payload
    return f+struct.pack('<H',crc16(f))
class ControlProbe(unittest.TestCase):
    def setUp(self):
        t=tempfile.TemporaryDirectory(prefix='zt30-control-test-');self.addCleanup(t.cleanup)
        self.master,self.slave=pty.openpty();self.addCleanup(os.close,self.master);self.addCleanup(os.close,self.slave)
        self.before=termios.tcgetattr(self.slave)
        self.binary=Path(t.name)/'probe'
        subprocess.run(['c++','-std=gnu++17','-O2','-Wall','-Wextra','-Werror','-DZT30_CONTROL_DEVICE="'+os.ttyname(self.slave)+'"','-I'+str(ROOT/'camera_app/include'),str(ROOT/'tools/zt30_control_probe.cpp'),str(ROOT/'camera_app/src/protocol/siyi.cpp'),str(ROOT/'camera_app/src/protocol/private_uart.cpp'),'-o',str(self.binary)],check=True)
    def launch(self):
        p=subprocess.Popen([str(self.binary)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        self.addCleanup(lambda:p.poll() is None and p.kill())
        return p
    def test_readonly_flow_fragmentation_and_unrelated_telemetry(self):
        p=self.launch();requests=[];errors=[]
        def fake():
            buffer=bytearray()
            try:
                while p.poll() is None:
                    if not select.select([self.master],[],[],0.1)[0]:continue
                    buffer+=os.read(self.master,4096)
                    while len(buffer)>=13:
                        n=13+buffer[3]
                        if len(buffer)<n:break
                        f=bytes(buffer[:n]);del buffer[:n]
                        self.assertEqual(f[:3],b'\xaa'+bytes([f[1],2]));self.assertEqual(f[4],crc8(f[:4]));self.assertEqual(crc16(f[:-2]),int.from_bytes(f[-2:],'little'))
                        self.assertEqual(f[7:10],bytes([44,46,107]))
                        op=f[10];payload=f[11:-2]
                        if op==22:
                            self.assertEqual(f[1],8);self.assertEqual(len(payload),10)
                            self.assertEqual(payload[:5],b'\x55\x66\x01\x00\x00');op=payload[7]
                            self.assertIn(op,[1,2,13,24,49]);requests.append(('SDK',op))
                            answer={1:bytes.fromhex('0901007b0102007a06010079'),2:b'7A00000000\x00\x00',13:struct.pack('<6h',1,2,3,4,5,6),24:b'\x01\x00',49:b'\x00'}[op]
                            reply=link(22,sdk(op,answer))
                        else:
                            self.assertEqual(f[1],9);self.assertEqual(payload,b'');self.assertIn(op,[21,23]);requests.append(('LINK',op));reply=link(op,b'\x01')
                        junk=bytearray(link(22,b'bad'));junk[-1]^=1
                        os.write(self.master,bytes(junk)+link(55,b'\0'*8,8)+reply[:4])
                        time.sleep(.001);os.write(self.master,reply[4:])
            except BaseException as exc:errors.append(exc)
        t=threading.Thread(target=fake,daemon=True);t.start()
        output,error=p.communicate(timeout=6);t.join(timeout=1)
        self.assertFalse(errors,errors);self.assertEqual(p.returncode,0,error)
        self.assertIn(b'RESULT PASS attitude_samples=20',output)
        self.assertEqual(len(requests),27)
        self.assertEqual(termios.tcgetattr(self.slave),self.before)
    def test_no_response_returns_failure_and_restores_termios(self):
        p=self.launch();_,error=p.communicate(timeout=4)
        self.assertEqual(p.returncode,4,error)
        self.assertEqual(termios.tcgetattr(self.slave),self.before)
    def test_existing_owner_lock_prevents_io(self):
        fcntl.flock(self.slave,fcntl.LOCK_EX|fcntl.LOCK_NB)
        p=self.launch();_,error=p.communicate(timeout=2)
        self.assertEqual(p.returncode,3,error)
        self.assertEqual(termios.tcgetattr(self.slave),self.before)
    def test_signal_interrupts_response_wait(self):
        p=self.launch()
        self.assertTrue(select.select([self.master],[],[],2)[0]);os.read(self.master,4096)
        p.send_signal(signal.SIGTERM);p.communicate(timeout=1)
        self.assertNotEqual(p.returncode,0)
        self.assertEqual(termios.tcgetattr(self.slave),self.before)
if __name__=='__main__':unittest.main()
