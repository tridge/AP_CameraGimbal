#!/usr/bin/env python3
"""Host process tests of bounded vendor ownership and detached rollback.

The fixture substitutes customer paths and the tmpfs check. Production has no
host-test bypass. Fake processes never touch hardware or persistent camera data.
"""
import os
from pathlib import Path
import signal
import shlex
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


def state(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().rsplit(') ', 1)[1].split()[0]
    except FileNotFoundError:
        return None


class Watchdog(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='zt30-watchdog-fixture-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.customer = cls.root / 'customer'
        cls.customer.mkdir()
        source = cls.root / 'vendor.c'
        source.write_text('''#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
int main(void) {
    char cwd[4096]; getcwd(cwd,sizeof(cwd));
    // Publish the fixture atomically, including when a vendor PID is reused.
    char temporary[4096];snprintf(temporary,sizeof(temporary),"%s.tmp",getenv("CAPTURE"));
    FILE *f=fopen(temporary,"w");if(!f)return 2;
    fprintf(f,"%ld\\n%s\\n%s\\n",(long)getpid(),cwd,getenv("TEST_VALUE"));
    fclose(f);if(rename(temporary,getenv("CAPTURE")))return 3;
    if (getenv("IGNORE_TERM")) signal(SIGTERM,SIG_IGN);
    for (;;) pause();
}
''')
        cls.vendor_path = cls.customer / 'siyi_app_359'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(cls.vendor_path)], check=True)
        cls.launcher_path = cls.root / 'vendor-exec'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-DZT30_VENDOR_APP="'+str(cls.vendor_path)+'"', str(ROOT / 'tools/zt30_vendor_exec.c'), '-o', str(cls.launcher_path)], check=True)
        map_source=cls.root/'mapped-owner.c'
        map_source.write_text('#include <fcntl.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <sys/mman.h>\n#include <unistd.h>\nint main(int argc,char **argv) { if(argc!=3)return 2;int fd=open(argv[1],O_RDONLY);if(fd<0)return 3;void *m=mmap(0,4096,PROT_READ,MAP_SHARED,fd,0);if(m==MAP_FAILED)return 4;close(fd);FILE *f=fopen(argv[2],"w");if(!f)return 5;fprintf(f,"%ld",(long)getpid());fclose(f);for(;;)pause();}\n')
        cls.map_owner=cls.root/'mapped-owner'
        subprocess.run(['cc','-Wall','-Wextra','-Werror',str(map_source),'-o',str(cls.map_owner)],check=True)
        cls.restore_path = cls.customer / 'app_restore.sh'
        cls.restore_path.write_text('while :; do sleep 1; done\n')
        cls.script = cls.root / 'watchdog.sh'
        text = (ROOT / 'tools/zt30_ram_watchdog.sh').read_text()
        text = text.replace('/customer/', str(cls.customer) + '/')
        text = text.replace('index(target,"/dev/")==1', '0')
        for device in ['mi_', 'isp_', 'cam_os', 'ttyS1', 'bus/usb/']:
            text = text.replace('/dev/' + device, str(cls.root / device))
        text = text.replace('[ ! -s "$session/owners.scan.errors" ] || return 1', ': # host fixture cannot inspect other users fd tables')
        text = text.replace('[ ! -s "$session/owners.maps.errors" ] || return 1', ': # host fixture cannot inspect other users maps')
        text = text.replace("awk '$2==\"/tmp\" && $3==\"tmpfs\" {found=1} END {exit !found}' /proc/mounts || exit 2", ': # fixture lives on the host /tmp filesystem')
        # Keep the production prefix policy intact, but relocate both shell
        # patterns with the host's configured temporary directory.
        prefix = str(Path(tempfile.gettempdir()).resolve())
        text = text.replace('/tmp/apcg-zt30.*', shlex.quote(prefix+'/apcg-zt30.')+'*')
        text = text.replace('${session#/tmp/}', '${session#'+shlex.quote(prefix+'/')+'}')
        cls.script.write_text(text)
        cls.shell = ['/bin/sh']
        cls.helper_path = os.environ['PATH']
        if os.environ.get('ZT30_TEST_BUSYBOX'):
            cls.shell = [os.environ['ZT30_TEST_BUSYBOX'], 'ash']
            applets = cls.root / 'applets'
            applets.mkdir()
            for applet in ['sh', 'setsid', 'xargs', 'ls', 'awk', 'sed', 'tr', 'cat', 'readlink', 'cmp', 'wc', 'grep', 'cp', 'sleep', 'pidof', 'env']:
                (applets / applet).symlink_to(os.environ['ZT30_TEST_BUSYBOX'])
            cls.helper_path = str(applets) + ':' + os.environ['PATH']


    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='apcg-zt30.')
        self.addCleanup(temp.cleanup)
        self.session = Path(temp.name)
        (self.session / 'vendor-exec').symlink_to(self.launcher_path)
        self.capture = self.session / 'vendor.capture'
        self.cwd = self.session / 'cwd'
        self.cwd.mkdir()
        self.env = dict(os.environ, CAPTURE=str(self.capture), TEST_VALUE='spaces\n$(touch should-not-exist) `false`')
        self.vendor = subprocess.Popen([str(self.vendor_path), str(self.customer / '359_default.ini')], cwd=self.cwd, env=self.env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        self.restore = subprocess.Popen(['/bin/sh', str(self.restore_path)], start_new_session=True)
        self.addCleanup(self.cleanup_processes)
        self.wait_for(self.capture.exists)
        self.wd = None

    def cleanup_processes(self):
        if self.wd and self.wd.poll() is None:
            self.wd.kill()
            self.wd.wait(timeout=5)
        restored = self.session / 'vendor-restored.pid'
        if restored.exists():
            try:
                os.kill(int(restored.read_text()), signal.SIGKILL)
            except ProcessLookupError:
                pass
        for process in [self.vendor, self.restore]:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)
        experiment = self.session / 'experiment.pid'
        if experiment.exists():
            try:
                os.killpg(int(experiment.read_text()), signal.SIGKILL)
            except ProcessLookupError:
                pass

    def wait_for(self, predicate, seconds=10):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.05)
        self.fail('timeout waiting for fixture condition')

    def launch(self, command, duration=2, expected_exit=None, preexec_fn=None):
        (self.session / 'preflight.ok').touch()
        self.log = open(self.session / 'watchdog.log', 'w')
        self.addCleanup(self.log.close)
        self.wd = subprocess.Popen([*self.shell, str(self.script), str(duration), str(self.session), str(self.vendor.pid), str(self.restore.pid), *command], stdin=subprocess.DEVNULL, stdout=self.log, stderr=self.log, start_new_session=True, env=dict(os.environ, PATH=self.helper_path, APCG_ZT30_VERIFY_SECONDS='10'), preexec_fn=preexec_fn)
        self.wait_for(lambda: (self.session / 'watchdog.ready').exists() or self.wd.poll() is not None)
        if expected_exit is not None:
            self.assertEqual(self.wd.poll(), expected_exit, (self.session / 'watchdog.log').read_text())
            return
        self.assertIsNone(self.wd.poll(), (self.session / 'watchdog.log').read_text() + (self.session / 'owners.preflight').read_text())

    def start(self):
        (self.session / 'start').touch()

    def verify_restart(self, expected_rc=0):
        # Vendor stop, experiment deadline and TERM/KILL grace can exceed10s.
        # Bound this host observation independently of the camera's watchdog.
        self.wait_for(lambda: (self.session / 'vendor-restored.pid').exists(), seconds=20)
        self.vendor.wait(timeout=5)
        restored = int((self.session / 'vendor-restored.pid').read_text())
        self.wait_for(lambda: self.capture.read_text().splitlines()[0] == str(restored))
        self.assertEqual(self.capture.read_text(), f'{restored}\n{self.cwd}\n{self.env["TEST_VALUE"]}\n')
        self.assertEqual(state(self.restore.pid), 'T')
        (self.session / 'vendor-verified').touch()
        self.assertEqual(self.wd.wait(timeout=5), expected_rc, (self.session / 'watchdog.log').read_text())
        self.assertNotEqual(state(self.restore.pid), 'T')
        self.assertEqual((self.session / 'result').read_text().strip(), 'vendor restored and externally verified')

    def test_null_command_restores_exact_environment_and_cwd(self):
        self.launch(['/bin/true'])
        self.start()
        self.verify_restart()
        self.assertFalse((self.cwd / 'should-not-exist').exists())

    def test_deadline_cleans_up_term_ignoring_descendants(self):
        self.launch(['/bin/sh', '-c', 'trap "" TERM; sleep 100 & echo $! > child.pid; wait'])
        self.start()
        self.wait_for(lambda: (self.session / 'child.pid').exists())
        child = int((self.session / 'child.pid').read_text())
        self.verify_restart()
        self.assertIn(state(child), (None, 'Z', 'X'))

    def test_abort_before_start_does_not_touch_services(self):
        self.launch(['/bin/true'])
        (self.session / 'stop').touch()
        self.assertEqual(self.wd.wait(timeout=5), 3)
        self.assertIsNone(self.vendor.poll())
        self.assertNotEqual(state(self.restore.pid), 'T')
        self.assertEqual((self.session / 'result').read_text().strip(), 'no service changes')

    def test_repeated_signals_during_verification_do_not_interrupt_rollback(self):
        self.launch(['/bin/true'])
        self.start()
        # Vendor stop, experiment deadline and TERM/KILL grace can exceed10s.
        # Bound this host observation independently of the camera's watchdog.
        self.wait_for(lambda: (self.session / 'vendor-restored.pid').exists(), seconds=20)
        for sig in [signal.SIGHUP, signal.SIGTERM, signal.SIGINT, signal.SIGTERM]:
            os.kill(self.wd.pid, sig)
            time.sleep(0.05)
        self.verify_restart()

    def test_signal_during_experiment_triggers_rollback(self):
        self.launch(['/bin/sleep', '100'], duration=60)
        self.start()
        self.wait_for(lambda: (self.session / 'experiment.pid').exists())
        os.kill(self.wd.pid, signal.SIGTERM)
        self.verify_restart(expected_rc=143)

    def test_escaping_device_owner_prevents_vendor_restart(self):
        device = self.root / 'mi_fake'
        device.touch()
        self.launch(['/bin/sh', '-c', "setsid sh -c 'exec 3< \"$1\"; echo $$ > \"$2\"; sleep 100' sh \"$1\" \"$2\" & sleep 1", 'sh', str(device), str(self.session / 'escaped.pid')])
        self.start()
        self.wait_for(lambda: (self.session / 'escaped.pid').exists())
        escaped = int((self.session / 'escaped.pid').read_text())
        try:
            self.assertEqual(self.wd.wait(timeout=10), 5)
            self.assertFalse((self.session / 'vendor-restored.pid').exists())
            self.assertEqual(state(self.restore.pid), 'T')
            self.assertIn('remaining SDK/peripheral owner', (self.session / 'result').read_text())
        finally:
            try: os.killpg(escaped, signal.SIGKILL)
            except ProcessLookupError: pass

    def test_unreadable_mapping_scan_fails_before_service_changes(self):
        fixture=self.session/'denied-maps.sh'
        text=self.script.read_text().replace(': # host fixture cannot inspect other users maps','[ ! -s "$session/owners.maps.errors" ] || return 1')
        text=text.replace('set -eu', 'set -eu\ngrep() { if [ "$1" = -H ]; then echo "grep: Permission denied" >&2; return 2; fi; command grep "$@"; }',1)
        fixture.write_text(text);self.script=fixture
        self.launch(['/bin/true'],expected_exit=2)
        self.assertIsNone(self.vendor.poll())
        self.assertNotEqual(state(self.restore.pid),'T')
        self.assertIn('Permission denied',(self.session/'owners.maps.errors').read_text())

    def test_mapping_without_fd_prevents_restart(self):
        device=self.root/'mi_mapped'
        device.write_bytes(b'\0'*4096)
        self.launch(['/bin/sh','-c','setsid "$1" "$2" "$3" & sleep 1','sh',str(self.map_owner),str(device),str(self.session/'mapped.pid')])
        self.start()
        self.wait_for(lambda:(self.session/'mapped.pid').exists())
        escaped=int((self.session/'mapped.pid').read_text())
        try:
            self.assertEqual(self.wd.wait(timeout=12),5)
            self.assertFalse((self.session/'vendor-restored.pid').exists())
            self.assertEqual(state(self.restore.pid),'T')
            self.assertIn('mapped', (self.session/'owners.before-restart').read_text())
        finally:
            try:os.killpg(escaped,signal.SIGKILL)
            except ProcessLookupError:pass

    def test_failed_vendor_kill_refuses_duplicate_owner(self):
        self.vendor.kill()
        self.vendor.wait(timeout=5)
        env = dict(self.env, IGNORE_TERM='1')
        self.vendor = subprocess.Popen([str(self.vendor_path), str(self.customer / '359_default.ini')], cwd=self.cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        self.wait_for(lambda: self.capture.read_text().splitlines()[0] == str(self.vendor.pid))
        # Private fixture injects a failed KILL; production has no such bypass.
        fixture = self.session / 'failed-kill.sh'
        text = self.script.read_text().replace('set -eu', 'set -eu\nkill() { case "$1" in -KILL) return 0;; *) command kill "$@";; esac; }', 1)
        fixture.write_text(text)
        self.script = fixture
        self.launch(['/bin/true'])
        self.start()
        self.assertEqual(self.wd.wait(timeout=10), 5)
        self.assertIsNone(self.vendor.poll())
        self.assertFalse((self.session / 'vendor-restored.pid').exists())
        self.assertEqual(state(self.restore.pid), 'T')
        self.assertIn('original vendor still alive', (self.session / 'result').read_text())

    def test_inherited_ignored_term_is_rejected(self):
        self.launch(['/bin/true'], expected_exit=2, preexec_fn=lambda: signal.signal(signal.SIGTERM, signal.SIG_IGN))
        self.assertIsNone(self.vendor.poll())
        self.assertNotEqual(state(self.restore.pid), 'T')

    def test_unreadable_owner_scan_fails_before_service_changes(self):
        fixture = self.session / 'denied-scan.sh'
        text = self.script.read_text().replace(': # host fixture cannot inspect other users fd tables', '[ ! -s "$session/owners.scan.errors" ] || return 1')
        text = text.replace('set -eu', 'set -eu\nls() { echo "ls: Permission denied" >&2; return 1; }', 1)
        fixture.write_text(text)
        self.script = fixture
        self.launch(['/bin/true'], expected_exit=2)
        self.assertIsNone(self.vendor.poll())
        self.assertNotEqual(state(self.restore.pid), 'T')
        self.assertIn('Permission denied', (self.session / 'owners.scan.errors').read_text())

    def test_missing_external_verification_leaves_supervisor_suspended(self):
        self.launch(['/bin/true'])
        self.start()
        # Vendor stop, experiment deadline and TERM/KILL grace can exceed10s.
        # Bound this host observation independently of the camera's watchdog.
        self.wait_for(lambda: (self.session / 'vendor-restored.pid').exists(), seconds=20)
        self.assertEqual(self.wd.wait(timeout=15), 5)
        self.assertEqual(state(self.restore.pid), 'T')
        self.assertIn('verification missing', (self.session / 'result').read_text())


if __name__ == '__main__':
    unittest.main()
