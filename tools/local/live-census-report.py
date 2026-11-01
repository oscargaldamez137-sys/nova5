#!/usr/bin/env python3
"""Summarize LIVE_CENSUS lines (stdin): per-kind totals and the costliest entries."""
import collections, re, sys
KINDS = {0: 'dispatch', 1: 'draw', 2: 'native-xpr', 3: 'dispatch-phase', 4: 'gpu-wait', 5: 'readback-wait', 6: 'sync-download', 7: 'graphics-programs', 8: 'native-gather',
         9: 'queue-run', 10: 'srt-interpreter', 11: 'command-sync', 12: 'draw-phase'}
PHASES = ['program', 'special', 'pipeline', 'prepare', 'find', 'bda', 'rebind-buffers', 'rebind-images', 'commit', 'record']
DRAW_PHASES = ['start', 'color-target', 'depth-target', 'programs', 'bindings', 'vertex-buffers', 'pipeline', 'rendering', 'draw']
top = int(sys.argv[1]) if len(sys.argv) > 1 else 25
rows, frames, base, exe = [], None, 0, None
for line in sys.stdin:
    if line.startswith('LIVE_CENSUS '):
        frames = int(re.search(r'frames=(\d+)', line)[1])
        m = re.search(r'base=([0-9a-f]+) exe=(\S+)', line)
        if m: base, exe = int(m[1], 16), m[2]
    elif line.startswith('LIVE_CENSUS_ENTRY'):
        d = dict(re.findall(r'(\w+)=(\S+)', line))
        rows.append((int(d['kind']), d['a'], d['b'], float(d['calls_per_frame']), float(d['ms_per_frame'])))
print(f'frames={frames}')
tot = collections.defaultdict(lambda: [0.0, 0.0, 0])
for k, a, b, c, ms in rows:
    tot[k][0] += c; tot[k][1] += ms; tot[k][2] += 1
for k, (c, ms, n) in sorted(tot.items()):
    print(f'{KINDS.get(k, str(k)):11s} {n:5d} keys {c:9.1f} calls/frame {ms:7.2f} ms/frame {1000*ms/max(c,1e-9):7.2f} us/call')
# The queue a call ran in (b's top bits, LiveCensus::g_queue): async compute queue runs fill the graphics
# queue's waits, so their time may not lengthen the frame.
QUEUES = {0: 'graphics', 1: 'no queue', 2: 'async compute'}
by_queue = collections.defaultdict(lambda: [0.0, 0.0])
for k, a, b, c, ms in rows:
    if k in (0, 1, 2, 3, 12):
        by_queue[(k, int(b, 16) >> 62)][0] += c
        by_queue[(k, int(b, 16) >> 62)][1] += ms
for (k, q), (c, ms) in sorted(by_queue.items()):
    print(f'  {KINDS.get(k, str(k)):14s} {QUEUES.get(q, str(q)):13s} {c:9.1f} calls/frame {ms:7.2f} ms/frame')
import bisect, subprocess
_syms = None
def sym(address):
    global _syms
    if _syms is None:
        import shutil
        tool = shutil.which('nm') or shutil.which('llvm-nm') or 'C:/Program Files/LLVM/bin/llvm-nm.exe'
        try:
            out = subprocess.run([tool, '-C', '--defined-only', exe], capture_output=True, text=True).stdout if exe else ''
        except OSError:
            out = ''  # no symbol tool: raw addresses
        _syms = sorted((int(p[0], 16), p[2]) for p in (l.split(' ', 2) for l in out.splitlines()) if len(p) == 3 and p[1] in 'tTwW')
    off = int(address, 16) - base
    i = bisect.bisect_right(_syms, (off, '\uffff')) - 1
    return _syms[i][1][:70] if i >= 0 else address
for k in sorted(t for t in tot if t not in (3, 12)):
    print(f'-- {KINDS.get(k, str(k))} top by time')
    for kk, a, b, c, ms in sorted((r for r in rows if r[0] == k), key=lambda r: -r[4])[:top]:
        label = f'{sym(a)} <- {sym(b)}' if k in (4, 5) else f'{a} {b[-4:] + (" async" if int(b, 16) >> 63 else "") if k == 0 else b}'
        print(f'   {label}  {c:8.2f}/frame {ms:7.3f} ms {1000*ms/max(c,1e-9):7.2f} us/call')

if 3 in tot:
    shaders = sorted({r[1] for r in rows if r[0] == 0}, key=lambda a: -sum(r[4] for r in rows if r[0] == 0 and r[1] == a))[:8]
    print('-- dispatch phases (us/call) for the costliest shaders')
    print('   shader            calls ' + ' '.join(f'{p[:8]:>8s}' for p in PHASES))
    for a in shaders:
        calls = sum(r[3] for r in rows if r[0] == 0 and r[1] == a)
        cells = []
        for i in range(len(PHASES)):
            ms = sum(r[4] for r in rows if r[0] == 3 and r[1] == a and int(r[2], 16) & 0xff == i)
            cells.append(f'{1000*ms/max(calls,1e-9):8.2f}')
        print(f'   {a} {calls:8.1f} ' + ' '.join(cells))
    print('   all phases, ms/frame: ' + ' '.join(f'{PHASES[i]}={sum(r[4] for r in rows if r[0] == 3 and int(r[2], 16) & 0xff == i):.2f}' for i in range(len(PHASES))))

if 12 in tot:
    # Draw phases per pixel shader (kind 1 rows carry the pixel shader in b).
    draw_ms = collections.defaultdict(float)
    draw_calls = collections.defaultdict(float)
    for k, a, b, c, ms in rows:
        if k == 1:
            draw_ms[int(b, 16) & ((1 << 62) - 1)] += ms
            draw_calls[int(b, 16) & ((1 << 62) - 1)] += c
    phase_ms = collections.defaultdict(float)
    for k, a, b, c, ms in rows:
        if k == 12:
            phase_ms[(int(a, 16), int(b, 16) & 0xff)] += ms
    print('-- draw phases (us/call) for the costliest pixel shaders')
    print('   pixel shader       calls ' + ' '.join(f'{p[:8]:>8s}' for p in DRAW_PHASES))
    for ps in sorted(draw_ms, key=lambda x: -draw_ms[x])[:top]:
        calls = draw_calls[ps]
        print(f'   {ps:016x} {calls:8.1f} ' + ' '.join(f'{1000 * phase_ms[(ps, i)] / max(calls, 1e-9):8.2f}' for i in range(len(DRAW_PHASES))))
    print('   all draw phases, ms/frame: ' + ' '.join(f'{DRAW_PHASES[i]}={sum(v for (a, b), v in phase_ms.items() if b == i):.2f}' for i in range(len(DRAW_PHASES))))
