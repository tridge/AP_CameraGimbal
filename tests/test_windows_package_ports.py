#!/usr/bin/env python3
"""Deterministically reproduce the packaged multi-SITL raw-thermal collision."""
from pathlib import Path
import socket
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from windows.test_package import TestPorts


class PortTests(unittest.TestCase):
    def test_later_mavlink_avoids_raw_thermal(self):
        ports = TestPorts()
        with patch('windows.test_package.random.randrange', side_effect=[32426, 32428, 32500]), \
                patch('windows.test_package.socket.socket'):
            self.assertEqual(ports(socket.SOCK_STREAM, offsets=(0, 1, 2)), 32426)
            self.assertEqual(ports(socket.SOCK_STREAM), 32500)
        self.assertTrue({32426, 32427, 32428, 32500}.issubset(ports.allocated))

    def test_group_avoids_previously_selected_listener(self):
        ports = TestPorts()
        with patch('windows.test_package.random.randrange', side_effect=[32428, 32426, 32500]), \
                patch('windows.test_package.socket.socket'):
            self.assertEqual(ports(socket.SOCK_STREAM), 32428)
            self.assertEqual(ports(socket.SOCK_STREAM, offsets=(0, 1, 2)), 32500)

    def test_busy_raw_thermal_rejects_entire_group(self):
        ports = TestPorts()
        seen = []
        def bind(address):
            seen.append(address[1])
            if address[1] == 32428:
                raise OSError('raw thermal port in use')
        with patch('windows.test_package.random.randrange', side_effect=[32426, 32500]), \
                patch('windows.test_package.socket.socket') as sockets:
            sockets.return_value.__enter__.return_value.bind.side_effect = bind
            self.assertEqual(ports(socket.SOCK_STREAM, offsets=(0, 1, 2)), 32500)
        self.assertEqual(seen, [32426, 32427, 32428, 32500, 32501, 32502])
        self.assertEqual(ports.allocated, {32500, 32501, 32502})
        self.assertEqual(sockets.return_value.__exit__.call_count, 6)


if __name__ == '__main__':
    unittest.main()
