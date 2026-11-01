#!/usr/bin/env python3
"""Pure-Python XXH3_64bits (seed 0, default secret) written from the public XXH3 specification
(3rdparty/xxHash/doc/xxhash_spec.md + xxhash.h); no external dependency.

Used to (a) recompute the runtime `ShaderParams.hash` = XXH3_64bits(code bytes) (src/graphics/shader/shader.cpp
HashShaderCode) and (b) verify the warmup-file payload checksum."""
import struct

M64 = (1 << 64) - 1
M32 = (1 << 32) - 1

P32_1, P32_2, P32_3 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D
P64_1, P64_2, P64_3 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9
P64_4, P64_5 = 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5
PMX1, PMX2 = 0x165667919E3779F9, 0x9FB21C651E98DF25

SECRET = bytes([
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
])
assert len(SECRET) == 192

_q = struct.Struct('<Q').unpack_from
_i = struct.Struct('<I').unpack_from


def _r64(b, o):
    return _q(b, o)[0]


def _r32(b, o):
    return _i(b, o)[0]


def _rotl64(x, r):
    return ((x << r) | (x >> (64 - r))) & M64


def _mul128_fold64(a, b):
    p = a * b
    return (p & M64) ^ (p >> 64)


def _xxh64_avalanche(h):
    h ^= h >> 33
    h = (h * P64_2) & M64
    h ^= h >> 29
    h = (h * P64_3) & M64
    h ^= h >> 32
    return h


def _xxh3_avalanche(h):
    h ^= h >> 37
    h = (h * PMX1) & M64
    h ^= h >> 32
    return h


def _rrmxmx(h, length):
    h ^= _rotl64(h, 49) ^ _rotl64(h, 24)
    h = (h * PMX2) & M64
    h ^= (h >> 35) + length
    h = (h * PMX2) & M64
    return h ^ (h >> 28)


def _mix16(inp, io, sec, so):
    lo = _r64(inp, io)
    hi = _r64(inp, io + 8)
    return _mul128_fold64(lo ^ _r64(sec, so), hi ^ _r64(sec, so + 8))


def _acc512(acc, inp, io, sec, so):
    for i in range(8):
        dv = _r64(inp, io + 8 * i)
        dk = dv ^ _r64(sec, so + 8 * i)
        acc[i ^ 1] = (acc[i ^ 1] + dv) & M64
        acc[i] = (acc[i] + (dk & M32) * (dk >> 32)) & M64


def _scramble(acc, sec, so):
    for i in range(8):
        a = acc[i]
        a ^= a >> 47
        a ^= _r64(sec, so + 8 * i)
        acc[i] = (a * P32_1) & M64


def _merge(acc, sec, so, start):
    r = start
    for i in range(4):
        r = (r + _mul128_fold64(acc[2 * i] ^ _r64(sec, so + 16 * i), acc[2 * i + 1] ^ _r64(sec, so + 16 * i + 8))) & M64
    return _xxh3_avalanche(r)


def xxh3_64(data):
    data = bytes(data)
    n = len(data)
    s = SECRET
    if n == 0:
        return _xxh64_avalanche(_r64(s, 56) ^ _r64(s, 64))
    if n <= 3:
        c1, c2, c3 = data[0], data[n >> 1], data[n - 1]
        combined = (c1 << 16) | (c2 << 24) | c3 | (n << 8)
        bitflip = (_r32(s, 0) ^ _r32(s, 4))
        return _xxh64_avalanche(combined ^ bitflip)
    if n <= 8:
        i1, i2 = _r32(data, 0), _r32(data, n - 4)
        bitflip = _r64(s, 8) ^ _r64(s, 16)
        i64 = i2 + (i1 << 32)
        return _rrmxmx(i64 ^ bitflip, n)
    if n <= 16:
        bf1 = _r64(s, 24) ^ _r64(s, 32)
        bf2 = _r64(s, 40) ^ _r64(s, 48)
        lo = _r64(data, 0) ^ bf1
        hi = _r64(data, n - 8) ^ bf2
        acc = (n + int.from_bytes(lo.to_bytes(8, 'little'), 'big') + hi + _mul128_fold64(lo, hi)) & M64
        return _xxh3_avalanche(acc)
    if n <= 128:
        acc = (n * P64_1) & M64
        if n > 32:
            if n > 64:
                if n > 96:
                    acc += _mix16(data, 48, s, 96)
                    acc += _mix16(data, n - 64, s, 112)
                acc += _mix16(data, 32, s, 64)
                acc += _mix16(data, n - 48, s, 80)
            acc += _mix16(data, 16, s, 32)
            acc += _mix16(data, n - 32, s, 48)
        acc += _mix16(data, 0, s, 0)
        acc += _mix16(data, n - 16, s, 16)
        return _xxh3_avalanche(acc & M64)
    if n <= 240:
        acc = (n * P64_1) & M64
        rounds = n // 16
        for i in range(8):
            acc += _mix16(data, 16 * i, s, 16 * i)
        acc = _xxh3_avalanche(acc & M64)
        for i in range(8, rounds):
            acc += _mix16(data, 16 * i, s, 16 * (i - 8) + 3)
        acc += _mix16(data, n - 16, s, 136 - 17)
        return _xxh3_avalanche(acc & M64)
    # long input
    acc = [P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1]
    stripes_per_block = (192 - 64) // 8
    block_len = 64 * stripes_per_block
    nb_blocks = (n - 1) // block_len
    for b in range(nb_blocks):
        for st in range(stripes_per_block):
            _acc512(acc, data, b * block_len + st * 64, s, st * 8)
        _scramble(acc, s, 192 - 64)
    nb_stripes = ((n - 1) - block_len * nb_blocks) // 64
    for st in range(nb_stripes):
        _acc512(acc, data, nb_blocks * block_len + st * 64, s, st * 8)
    _acc512(acc, data, n - 64, s, 192 - 64 - 7)
    return _merge(acc, s, 11, (n * P64_1) & M64)


if __name__ == '__main__':
    # published test vectors (XXH3_64bits, seed 0)
    assert xxh3_64(b'') == 0x2D06800538D394C2
    print('empty ok', hex(xxh3_64(b'abc')))
