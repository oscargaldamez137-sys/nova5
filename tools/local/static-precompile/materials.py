"""Demon's Souls '.cmat' materials: which shader bundle a material uses and its techniques.

Layout (little endian, validated over every material of the game):
  0x00 u32 material id   0x04 u32 version   0x08 u32 flags (bit 0: no culling)   0x0c..0x1f misc
  0x20 bundle path: LEB128 length + '$/materials/!cookedshaders/version99/<family>/<hash8>/****/bundledshaders_<N>.csdr'
  u32 technique count, then per technique {u32 pass id, u32 kind}: kind 0x11 = a (VS, PS) pair (two bundle
  entries, Gs then Ps), kind 0x20 = one compute shader (one entry); techniques take the entries in order.
  u32 texture count, then LEB128-length '$/...ctxr' paths; a parameter blob; footer 'STAM' u32 0x63 u32 (size - 12)
"""
import struct
from pathlib import Path

import gamefs

KIND_PAIR, KIND_COMPUTE = 0x11, 0x20


class MaterialError(ValueError):
    pass


def parse(b):
    if len(b) < 0x30 or b[-12:-8] != b'STAM' or struct.unpack_from('<I', b, len(b) - 4)[0] != len(b) - 12:
        raise MaterialError('missing STAM footer')
    out = {'material_id': struct.unpack_from('<I', b, 0)[0], 'flags': struct.unpack_from('<I', b, 8)[0]}
    p = 0x20

    def string(p):
        n, shift = 0, 0
        while True:
            c = b[p]
            p += 1
            n |= (c & 0x7f) << shift
            shift += 7
            if not c & 0x80:
                break
            if shift > 28:
                raise MaterialError('bad length')
        s = b[p:p + n]
        if len(s) != n or (n and not s.startswith(b'$')):
            raise MaterialError(f'bad path at {p}')
        return (s[1:].decode('ascii') if n else ''), p + n

    out['bundle'], p = string(p)
    (count,) = struct.unpack_from('<I', b, p)
    p += 4
    if count > 64:
        raise MaterialError('implausible technique count')
    out['techniques'] = [struct.unpack_from('<II', b, p + 8 * i) for i in range(count)]
    return out


def bundle_file(game, bundle_path, platform='_ps5'):
    return gamefs.game_path(game) / bundle_path.lstrip('/').replace('****', platform)


def techniques(game):
    """{bundle file: (flags, [(pass id, kind)])} for every material; materials sharing a bundle share its list."""
    out = {}
    for f in sorted(gamefs.game_path(game).rglob('*.cmat')):
        m = parse(f.read_bytes())
        out.setdefault(bundle_file(game, m['bundle']), (m['flags'], m['techniques']))
    return out


def technique_entries(bundle_entries, technique_list):
    """(pass id, kind, entry indices) per technique, or None when the list does not fit the bundle."""
    out, cursor = [], 0
    for pass_id, kind in technique_list:
        take = 2 if kind == KIND_PAIR else 1 if kind == KIND_COMPUTE else None
        if take is None or cursor + take > len(bundle_entries):
            return None
        out.append((pass_id, kind, list(range(cursor, cursor + take))))
        cursor += take
    return out if cursor == len(bundle_entries) else None
