#!/usr/bin/env python3
"""Audit captured WGSL (brush-worker.js captureShaders) for function-scope `var`s declared
without an initializer inside a loop body. Chrome 155 Linux/Vulkan does not re-zero those on each
iteration (notes/stage4.md); code relying on the implicit zero would be wrong on the laptop.

usage: audit_wgsl.py DIR_OF_.wgsl"""
import re
import sys
from pathlib import Path

LOOP = re.compile(r'\b(loop|while)\b[^{;]*\{|\bfor\s*\(.*\)\s*\{')
VAR = re.compile(r'^\s*var(?:<function>)?\s+(\w+)\s*:\s*([^=;]+?)\s*(=|;)')


def audit(code):
    findings, uninit_total, in_loop_total = [], 0, 0
    stack = []  # one entry per open brace: True if it opens a loop body
    for n, line in enumerate(code.splitlines(), 1):
        m = VAR.match(line)
        if m:
            in_loop_total += any(stack)
            if m.group(3) == ';':
                uninit_total += 1
                if any(stack):
                    findings.append((n, line.strip()))
        opens_loop = bool(LOOP.search(line))
        for ch in line:
            if ch == '{':
                stack.append(opens_loop)
                opens_loop = False
            elif ch == '}' and stack:
                stack.pop()
    return findings, uninit_total, in_loop_total


def first_use(lines, start, name):
    """How the variable declared at lines[start] is first used: 'write' when the first later line
    that mentions it (directly or through `let p = &name;`) assigns it at the declaration's brace
    depth, otherwise 'read' or 'conditional' (needs a manual look)."""
    names = {name}
    depth = 0
    for line in lines[start + 1:]:
        m = re.match(r'\s*let\s+(\w+)\s*=\s*&(\w+)\s*;', line)
        if m and m.group(2) in names:
            names.add(m.group(1))
            continue
        used = [x for x in names if re.search(r'\b%s\b' % re.escape(x), line)]
        if used:
            write = any(re.match(r'\s*\*?%s(\[[^\]]*\])?\s*=[^=]' % re.escape(x), line) for x in used)
            whole = any(re.match(r'\s*\*?%s\s*=[^=]' % re.escape(x), line) for x in used)
            if write and whole and depth == 0:
                return 'write'
            return 'conditional' if depth else 'read'
        depth += line.count('{') - line.count('}')
        if depth < 0:
            return 'unused'
    return 'unused'


def main():
    files = sorted(Path(sys.argv[1]).glob('*.wgsl'))
    total = uninit_all = in_loop_all = 0
    kinds = {}
    for f in files:
        found, uninit, in_loop = audit(f.read_text())
        total += len(found)
        uninit_all += uninit
        in_loop_all += in_loop
        lines = f.read_text().splitlines()
        for n, line in found:
            kind = first_use(lines, n - 1, VAR.match(line).group(1))
            kinds[kind] = kinds.get(kind, 0) + 1
            if kind != 'write':
                print('%s:%d: %s  [first use: %s]' % (f.name, n, line, kind))
    print('%d shaders: %d function-scope vars declared inside loops, %d uninitialised anywhere, '
          '%d uninitialised inside loops (first use: %s)' % (len(files), in_loop_all, uninit_all, total, kinds))


if __name__ == '__main__':
    main()
