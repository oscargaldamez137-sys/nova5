#!/usr/bin/env python3
"""Per-second table of a `walk-run.ps1 -Measure -PerSecond` log (LIVE_MEASURE / LIVE_COUNTERS
lines labelled s0, s1, ...), or several logs side by side (fps and render CPU per second).

    persec-report.py RUN.txt [--keys xpr_tries,backing_read_bytes]
    persec-report.py BASE.txt NEW.txt ...
"""
import argparse
import re

DEFAULT_KEYS = ['backing_read_bytes', 'async_image_bytes', 'xpr_tries', 'xpr_stores', 'xpr_miss_key',
                'region_syncs', 'guest_commands', 'protect_calls_render']


def parse(path):
    rows = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.search(r'LIVE_MEASURE .*label=s(\d+) .*fps=([\d.]+) render_ms=([\d.]+) record_ms=([\d.]+) '
                      r'render_busy=([\d.]+)', line)
        if m:
            rows.setdefault(int(m[1]), {}).update(fps=float(m[2]), render=float(m[3]), record=float(m[4]),
                                                   busy=float(m[5]))
            continue
        m = re.search(r'LIVE_COUNTERS .*label=s(\d+) (.*)', line)
        if m:
            row = rows.setdefault(int(m[1]), {})
            for pair in m[2].split():
                key, _, value = pair.partition('=')
                row[key] = float(value)
    return rows


def main():
    p = argparse.ArgumentParser()
    p.add_argument('runs', nargs='+')
    p.add_argument('--keys', default=','.join(DEFAULT_KEYS))
    a = p.parse_args()
    runs = [parse(path) for path in a.runs]
    if len(runs) > 1:
        print('sec ' + ''.join(f'{"fps/render " + str(i):>18s}' for i in range(len(runs))))
        for second in sorted(set().union(*runs)):
            cells = [f'{r[second]["fps"]:8.1f}{r[second]["render"]:8.1f}ms' if second in r else ' ' * 18
                     for r in runs]
            print(f'{second:3d} ' + ''.join(cells))
        for i, r in enumerate(runs):
            walk = [r[s]['fps'] for s in sorted(r) if 1 <= s <= 11]
            if walk:
                print(f'run {i}: walking seconds 1-11 min {min(walk):.1f} mean {sum(walk) / len(walk):.1f} fps')
        return
    keys = [k for k in a.keys.split(',') if k]
    print('sec   fps render record busy ' + ' '.join(f'{k[:14]:>14s}' for k in keys))
    for second, row in sorted(runs[0].items()):
        print(f'{second:3d} {row.get("fps", 0):5.1f} {row.get("render", 0):6.1f} {row.get("record", 0):6.1f} '
              f'{row.get("busy", 0):4.0f} ' + ' '.join(f'{row.get(k, 0):14.1f}' for k in keys))


if __name__ == '__main__':
    main()
