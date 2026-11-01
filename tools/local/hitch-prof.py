#!/usr/bin/env python3
"""Render-thread samples of the slow frames: splits a live `prof` capture (PROF, with PROF.tsc, each
sample's TSC) by the run log's "SLOW Frame <ms>" lines (KYTY_HITCH_LOG_MS; their TSC stamp is the
frame's end) into PROF.hitch.prof (samples inside frames of --min ms or more) and PROF.normal.prof
(the rest), for prof-inline.py.

    hitch-prof.py PROF LOG [--min 45] [--max MS] [--hz TSC_HZ]

The TSC rate is taken from the log's LIVE_PROF line for PROF (tsc_hz=...) unless --hz is given.
"""
import argparse
import re
import struct
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument('prof')
    p.add_argument('log')
    p.add_argument('--min', type=float, default=45.0)
    p.add_argument('--max', type=float, default=float('inf'))
    p.add_argument('--hz', type=float, default=0.0)
    args = p.parse_args()
    prof = Path(args.prof)
    data = prof.read_bytes()
    tsc = struct.unpack(f'<{len(data) // 128}Q', Path(str(prof) + '.tsc').read_bytes()[:len(data) // 128 * 8])
    text = Path(args.log).read_text(encoding='utf-8', errors='replace')
    hz = args.hz
    if not hz:
        for line in text.splitlines():
            if 'LIVE_PROF' in line and prof.name in line.replace('\\', '/') and 'tsc_hz=' in line:
                hz = float(re.search(r'tsc_hz=([\d.]+)', line).group(1))
    if not hz:
        raise SystemExit('no tsc_hz: pass --hz')
    frames = [(int(m.group(1)) - float(m.group(2)) * hz / 1e3, int(m.group(1)), float(m.group(2)))
              for m in re.finditer(r'\[tsc (\d+)\] SLOW Frame ([\d.]+) ms', text) if args.min <= float(m.group(2)) < args.max]
    frames = [f for f in frames if f[1] >= tsc[0] and f[0] <= tsc[-1]]
    hitch, normal = bytearray(), bytearray()
    j = 0
    for i, t in enumerate(tsc):
        while j < len(frames) and frames[j][1] < t:
            j += 1
        inside = j < len(frames) and frames[j][0] <= t <= frames[j][1]
        (hitch if inside else normal).extend(data[i * 128:(i + 1) * 128])
    Path(str(prof) + '.hitch.prof').write_bytes(bytes(hitch))
    Path(str(prof) + '.normal.prof').write_bytes(bytes(normal))
    print(f'{len(frames)} slow frames ({sum(f[2] for f in frames):.0f} ms) in the capture: '
          f'{len(hitch) // 128} hitch samples, {len(normal) // 128} others')


if __name__ == '__main__':
    main()
