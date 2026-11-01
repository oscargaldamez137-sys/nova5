#!/usr/bin/env python3
"""Demon's Souls (PPSA01341) saves: what each slot holds, and test saves that start on another map.

    demons-souls-save.py info SAVE_DIR             every USR-DATA under SAVE_DIR/PPSA01341
    demons-souls-save.py spawns [--game DIR]       the player spawn points of every map (the game's MSB files)
    demons-souls-save.py set-map SRC_DIR DST_DIR MAP [--spawn NAME | --at X,Y,Z,YAW]
                                                   copy SRC_DIR (PPSA01341 and .kyty-capacity) to DST_DIR,
                                                   the copy's character on MAP (m04_00_00_00, or a MAPUID
                                                   such as 0x04000000) at a spawn point of that map

A test save is a bench baseline: KYTY_BENCH_BASELINE=DST_DIR for tools/local/bench-windows.py.
Maps (levels/, cp11demonssouls/dvdroot/map/mapstudio/<map>.msb): m01 the Nexus, m02 Boletarian Palace,
m03 Shrine of Storms, m04 Tower of Latria, m05 Valley of Defilement, m06 Stonefang Tunnel, m08 the
tutorial (the bench baseline); MAPUID 0xWWBBCCDD for mWW_BB_CC_DD. A map with no saved position starts
at its default start (only some have one; m03_00 and m04_00 crashed or returned to the title, with copies
that still had the position flag set); the saved position is in the MSB's coordinates (the slot's spawn:
m02_00's c0000_0001). A slot without one (a character still in the tutorial) gets the position entries.

USR-DATA: u32 version, u32 FNV-1a 32 of the payload, u32 payload size, u32, payload: entries of key
(FNV-1a 32 of an upper-case name: MAPUID, PLAYTIME, PLAYER_NAME, ...), type byte, value.
"""
import argparse
import math
import re
import shutil
import struct
import sys
from pathlib import Path


def fnv1a(data):
    h = 0x811c9dc5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def key(name):
    return struct.pack('<I', fnv1a(name.encode()))


MAPUID = key('MAPUID') + b'\x04'
POSITION = struct.pack('<I', 0xfb0faeb2) + b'\x0e\x10'  # name unknown: 16-byte blob x y z 1
ROTATION = struct.pack('<I', 0x739273b8) + b'\x0e\x10'  # name unknown: 16-byte blob 0 yaw(radians) 0 0
# name unknown: byte, 1 when POSITION and ROTATION follow it (a character still in the tutorial has 0 and neither)
PLACED = struct.pack('<I', 0x4819bb27) + b'\x0c'
GAME = Path.home() / 'Documents' / 'PPSA01341-app0'


def fields(data):
    out = {}
    at = data.find(MAPUID)
    if at >= 0:
        out['map'] = f'{struct.unpack_from("<I", data, at + 5)[0]:#010x}'
    at = data.find(key('PLAYTIME') + b'\x0a')
    if at >= 0:
        out['playtime'] = f'{struct.unpack_from("<f", data, at + 5)[0] / 1000 / 60:.0f} min'
    at = data.find(key('PLAYER_NAME') + b'\x0d')
    if at >= 0:
        out['name'] = data[at + 6:at + 6 + data[at + 5]].decode('latin-1')
    at = data.find(POSITION)
    if at >= 0:
        out['position'] = ' '.join(f'{v:.1f}' for v in struct.unpack_from('<3f', data, at + 6))
    return out


def info(save_dir):
    for path in sorted(Path(save_dir, 'PPSA01341').glob('*/USR-DATA'), key=lambda p: p.stat().st_mtime):
        data = path.read_bytes()
        valid = fnv1a(data[16:]) == struct.unpack_from('<I', data, 4)[0]
        print(f'{path.parent.name:28} {"" if valid else "BAD CHECKSUM "}' +
              '  '.join(f'{k}={v}' for k, v in fields(data).items()))


def msb_players(path):
    """Player parts of a remake MSB (64-bit offsets, little-endian): (name, x, y, z, yaw degrees)."""
    data = path.read_bytes()
    at, out = 0, []
    while True:
        name_offset, count = struct.unpack_from('<qi', data, at + 8)
        offsets = struct.unpack_from(f'<{count}q', data, at + 24)
        if data[name_offset:data.index(b'\0', name_offset)] == b'PARTS_PARAM_ST':
            for entry in offsets[:-1]:
                name_at, part_type = struct.unpack_from('<qi', data, entry)
                if part_type != 4:  # Player
                    continue
                name = data[entry + name_at:data.index(b'\0', entry + name_at)].decode('latin-1')
                x, y, z = struct.unpack_from('<3f', data, entry + 0x20)
                out.append((name, x, y, z, struct.unpack_from('<f', data, entry + 0x30)[0]))
        if offsets[-1] == 0:
            return out
        at = offsets[-1]


