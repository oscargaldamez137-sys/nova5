#!/usr/bin/env python3
"""Shader and pipeline compile stalls of an emulator run (KYTY_SLOW_LOG_MS=<ms> in its environment).

  stalls.py [LOG]    the newest _Build/run-logs/*.out.log by default

Counts the SLOW lines of program compiles (TranslateProgram: translation, SPIR-V and module) and of
graphics and compute pipeline creations (the driver compile unless its cache holds the pipeline).
"""
import collections
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
PATTERN = re.compile(r'SLOW (TranslateProgram|CreateGraphicsPipeline|CreateComputePipeline) ([\d.]+) ms')
BANDS = (30, 100, 250, 500, 1000, 2000)


def summarize(path):
    events = collections.defaultdict(list)
    compiles = 0
    for line in Path(path).read_text(errors='replace').split('\n'):
        if line.startswith('Shaders: VS'):
            compiles += 1
        match = PATTERN.search(line)
        if match:
            events[match.group(1)].append(float(match.group(2)))
    print(f'{path}: {compiles} programs compiled at run time')
    for kind in ('TranslateProgram', 'CreateGraphicsPipeline', 'CreateComputePipeline'):
        times = sorted(events.get(kind, []))
        if not times:
            print(f'  {kind:24s} none')
            continue
        bands = ', '.join(f'>={band} ms {sum(1 for t in times if t >= band)}' for band in BANDS)
        print(f'  {kind:24s} {len(times):4d} events, {sum(times) / 1000:6.1f} s in all, max {times[-1]:6.0f} ms; {bands}')
    return events


def main():
    if len(sys.argv) > 1:
        path = Path(sys.argv[1])
    else:
        logs = sorted((REPO / '_Build' / 'run-logs').glob('*.out.log'), key=lambda p: p.stat().st_mtime)
        path = logs[-1]
    summarize(path)


if __name__ == '__main__':
    main()
