#!/usr/bin/env python3
"""Show who holds winewayland's window data lock in a running (or hung) Wine process.

Reads the record kept by patches/wine-hotfixes/pending/0011-winewayland-trace-the-window-data-lock.patch
(wayland_win_data_lock_trace in winewayland.so) through /proc/<pid>/mem, without stopping the
process, and names the code that took the lock.

usage: tools/wwm/wwm_wayland_lock_trace.py [pid]     (default: the running wwm.exe)

When a code path returns with the lock still held, the lock is never free again, so the events
listed start with the acquisition that leaked. "Acquisitions never released" replays them and
lists the acquisitions left without a later release; with nested locking a release cannot be
matched to its acquisition exactly, so read it together with the event list.
"""

import os
import struct
import subprocess
import sys

TRACE_SYMBOL = 'wayland_win_data_lock_trace'
MUTEX_SYMBOL = 'win_data_mutex'
MAGIC = b'WAYLAND_LOCK_TRC'
HEADER = struct.Struct('<16siiIIQIiQ')   # magic .. bad_release_caller, 56 bytes
EVENT = struct.Struct('<Qihh')           # caller, tid, depth, kind
NB_EVENTS = 256
KINDS = {1: 'get', 2: 'create', 3: 'release', 4: 'destroy'}


def find_game():
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            with open(f'/proc/{pid}/comm') as f:
                if f.read().strip() == 'wwm.exe':
                    return int(pid)
        except OSError:
            continue
    return None


def read_maps(pid):
    maps = []
    with open(f'/proc/{pid}/maps') as f:
        for line in f:
            parts = line.split()
            lo, hi = (int(x, 16) for x in parts[0].split('-'))
            maps.append((lo, hi, parts[1], int(parts[2], 16), parts[5] if len(parts) > 5 else ''))
    return maps


def symbol_offsets(path, names):
    out = subprocess.run(['nm', path], capture_output=True, text=True, check=True).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in names:
            found[parts[2]] = int(parts[0], 16)
    return found


def thread_name(pid, tid):
    try:
        with open(f'/proc/{pid}/task/{tid}/comm') as f:
            return f.read().strip()
    except OSError:
        return 'exited'


class Symbolizer:
    def __init__(self, maps, wayland_path, wayland_base):
        self.maps = maps
        self.wayland_path = wayland_path
        self.wayland_base = wayland_base
        self.cache = {}

    def name(self, addr):
        if not addr:
            return '-'
        if addr in self.cache:
            return self.cache[addr]
        result = f'{addr:#x}'
        for lo, hi, _, off, path in self.maps:
            if lo <= addr < hi:
                if path == self.wayland_path:
                    rel = addr - self.wayland_base
                    out = subprocess.run(['addr2line', '-f', '-e', path, f'{rel - 1:#x}'],
                                         capture_output=True, text=True, check=True).stdout.split('\n')
                    result = f'{out[0]}+{rel:#x}'
                else:
                    result = f'{os.path.basename(path) or "[anon]"}+{addr - lo + off:#x}'
                break
        self.cache[addr] = result
        return result


def main():
    if len(sys.argv) > 2:
        sys.exit(__doc__)
    pid = int(sys.argv[1]) if len(sys.argv) == 2 else find_game()
    if not pid:
        sys.exit('wwm.exe is not running; pass a pid.')

    maps = read_maps(pid)
    wayland = [(lo, path) for lo, hi, perm, off, path in maps
               if path.endswith('/winewayland.so') and off == 0]
    if not wayland:
        sys.exit(f'process {pid} has not loaded winewayland.so (not on the Wayland driver).')
    base, path = wayland[0]
    syms = symbol_offsets(path, {TRACE_SYMBOL, MUTEX_SYMBOL})
    if TRACE_SYMBOL not in syms:
        sys.exit(f'{path} has no {TRACE_SYMBOL}: the runner was built without patch 0011.')

    with open(f'/proc/{pid}/mem', 'rb') as mem:
        mem.seek(base + syms[TRACE_SYMBOL])
        raw = mem.read(HEADER.size + EVENT.size * NB_EVENTS)
        mutex = None
        if MUTEX_SYMBOL in syms:
            mem.seek(base + syms[MUTEX_SYMBOL])
            mutex = struct.unpack_from('<iIi', mem.read(12))  # __lock, __count, __owner

    (magic, owner, depth, outer_count, span_events, outer_caller,
     bad_releases, bad_tid, bad_caller) = HEADER.unpack_from(raw)
    if magic != MAGIC:
        sys.exit(f'{TRACE_SYMBOL} in {path} does not hold a trace (magic {magic!r}).')

    sym = Symbolizer(maps, path, base)
    print(f'process {pid}, {path}')
    if mutex:
        lock, count, mutex_owner = mutex
        held = f'held by {mutex_owner} ({thread_name(pid, mutex_owner)}), count {count}' if mutex_owner else 'free'
        print(f'win_data_mutex:  {held}, lock word {lock}')
    if owner:
        print(f'trace owner:     {owner} ({thread_name(pid, owner)}), depth {depth}')
        print(f'held since:      {sym.name(outer_caller)} (outermost acquisition #{outer_count})')
    else:
        print(f'trace owner:     none ({outer_count} outermost acquisitions so far)')
    if bad_releases:
        print(f'bad releases:    {bad_releases}, last by {bad_tid} ({thread_name(pid, bad_tid)}) '
              f'from {sym.name(bad_caller)}')

    shown = min(span_events, NB_EVENTS)
    if not owner or not shown:
        return
    print(f'\nevents since that acquisition ({span_events}, first {shown} kept):')
    stack = []
    for i in range(shown):
        caller, tid, ev_depth, kind = EVENT.unpack_from(raw, HEADER.size + EVENT.size * i)
        kind_name = KINDS.get(kind, str(kind))
        print(f'  {i:3d} {kind_name:8s} depth {ev_depth:2d} tid {tid} {sym.name(caller)}')
        if kind_name in ('get', 'create'):
            stack.append((i, caller))
        elif stack:
            stack.pop()
    if stack:
        print('\nacquisitions never released (bottom first):')
        for i, caller in stack:
            print(f'  event {i}: {sym.name(caller)}')


if __name__ == '__main__':
    main()
