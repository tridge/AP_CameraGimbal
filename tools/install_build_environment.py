#!/usr/bin/env python3
"""Install Linux hardware-release prerequisites without camera firmware bundles.

Supports x86_64 Debian/Ubuntu. --skip-system uses existing system packages.
Toolchains/SDKs are pinned and installed under build/; no shell setup is needed
for subsequent top-level make commands.
"""
import argparse
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
from safe_tar import safe_extract

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build'
TOOLCHAINS = {
    'a8': ('armv7-eabihf--glibc--stable-2020.02-2',
           'https://toolchains.bootlin.com/downloads/releases/toolchains/armv7-eabihf/tarballs/armv7-eabihf--glibc--stable-2020.02-2.tar.bz2',
           '72993db0eb4b1d0f9896200eb6645e02affe039009d41749418082004a554fbc',
           'arm-linux-'),
    'z1mini': ('gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf',
               'https://developer.arm.com/-/media/Files/downloads/gnu-a/10.2-2020.11/binrel/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf.tar.xz',
               '102825ae56c9e00142d06f35d2bdd3299edb6060e84a275a25b095e66fd3fc2a',
               'arm-none-linux-gnueabihf-'),
}
AX_REVISION = 'd61e665f44ee145e5bb7cda67d64ac6be0b9097e'


def run(*args, **kwargs):
    subprocess.run(args, check=True, cwd=ROOT, **kwargs)


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


# Unpack the downloaded compiler archive into a temporary directory
def extract_toolchain(archive, destination):
    safe_extract(archive, destination)


def toolchain(name):
    directory, url, digest, prefix = TOOLCHAINS[name]
    base = BUILD / 'toolchains'
    base.mkdir(parents=True, exist_ok=True)
    target = base / directory
    stamp = target / '.apcam-sha256'
    if not (stamp.is_file() and stamp.read_text().strip() == digest):
        if target.exists():
            raise RuntimeError(f'Unverified toolchain directory already exists: {target}')
        archive = base / url.rsplit('/', 1)[-1]
        if not archive.exists():
            temporary = archive.with_name(archive.name + '.part')
            run('curl', '-fL', '--retry', '3', url, '-o', str(temporary))
            if sha256(temporary) != digest:
                raise RuntimeError(f'Toolchain checksum mismatch: {temporary}')
            temporary.replace(archive)
        if sha256(archive) != digest:
            raise RuntimeError(f'Toolchain checksum mismatch: {archive}')
        with tempfile.TemporaryDirectory(prefix='extract-', dir=base) as work:
            extract_toolchain(archive, work)
            (Path(work) / directory).rename(target)
        stamp.write_text(digest + '\n')
    compiler = str(target / 'bin' / prefix)
    run(compiler + 'gcc', '-dumpmachine')
    return compiler


def ax_headers():
    destination = BUILD / 'deps/axpi-bsp-sdk'
    if not destination.exists():
        run('git', 'clone', '--filter=blob:none', '--no-checkout',
            'https://github.com/sipeed/axpi_bsp_sdk.git', str(destination))
        run('git', '-C', str(destination), 'sparse-checkout', 'set', 'msp/out/include')
        run('git', '-C', str(destination), 'checkout', '--detach', AX_REVISION)
    revision = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != AX_REVISION:
        raise RuntimeError(f'Unexpected AX SDK revision in {destination}: {revision}')
    headers = destination / 'msp/out/include'
    if not (headers / 'ax_sys_api.h').is_file():
        raise RuntimeError(f'AX SDK headers are missing: {headers}')
    return str(headers)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-system', action='store_true')
    parser.add_argument('--with-tool-build-deps', action='store_true',
                        help='also install prerequisites for rebuilding the prebuilt MT11 tools')
    parser.add_argument('--targets', nargs='+', choices=['mt11', 'a8', 'zr10', 'z1mini'],
                        default=['mt11', 'a8', 'zr10', 'z1mini'])
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        parser.error('Hardware toolchains require x86_64 Linux; see windows/ for Windows SITL')
    if not args.skip_system:
        prefix = [] if os.geteuid() == 0 else ['sudo']
        if shutil.which('apt-get') is None:
            parser.error('Use Debian/Ubuntu, or install prerequisites yourself and use --skip-system')
        run(*prefix, 'apt-get', 'update')
        tool_build_deps = (['autoconf', 'automake', 'libtool', 'pkg-config',
                            'bison', 'flex', 'gawk'] if args.with_tool_build_deps else [])
        run(*prefix, 'apt-get', 'install', '-y', 'build-essential', 'gcc-aarch64-linux-gnu',
            'g++-aarch64-linux-gnu', 'python3-venv', 'python3-dev', 'git', 'curl', 'ca-certificates',
            'mtd-utils', 'fakeroot', 'zlib1g-dev', 'liblzo2-dev', 'libzstd-dev', 'liblzma-dev',
            'openssl', 'patch', 'zip', 'unzip', 'xz-utils', 'bzip2', 'ffmpeg', 'nodejs',
            'python3-av', 'python3-opencv', *tool_build_deps)
    BUILD.mkdir(exist_ok=True)
    env = BUILD / 'environment'
    if not (env / 'bin/python3').exists():
        run(sys.executable, '-m', 'venv', str(env))
    run(str(env / 'bin/python3'), '-m', 'pip', 'install', '-r', str(ROOT / 'tools/build-requirements.txt'))
    os.environ['PATH'] = str(env / 'bin') + os.pathsep + os.environ['PATH']
    run('git', 'submodule', 'update', '--init', '--recursive')
    run('sh', 'tools/bootstrap_dependencies.sh', str(BUILD / 'deps'))
    settings = {}
    if 'a8' in args.targets:
        settings['A8_CROSS_COMPILE'] = toolchain('a8')
    if 'zr10' in args.targets:
        run('sh', 'tools/bootstrap_zr10_toolchain.sh')
    if 'z1mini' in args.targets:
        settings['Z1MINI_CROSS_COMPILE'] = toolchain('z1mini')
        settings['Z1MINI_AX_SDK_INCLUDE'] = ax_headers()
    # Preserve settings for other targets when installing a subset later.
    config = BUILD / 'environment.mk'
    previous = config.read_text().splitlines() if config.exists() else []
    lines = [line for line in previous if ' ?= ' in line and line.split(' ?= ')[0] not in settings]
    lines += [f'{key} ?= {value}' for key, value in settings.items()]
    lines.append(f'export PATH := {env / "bin"}:$(PATH)')
    temporary = config.with_suffix('.mk.tmp')
    temporary.write_text('# Generated by tools/install_build_environment.py\n' + '\n'.join(lines) + '\n')
    temporary.replace(config)
    print('Build environment ready. Run make release (or select RELEASE_TARGETS).')


if __name__ == '__main__':
    main()
