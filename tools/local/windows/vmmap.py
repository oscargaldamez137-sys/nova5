#!/usr/bin/env python3
"""vmmap.py PID [LO HI]: committed/reserved regions of a process between LO and HI (hex), merged by
allocation base, for finding what a driver placed where."""
import ctypes
import ctypes.wintypes as wt
import sys


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_uint64), ('AllocationBase', ctypes.c_uint64),
                ('AllocationProtect', wt.DWORD), ('PartitionId', wt.WORD), ('pad0', wt.WORD),
                ('RegionSize', ctypes.c_uint64), ('State', wt.DWORD), ('Protect', wt.DWORD),
                ('Type', wt.DWORD), ('pad1', wt.DWORD)]


def main():
    pid = int(sys.argv[1])
    lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x10_0000_0000
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.OpenProcess.restype = wt.HANDLE
    kernel32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(MBI), ctypes.c_size_t]
    kernel32.VirtualQueryEx.restype = ctypes.c_size_t
    process = kernel32.OpenProcess(0x0400 | 0x0010, False, pid)
    if not process:
        raise SystemExit(f'OpenProcess failed: {ctypes.get_last_error()}')
    states = {0x1000: 'COMMIT', 0x2000: 'RESERVE', 0x10000: 'FREE'}
    types = {0x20000: 'PRIVATE', 0x40000: 'MAPPED', 0x1000000: 'IMAGE', 0: '-'}
    address = lo
    groups = {}
    order = []
    while address < hi:
        mbi = MBI()
        if kernel32.VirtualQueryEx(process, address, ctypes.byref(mbi), ctypes.sizeof(mbi)) == 0:
            break
        state = states.get(mbi.State, hex(mbi.State))
        if state != 'FREE':
            key = (mbi.AllocationBase, types.get(mbi.Type, hex(mbi.Type)))
            if key not in groups:
                groups[key] = [mbi.BaseAddress, 0, set()]
                order.append(key)
            groups[key][1] = mbi.BaseAddress + mbi.RegionSize - groups[key][0]
            groups[key][2].add(state)
        else:
            order.append(('free', mbi.BaseAddress, mbi.RegionSize))
        address = mbi.BaseAddress + mbi.RegionSize
    for item in order:
        if item[0] == 'free':
            if item[2] >= 0x100000:
                print(f'  {item[1]:#014x} {item[2] / 2**20:12.1f} MiB  free')
            continue
        base, size, st = groups[item]
        print(f'  {base:#014x} {size / 2**20:12.1f} MiB  {item[1]:8s} {"/".join(sorted(st))}')


if __name__ == '__main__':
    main()
