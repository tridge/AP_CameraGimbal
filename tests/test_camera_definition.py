#!/usr/bin/env python3
"""Validate generated camera definitions and the bounded read-only MAVFTP service."""
import binascii
import ctypes
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


class Definitions(unittest.TestCase):
    def test_recording_resolution_only_where_it_can_be_chosen(self):
        """A target that records the stream it serves has no recording resolution setting."""
        for target in ('mt11', 'a8', 'zr10', 'zt30', 'z1mini', 'rpi_libcam_caddx'):
            with self.subTest(target=target):
                root = ET.parse(ROOT / 'build/camera-definitions' / (target + '.xml')).getroot()
                params = {p.get('name') for p in root.findall('parameters/parameter')}
                self.assertEqual('REC_RESOLUTION' in params, target != 'rpi_libcam_caddx')

    def test_single_stream_target_has_no_sub_stream_settings(self):
        """A target with one stream does not list settings for a second one."""
        for target in ('mt11', 'a8', 'zr10', 'zt30', 'z1mini', 'rpi_libcam_caddx'):
            with self.subTest(target=target):
                root = ET.parse(ROOT / 'build/camera-definitions' / (target + '.xml')).getroot()
                params = {p.get('name') for p in root.findall('parameters/parameter')}
                self.assertIn('VIDEO_MAIN_RES', params)
                self.assertEqual('VIDEO_SUB_RES' in params, target != 'rpi_libcam_caddx')
                self.assertEqual('VIDEO_SUB_CODEC' in params, target != 'rpi_libcam_caddx')

    def test_target_capabilities(self):
        for target in ('mt11', 'a8', 'zr10', 'zt30', 'z1mini'):
            with self.subTest(target=target):
                root = ET.parse(ROOT / 'build/camera-definitions' / (target + '.xml')).getroot()
                params = {p.get('name'): p for p in root.findall('parameters/parameter')}
                self.assertEqual('CAM_MODE' in params, target != 'z1mini')
                if 'CAM_MODE' in params:
                    options = params['CAM_MODE'].findall('options/option')
                    self.assertEqual([o.get('value') for o in options], ['0', '1', '2'] if target in ('mt11', 'zt30') else ['0', '1'])
                self.assertEqual('SURV_LENS' in params, target in ('mt11', 'zt30'))
                self.assertEqual('SURV_PATTERN' in params, target in ('mt11', 'zt30'))
                if target == 'mt11':
                    self.assertEqual(params['SURV_PATTERN'].get('default'), '0')
                    self.assertEqual([o.get('value') for o in params['SURV_PATTERN'].findall('options/option')], ['0','1','2','3','4'])
                self.assertIn('OSD_CROSS', params)
                self.assertEqual('OSD_RECORD' in params, target != 'z1mini')
                if 'OSD_RECORD' in params:
                    self.assertEqual(params['OSD_RECORD'].get('default'), '0')
                self.assertEqual('OSD_THERMAL_FOV' in params, target in ('mt11', 'zt30'))
                self.assertEqual(params['OSD_CROSS'].get('default'), '0')
                self.assertEqual('CAM_PALETTE' in params, target in ('mt11', 'zt30'))
                self.assertEqual('CAM_LENS' in params, target in ('mt11', 'zt30'))
                self.assertEqual('CAM_AUTOFOCUS' in params, target in ('mt11', 'zt30'))
                self.assertEqual('CAM_ZOOM' in params, target in ('mt11', 'a8', 'zt30'))
                self.assertEqual('CAM_OPT_ZOOM' in params, target in ('mt11', 'zt30'))
                if target in ('mt11','zt30'):
                    self.assertAlmostEqual(float(params['CAM_OPT_ZOOM'].get('max')), 3.2 if target=='mt11' else 30)
                    self.assertEqual(params['CAM_OPT_ZOOM'].get('step'), '0.1')
                    lens_options = params['CAM_LENS'].findall('options/option')
                    self.assertEqual([o.findtext('exclusions/exclude') for o in lens_options],
                                     ['CAM_OPT_ZOOM', 'CAM_ZOOM'])
                if 'CAM_ZOOM' in params:
                    self.assertEqual((params['CAM_ZOOM'].get('min'), params['CAM_ZOOM'].get('max'),
                                      params['CAM_ZOOM'].get('step')), ('1', '10' if target == 'mt11' else '1' if target == 'zt30' else '6', '0.1'))
                self.assertEqual('IMG_ISO' in params, target in ('mt11', 'a8', 'zt30'))
                expected_res = ({'0', '1', '2', '3'} if target == 'zt30' else {'0', '1', '2'} if target in ('mt11', 'a8')
                                else {'0', '1', '3'} if target == 'zr10' else {'1'})
                self.assertEqual({o.get('value') for o in params['VIDEO_MAIN_RES'].findall('options/option')}, expected_res)
                if target == 'z1mini':
                    self.assertEqual({o.get('value') for o in params['REC_RESOLUTION'].findall('options/option')}, {'1', '2'})
                if target == 'zt30':
                    self.assertEqual(params['CAM_LENS'].get('default'), '0')
                    # The inactive RGB lens remains configurable even when
                    # thermal is primary; CAM_LENS selects its zoom control.
                    self.assertIsNone(params['CAM_SOURCE'].findall('options/option')[1].find('exclusions'))
                    self.assertEqual([u.text for u in params['CAM_SOURCE'].findall('updates/update')],
                                     ['CAM_LENS','CAM_ZOOM','CAM_OPT_ZOOM'])
                for name, p in params.items():
                    boolean = name in {'OSD_CROSS', 'OSD_RECORD', 'OSD_THERMAL_FOV',
                                       'LOG_DISARMED', 'MAV_POS_TARGET'}
                    self.assertEqual(p.get('type'), 'bool' if boolean else
                                     'float' if name in ('CAM_ZOOM', 'CAM_OPT_ZOOM') else 'int32')
                    if boolean:
                        self.assertEqual((p.get('min'), p.get('max')), ('0', '1'))
                        self.assertIn(p.get('default'), ('0', '1'))
                    self.assertTrue(name.isupper() and len(name) <= 16)
                    self.assertNotIn('PROXY_', name)
                    for update in p.findall('updates/update'):
                        self.assertIn(update.text, params)


