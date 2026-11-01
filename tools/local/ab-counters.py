#!/usr/bin/env python3
"""Counters of an ab-spot.ps1 run (its live.txt: LIVE_MEASURE / LIVE_COUNTERS per window labelled
A1, B1, B2, A2, ...): per-window values and the per-setting means of the chosen counters.

    ab-counters.py _Build/ab/<label>/live.txt [--keys backing_lock_wait_us,sync_downloads]
"""
import argparse
import collections
import re


def main():
    p = argparse.ArgumentParser()
    p.add_argument('live')
    p.add_argument('--keys', default='backing_lock_waits,backing_lock_wait_us,sync_downloads,srt_watched_reads')
    a = p.parse_args()
    keys = a.keys.split(',')
    windows = collections.OrderedDict()
    for line in open(a.live, encoding='utf-8', errors='replace'):
        m = re.search(r'LIVE_MEASURE .*label=([AB]\d+) .*fps=([\d.]+) render_ms=([\d.]+)', line)
        if m:
            windows.setdefault(m[1], {}).update(fps=float(m[2]), render=float(m[3]))
            continue
        m = re.search(r'LIVE_COUNTERS .*label=([AB]\d+) (.*)', line)
        if m:
            row = windows.setdefault(m[1], {})
            for pair in m[2].split():
                key, _, value = pair.partition('=')
                row[key] = float(value)
    columns = ['fps', 'render'] + keys
    print('win ' + ''.join(f'{k[:14]:>15s}' for k in columns))
    sums = collections.defaultdict(lambda: collections.defaultdict(float))
    counts = collections.Counter()
    for label, row in windows.items():
        print(f'{label:3s} ' + ''.join(f'{row.get(k, float("nan")):15.2f}' for k in columns))
        counts[label[0]] += 1
        for k in columns:
            sums[label[0]][k] += row.get(k, 0.0)
    for side in 'AB':
        if counts[side]:
            print(f'{side}   ' + ''.join(f'{sums[side][k] / counts[side]:15.2f}' for k in columns))


if __name__ == '__main__':
    main()
