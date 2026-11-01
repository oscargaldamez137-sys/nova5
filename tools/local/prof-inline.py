#!/usr/bin/env python3
"""Source-level view of a live `prof` capture: every sampled address is symbolized with its inlined
frames (llvm-symbolizer and the PDB of the same build, which needs line tables: -gline-tables-only),
so time inside a big function is split by the functions and lines inlined into it.

    prof-inline.py PROF EXE [--function REGEX [--callers N] [--depth N]] [--top 40] [--lines]
                   [--modules MODULES] [--range 10:22]

Without --function: self time by source function (innermost inlined frame) and inclusive time by
source function (every frame of every stack, inlined ones included). With --function: the samples
whose stack holds a frame matching REGEX, split by the innermost frame below it (--depth N: by the
chain of N frames below it; --lines: by source line; --callers N: by the N frames above the
outermost match instead). --range: only the
samples of that span of seconds (4 kHz). MODULES (prof-spot.ps1 writes modules.txt: base, size, name per line) names the
frames outside the executable by module.
"""
import argparse
import bisect
import collections
import json
import re
import struct
import subprocess
from pathlib import Path

SYMBOLIZER = r'C:\Program Files\LLVM\bin\llvm-symbolizer.exe'
BASE, END = 0x140001000, 0x142000000


def short(name):
    name = re.sub(r'\(.*$', '', name)
    return name.replace('Libs::Graphics::', '').replace('Libs::LibKernel::', '')[:100]


def symbolize(exe, addresses):
    """address -> [(function, file:line)], innermost first."""
    result = {}
    addresses = sorted(addresses)
    for i in range(0, len(addresses), 20000):
        chunk = addresses[i:i + 20000]
        out = subprocess.run([SYMBOLIZER, f'--obj={exe}', '--inlines', '--output-style=JSON'],
                             input='\n'.join(f'0x{a:x}' for a in chunk), capture_output=True, text=True).stdout
        for line in out.splitlines():
            entry = json.loads(line)
            address = int(entry['Address'], 16)
            frames = []
            for frame in entry.get('Symbol', []):
                function = frame.get('FunctionName') or '?'
                where = f"{Path(frame.get('FileName') or '?').name}:{frame.get('Line', 0)}"
                frames.append((short(function), where))
            result[address] = frames or [('?', '?')]
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument('prof')
    p.add_argument('exe')
    p.add_argument('--function', default='')
    p.add_argument('--top', type=int, default=40)
    p.add_argument('--lines', action='store_true')
    p.add_argument('--callers', type=int, default=0)
    p.add_argument('--depth', type=int, default=1)
    p.add_argument('--modules', default='')
    p.add_argument('--range', default='')
    p.add_argument('--hz', type=float, default=4000.0)
    args = p.parse_args()
    data = Path(args.prof).read_bytes()
    words = struct.unpack(f'<{len(data) // 8}Q', data[:len(data) // 8 * 8])
    samples = [words[i:i + 16] for i in range(0, len(words) - 15, 16)]
    if args.range:
        lo, hi = (float(v) for v in args.range.split(':'))
        samples = samples[int(lo * args.hz):int(hi * args.hz)]
    samples = [s for s in samples if s[0]]
    modules = []
    if args.modules:
        for line in open(args.modules):
            base, size, name = line.split(None, 2)
            modules.append((int(base, 16), int(size, 16), name.strip()))
        modules.sort()

    def module(address):
        i = bisect.bisect_right([m[0] for m in modules], address) - 1
        if i >= 0 and address < modules[i][0] + modules[i][1]:
            return modules[i][2]
        return 'guest' if 0x800000000 <= address < 0x1000000000 else 'dll'

    own = {a for s in samples for a in s if BASE <= a < END}
    # Return addresses point after the call: symbolize the call instruction.
    table = symbolize(args.exe, own | {a - 1 for s in samples for a in s[1:] if BASE <= a < END})

    def frames(sample):
        """Outermost-last list of (function, line) for one stack, inlined frames expanded."""
        out = []
        for k, a in enumerate(sample):
            if not a:
                continue
            if BASE <= a < END:
                out.extend(table.get(a if k == 0 else a - 1, [('?', '?')]))
            elif k == 0:
                out.append((module(a), ''))
        return out

    total = len(samples)
    if not args.function:
        self_counts, inclusive = collections.Counter(), collections.Counter()
        for s in samples:
            f = frames(s)
            if not f:
                continue
            self_counts[f[0][0]] += 1
            for name in {x[0] for x in f}:
                inclusive[name] += 1
        print(f'== self by source function ({total} samples)')
        for name, n in self_counts.most_common(args.top):
            print(f'{100 * n / total:6.2f}% {n:7d}  {name}')
        print(f'== inclusive by source function')
        for name, n in inclusive.most_common(args.top):
            print(f'{100 * n / total:6.2f}% {n:7d}  {name}')
        return
    pattern = re.compile(args.function)
    below = collections.Counter()
    hits = 0
    for s in samples:
        f = frames(s)
        at = next((k for k, x in enumerate(f) if pattern.search(x[0])), None)
        if at is None:
            continue
        hits += 1
        if args.callers:
            top = max(k for k, x in enumerate(f) if pattern.search(x[0]))
            below[' < '.join(f'{x[0]} @{x[1]}' for x in f[top + 1:top + 1 + args.callers])] += 1
            continue
        leaf = f[0] if at == 0 else f[at - 1]
        key = f'{f[0][0]}  @ {f[0][1]}' if args.lines else (leaf[0] if at else '(self) ' + f[0][0])
        if args.depth > 1 and not args.lines and at:
            key = ' > '.join(x[0] for x in reversed(f[max(0, at - args.depth):at]))
        below[key] += 1
    print(f'{hits} samples with {args.function} ({100 * hits / max(total, 1):.2f}% of {total})')
    for key, n in below.most_common(args.top):
        print(f'{100 * n / max(hits, 1):6.2f}% {n:7d}  {key}')


if __name__ == '__main__':
    main()
