#!/usr/bin/env python3
"""Slow renderer calls (KYTY_SLOW_LOG_MS lines) on a walk's timeline (tools/local/windows/walk-run.ps1).

    slow-timeline.py TRACE LOG [--frames 60] [--around 0.4]

Takes the slowest frames of the live trace and prints, for each, the slow-log lines whose TSC
stamp falls within --around seconds before its flip (the lines of the calls that made it slow),
plus the total slow time per operation over the walk.
"""
import argparse
import collections
import importlib.util
import re
from pathlib import Path


def load_trace_module():
    spec = importlib.util.spec_from_file_location('live_trace', Path(__file__).with_name('live-trace.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


LINE = re.compile(r'\[tsc (\d+)\] SLOW (\S+) ([\d.]+) ms(.*)')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('trace')
    p.add_argument('log')
    p.add_argument('--hz', type=float, default=3187200000.0)
    p.add_argument('--frames', type=int, default=8, help='slowest frames to explain')
    p.add_argument('--around', type=float, default=0.3, help='seconds before a flip to list')
    args = p.parse_args()
    lt = load_trace_module()
    t = lt.Trace(args.trace, args.hz)
    flips = t.flips
    base = flips[0]
    calls = []
    for line in Path(args.log).read_text(errors='replace').splitlines():
        m = LINE.search(line)
        if m:
            calls.append((t.ms(int(m.group(1))) - base, m.group(2), float(m.group(3)), m.group(4).strip()))
    walk = [c for c in calls if 0 <= c[0] <= flips[-1] - base]
    total = collections.defaultdict(lambda: [0, 0.0])
    for _, name, ms, _ in walk:
        total[name][0] += 1
        total[name][1] += ms
    print(f'{len(walk)} slow calls during the walk (nested calls counted in each):')
    for name, (count, ms) in sorted(total.items(), key=lambda kv: -kv[1][1]):
        print(f'  {name:28s} {count:5d} calls {ms:8.1f} ms')
    frames = sorted(((flips[i + 1] - base, flips[i + 1] - flips[i]) for i in range(len(flips) - 1)),
                    key=lambda f: -f[1])[:args.frames]
    for end, duration in sorted(frames):
        print(f'frame ending {end / 1000:6.2f} s: {duration:6.1f} ms')
        lo = end - max(duration, args.around * 1000)
        for when, name, ms, rest in walk:
            if lo <= when <= end:
                print(f'    {when / 1000:6.2f} s {name:24s} {ms:6.1f} ms {rest[:110]}')


if __name__ == '__main__':
    main()
