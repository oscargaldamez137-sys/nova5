#!/usr/bin/env python3
"""Summarize live `proft` samples of a guest thread (16 words each: pc, then callers).

    guest-prof.py PROF MAP [--hz 1000] [--range 5:11] [--top 30]

Each sample is put in one bucket: guest code (by 64-byte granule of the pc, with the word at rsp
as the likely caller), an emulator function (the host code the guest called into, with the
guest return address that led there) or a DLL (waits and system calls, with the innermost
emulator frame).
"""
import argparse
import bisect
import collections
import struct

GUEST = (0x800000000, 0x1000000000)
HOST = (0x140001000, 0x142000000)


def load_map(path, base=0x140000000):
    entries = []
    for line in open(path, encoding='utf-8', errors='replace'):
        parts = line.split(None, 4)
        if len(parts) >= 5 and parts[2] == '0':
            try:
                entries.append((int(parts[0], 16) + base, parts[4].strip()))
            except ValueError:
                pass
    entries.sort()
    keys = [e[0] for e in entries]
    names = [e[1] for e in entries]

    def name(address):
        i = bisect.bisect_right(keys, address) - 1
        text = names[i] if i >= 0 else '?'
        for prefix in ('public: ', 'private: ', 'protected: ', 'static ', 'virtual '):
            if text.startswith(prefix):
                text = text[len(prefix):]
        return text.replace('__cdecl ', '').replace('Libs::Graphics::', '').replace('Libs::LibKernel::', '')[:110]
    return name


def is_guest(a):
    return GUEST[0] <= a < GUEST[1]


def is_host(a):
    return HOST[0] <= a < HOST[1]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('prof')
    p.add_argument('map')
    p.add_argument('--hz', type=float, default=1000.0)
    p.add_argument('--range')
    p.add_argument('--top', type=int, default=30)
    a = p.parse_args()
    data = open(a.prof, 'rb').read()
    samples = [struct.unpack_from('<16Q', data, i * 128) for i in range(len(data) // 128)]
    if a.range:
        lo, hi = (float(v) for v in a.range.split(':'))
        samples = samples[int(lo * a.hz):int(hi * a.hz)]
    name = load_map(a.map)
    kinds = collections.Counter()
    guest_pc = collections.Counter()
    guest_caller = collections.defaultdict(collections.Counter)
    host_fn = collections.Counter()
    host_entry = collections.defaultdict(collections.Counter)
    dll_owner = collections.Counter()
    for s in samples:
        pc = s[0]
        if pc == 0:
            continue
        if is_guest(pc):
            kinds['guest'] += 1
            granule = pc & ~0x3f
            guest_pc[granule] += 1
            guest_caller[granule][s[1] if is_guest(s[1]) else 0] += 1
            continue
        # First guest address up the stack: the guest call that entered the emulator.
        entry = next((w for w in s[1:] if is_guest(w)), 0)
        if is_host(pc):
            kinds['emulator'] += 1
            fn = name(pc)
            host_fn[fn] += 1
            host_entry[fn][entry] += 1
        else:
            kinds['dll'] += 1
            owner = next((name(w) for w in s[1:] if is_host(w)), '(no emulator frame)')
            dll_owner[(owner, entry)] += 1
    total = max(1, sum(kinds.values()))
    print(f'{total} samples: ' + ', '.join(f'{k} {100 * v / total:.1f}%' for k, v in kinds.most_common()))
    print('\nguest code by 64-byte granule (likely caller = word at rsp):')
    for granule, count in guest_pc.most_common(a.top):
        callers = ', '.join(f'{c:x}:{n}' for c, n in guest_caller[granule].most_common(3))
        print(f'  {100 * count / total:5.1f}%  {granule:012x}   callers {callers}')
    print('\nemulator functions (guest entry = first guest return address on the stack):')
    for fn, count in host_fn.most_common(a.top):
        entries = ', '.join(f'{e:x}:{n}' for e, n in host_entry[fn].most_common(3))
        print(f'  {100 * count / total:5.1f}%  {fn}   [{entries}]')
    print('\nDLL samples by innermost emulator frame and guest entry:')
    for (owner, entry), count in dll_owner.most_common(a.top):
        print(f'  {100 * count / total:5.1f}%  {owner}   [guest {entry:x}]')


if __name__ == '__main__':
    main()
