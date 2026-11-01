#!/usr/bin/env python3
"""Frame rate along a walk from a live `trace` capture (tools/local/windows/walk-run.ps1).

    walk-report.py TRACE [--window 1.0] [--worst 8]

Prints fps per second of the walk, the lowest 1 s window, the 1% low and the slowest frames, and
for the lowest window how the frame time splits into render-thread idle and GPU busy time (which
side limits it: the render thread's CPU work, or waiting on the GPU / the guest).
"""
import argparse
import importlib.util
import statistics
from pathlib import Path


def load_trace_module():
    spec = importlib.util.spec_from_file_location('live_trace', Path(__file__).with_name('live-trace.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def overlap(intervals, lo, hi):
    return sum(max(0.0, min(e, hi) - max(s, lo)) for s, e in intervals)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('trace')
    p.add_argument('--hz', type=float, default=3187200000.0)
    p.add_argument('--window', type=float, default=1.0, help='window for the fps curve (s)')
    p.add_argument('--worst', type=int, default=8)
    args = p.parse_args()
    lt = load_trace_module()
    t = lt.Trace(args.trace, args.hz)
    flips = t.flips
    if len(flips) < 3:
        raise SystemExit('fewer than 3 flips in the trace')
    frames = [(flips[i], flips[i + 1] - flips[i]) for i in range(len(flips) - 1)]
    span = flips[-1] - flips[0]
    print(f'{len(frames)} frames over {span / 1000:.1f} s: {len(frames) / span * 1000:.2f} fps average')

    window = args.window * 1000.0
    curve = []
    start = flips[0]
    while start + window <= flips[-1]:
        count = sum(1 for x in flips if start <= x < start + window)
        curve.append((start - flips[0], count * 1000.0 / window))
        start += window / 2
    print('fps per window (start s: fps):')
    for i in range(0, len(curve), 2):
        print('  ' + '  '.join(f'{s / 1000:5.1f}:{fps:5.1f}' for s, fps in curve[i:i + 2]))
    low_start, low_fps = min(curve, key=lambda c: c[1])
    durations = sorted(d for _, d in frames)
    one_percent = durations[max(0, int(len(durations) * 0.99) - 1)]
    print(f'lowest {args.window:.1f} s window: {low_fps:.1f} fps at {low_start / 1000:.1f} s; '
          f'1% low frame {one_percent:.1f} ms ({1000 / one_percent:.1f} fps); median {statistics.median(durations):.1f} ms')
    print(f'slowest frames:')
    for x, d in sorted(frames, key=lambda f: -f[1])[:args.worst]:
        print(f'  at {(x - flips[0]) / 1000:6.2f} s: {d:6.1f} ms')

    lo = flips[0] + low_start
    hi = lo + window
    idle = overlap([(s, e) for s, e, _ in t.render_idle], lo, hi)
    gpu = overlap(lt.merged([(s, e) for s, e, _ in t.gpu], lo, hi), lo, hi) if hasattr(lt, 'merged') else float('nan')
    in_window = [d for x, d in frames if lo <= x < hi]
    if not in_window: return
    print(f'lowest window: {len(in_window)} frames, mean {statistics.mean(in_window):.1f} ms; '
          f'render idle {idle / len(in_window):.1f} ms/frame, GPU busy {gpu / len(in_window):.1f} ms/frame')


if __name__ == '__main__':
    main()