def msb_dir(game):
    return Path(game, 'cp11demonssouls', 'dvdroot', 'map', 'mapstudio')


def spawns(game):
    for path in sorted(msb_dir(game).glob('m0*.msb')):
        for name, x, y, z, yaw in msb_players(path):
            print(f'{path.stem} {name:12} {x:9.2f} {y:9.2f} {z:9.2f} yaw {yaw:7.2f}')


def set_map(src, dst, map_name, profile, spawn, at, game):
    dst = Path(dst)
    if dst.exists():
        sys.exit(f'{dst} exists')
    if re.fullmatch(r'm\d\d_\d\d_\d\d_\d\d', map_name):
        mapuid = int(map_name[1:].replace('_', ''), 16)  # m04_01_00_00 -> 0x04010000
    else:
        mapuid = int(map_name, 0)
        map_name = 'm{:02x}_{:02x}_{:02x}_{:02x}'.format(*mapuid.to_bytes(4, 'big'))
    if at is None and spawn is not None:
        players = {name: (x, y, z, yaw) for name, x, y, z, yaw in msb_players(msb_dir(game) / f'{map_name}.msb')}
        if spawn not in players:
            sys.exit(f'{map_name} has no player part {spawn} (has {", ".join(players)})')
        at = players[spawn]
    path = Path(src, 'PPSA01341', profile, 'USR-DATA')
    data = path.read_bytes()
    if fnv1a(data[16:]) != struct.unpack_from('<I', data, 4)[0]:
        sys.exit(f'{path}: checksum does not match')
    payload = bytearray(data[16:])
    found = payload.find(MAPUID)
    if found < 0 or payload.find(MAPUID, found + 1) >= 0:
        sys.exit(f'{path}: no single MAPUID')
    old = struct.unpack_from('<I', payload, found + 5)[0]
    struct.pack_into('<I', payload, found + 5, mapuid)
    placed = payload.find(PLACED)
    if placed < 0 or payload.find(PLACED, placed + 1) >= 0:
        sys.exit(f'{path}: no single position flag')
    placed += len(PLACED)
    # The two entries right after the flag, or neither (the game writes no other layout): removed, then
    # written again with the flag when the copy gets a position.
    if payload[placed] == 1 and payload[placed + 1:placed + 1 + len(POSITION)] == POSITION:
        del payload[placed + 1:placed + 1 + 2 * (len(POSITION) + 16)]
    elif payload[placed] != 0 or payload.find(POSITION) >= 0 or payload.find(ROTATION) >= 0:
        sys.exit(f'{path}: unexpected position entries')
    payload[placed] = 0
    if at is not None:
        x, y, z, yaw = at
        payload[placed] = 1
        payload[placed + 1:placed + 1] = (POSITION + struct.pack('<4f', x, y, z, 1.0) +
                                          ROTATION + struct.pack('<4f', 0.0, math.radians(yaw), 0.0, 0.0))
    shutil.copytree(src, dst)
    header = struct.pack('<4I', struct.unpack_from('<I', data, 0)[0], fnv1a(payload), len(payload),
                         struct.unpack_from('<I', data, 12)[0])
    Path(dst, 'PPSA01341', profile, 'USR-DATA').write_bytes(header + bytes(payload))
    where = 'at its default start' if at is None else 'at ({:.2f}, {:.2f}, {:.2f}) yaw {:.0f}'.format(*at)
    print(f'{dst}: {profile} map {old:#010x} -> {mapuid:#010x} ({map_name}) {where}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('info')
    p.add_argument('save_dir')
    p = sub.add_parser('spawns')
    p.add_argument('--game', default=GAME)
    p = sub.add_parser('set-map')
    p.add_argument('src')
    p.add_argument('dst')
    p.add_argument('map')
    p.add_argument('--profile', default='SAVEDATA0PlayerProfile0')
    p.add_argument('--spawn', help='a player part of the map (spawns lists them), e.g. c0000_0000')
    p.add_argument('--at', type=lambda s: tuple(float(v) for v in s.split(',')), help='X,Y,Z,YAW (degrees)')
    p.add_argument('--game', default=GAME)
    args = parser.parse_args()
    if args.command == 'info':
        info(args.save_dir)
    elif args.command == 'spawns':
        spawns(args.game)
    else:
        set_map(args.src, args.dst, args.map, args.profile, args.spawn, args.at, args.game)
