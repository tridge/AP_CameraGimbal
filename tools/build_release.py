#!/usr/bin/env python3
"""Build firmware and installation guides in release/<version>/<vendor>_<camera>/."""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# These are packaging entry points and bootloader filenames; hardware
# capabilities remain in include/apcam/target_*.h.
TARGETS = {
    'A8': ('SIYI', 'a8_package', 'A8_PACKAGE_OUT', 'SIYI_4K_MINI_UpgradeSD.bin'),
    'MT11': ('SIYI', 'mt11_package', 'MT11_PACKAGE_OUT', 'MT11_FW_ArduPilot_{version}_{short_revision}.bin'),
    'ZR10': ('SIYI', 'zr10_firmware', 'ZR10_FIRMWARE_OUT', 'ZR10_UpgradeSD.bin'),
    'ZT30': ('SIYI', 'zt30_package', 'ZT30_PACKAGE_OUT', 'ZT30_AP_{version}_{short_revision}.apbundle'),
    'Z1-Mini': ('XFRobot', 'z1mini_native_package', 'Z1MINI_NATIVE_PACKAGE_OUT', 'Z1Mini_AP_native_{version}_{short_revision}.gcu'),
}


def target_name(value):
    for name in TARGETS:
        if value.lower().replace('-', '') == name.lower().replace('-', ''):
            return name
    raise argparse.ArgumentTypeError('Unknown release target: ' + value)


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def publish(staging, destination):
    """Keep the previous camera package until its replacement is complete."""
    previous = destination.with_name('.' + destination.name + '.previous')
    if previous.exists():
        raise RuntimeError(f'Previous interrupted release remains at {previous}; inspect it before rebuilding')
    if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
        raise RuntimeError(f'Release destination is not a regular directory: {destination}')
    replaced = destination.exists()
    if replaced:
        destination.rename(previous)
    try:
        staging.rename(destination)
    except BaseException:
        if replaced:
            previous.rename(destination)
        raise
    if replaced:
        shutil.rmtree(previous)


def build_release(repo, output, version, targets, make='make'):
    if not re.fullmatch(r'v[0-9]+\.[0-9]+(\.[0-9]+)?', version):
        raise ValueError('Release requires a reachable version tag of the form vX.y or vX.y.z (or an MT11_VERSION override)')
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip()
    dirty = subprocess.run(['git', 'diff', '--quiet', 'HEAD', '--'], cwd=repo).returncode != 0
    identity = dict(version=version, revision=revision, short_revision=revision[:6], dirty=dirty)
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Also serialize separate invocations sharing the native build directory.
    (repo / 'build').mkdir(exist_ok=True)
    with (repo / 'build/release.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        version_dir = output / version
        version_dir.mkdir(exist_ok=True)
        for name in dict.fromkeys(targets):
            vendor, target, variable, pattern = TARGETS[name]
            directory = f'{vendor}_{name}'
            filename = pattern.format(**identity)
            template = repo / 'packaging/release' / (name + '.md')
            instructions = template.read_text().format(filename=filename, **identity)
            with tempfile.TemporaryDirectory(prefix='.' + directory + '-', dir=version_dir) as work:
                staging = Path(work) / directory
                staging.mkdir()
                package = staging / filename
                print(f'Building {name} {version} ({revision[:6]})', flush=True)
                # The recursive recipe inherits GNU make's jobserver FDs.
                subprocess.run([make, target, f'{variable}={package}',
                                f'MT11_VERSION={version}', f'MT11_GIT_HASH={revision[:6]}'],
                               cwd=repo, check=True, close_fds=False)
                if not package.is_file() or not package.stat().st_size:
                    raise RuntimeError(f'{target} did not produce {filename}')
                # Builders use differing checksum sidecar formats; release
                # folders use one standard SHA256SUMS with relative filenames.
                package.with_name(filename + '.sha256').unlink(missing_ok=True)
                (staging / 'README.md').write_text(instructions)
                manifest = dict(identity, target=name, package=filename, sha256=sha256(package))
                (staging / 'BUILD_INFO.json').write_text(json.dumps(manifest, indent=2) + '\n')
                (staging / 'SHA256SUMS').write_text(''.join(
                    f'{sha256(path)}  {path.name}\n' for path in sorted(staging.iterdir())))
                destination = version_dir / directory
                publish(staging, destination)
                print(f'Ready: {destination / filename}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', type=Path, default=Path('release'))
    parser.add_argument('--targets', nargs='+', type=target_name, default=list(TARGETS))
    parser.add_argument('--make', default='make')
    args = parser.parse_args()
    try:
        build_release(ROOT, args.output, args.version, args.targets, args.make)
    except (ValueError, RuntimeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'release: {error}\n')


if __name__ == '__main__':
    main()
