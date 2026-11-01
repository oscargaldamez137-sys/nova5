#!/usr/bin/env python3
"""Hot instructions of one function in a live `prof` capture: the leaf samples whose pc falls in
FUNCTION, grouped by pc, with the disassembly around the hottest ones (llvm-objdump on the
executable of the same build; the Windows build links at a fixed base).

    prof-hot-lines.py PROF MAP EXE FUNCTION [--range 9:13] [--top 12] [--context 6]
"""
import argparse
import collections
import importlib.util
import re
import struct
import subprocess
from pathlib import Path

OBJDUMP = r'C:\Program Files\LLVM\bin\llvm-objdump.exe'


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), Path(__file__).with_name(name))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    p = argparse.ArgumentParser()
    p.add_argument('prof')
    p.add_argument('map')
    p.add_argument('exe')
    p.add_argument('function')
    p.add_argument('--range', default='')
    p.add_argument('--top', type=int, default=12)
    p.add_argument('--context', type=int, default=6)
    p.add_argument('--hz', type=float, default=4000.0)
    args = p.parse_args()
    symbols = load('prof-slices.py').Symbols(args.map)
    data = Path(args.prof).read_bytes()
    words = struct.unpack(f'<{len(data) // 8}Q', data[: len(data) // 8 * 8])
    count = len(words) // 16
    first, last = 0, count
    if args.range:
        lo, hi = (float(v) for v in args.range.split(':'))
        first, last = int(lo * args.hz), min(count, int(hi * args.hz))
    pattern = re.compile(args.function)
    pcs = collections.Counter()
    total = 0
    for i in range(first, last):
        pc = words[i * 16]
        if pc == 0:
            continue
        total += 1
        if pattern.search(symbols(pc)):
            pcs[pc] += 1
    hits = sum(pcs.values())
    print(f'{hits} leaf samples in {args.function} of {total} ({100.0 * hits / max(total, 1):.2f}%)')
    for pc, n in pcs.most_common(args.top):
        print(f'\n== 0x{pc:x}: {n} samples ({100.0 * n / max(hits, 1):.1f}% of the function)')
        start = pc - 4 * args.context
        out = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', '-M', 'intel',
                              f'--start-address=0x{start:x}', f'--stop-address=0x{pc + 4 * args.context:x}',
                              args.exe], capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.match(r'\s*([0-9a-f]+):\s+(.*)', line)
            if m:
                address = int(m[1], 16)
                marker = '>>' if address == pc else '  '
                print(f'  {marker} {address:x}: {m[2]}')


if __name__ == '__main__':
    main()