class DefinitionVersion(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='apcam-definition-version-')
        library = Path(cls.work.name) / 'definition.so'
        subprocess.run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-I' + str(ROOT / 'include'),
                        '-I' + str(ROOT / 'camera_app/include'),
                        '-DAPCAM_TARGET=APCAM_TARGET_MT11',
                        str(ROOT / 'camera_app/src/protocol/camera_definition.cpp'),
                        str(ROOT / 'camera_app/src/config.cpp'), '-lm', '-o', str(library)], check=True)
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
                changed = xml.replace(b'Recording resolution', b'Recording size')
                self.assertNotEqual(xml, changed)
                self.assertNotEqual(version, self.version(changed, len(changed)))


class Session(ctypes.Structure):
    _fields_ = [('system', ctypes.c_uint8), ('component', ctypes.c_uint8),
                ('id', ctypes.c_uint8), ('active', ctypes.c_bool), ('last_ms', ctypes.c_uint64),
                ('fd', ctypes.c_int), ('size', ctypes.c_uint64),
                ('burst_offset', ctypes.c_uint32), ('burst_remaining', ctypes.c_uint32),
                ('burst_seq', ctypes.c_uint16), ('burst_size', ctypes.c_uint8)]


class FTP(ctypes.Structure):
    _fields_ = [('sessions', Session * 4), ('roots', (ctypes.c_char * 256) * 3),
                ('burst_packets', ctypes.c_uint), ('xml', ctypes.c_char_p),
                ('xml_length', ctypes.c_size_t)]


class FTPTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='apcam-ftp-test-')
        library = Path(cls.work.name) / 'ftp.so'
        subprocess.run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'camera_app/include'),
                        str(ROOT / 'camera_app/src/protocol/camera_ftp.cpp'), '-o', str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.reply = cls.library.ca_camera_ftp_reply
        cls.reply.argtypes = [ctypes.POINTER(FTP), ctypes.c_char_p, ctypes.c_size_t,
                              ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint64,
                              ctypes.c_void_p, ctypes.c_void_p]
        cls.reply.restype = None
        cls.init = cls.library.ca_camera_ftp_init
        cls.init.argtypes = [ctypes.POINTER(FTP), ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
        cls.init.restype = None
        cls.burst_next = cls.library.ca_camera_ftp_burst_next
        cls.burst_next.argtypes = [ctypes.POINTER(FTP), ctypes.c_uint8, ctypes.c_uint8, ctypes.c_void_p, ctypes.c_bool]
        cls.burst_next.restype = ctypes.c_bool
        cls.close = cls.library.ca_camera_ftp_close
        cls.close.argtypes = [ctypes.POINTER(FTP)]
        cls.close.restype = None

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def setUp(self):
        self.ftp = FTP()
        self.xml = b'<camera/>' * 100

    def tearDown(self):
        self.close(ctypes.byref(self.ftp))

    def card(self):
        """Export a small card tree: record/2026-09-18/{a,b}.mp4, capture/photo.jpg, empty logs."""
        root = Path(self.work.name) / 'card'
        (root / 'record/2026-09-18').mkdir(parents=True, exist_ok=True)
        (root / 'capture').mkdir(exist_ok=True)
        (root / 'logs').mkdir(exist_ok=True)
        (root / 'record/2026-09-18/a.mp4').write_bytes(bytes(range(256)) * 4)
        (root / 'record/2026-09-18/b.mp4').write_bytes(b'')
        (root / 'record/2026-09-18/link.mp4').unlink(missing_ok=True)
        os.symlink('a.mp4', root / 'record/2026-09-18/link.mp4')
        (root / 'record/note').mkdir(exist_ok=True)
        (root / 'capture/photo.jpg').write_bytes(b'JPEG' * 100)
        os.utime(root / 'capture/photo.jpg', (1700000000, 1700000000))
        self.init(ctypes.byref(self.ftp), str(root / 'record').encode(),
                  str(root / 'capture').encode(), str(root / 'logs').encode())
        return root

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

    def entries(self, path, opcode=3):
        listing = []
        while True:
            header, payload = self.request(opcode, data=path, offset=len(listing))
            if header[2] == 129:
                self.assertEqual(payload, b'\x06')
                return listing
            self.assertEqual(header[2], 128)
            listing.extend(e.decode() for e in payload.split(b'\0') if e)

    def test_card_listing(self):
        root = self.card()
        self.assertEqual(self.entries(b'/'), [f'Fcamera.xml\t{len(self.xml)}', 'Drecord', 'Dcapture', 'Dlogs'])
        with_time = self.entries(b'/', 16)
        self.assertEqual(with_time[0], f'Fcamera.xml\t{len(self.xml)}\t0')
        self.assertRegex(with_time[1], r'^Drecord\t0\t\d+$')
        self.assertEqual(self.entries(b'/record'), ['D2026-09-18', 'Dnote'])
        self.assertEqual(self.entries(b'record/'), ['D2026-09-18', 'Dnote'])
        # symbolic links are neither files nor directories to the client
        self.assertEqual(self.entries(b'/record/2026-09-18'), ['Fa.mp4\t1024', 'Fb.mp4\t0'])
        self.assertEqual(self.entries(b'/capture', 16), ['Fphoto.jpg\t400\t1700000000'])
        self.assertEqual(self.entries(b'/logs'), [])
        self.assertEqual(self.entries(b'/record/2026-09-18/'), ['Fa.mp4\t1024', 'Fb.mp4\t0'])
        for path in (b'/record/../capture', b'/record/./note', b'/record//note', b'/record/2026-09-18/a.mp4',
                     b'/recordx', b'/rec', b'/record/missing', b'/logs/../../record', b'/record/\x01'):
            with self.subTest(path=path):
                self.assertEqual(self.request(3, data=path)[1], b'\x0a')
        # the camera definition is still served from the virtual root only
        self.assertEqual(self.request(4, data=b'/record/camera.xml')[1], b'\x0a')
        (root / 'record/2026-09-18' / ('x' * 250)).write_bytes(b'')
        self.assertEqual(self.entries(b'/record/2026-09-18'), ['Fa.mp4\t1024', 'Fb.mp4\t0'])

    def test_card_symlink_confinement(self):
        root = self.card()
        outside = root / 'outside'
        (outside / 'nested').mkdir(parents=True)
        (outside / 'secret').write_bytes(b'not exported')
        (outside / 'nested/secret').write_bytes(b'not exported')
        os.symlink(outside, root / 'record/escape')
        os.symlink('../outside', root / 'capture/escape')
        for base in (b'/record/escape', b'/capture/escape'):
            for path in (base, base + b'/', base + b'/secret',
                         base + b'/nested', base + b'/nested/secret'):
                for opcode in (3, 16, 4):
                    with self.subTest(path=path, opcode=opcode):
                        header, payload = self.request(opcode, data=path)
                        self.assertEqual((header[2], payload), (129, b'\x0a'))
        # Cygwin accepts backslashes as separators; they must not bypass
        # component validation even though Linux treats them as plain names.
        for path in (b'/record/..\\outside', b'/record/..\\outside/secret',
                     b'/record/..\\outside\\secret', b'/record/C:/'):
            for opcode in (3, 16, 4):
                with self.subTest(path=path, opcode=opcode):
                    header, payload = self.request(opcode, data=path)
                    self.assertEqual((header[2], payload), (129, b'\x0a'))
        self.assertFalse(any(s.active for s in self.ftp.sessions))

    @unittest.skipIf(sys.platform == 'cygwin', 'Windows locks directories containing open files')
    def test_card_open_session_survives_directory_replacement(self):
        root = self.card()
        outside = root / 'outside-session'
        outside.mkdir()
        (outside / 'secret').write_bytes(b'not exported')
        # Replacing a directory with a symlink must not redirect an open session.
        source = root / 'record/stable'
        source.mkdir()
        (source / 'secret').write_bytes(b'exported')
        session = self.request(4, data=b'/record/stable/secret')[0][1]
        source.rename(root / 'record/moved')
        os.symlink(outside, source)
        self.assertEqual(self.request(5, session, size=100)[1], b'exported')
        self.assertEqual(self.request(4, data=b'/record/stable/secret')[1], b'\x0a')

    def test_card_pagination_skips_unexportable_entries(self):
        root = self.card()
        directory = root / 'record/pagination'
        directory.mkdir()
        (directory / ('a' * 240)).write_bytes(b'')
        os.symlink('missing', directory / 'a-link')
        names = ['b%02d-' % i + 'x' * 40 for i in range(20)]
        for name in names:
            (directory / name).write_bytes(b'abc')
            os.utime(directory / name, (1700000000, 1700000000))
        for opcode in (3, 16):
            with self.subTest(opcode=opcode):
                suffix = '\t1700000000' if opcode == 16 else ''
                expected = ['F' + name + '\t3' + suffix for name in names]
                self.assertEqual(self.entries(b'/record/pagination', opcode), expected)
                first = self.request(opcode, data=b'/record/pagination', offset=2)
                self.assertEqual(self.request(opcode, data=b'/record/pagination', offset=2), first)
                self.assertEqual(first[1].split(b'\0')[0].decode(), expected[2])
                self.assertEqual(self.request(opcode, data=b'/record/pagination', offset=len(names))[1], b'\x06')
        for name in names:
            (directory / name).unlink()
        for opcode in (3, 16):
            self.assertEqual(self.entries(b'/record/pagination', opcode), [])

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'requires FIFO support')
    def test_card_fifo_open_does_not_block(self):
        root = self.card()
        os.mkfifo(root / 'record/fifo')
        # Run in a bounded subprocess so a blocking open fails instead of
        # hanging the whole CI job. Exercise production request handling.
        script = """
import ctypes, runpy, sys
cls = runpy.run_path(sys.argv[1])['FTPTest']
cls.setUpClass()
test = cls()
test.setUp()
try:
    test.init(ctypes.byref(test.ftp), sys.argv[2].encode(), None, None)
    header, payload = test.request(4, data=b'/record/fifo')
    assert (header[2], payload) == (129, bytes([10])), (header, payload)
    assert not any(s.active for s in test.ftp.sessions)
finally:
    test.tearDown()
    cls.tearDownClass()
"""
        subprocess.run([sys.executable, '-c', script, str(Path(__file__).resolve()),
                        str(root / 'record')], check=True, timeout=10)

    def test_card_download_and_bursts(self):
        root = self.card()
        data = (root / 'record/2026-09-18/a.mp4').read_bytes()
        for path in (b'/record/2026-09-18/a.mp4', b'record/2026-09-18/a.mp4', b'//record/2026-09-18/a.mp4'):
            header, payload = self.request(4, data=path)
            self.assertEqual(header[2], 128, path)
            self.assertEqual(struct.unpack('<I', payload)[0], len(data))
        session = header[1]
        self.assertEqual(self.request(5, session, size=16, offset=1000)[1], data[1000:1016])
        self.assertEqual(self.request(5, session, size=16, offset=1024)[1], b'\x06')
        # single-packet bursts by default
        header, payload = self.request(15, session, size=239)
        self.assertEqual((header[5], payload), (1, data[:239]))
        self.assertFalse(self.burst_next(ctypes.byref(self.ftp), 255, 190, ctypes.create_string_buffer(251), False))
        # multi-packet bursts continue from the reply until the count or EOF
        self.ftp.burst_packets = 3
        header, payload = self.request(15, session, size=239, offset=100, seq=10)
        self.assertEqual((header[5], payload), (0, data[100:339]))
        received = payload
        expected_seq = 12
        while True:
            response = ctypes.create_string_buffer(251)
            if not self.burst_next(ctypes.byref(self.ftp), 255, 190, response, False):
                break
            header = struct.unpack('<HBBBBBBI', response.raw[:12])
            self.assertEqual((header[0], header[1], header[4]), (expected_seq, session, 15))
            expected_seq += 1
            self.assertEqual(header[7], 100 + len(received))
            self.assertEqual(header[2], 128)
            received += response.raw[12:12 + header[3]]
        self.assertEqual((expected_seq, header[5]), (14, 1))
        self.assertEqual(received, data[100:100 + 3 * 239])
        # a burst reaching EOF ends with the autopilot's EOF NACK at that offset
        header, payload = self.request(15, session, size=239, offset=1024 - 300)
        self.assertEqual((header[5], payload), (0, data[724:963]))
        response = ctypes.create_string_buffer(251)
        self.assertTrue(self.burst_next(ctypes.byref(self.ftp), 255, 190, response, False))
        header = struct.unpack('<HBBBBBBI', response.raw[:12])
        self.assertEqual((header[2], header[5], header[7]), (128, 1, 963))
        self.assertEqual(response.raw[12:12 + header[3]], data[963:])
        self.assertFalse(self.burst_next(ctypes.byref(self.ftp), 255, 190, response, False))
        header, payload = self.request(15, session, size=239, offset=1024 - 239)
        self.assertEqual((header[5], payload), (1, data[-239:]))
        # another client's session is unaffected and bursts are per client
        other = self.open(owner=254)
        self.assertEqual(self.request(15, other, size=239, offset=1024 - 239, owner=254)[0][5], 1)
        self.assertFalse(self.burst_next(ctypes.byref(self.ftp), 254, 190, response, False))
        self.assertFalse(self.burst_next(ctypes.byref(self.ftp), 255, 190, response, False))
        self.assertEqual(self.request(15, session, size=100)[1], data[:100])
        self.assertTrue(self.burst_next(ctypes.byref(self.ftp), 255, 190, response, False))
        self.assertFalse(self.burst_next(ctypes.byref(self.ftp), 254, 190, response, False))
        # a link without room ends the burst early with burst_complete set
        self.ftp.burst_packets = 8
        self.assertEqual(self.request(15, session, size=100)[0][5], 0)
        self.assertTrue(self.burst_next(ctypes.byref(self.ftp), 255, 190, response, True))
        header = struct.unpack('<HBBBBBBI', response.raw[:12])
        self.assertEqual((header[2], header[5], header[7]), (128, 1, 100))
        self.assertFalse(self.burst_next(ctypes.byref(self.ftp), 255, 190, response, False))
        # terminating closes the descriptor; empty files and directories are not readable
        self.assertEqual(self.request(1, session)[0][2], 128)
        self.assertEqual(self.ftp.sessions[0].fd, -1)
        header, payload = self.request(4, data=b'/record/2026-09-18/b.mp4')
        self.assertEqual(struct.unpack('<I', payload)[0], 0)
        self.assertEqual(self.request(5, header[1], size=1)[1], b'\x06')
        for path in (b'/record', b'/record/2026-09-18/link.mp4', b'/record/2026-09-18/../2026-09-18/a.mp4', b'/'):
            with self.subTest(path=path):
                self.assertEqual(self.request(4, data=path)[1], b'\x0a')
        # reopening a card file releases the previous descriptor of the same session
        first = self.request(4, data=b'/record/2026-09-18/a.mp4')[0][1]
        self.assertGreaterEqual(self.ftp.sessions[0].fd, 0)
        self.assertEqual(self.request(4, data=b'/capture/photo.jpg')[0][1], first)
        self.assertEqual(self.request(5, first, size=4)[1], b'JPEG')
        self.request(2)
        self.assertEqual(self.request(5, first, size=4)[1], b'\x04')
        self.assertTrue(all(s.fd == -1 or not s.active for s in self.ftp.sessions))


if __name__ == '__main__':
    unittest.main()
