#!/usr/bin/env python3
"""Side-by-side render-thread census totals (live `census` output files).

    census-compare.py A.txt [B.txt ...]

Per call kind: ms per frame and calls per frame; then the dispatch phases (kind 3, b = phase).
"""
import collections
import re
import sys

KINDS = {0: 'dispatch', 1: 'draw', 2: 'native xpr', 3: 'dispatch phase', 4: 'gpu wait', 5: 'readback wait',
         6: 'sync download', 7: 'graphics programs', 8: 'native gather', 9: 'queue run', 10: 'srt interpreter',
         11: 'command sync'}


def load(path):
    kinds, phases = collections.defaultdict(lambda: [0.0, 0.0]), collections.defaultdict(lambda: [0.0, 0.0])
    frames = None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.search(r'LIVE_CENSUS id=\d+ frames=(\d+)', line)
        if m:
            frames = int(m[1])
            continue
        m = re.search(r'kind=(\d+) a=([0-9a-f]+) b=([0-9a-f]+) calls_per_frame=([\d.]+) ms_per_frame=([\d.]+)', line)
        if not m:
            continue
        kind, b, calls, ms = int(m[1]), int(m[3], 16), float(m[4]), float(m[5])
        kinds[kind][0] += ms
        kinds[kind][1] += calls
        if kind == 3:
            phases[b & 0xff][0] += ms
            phases[b & 0xff][1] += calls
    return frames, kinds, phases


def main():
    runs = [load(p) for p in sys.argv[1:]]
    print('frames: ' + '  '.join(str(r[0]) for r in runs))
    print(f'{"kind":20s}' + ''.join(f'{"ms  calls":>20s}' for _ in runs))
    for kind in sorted(set().union(*(r[1].keys() for r in runs))):
        print(f'{KINDS.get(kind, kind):20s}' +
              ''.join(f'{r[1][kind][0]:9.2f} {r[1][kind][1]:9.0f} ' for r in runs))
    print('dispatch phases (us per dispatch):')
    for phase in sorted(set().union(*(r[2].keys() for r in runs))):
        print(f'  phase {phase:<12d}' + ''.join(
            f'{r[2][phase][0]:9.3f} {1000 * r[2][phase][0] / max(r[2][phase][1], 1):8.2f}us ' for r in runs))


if __name__ == '__main__':
    main()
