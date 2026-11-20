#!/usr/bin/env python3
"""USB-only identity, forwarding and pinned output failures using no hardware."""
import struct,unittest
from test_zt30_ioctl_trace import IoctlTrace
class UsbTrace(unittest.TestCase):
    fixture=IoctlTrace.fixture
    def test_forwarding_filtering_and_accounting(self):
        for options in [{},{'negative':True},{'zero':True},{'count':80,'collision':True}]:
            with self.subTest(options=options):
                result,data,counts=self.fixture(usb=True,**options)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(data[:8],b'ZT30USB0');self.assertEqual(counts[:8],b'ZT30USC1')
                types=[data[i+45] for i in range(8,len(data),64)]
                self.assertEqual(types.count(ord('B')),types.count(ord('A')))
                self.assertEqual(set(data[i+44] for i in range(8,len(data),64)),{0})
    def test_missing_device_explicit_failure_preserves_forwarding(self):
        result,data,counts=self.fixture(usb=True,missing=True)
        self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(data,b'FAILUSB0')
    def test_output_reuse_does_not_redirect_writes(self):
        result,data,counts=self.fixture(usb=True,reuse=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertGreater(struct.unpack_from('<8I',counts)[5],0)
    def test_counts_output_reuse_does_not_redirect_writes(self):
        result,data,counts=self.fixture(usb=True,reuse_counts=True)
        self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(counts,b'')
    def test_existing_outputs_and_inactive_vendor(self):
        for opt,expected in [({'existing':True},b'do not overwrite'),({'counts_existing':True},b''),({'inactive':True},b'')]:
            result,data,counts=self.fixture(usb=True,**opt)
            self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(data,expected)
    def test_caps_and_saturation_fail_with_forwarding_intact(self):
        for options,field in [({'count':22000},6),({'count':1100,'distinct':True},4)]:
            result,data,counts=self.fixture(usb=True,**options)
            self.assertEqual(result.returncode,0,result.stderr);self.assertGreater(struct.unpack_from('<8I',counts)[field],0)
            self.assertLessEqual(len(data),2*1024*1024)
    def test_abrupt_exit_has_no_counts(self):
        result,data,counts=self.fixture(usb=True,abrupt=True)
        self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(counts,b'')
if __name__=='__main__':unittest.main()
