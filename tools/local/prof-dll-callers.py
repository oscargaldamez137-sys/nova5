#!/usr/bin/env python3
"""Where the render thread's time inside DLLs (driver, heap, CRT, kernel calls) comes from: each
sample whose leaf is outside the executable is charged to the first executable frame above it.

    prof-dll-callers.py PROF MAP [--range 9:13] [--top 30] [--depth 1]
"""
import argparse
import collections
import importlib.util
import re
import struct
from pathlib import Path


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), Path(__file__).with_name(name))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    p = argparse.ArgumentParser()
    p.add_argument('prof')
    p.add_argument('map')
    p.add_argument('--range', default='')
    p.add_argument('--top', type=int, default=30)
    p.add_argument('--depth', type=int, default=1, help='executable frames per path')
    p.add_argument('--hz', type=float, default=4000.0)
    args = p.parse_args()
    slices = load('prof-slices.py')
    short = load('prof-callees.py').short
    symbols = slices.Symbols(args.map)
    data = Path(args.prof).read_bytes()
    words = struct.unpack(f'<{len(data) // 8}Q', data[: len(data) // 8 * 8])
    count = len(words) // 16
    first, last = 0, count
    if args.range:
        lo, hi = (float(v) for v in args.range.split(':'))
        first, last = int(lo * args.hz), min(count, int(hi * args.hz))
    total = 0
    in_dll = 0
    paths = collections.Counter()
    for i in range(first, last):
        stack = [w for w in words[i * 16:(i + 1) * 16] if w]
        if not stack:
            continue
        total += 1
        names = [symbols(w) for w in stack]
        if names[0] != 'dll':
            continue
        in_dll += 1
        own = [short(n) for n in names if n != 'dll']
        paths[' < '.join(own[:args.depth]) if own else '(no executable frame)'] += 1
    print(f'{in_dll} of {total} samples ({100.0 * in_dll / max(total, 1):.1f}%) have their leaf in a DLL')
    for path, n in paths.most_common(args.top):
        print(f'{100.0 * n / max(total, 1):6.2f}% of all {n:6d}  {path}')


if __name__ == '__main__':
    main()
