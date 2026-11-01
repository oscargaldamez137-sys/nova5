#!/usr/bin/env python3
"""Summarize a live `capture` (src/local/frame-capture.h) into passes and image flows.

    python tools/local/frame-capture-report.py <capture-dir> [frame] [--min-width N]

Consecutive draws with the same targets, or consecutive dispatches of one compute shader,
form a pass. For each pass the report lists its calls, render-thread time, shaders, targets
and the textures it reads; then, for every image at least --min-width wide (default 1280),
which passes write it and which read it, in frame order; last, images read in frame N+1
that frame N wrote (temporal history such as TAA), when frame N+1 was captured too.
"""
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

VULKAN_HEADER = Path('C:/VulkanSDK/1.4.357.0/Include/vulkan/vulkan_core.h')


def format_names():
    names = {}
    if VULKAN_HEADER.exists():
        for match in re.finditer(r'VK_FORMAT_(\w+) = (\d+),', VULKAN_HEADER.read_text(errors='replace')):
            names.setdefault(int(match.group(2)), match.group(1))
    return names


FORMATS = format_names()


def fmt(number):
    return FORMATS.get(number, str(number))


def load(path):
    calls = []
    for line in path.open(encoding='utf-8', errors='replace'):
        record = json.loads(line)
        if 'i' in record:
            calls.append(record)
    return calls


def image_label(image):
    return f"{image['a']}:{fmt(image['f'])}:{image['w']}x{image['h']}" + (
        f"x{image['arr']}" if image['arr'] > 1 else '') + (f" m{image['mip']}" if image['mip'] > 1 else '')


def pass_key(call):
    if call['t'] == 'Dispatch':
        return ('cs', call.get('cs'))
    targets = tuple((m['a'], m['f']) for m in call.get('img', []) if m['k'] in (0, 1))
    return ('draw', targets)


def split_passes(calls):
    passes = []
    for call in calls:
        key = pass_key(call)
        if passes and passes[-1]['key'] == key:
            passes[-1]['calls'].append(call)
        else:
            passes.append({'key': key, 'calls': [call]})
    return passes


def describe(passes):
    lines = []
    for number, group in enumerate(passes):
        calls = group['calls']
        us = sum(c['us'] for c in calls)
        first, last = calls[0]['i'], calls[-1]['i']
        if group['key'][0] == 'cs':
            written = {image_label(m) for c in calls for m in c.get('img', []) if m['rw']}
            read = {image_label(m) for c in calls for m in c.get('img', []) if not m['rw']}
            wbuf = {b['a'] for c in calls for b in c.get('buf', []) if b['rw']}
            groups = calls[0].get('groups')
            consumed = {c.get('consumed') for c in calls if c.get('consumed')}
            lines.append(f"P{number:04d} [{first}-{last}] CS {group['key'][1]} x{len(calls)} {us / 1000:.3f} ms"
                         f" groups0={groups} local={calls[0].get('local')}"
                         + (f" consumed={sorted(consumed)}" if consumed else ''))
            if written:
                lines.append('    writes img: ' + ', '.join(sorted(written)))
            if read:
                lines.append('    reads  img: ' + ', '.join(sorted(read)[:12]) + (' ...' if len(read) > 12 else ''))
            if wbuf:
                lines.append(f'    writes buf: {len(wbuf)} ranges')
        else:
            shaders = defaultdict(int)
            for c in calls:
                shaders[(c.get('vs'), c.get('ps'))] += 1
            targets = [m for m in calls[0].get('img', []) if m['k'] in (0, 1)]
            reads = {image_label(m) for c in calls for m in c.get('img', []) if m['k'] == 2}
            blend = {c.get('blend') for c in calls}
            zstate = {tuple(c.get('z', [])) for c in calls}
            small = sum(1 for c in calls if c.get('n', 0) <= 4)
            lines.append(f"P{number:04d} [{first}-{last}] DRAW x{len(calls)} {us / 1000:.3f} ms"
                         f" shaders={len(shaders)} fullscreen-ish={small} blend={sorted(blend)} z={sorted(zstate)}")
            lines.append('    targets: ' + ', '.join(('D ' if m['k'] == 1 else f"C{m['slot']} ") + image_label(m)
                                                for m in targets))
            top = sorted(shaders.items(), key=lambda item: -item[1])[:4]
            lines.append('    vs/ps: ' + ', '.join(f'{vs}/{ps} x{n}' for (vs, ps), n in top)
                         + (' ...' if len(shaders) > 4 else ''))
            if reads:
                lines.append('    reads  img: ' + ', '.join(sorted(reads)[:12]) + (' ...' if len(reads) > 12 else ''))
    return lines


def flows(passes, min_width):
    events = defaultdict(list)
    for number, group in enumerate(passes):
        for call in group['calls']:
            for m in call.get('img', []):
                if m['w'] < min_width:
                    continue
                writes = m['k'] in (0, 1) or m['rw']
                events[(m['a'], m['f'], m['w'], m['h'])].append((number, 'W' if writes else 'R'))
    lines = []
    for (address, format_number, width, height), items in sorted(events.items(), key=lambda kv: kv[1][0][0]):
        compact = []
        for number, kind in items:
            token = f'{kind}P{number:04d}'
            if not compact or compact[-1] != token:
                compact.append(token)
        lines.append(f'{address} {fmt(format_number)} {width}x{height}: ' + ' '.join(compact[:40])
                     + (' ...' if len(compact) > 40 else ''))
    return lines


def history(frame_a, frame_b):
    written = defaultdict(set)
    for call in frame_a:
        for m in call.get('img', []):
            if m['k'] in (0, 1) or m['rw']:
                written[m['a']].add(call.get('cs') or call.get('ps'))
    first_write = {}
    for call in frame_b:
        for m in call.get('img', []):
            if m['k'] in (0, 1) or m['rw']:
                first_write.setdefault(m['a'], call['i'])
    lines = []
    seen = set()
    for call in frame_b:
        for m in call.get('img', []):
            if m['k'] == 2 and not m['rw'] and m['a'] in written:
                shader = call.get('cs') or call.get('ps')
                key = (m['a'], shader)
                if key in seen:
                    continue
                seen.add(key)
                # Read before any write of the same address in frame B -> it saw frame A's data.
                if m['a'] not in first_write or first_write[m['a']] > call['i']:
                    lines.append(f"call {call['i']} shader {shader} reads {image_label(m)} "
                                 f"written last frame by {sorted(filter(None, written[m['a']]))[:3]}")
    return lines


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    min_width = 1280
    if '--min-width' in sys.argv:
        min_width = int(sys.argv[sys.argv.index('--min-width') + 1])
        args = [a for a in args if a != str(min_width)]
    directory = Path(args[0])
    frame = int(args[1]) if len(args) > 1 else 0
    calls = load(directory / f'frame-{frame}.jsonl')
    passes = split_passes(calls)
    draws = sum(1 for c in calls if c['t'] != 'Dispatch')
    print(f'frame {frame}: {len(calls)} calls ({draws} draws, {len(calls) - draws} dispatches), '
          f'{sum(c["us"] for c in calls) / 1000:.2f} ms render-thread time in calls, {len(passes)} passes')
    print('\n'.join(describe(passes)))
    print(f'\n== images >= {min_width} wide: writers (W) and readers (R) by pass ==')
    print('\n'.join(flows(passes, min_width)))
    following = directory / f'frame-{frame + 1}.jsonl'
    if following.exists():
        print('\n== reads in the next frame of images this frame wrote (temporal history) ==')
        print('\n'.join(history(calls, load(following))))


if __name__ == '__main__':
    main()
