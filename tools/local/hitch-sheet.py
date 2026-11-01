#!/usr/bin/env python3
"""What the screen showed at each slow frame: the run log's SLOW Frame lines (their unix_ms, the frame's end) matched
to the screen, as one contact sheet: per slow frame the screen just before the frame began and just after it ended,
labelled with the frame time and its counters. CAPTURES is a video of tools/local/windows/record.ps1 (wall-clock
timestamps) or a directory of tools/local/windows/capture.ps1 stills (<unix ms>.jpg).

    hitch-sheet.py LOG CAPTURES OUT.jpg [--min 50] [--from UNIX_MS] [--to UNIX_MS] [--max 24]
"""
import argparse
import bisect
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw


def tool(name):
    found = shutil.which(name)
    if found:
        return found
    packages = Path(os.environ.get('LOCALAPPDATA', '')) / 'Microsoft/WinGet/Packages'
    return next((str(p) for p in packages.glob(f'Gyan.FFmpeg*/*/bin/{name}.exe')), name)


class Video:
    """Frames of a record.ps1 video by wall-clock time."""

    def __init__(self, path):
        self.path = str(path)
        out = subprocess.run([tool('ffprobe'), '-v', 'error', '-show_entries', 'format=start_time,duration', '-of',
                              'csv=p=0', self.path], capture_output=True, text=True, check=True).stdout.strip().split(',')
        self.start = float(out[0])
        # The duration of a stream with wall-clock timestamps counts from 0: the end is the duration.
        self.end = float(out[1]) if float(out[1]) > self.start else self.start + float(out[1])
        self.scratch = tempfile.mkdtemp(prefix='hitch-sheet-')

    def range_ms(self):
        return int(self.start * 1000), int(self.end * 1000)

    def at(self, unix_ms, before):
        """The frame shown at unix_ms: the last one at or before it (before) or the first one after it."""
        offset = max(0.0, unix_ms / 1000 - self.start - (1 / 30 if before else 0))
        out = os.path.join(self.scratch, f'{unix_ms}-{int(before)}.jpg')
        subprocess.run([tool('ffmpeg'), '-y', '-hide_banner', '-loglevel', 'error', '-ss', f'{offset:.3f}', '-i',
                        self.path, '-frames:v', '1', '-q:v', '3', out], check=True)
        return Image.open(out), unix_ms


class Stills:
    """capture.ps1 stills by wall-clock time."""

    def __init__(self, path):
        self.shots = sorted((int(f.stem), f) for f in Path(path).glob('*.jpg') if f.stem.isdigit())
        if not self.shots:
            raise SystemExit('no captures')
        self.stamps = [s for s, _ in self.shots]

    def range_ms(self):
        return self.stamps[0], self.stamps[-1] + 1000

    def at(self, unix_ms, before):
        index = (max(0, bisect.bisect_right(self.stamps, unix_ms) - 1) if before
                 else min(len(self.shots) - 1, bisect.bisect_left(self.stamps, unix_ms)))
        return Image.open(self.shots[index][1]), self.shots[index][0]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('log')
    p.add_argument('captures')
    p.add_argument('out')
    p.add_argument('--min', type=float, default=50.0)
    p.add_argument('--from', dest='start', type=int, default=0)
    p.add_argument('--to', type=int, default=1 << 62)
    p.add_argument('--max', type=int, default=24)
    args = p.parse_args()
    source = Video(args.captures) if Path(args.captures).is_file() else Stills(args.captures)
    first, last = source.range_ms()
    slow = []
    for line in Path(args.log).read_text(encoding='utf-8', errors='replace').splitlines():
        m = re.search(r'SLOW Frame ([\d.]+) ms(.*) unix_ms=(\d+)', line)
        if m and float(m.group(1)) >= args.min and max(args.start, first) <= int(m.group(3)) <= min(args.to, last):
            slow.append((int(m.group(3)), float(m.group(1)), m.group(2).strip()))
    if not slow:
        raise SystemExit('no slow frames within the captures')
    slow = slow[:args.max]
    tiles = []
    for end, ms, rest in slow:
        tiles.append((source.at(int(end - ms), True), source.at(end, False)))
    tile_w, tile_h = tiles[0][0][0].size
    label_h = 34
    sheet = Image.new('RGB', (2 * tile_w, len(slow) * (tile_h + label_h)), 'black')
    draw = ImageDraw.Draw(sheet)
    for row, ((end, ms, rest), pair) in enumerate(zip(slow, tiles)):
        y = row * (tile_h + label_h)
        for col, (image, stamp) in enumerate(pair):
            sheet.paste(image.resize((tile_w, tile_h)), (col * tile_w, y + label_h))
            draw.text((col * tile_w + 4, y + label_h + 2), f'{(stamp - first) / 1000:.2f} s', fill='yellow')
        counters = ' '.join(word for word in rest.split() if not word.startswith(('upload_bytes', 'readback', 'guest_commands')))
        draw.text((4, y + 2), f'{(end - first) / 1000:.2f} s  {ms:.1f} ms', fill='white')
        draw.text((4, y + 17), counters[:2 * tile_w // 6], fill='gray')
    sheet.save(args.out, quality=80)
    print(f'{len(slow)} slow frames -> {args.out}')


if __name__ == '__main__':
    main()
