#!/usr/bin/env python3
"""Validate generated camera definitions and the bounded read-only MAVFTP service."""
import binascii
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


class Definitions(unittest.TestCase):
    def test_target_capabilities(self):
        for target in ('mt11', 'a8', 'zr10', 'z1mini'):
            with self.subTest(target=target):
                root = ET.parse(ROOT / 'build/camera-definitions' / (target + '.xml')).getroot()
                params = {p.get('name'): p for p in root.findall('parameters/parameter')}
                self.assertIn('CAM_MODE', params)
                self.assertIn('OSD_CROSS', params)
                self.assertEqual('OSD_RECORD' in params, target != 'z1mini')
                if 'OSD_RECORD' in params:
                    self.assertEqual(params['OSD_RECORD'].get('default'), '0')
                self.assertEqual('OSD_THERMAL_FOV' in params, target == 'mt11')
                self.assertEqual(params['OSD_CROSS'].get('default'), '0')
                self.assertEqual('CAM_PALETTE' in params, target == 'mt11')
                self.assertEqual('CAM_LENS' in params, target == 'mt11')
                self.assertEqual('CAM_AUTOFOCUS' in params, target == 'mt11')
                self.assertEqual('CAM_ZOOM' in params, target in ('mt11', 'a8'))
                self.assertEqual('IMG_ISO' in params, target in ('mt11', 'a8'))
                expected_res = ({'0', '1', '2'} if target in ('mt11', 'a8')
                                else {'0', '1', '3'} if target == 'zr10' else {'1'})
                self.assertEqual({o.get('value') for o in params['VIDEO_MAIN_RES'].findall('options/option')}, expected_res)
                if target == 'z1mini':
                    self.assertEqual({o.get('value') for o in params['REC_RESOLUTION'].findall('options/option')}, {'1', '2'})
                    self.assertEqual({o.get('value') for o in params['CAM_MODE'].findall('options/option')}, {'1'})
                for name, p in params.items():
                    self.assertTrue(name.isupper() and len(name) <= 16)
                    self.assertNotIn('PROXY_', name)
                    for update in p.findall('updates/update'):
                        self.assertIn(update.text, params)


class DefinitionVersion(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='apcam-definition-version-')
        library = Path(cls.work.name) / 'definition.so'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-I' + str(ROOT / 'include'),
                        '-I' + str(ROOT / 'camera_app/include'),
                        '-DAPCAM_TARGET=APCAM_TARGET_MT11',
                        str(ROOT / 'camera_app/src/protocol/camera_definition.c'),
                        str(ROOT / 'camera_app/src/config.c'), '-lm', '-o', str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.version = cls.library.ca_camera_definition_version
        cls.version.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        cls.version.restype = ctypes.c_uint16

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def test_crc_vectors(self):
        self.assertEqual(self.version(b'123456789', 9), 0x29b1)
        self.assertEqual(self.version(b'', 0), 0xffff)
        self.assertEqual(binascii.crc_hqx(b'\xff\xff', 0xffff), 0)
        self.assertEqual(self.version(b'\xff\xff', 2), 0xffff)

    def test_exported_bytes_and_changes(self):
        for target in ('mt11', 'a8', 'zr10', 'z1mini'):
            with self.subTest(target=target):
                xml = (ROOT / 'build/camera-definitions' / (target + '.xml')).read_bytes()
                version = self.version(xml, len(xml))
                self.assertEqual(version, binascii.crc_hqx(xml, 0xffff) or 0xffff)
                self.assertEqual(version, self.version(xml + b'ignored', len(xml)))
                # UI-only changes must invalidate the definition cache too.
                changed = xml.replace(b'Targeting cross', b'Targeting reticle')
                self.assertNotEqual(xml, changed)
                self.assertNotEqual(version, self.version(changed, len(changed)))


class Session(ctypes.Structure):
    _fields_ = [('system', ctypes.c_uint8), ('component', ctypes.c_uint8),
                ('id', ctypes.c_uint8), ('active', ctypes.c_bool), ('last_ms', ctypes.c_uint64)]


class FTP(ctypes.Structure):
    _fields_ = [('sessions', Session * 4)]


class FTPTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='apcam-ftp-test-')
        library = Path(cls.work.name) / 'ftp.so'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-I' + str(ROOT / 'camera_app/include'),
                        str(ROOT / 'camera_app/src/protocol/camera_ftp.c'), '-o', str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.reply = cls.library.ca_camera_ftp_reply
        cls.reply.argtypes = [ctypes.POINTER(FTP), ctypes.c_char_p, ctypes.c_size_t,
                              ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint64,
                              ctypes.c_void_p, ctypes.c_void_p]
        cls.reply.restype = None

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def setUp(self):
        self.ftp = FTP()
        self.xml = b'<camera/>' * 100

    def request(self, opcode, session=0, data=b'', size=None, offset=0, owner=255, now=1000, seq=65535):
        size = len(data) if size is None else size
        request = struct.pack('<HBBBBBBI', seq, session, opcode, size, 0, 0, 0, offset) + data
        request = request.ljust(251, b'\0')
        response = ctypes.create_string_buffer(251)
        self.reply(ctypes.byref(self.ftp), self.xml, len(self.xml), owner, 190, now, request, response)
        header = struct.unpack('<HBBBBBBI', response.raw[:12])
        self.assertEqual(header[0], (seq + 1) & 65535)
        self.assertEqual(header[4], opcode)
        return header, response.raw[12:12 + header[3]]

    def open(self, owner=255, now=1000):
        header, payload = self.request(4, session=owner, data=b'/camera.xml', owner=owner, now=now)
        self.assertEqual(header[2], 128)
        self.assertEqual(struct.unpack('<I', payload)[0], len(self.xml))
        return header[1]

    def test_download_retry_seek_and_eof(self):
        session = self.open()
        self.assertEqual(self.open(), session)
        result = b''
        while len(result) < len(self.xml):
            header, payload = self.request(15, session, size=239, offset=len(result))
            self.assertEqual(header[2], 128)
            self.assertEqual(header[5], 1)
            result += payload
        self.assertEqual(result, self.xml)
        self.assertEqual(self.request(5, session, size=1, offset=len(result))[1], b'\x06')
        self.assertEqual(self.request(5, session, size=11, offset=20)[1], self.xml[20:31])
        self.assertEqual(self.request(1, session)[0][2], 128)
        self.assertEqual(self.request(5, session, size=1)[1], b'\x04')

    def test_download_with_leading_slashes(self):
        for path in (b'camera.xml', b'/camera.xml', b'//camera.xml',
                     b'////camera.xml', b'/' * 229 + b'camera.xml'):
            with self.subTest(path=path):
                header, payload = self.request(4, data=path)
                self.assertEqual(header[2], 128)
                self.assertEqual(struct.unpack('<I', payload)[0], len(self.xml))
                session = header[1]
                result = b''
                while len(result) < len(self.xml):
                    header, payload = self.request(15, session, size=239, offset=len(result))
                    self.assertEqual(header[2], 128)
                    result += payload
                self.assertEqual(result, self.xml)
                self.assertEqual(self.request(1, session)[0][2], 128)

    def test_scope_bounds_and_independent_sessions(self):
        first, second = self.open(), self.open(owner=254)
        self.assertNotEqual(first, second)
        self.assertEqual(self.request(5, second, size=1)[1], b'\x04')
        self.request(2)
        self.assertEqual(self.request(5, second, size=1, owner=254)[0][2], 128)
        for path in (b'/../../etc/passwd', b'//../camera.xml', b'//etc/passwd',
                     b'//camera.xml/..', b'//missing.xml', b'', b'//', b'/' * 239):
            with self.subTest(path=path):
                header, payload = self.request(4, data=path)
                self.assertEqual(header[2], 129)
                self.assertEqual(payload, b'\x0a')
        self.assertEqual(self.request(7, data=b'malicious')[1], b'\x07')
        self.assertEqual(self.request(5, second, size=240, owner=254)[1], b'\x03')
        self.assertEqual(self.request(5, second, size=1, offset=0xffffffff, owner=254)[1], b'\x06')
        self.open(owner=253)
        self.open(owner=252)
        self.open(owner=251)
        self.assertEqual(self.request(4, data=b'/camera.xml')[1], b'\x05')
        self.open(now=62000)

    def test_same_client_can_choose_concurrent_session_ids(self):
        first = self.open()
        second, _ = self.request(4, session=73, data=b'/camera.xml')
        self.assertEqual(second[1], 73)
        self.assertEqual(second[2], 128)
        self.assertEqual(self.request(5, first, size=1)[1], self.xml[:1])
        self.assertEqual(self.request(5, 73, size=1)[1], self.xml[:1])
        self.request(1, 73)
        self.assertEqual(self.request(5, first, size=1)[1], self.xml[:1])

    def test_directory_listing(self):
        for opcode in (3, 16):
            for root in (b'', b'/', b'.', b'./'):
                header, payload = self.request(opcode, data=root)
                self.assertEqual(header[2], 128)
                suffix = '\t0' if opcode == 16 else ''
                self.assertEqual(payload, f'Fcamera.xml\t{len(self.xml)}{suffix}\0'.encode())
                self.assertEqual(self.request(opcode, data=root)[1], payload)  # retry
                for offset in (1, 239, 0xffffffff):
                    header, payload = self.request(opcode, data=root, offset=offset)
                    self.assertEqual(header[2], 129)
                    self.assertEqual(header[7], offset)
                    self.assertEqual(payload, b'\x06')
            for path in (b'/camera.xml', b'/missing', b'/../', b'/etc'):
                self.assertEqual(self.request(opcode, data=path)[1], b'\x0a')
        self.assertFalse(any(session.active for session in self.ftp.sessions))


if __name__ == '__main__':
    unittest.main()
