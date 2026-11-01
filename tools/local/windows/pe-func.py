#!/usr/bin/env python3
"""pe-func.py EXE RVA...: the .pdata function range (begin, end) containing each RVA, so a
disassembler can show the whole function even where the PDB has no symbol for it."""
import bisect
import struct
import sys
from pathlib import Path


def sections(data):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    count = struct.unpack_from('<H', data, pe + 6)[0]
    optional = struct.unpack_from('<H', data, pe + 20)[0]
    table = pe + 24 + optional
    for i in range(count):
        name, vsize, va, rsize, raw = struct.unpack_from('<8sIIII', data, table + 40 * i)
        yield name.rstrip(b'\0').decode(), va, vsize, raw


def main():
    data = Path(sys.argv[1]).read_bytes()
    pdata = next(s for s in sections(data) if s[0] == '.pdata')
    _, va, vsize, raw = pdata
    entries = sorted(struct.unpack_from('<III', data, raw + 12 * i) for i in range(vsize // 12))
    begins = [e[0] for e in entries]
    for text in sys.argv[2:]:
        rva = int(text, 16)
        i = bisect.bisect_right(begins, rva) - 1
        if i >= 0 and entries[i][0] <= rva < entries[i][1]:
            print(f'{rva:#x}: function {entries[i][0]:#x}-{entries[i][1]:#x} (+{rva - entries[i][0]:#x})')
        else:
            print(f'{rva:#x}: no .pdata entry (leaf function without unwind info)')


if __name__ == '__main__':
    main()
