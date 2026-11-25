#!/usr/bin/env python3
"""Verify target payloads and ensure corrupt tools cannot replace a header."""
from pathlib import Path
import importlib.util
import json
import shutil
import struct
import tempfile

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('vpn_resources',ROOT/'tools/build_vpn_resources.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
notices=ROOT/'web/webroot/licenses/vpn'
for source,name in [(ROOT/'packaging/vpn'/n,n) for n in ('LICENSE-GPL-2.0.txt','LICENSE-MPL.txt','LICENSE-ZeroTier.txt')] + [
    (ROOT/'packaging/vpn/licenses'/n,n.split('/')[0]+'-LICENSE.txt') for n in
    ('http-parser/LICENSE-MIT','libnatpmp/LICENSE','miniupnpc/LICENSE','nlohmann/LICENSE.MIT')]:
    assert (notices/name).read_bytes()==source.read_bytes(),name
sources=(notices/'SOURCES.txt').read_text()
assert '1.16.2' in sources and 'linux-4.9.227.tar.xz' in sources and 'linux-4.19.90.tar.xz' in sources
for target,machine in [('mt11',183),('zt30',40)]:
    manifest,data=m.resources(ROOT/'packaging/vpn'/target)
    assert manifest['kernel_size']>0 and len(manifest['kernel_sha256'])==64
    for name,blob in data.items():
        assert blob[:4]==b'\x7fELF' and struct.unpack_from('<H',blob,18)[0]==machine,(target,name)
    print(target, {name:len(blob) for name,blob in data.items()})
with tempfile.TemporaryDirectory(prefix='vpn-resources-') as directory:
    directory=Path(directory)
    source=directory/'vpn';shutil.copytree(ROOT/'packaging/vpn',source)
    header=directory/'vpn.h';m.build(source,header)
    before=header.read_bytes()
    p=source/'mt11/manifest.json';manifest=json.loads(p.read_text());manifest['files']['zerotier-one']['sha256']='0'*64;p.write_text(json.dumps(manifest))
    try:m.build(source,header)
    except ValueError:pass
    else:raise AssertionError('corrupt client accepted')
    assert header.read_bytes()==before
    # Reject malformed/mistyped compatibility pins before publishing a header.
    p=source/'mt11/manifest.json';original=json.loads((ROOT/'packaging/vpn/mt11/manifest.json').read_text())
    for extra in ('*',['4.9.227'],['A'*64],[original['kernel_sha256']],['0'*64]*8):
        manifest=dict(original,compatible_kernel_sha256=extra);p.write_text(json.dumps(manifest))
        try:m.build(source,header)
        except ValueError:pass
        else:raise AssertionError('invalid kernel allowlist accepted: '+repr(extra))
        assert header.read_bytes()==before
    manifest=dict(original,compatible_kernel_sha256=['0'*64]);p.write_text(json.dumps(manifest))
    m.build(source,header)
    assert '"'+original['kernel_sha256']+'","'+'0'*64+'"' in header.read_text()
print('PASS pinned VPN resources, target architecture and rejected-corruption rollback')
