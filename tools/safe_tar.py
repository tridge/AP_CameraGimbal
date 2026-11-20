#!/usr/bin/env python3
"""Extract pinned toolchain tarballs with equivalent safety on old Python."""
import copy
import inspect
import os
from pathlib import Path, PurePosixPath
import sys
import tarfile


def _inside(path, root):
    try:
        resolved = os.path.realpath(str(path))
        root = os.path.realpath(str(root))
        return resolved != root and os.path.commonpath((resolved, root)) == root
    except (OSError, RuntimeError, ValueError):
        return False


def safe_extract(archive, destination):
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    if hasattr(archive, 'read'):
        tar_context = tarfile.open(fileobj=archive, mode='r:*')
    else:
        tar_context = tarfile.open(archive, mode='r:*')
    with tar_context as tar:
        if callable(getattr(tarfile, 'data_filter', None)):
            tar.extractall(destination, filter='data')
            return

        # Apply each member only after validating it against the filesystem
        # produced by prior members. This catches chained symlink escapes.
        directories = []
        for original in tar:
            member = copy.copy(original)
            name = PurePosixPath(member.name)
            if name.is_absolute() or '..' in name.parts:
                raise RuntimeError(f'Unsafe path in toolchain archive: {member.name}')
            if not name.parts:
                if member.isdir():
                    continue
                raise RuntimeError(f'Unsafe path in toolchain archive: {member.name}')
            target = destination.joinpath(*name.parts)
            if not _inside(target, destination):
                raise RuntimeError(f'Unsafe path in toolchain archive: {member.name}')

            if member.issym():
                link = PurePosixPath(member.linkname)
                if link.is_absolute():
                    raise RuntimeError(f'Unsafe symlink in toolchain archive: {member.name}')
                link_target = target.parent.joinpath(*link.parts)
                if not _inside(link_target, destination):
                    raise RuntimeError(f'Unsafe symlink in toolchain archive: {member.name}')
            elif member.islnk():
                link = PurePosixPath(member.linkname)
                if link.is_absolute() or '..' in link.parts or not link.parts:
                    raise RuntimeError(f'Unsafe hardlink in toolchain archive: {member.name}')
                link_target = destination.joinpath(*link.parts)
                if not _inside(link_target, destination) or not link_target.is_file():
                    raise RuntimeError(f'Unsafe hardlink in toolchain archive: {member.name}')
            elif not (member.isdir() or member.isreg()):
                raise RuntimeError(f'Unsupported archive member: {member.name}')

            # Resolve the full destination again now that earlier symlinks
            # exist, and reject replacing a symlink itself.
            if target.is_symlink() or not _inside(target, destination):
                raise RuntimeError(f'Unsafe archive member: {member.name}')
            if member.isdir():
                # Ignore archive directory permissions like data_filter. Do
                # not pass mode=None to old tarfile: its chmod cannot handle it.
                target.mkdir(parents=True, exist_ok=True)
                directories.append((member, target))
                continue
            if not member.issym():
                member.mode &= 0o755
                if not (member.mode & 0o100):
                    member.mode &= ~0o111
                member.mode |= 0o600
            # Skip archive ownership entirely, including when run as root.
            # Apply only the validated file mode and timestamp ourselves.
            if 'filter' in inspect.signature(tar.extract).parameters:
                tar.extract(member, destination, set_attrs=False, filter='fully_trusted')
            else:
                tar.extract(member, destination, set_attrs=False)
            if not member.issym():
                tar.chmod(member, target)
                tar.utime(member, target)
        for member, target in reversed(directories):
            tar.utime(member, target)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: safe_tar.py ARCHIVE DESTINATION')
    safe_extract(sys.argv[1], sys.argv[2])
