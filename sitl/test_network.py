#!/usr/bin/env python3
"""Test camera IP configuration in a disposable network namespace, never on the host."""
import argparse
import json
import socket
from pathlib import Path
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[1]
INTERFACE = 'ca-net-test'


def ip(*args):
    return subprocess.check_output(['ip', *args], text=True)


def addresses(interface=INTERFACE, family='-4'):
    return {f"{a['local']}/{a['prefixlen']}" for entry in json.loads(ip('-j', family, 'addr', 'show', 'dev', interface))
            for a in entry['addr_info']}


def defaults():
    return json.loads(ip('-j', '-4', 'route', 'show', 'default'))


def inside(binary):
    for interface in [INTERFACE, 'ca-other-test']:
        ip('link', 'add', interface, 'type', 'dummy')
        ip('link', 'set', interface, 'up')
    ip('addr', 'add', '198.51.100.25/24', 'dev', INTERFACE)
    ip('-6', 'addr', 'add', '2001:db8::25/64', 'dev', INTERFACE)
    ip('addr', 'add', '203.0.113.25/24', 'dev', 'ca-other-test')
    ipv6 = addresses(family='-6')
    with tempfile.TemporaryDirectory(prefix='camera-network-state-') as temp:
        state = Path(temp) / 'network.state'

        def configure(primary='', secondary='', gateway='', *, ok=True, iface=INTERFACE, path=state):
            run = subprocess.run([str(binary), iface, primary, secondary, gateway, str(path)], capture_output=True, text=True)
            assert (run.returncode == 0) == ok, run.stderr
            assert addresses('ca-other-test') == {'203.0.113.25/24'}
            assert addresses(family='-6') == ipv6

        def expect(ips, gateway=''):
            assert addresses() == set(ips), addresses()
            routes = defaults()
            assert [(r.get('gateway'), r['dev']) for r in routes] == ([(gateway, INTERFACE)] if gateway else []), routes

        configure()
        expect(['198.51.100.25/24'])
        assert not state.exists(), 'blank configuration should not need state'
        configure(secondary='198.51.100.25/24')
        configure()  # Do not remove an address that existed before we configured it.
        expect(['198.51.100.25/24'])
        for _ in range(2):
            configure(secondary='192.0.2.25/24', gateway='192.0.2.1')
            expect(['198.51.100.25/24', '192.0.2.25/24'], '192.0.2.1')
        configure(secondary='192.0.2.27/24', gateway='192.0.2.2')
        expect(['198.51.100.25/24', '192.0.2.27/24'], '192.0.2.2')
        configure()
        expect(['198.51.100.25/24'])
        for _ in range(2):
            configure('198.51.100.27/24', '192.0.2.25/24', '192.0.2.1')
            expect(['198.51.100.27/24', '192.0.2.25/24'], '192.0.2.1')
        for _ in range(2):
            configure('198.51.100.28/24', '198.51.100.29/24', '198.51.100.1')
            expect(['198.51.100.28/24', '198.51.100.29/24'], '198.51.100.1')
        # Promoting a same-subnet secondary must change the kernel's primary
        # flag and chosen source, not just leave both addresses present.
        configure('198.51.100.29/24', '198.51.100.28/24', '198.51.100.1')
        source = json.loads(ip('-j', 'route', 'get', '198.51.100.100'))[0]['prefsrc']
        assert source == '198.51.100.29', source
        configure('198.51.100.28/24', '198.51.100.29/24', '198.51.100.1')
        source = json.loads(ip('-j', 'route', 'get', '198.51.100.100'))[0]['prefsrc']
        assert source == '198.51.100.28', source
        # Subscribe before invoking the app: an unchanged restart must not
        # briefly remove/recreate either addresses or the default route.
        with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, socket.NETLINK_ROUTE) as monitor:
            monitor.bind((0, 0x10 | 0x40))  # RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE
            monitor.settimeout(0.1)
            configure('198.51.100.28/24', '198.51.100.29/24', '198.51.100.1')
            try:
                event = monitor.recv(65536)
            except TimeoutError:
                event = b''
            assert not event, 'unchanged restart emitted address/route changes'
        # Include non-default routes and an extended routing-table ID in the
        # rollback snapshot. Removing addresses can delete these implicitly.
        ip('route', 'add', '198.18.0.0/16', 'via', '198.51.100.1', 'dev', INTERFACE)
        ip('route', 'add', 'table', '1001', '198.19.0.0/16', 'via', '198.51.100.1', 'dev', INTERFACE)
        def all_routes():
            return sorted(json.dumps(r, sort_keys=True) for r in json.loads(ip('-j', '-4', 'route', 'show', 'table', 'all')))
        routes_before = all_routes()
        saved = state.read_bytes()
        for primary, secondary, gateway in [
            ('198.51.100.27', '', ''),
            ('198.51.100.27/33', '', ''),
            ('198.51.100.27/24', '198.51.100.27/25', ''),
            ('198.51.100.27/24', '', '192.0.2.1'),
            ('198.51.100.27/24', '', '198.51.100.255'),
            # Unknown boot-time subnet: kernel must reject after modification,
            # then restore both original addresses, route and state file.
            ('', '192.0.2.25/24', '203.0.113.1'),
        ]:
            configure(primary, secondary, gateway, ok=False)
            expect(['198.51.100.28/24', '198.51.100.29/24'], '198.51.100.1')
            assert state.read_bytes() == saved
            assert all_routes() == routes_before, 'rollback lost or changed routes'
        configure('198.51.100.27/24', ok=False, iface='missing')
        configure('198.51.100.27/24', ok=False, path=Path(temp) / 'missing' / 'state')
        expect(['198.51.100.28/24', '198.51.100.29/24'], '198.51.100.1')
        # A primary change that strands an existing user route must roll back.
        configure('192.0.2.25/24', ok=False)
        assert all_routes() == routes_before
        assert state.read_bytes() == saved
        ip('route', 'add', '198.20.0.0/16', 'nexthop', 'via', '198.51.100.1', 'dev', INTERFACE,
           'weight', '1', 'nexthop', 'via', '203.0.113.1', 'dev', 'ca-other-test', 'weight', '1')
        multipath_before = all_routes()
        configure('198.51.100.27/24', ok=False)
        assert all_routes() == multipath_before, 'shared multipath route changed'
        ip('route', 'del', '198.20.0.0/16')
        # Compatible static routes survive a successful primary change too.
        configure('198.51.100.27/25')
        assert '198.18.0.0/16' in ip('-4', 'route', 'show')
        assert '198.19.0.0/16' in ip('-4', 'route', 'show', 'table', '1001')
        expect(['198.51.100.27/25'])
        configure()  # Blank primary leaves the current address alone.
        expect(['198.51.100.27/25'])
        state.write_text('corrupt')
        configure('198.51.100.25/24')
        expect(['198.51.100.25/24'])
        # Recreating an address creates a kernel subnet route. Recovery must
        # restore the saved custom route's attributes, not silently accept
        # EEXIST and lose its MTU/protocol/preferred source.
        ip('route', 'replace', '198.51.100.0/24', 'dev', INTERFACE, 'proto', 'static',
           'src', '198.51.100.25', 'mtu', '1200')
        custom_before = all_routes()
        configure('', '192.0.2.25/24', '203.0.113.1', ok=False)
        assert all_routes() == custom_before, 'rollback lost custom subnet route attributes'
        configure('198.51.100.25/24', '198.51.100.26/24', '198.51.100.1')
        subnet = next(r for r in json.loads(ip('-j', 'route', 'show')) if r['dst'] == '198.51.100.0/24')
        assert subnet['protocol'] == 'static' and subnet['prefsrc'] == '198.51.100.25', subnet
        assert subnet['metrics'][0]['mtu'] == 1200, subnet
        # Idempotence must hold with ordinary, policy-table and custom subnet
        # routes present, not just a default and generated connected route.
        with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, socket.NETLINK_ROUTE) as monitor:
            monitor.bind((0, 0x10 | 0x40))
            monitor.settimeout(0.1)
            configure('198.51.100.25/24', '198.51.100.26/24', '198.51.100.1')
            try:
                event = monitor.recv(65536)
            except TimeoutError:
                event = b''
            assert not event, 'unchanged restart modified static routes'
    print('PASS primary selection, restart without route flaps, static/policy route recovery, removal and interface isolation')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inside', type=Path)
    parser.add_argument('--sudo', action='store_true', help='use sudo when user namespaces are disabled')
    args = parser.parse_args()
    if args.inside:
        inside(args.inside)
        return
    with tempfile.TemporaryDirectory(prefix='camera-network-') as directory:
        binary = Path(directory) / 'test-network'
        subprocess.run(['c++', '-std=gnu++17', '-Wno-missing-field-initializers', '-O2', '-Wall', '-Wextra', '-Werror', '-std=gnu++17',
                        '-I' + str(REPO / 'include'), '-I' + str(REPO / 'camera_app/include'),
                        str(REPO / 'camera_app/tests/test_network.cpp'),
                        str(REPO / 'camera_app/src/protocol/network.cpp'), '-o', str(binary)], check=True)
        namespace = ['sudo', '-n', 'unshare', '--net'] if args.sudo else ['unshare', '--user', '--map-root-user', '--net']
        subprocess.run([*namespace, sys.executable, str(Path(__file__).resolve()), '--inside', str(binary)], check=True)


if __name__ == '__main__':
    main()
