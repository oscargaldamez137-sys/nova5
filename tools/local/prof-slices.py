#!/usr/bin/env python3
"""Time-sliced view of a live `prof` capture (render thread, 4000 samples/s, 16 words each: pc
and the unwound callers), symbolized with the lld map of the same build.

    prof-slices.py PROF MAP [--window 0.5]             share of each category per window
    prof-slices.py PROF MAP --range 19.5:22 [--top 25]  top functions (self and inclusive) there

Categories match any frame of the stack, so they overlap (a draw that initializes a texture
counts for both).
"""
import argparse
import bisect
import collections
import re
import struct

CATEGORIES = {
    'pipeline': r'CreatePipelineInternal|CreateGraphicsPipeline|CreateComputePipeline|CompilePermutation',
    'teximg': r'TextureCache::InitializeImage|ObtainBufferForImage|TileManager|Tiler',
    'xpr': r'NativeXprRetrace|NativeXprGather|NativeXprRebind|NativeXprReevaluate|NativeXprStore',
    'guestcmd': r'SendCommandSync',
    'unmap': r'GpuResourceManager::UnmapMemory',
    'map': r'GpuResourceManager::MapMemory',
    'dispatch': r'RenderExecutor::DispatchDirect',
    'draw': r'RenderExecutor::ExecutePreparedDraw',
    'wait': r'Wait|Sleep|CondVar',
}


class Symbols:
    def __init__(self, path, base=0x140000000):
        entries = []
        for line in open(path, encoding='utf-8', errors='replace'):
            parts = line.split(None, 4)
            if len(parts) >= 5 and parts[2] == '0':
                try:
                    entries.append((int(parts[0], 16) + base, parts[4].strip()))
                except ValueError:
                    pass
        entries.sort()
        self.keys = [e[0] for e in entries]
        self.names = [e[1] for e in entries]
        self.cache = {}

    def __call__(self, address):
        name = self.cache.get(address)
        if name is None:
            if 0x140001000 <= address < 0x142000000:
                i = bisect.bisect_right(self.keys, address) - 1
                name = self.names[i] if i >= 0 else '?'
                for prefix in ('public: ', 'private: ', 'protected: ', 'static ', 'virtual '):
                    if name.startswith(prefix):
                        name = name[len(prefix):]
                name = name.replace('__cdecl ', '')
            elif 0x800000000 <= address < 0x1000000000:
                name = 'guest'
            else:
                name = 'dll'
            self.cache[address] = name
        return name


def main():
    p = argparse.ArgumentParser()
    p.add_argument('prof')
    p.add_argument('map')
    p.add_argument('--window', type=float, default=0.5)
    p.add_argument('--range')
    p.add_argument('--top', type=int, default=25)
    p.add_argument('--hz', type=float, default=4000.0, help='sampling rate (KYTY_PROF_HZ of the capture)')
    args = p.parse_args()
    data = open(args.prof, 'rb').read()
    samples = [struct.unpack_from('<16Q', data, i * 128) for i in range(len(data) // 128)]
    name = Symbols(args.map)
    if args.range:
        lo, hi = (float(v) for v in args.range.split(':'))
        chunk = samples[int(lo * args.hz):int(hi * args.hz)]
        self_counts, inclusive = collections.Counter(), collections.Counter()
        for s in chunk:
            stack = [name(a) for a in s if a]
            if not stack:
                continue
            self_counts[stack[0]] += 1
            for f in set(stack):
                inclusive[f] += 1
        total = max(1, len(chunk))
        print(f'{len(chunk)} samples, {lo}-{hi} s; self:')
        for f, c in self_counts.most_common(args.top):
            print(f'  {100 * c / total:5.1f}%  {f[:140]}')
        print('inclusive:')
        for f, c in inclusive.most_common(args.top):
            print(f'  {100 * c / total:5.1f}%  {f[:140]}')
        return
    patterns = {k: re.compile(v) for k, v in CATEGORIES.items()}
    step = int(args.window * args.hz)
    print('    t_s ' + ''.join(f'{k:>10s}' for k in CATEGORIES))
    for w0 in range(0, len(samples), step):
        chunk = samples[w0:w0 + step]
        counts = dict.fromkeys(CATEGORIES, 0)
        for s in chunk:
            joined = '|'.join(name(a) for a in s if a)
            for k, pattern in patterns.items():
                if pattern.search(joined):
                    counts[k] += 1
        print(f'{w0 / args.hz:7.1f} ' + ''.join(f'{100 * counts[k] / len(chunk):9.1f}%' for k in CATEGORIES))


if __name__ == '__main__':
    main()
