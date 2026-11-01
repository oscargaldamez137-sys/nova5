#!/usr/bin/env python3
"""Callee breakdown of one function in a live `prof` capture (render thread, 4000 samples/s,
16 words each: pc and the unwound callers), symbolized with the lld map of the same build.

    prof-callees.py PROF MAP FUNCTION [--range 10:15] [--depth 2] [--top 25]

For the samples whose stack holds FUNCTION (a regex), shows how that time splits by the
functions it calls, down to --depth levels (a path per line), as a share of FUNCTION's samples.
"""
import argparse
import collections
import importlib.util
import re
import struct
from pathlib import Path


def load_slices():
    spec = importlib.util.spec_from_file_location('prof_slices', Path(__file__).with_name('prof-slices.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def short(name):
    name = re.sub(r'^(public|private|protected): ', '', name)
    name = re.sub(r'\(.*$', '', name)
    name = re.sub(r'^.*? __cdecl ', '', name)
    return name.replace('Libs::Graphics::', '').replace('Libs::LibKernel::', '')[:90]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('prof')
    p.add_argument('map')
    p.add_argument('function')
    p.add_argument('--range', default='')
    p.add_argument('--depth', type=int, default=1)
    p.add_argument('--top', type=int, default=30)
    p.add_argument('--hz', type=float, default=4000.0)
    p.add_argument('--callers', action='store_true', help='the chain above FUNCTION instead of below')
    args = p.parse_args()
    slices = load_slices()
    symbols = slices.Symbols(args.map)
    data = Path(args.prof).read_bytes()
    words = struct.unpack(f'<{len(data) // 8}Q', data[: len(data) // 8 * 8])
    count = len(words) // 16
    first, last = 0, count
    if args.range:
        lo, hi = (float(v) for v in args.range.split(':'))
        first, last = int(lo * args.hz), min(count, int(hi * args.hz))
    pattern = re.compile(args.function)
    total = 0
    paths = collections.Counter()
    for i in range(first, last):
        stack = [w for w in words[i * 16:(i + 1) * 16] if w]
        names = [symbols(w) for w in stack]
        if args.callers:
            # Innermost match: the caller chain above it.
            at = next((k for k in range(len(names)) if pattern.search(names[k])), None)
            if at is None:
                continue
            total += 1
            chain = [short(names[k]) for k in range(at + 1, min(len(names), at + 1 + args.depth))]
            paths[' < '.join(chain) if chain else '(top)'] += 1
            continue
        # Outermost match: the callee chain below it (stack[0] is the leaf).
        at = next((k for k in range(len(names) - 1, -1, -1) if pattern.search(names[k])), None)
        if at is None:
            continue
        total += 1
        chain = []
        for k in range(at - 1, max(-1, at - 1 - args.depth), -1):
            chain.append(short(names[k]))
        paths[' > '.join(chain) if chain else '(self)'] += 1
    print(f'{total} samples with {args.function} ({(last - first)} in range)')
    for path, n in paths.most_common(args.top):
        print(f'{100.0 * n / max(total, 1):6.2f}% {n:6d}  {path}')


if __name__ == '__main__':
    main()
