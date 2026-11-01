#!/usr/bin/env python3
"""The render thread's own page faults in a live `trace` capture: host code that touched tracked
guest pages (FaultSite + FaultCaller + FaultDone records), grouped by faulting function and caller,
with the time until each fault was handled (a read fault usually waits for a GPU download).

    render-faults.py TRACE MAP [--top 25]
"""
import argparse
import bisect
import collections
import importlib.util
import struct
from pathlib import Path

RENDER_SLICE, FAULT_SITE, FAULT_CALLER, FAULT_DONE = 21, 32, 33, 36


def main():
    p = argparse.ArgumentParser()
    p.add_argument('trace')
    p.add_argument('map')
    p.add_argument('--top', type=int, default=25)
    a = p.parse_args()
    spec = importlib.util.spec_from_file_location('prof_slices', Path(__file__).with_name('prof-slices.py'))
    slices = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(slices)
    symbols = slices.Symbols(a.map)
    hz = float(Path(a.trace + '.hz').read_text()) if Path(a.trace + '.hz').exists() else 3187145000.0
    data = Path(a.trace).read_bytes()
    records = [struct.unpack_from('<QIIQQ', data, i * 32) for i in range(len(data) // 32)]
    records = sorted((r for r in records if r[0] != 0), key=lambda r: r[0])
    render = collections.Counter(r[1] for r in records if r[2] == RENDER_SLICE).most_common(1)[0][0]
    t0 = records[0][0]
    seconds = (records[-1][0] - t0) / hz
    mine = [r for r in records if r[1] == render and r[2] in (FAULT_SITE, FAULT_CALLER, FAULT_DONE)]
    groups = collections.defaultdict(lambda: [0, 0.0, 0])  # count, ms, writes
    pending = None
    for tsc, tid, kind, x, y in mine:
        if kind == FAULT_SITE:
            pending = [tsc, x, 0, y >> 63]
        elif kind == FAULT_CALLER and pending is not None:
            pending[2] = x
        elif kind == FAULT_DONE and pending is not None:
            key = (symbols(pending[1]), symbols(pending[2]) if pending[2] else '-')
            g = groups[key]
            g[0] += 1
            g[1] += (tsc - pending[0]) / hz * 1e3
            g[2] += pending[3]
            pending = None
    total = sum(g[0] for g in groups.values())
    total_ms = sum(g[1] for g in groups.values())
    print(f'render thread {render}: {total} handled faults over {seconds:.1f} s, {total_ms:.1f} ms '
          f'({total_ms / max(seconds, 1e-9):.2f} ms per second)')
    for (site, caller), (n, ms, writes) in sorted(groups.items(), key=lambda kv: -kv[1][1])[:a.top]:
        print(f'  {ms:8.2f} ms {n:6d} faults ({writes} writes)  {site[:60]}  <-  {caller[:60]}')


if __name__ == '__main__':
    main()
