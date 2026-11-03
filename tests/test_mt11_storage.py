#!/usr/bin/env python3
"""Exercise MT11 storage startup with an isolated card, mount table and mounter."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]
source = (REPO / 'packaging/mt11/app_init.sh').read_text()


def function(name):
    # Extract only named definitions. A missing/renamed helper fails closed;
    # changes to comments cannot make us execute pinmux or service startup.
    match = re.search(r'^' + re.escape(name) + r'\(\)\n\{\n.*?^\}\n', source, re.M | re.S)
    if not match:
        raise ValueError(f'Missing shell helper: {name}')
    return match[0]


helpers = '\n'.join(function(name) for name in ('log', 'mount_microsd', 'prepare_media_storage'))
# Exercise the real startup call as well as the helpers, without executing
# anything else from the boot script. Only these defined functions are allowed.
boot_calls = re.findall(r'^(\w+) \|\| true$', source, re.M)
if len(boot_calls) != 1 or boot_calls[0] not in ('prepare_media_storage', 'mount_microsd'):
    raise ValueError('Unexpected storage startup call')
boot_call = boot_calls[0] + ' || true'


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise ValueError(f'Unexpected hardware reference: {old}')
    return text.replace(old, new)


# Redirect every hardware path before executing, and represent device discovery
# with a regular file. Keep the mount helper's actual branching and exit status.
helpers = replace(helpers, 'sd_device=/dev/mmcblk1p1', 'sd_device=$TEST_SD_DEVICE')
helpers = replace(helpers, '-b "$sd_device"', '-f "$sd_device"', 2)
helpers = replace(helpers, '/proc/mounts', '"$TEST_MOUNTS"')
helpers = replace(helpers, "'[[:space:]]/mnt[[:space:]]'", '"[[:space:]]$TEST_MEDIA[[:space:]]"')
helpers = replace(helpers, '-d /mnt', '-d "$TEST_MEDIA"')
helpers = replace(helpers, '"$sd_device" /mnt', '"$sd_device" "$TEST_MEDIA"', 2)
helpers = replace(helpers, '/mnt/DCIM/capture', '"$TEST_MEDIA/DCIM/capture"')
helpers = replace(helpers, '/mnt/DCIM/record', '"$TEST_MEDIA/DCIM/record"')

fake_hardware = '''
mount()
{
    printf '%s\\n' "$2" >> "$TEST_MOUNT_CALLS"
    [ "$TEST_MOUNT_RESULT" = "$2" ]
}
sleep() { :; }
'''

with tempfile.TemporaryDirectory(prefix='mt11-storage-') as temporary:
    root = Path(temporary)

    def prepare(name, mounted=False, device=True, mountpoint=True, mount_result='exfat',
                blocked=False, boot=False):
        case = root / name
        case.mkdir(exist_ok=True)
        media = case / 'media'
        if mountpoint:
            media.mkdir(exist_ok=True)
        if blocked:
            (media / 'DCIM').write_text('not a directory')
        card = case / 'card'
        if device:
            card.touch()
        mounts = case / 'mounts'
        mounts.write_text(f'{card} {media} exfat rw 0 0\n' if mounted else '')
        calls = case / 'mount-calls'
        calls.write_text('')
        env = dict(os.environ,
                   TEST_SD_DEVICE=str(card), TEST_MEDIA=str(media), TEST_MOUNTS=str(mounts),
                   TEST_MOUNT_CALLS=str(calls), TEST_MOUNT_RESULT=mount_result)
        invocation = boot_call if boot else 'prepare_media_storage'
        result = subprocess.run(['sh'], input=helpers + fake_hardware + '\n' + invocation + '\n',
                                env=env, text=True, capture_output=True, timeout=5)
        return media, result, calls.read_text().splitlines()

    media, result, calls = prepare('already-mounted', mounted=True)
    assert result.returncode == 0, result.stderr
    assert calls == []
    assert (media / 'DCIM/capture').is_dir()
    assert (media / 'DCIM/record').is_dir()
    photo = media / 'DCIM/capture/existing.jpg'
    photo.write_bytes(b'existing capture')
    _, result, calls = prepare('already-mounted', mounted=True)
    assert result.returncode == 0, result.stderr
    assert calls == [] and photo.read_bytes() == b'existing capture'

    for filesystem, expected_calls in [('exfat', ['exfat']), ('vfat', ['exfat', 'vfat'])]:
        media, result, calls = prepare(filesystem, mount_result=filesystem)
        assert result.returncode == 0, result.stderr
        assert calls == expected_calls
        assert (media / 'DCIM/capture').is_dir() and (media / 'DCIM/record').is_dir()

    for name, options, expected_calls in [
        ('no-device', {'device': False}, []),
        ('no-mountpoint', {'mountpoint': False}, []),
        ('mount-failed', {'mount_result': 'fail'}, ['exfat', 'vfat']),
    ]:
        media, result, calls = prepare(name, **options)
        assert result.returncode != 0
        assert calls == expected_calls
        assert not (media / 'DCIM').exists(), 'must not write to rootfs without a mounted card'

    media, result, calls = prepare('unwritable', mounted=True, blocked=True)
    assert result.returncode != 0 and calls == []
    assert 'could not prepare photo and recording directories' in result.stderr
    assert (media / 'DCIM').read_text() == 'not a directory'

    media, result, calls = prepare('boot-with-card', boot=True)
    assert result.returncode == 0 and calls == ['exfat'], result.stderr
    assert (media / 'DCIM/capture').is_dir() and (media / 'DCIM/record').is_dir()
    media, result, calls = prepare('boot-without-card', device=False, boot=True)
    assert result.returncode == 0 and calls == [], 'missing card must not abort boot'
    assert not (media / 'DCIM').exists()

print('PASS MT11 storage: existing/exFAT/FAT cards, missing device/mountpoint, mount/mkdir failures')
