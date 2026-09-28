#!/usr/bin/env python3
"""CPU cost of the game process and of the wineserver serving its prefix, from /proc.

  tools/wwm/wwm_cpu.py record <out.tsv> --pid <wwm.exe pid>
      once a second: epoch, process (game / wineserver), CPU seconds (all threads, user+system),
      voluntary and involuntary context switches summed over live threads, thread count.
      The wineserver is the one whose WINEPREFIX matches the game's. Stops when the game exits.

  tools/wwm/wwm_cpu.py report [--from 5] [--to 80] telemetry/abrun_<run> [...]
      per run: CPU (cores) and context switches per second of each process over the window
      world_visible+from .. world_visible+to (clipped to quit_at), from the run's .cpu.tsv.
"""
import argparse, os, sys, time

TCK = os.sysconf('SC_CLK_TCK')


def environ_prefix(pid):
    try:
        with open(f'/proc/{pid}/environ', 'rb') as f:
            for kv in f.read().split(b'\0'):
                if kv.startswith(b'WINEPREFIX='):
                    return os.path.realpath(kv[len(b'WINEPREFIX='):].decode())
    except OSError:
        pass
    return None


def find_wineserver(prefix):
    for d in os.listdir('/proc'):
        if not d.isdigit():
            continue
        try:
            with open(f'/proc/{d}/comm') as f:
                if f.read().strip() != 'wineserver':
                    continue
        except OSError:
            continue
        if environ_prefix(int(d)) == prefix:
            return int(d)
    return None


def sample(pid):
    """(cpu seconds, voluntary csw, involuntary csw, threads) or None when the process is gone."""
    try:
        with open(f'/proc/{pid}/stat') as f:
            fields = f.read().rsplit(')', 1)[1].split()
        cpu = (int(fields[11]) + int(fields[12])) / TCK  # utime, stime: fields 14, 15
        vol = invol = n = 0
        for tid in os.listdir(f'/proc/{pid}/task'):
            try:
                with open(f'/proc/{pid}/task/{tid}/status') as f:
                    for line in f:
                        if line.startswith('voluntary_ctxt_switches:'):
                            vol += int(line.split()[1])
                        elif line.startswith('nonvoluntary_ctxt_switches:'):
                            invol += int(line.split()[1])
                n += 1
            except OSError:
                pass  # thread exited between listdir and open
        return cpu, vol, invol, n
    except OSError:
        return None


def cmd_record(a):
    prefix = environ_prefix(a.pid)
    if not prefix:
        sys.exit(f'error: no WINEPREFIX in the environment of pid {a.pid}')
    ws = None
    with open(a.out, 'w', buffering=1) as out:
        while True:
            t = time.time()
            g = sample(a.pid)
            if g is None:
                break
            out.write(f'{t:.3f}\tgame\t{g[0]:.2f}\t{g[1]}\t{g[2]}\t{g[3]}\n')
            if ws is None:
                ws = find_wineserver(prefix)
                if ws is not None:
                    print(f'wineserver pid {ws} for {prefix}', flush=True)
            if ws is not None:
                w = sample(ws)
                if w is None:
                    print(f'wineserver pid {ws} exited', flush=True)
                    ws = None
                else:
                    out.write(f'{t:.3f}\twineserver\t{w[0]:.2f}\t{w[1]}\t{w[2]}\t{w[3]}\n')
            time.sleep(max(0.0, a.interval - (time.time() - t)))
    return 0


def markers(path):
    m = {}
    for line in open(path):
        f = line.split(None, 1)
        if len(f) == 2:
            try:
                m[f[0]] = float(f[1])
            except ValueError:
                pass
    return m


def cmd_report(a):
    print(f"{'run':34s} {'process':10s} {'cores':>6s} {'csw/s':>8s} {'invol/s':>8s} {'threads':>7s}"
          f"  (window world+{a.t_from:g}..+{a.t_to:g}s)")
    for run in a.runs:
        base = os.path.basename(run)[len('abrun_'):]
        m = markers(run + '.markers')
        path = run + '.cpu.tsv'
        if 'world_visible' not in m or not os.path.exists(path):
            print(f'{base:34s} (no world_visible marker or no {os.path.basename(path)})')
            continue
        lo = m['world_visible'] + a.t_from
        hi = min(m['world_visible'] + a.t_to, m.get('quit_at', float('inf')))
        rows = {}
        for line in open(path):
            t, proc, cpu, vol, invol, n = line.split('\t')
            t = float(t)
            if lo <= t <= hi:
                rows.setdefault(proc, []).append((t, float(cpu), int(vol), int(invol), int(n)))
        total = 0.0
        for proc in ('game', 'wineserver'):
            r = rows.get(proc, [])
            if len(r) < 2:
                print(f'{base:34s} {proc:10s} (fewer than 2 samples in the window)')
                continue
            dt = r[-1][0] - r[0][0]
            cores = (r[-1][1] - r[0][1]) / dt
            total += cores
            # summed over live threads: a thread exiting mid-window lowers the sum, so clamp at 0
            csw = max(0, r[-1][2] + r[-1][3] - r[0][2] - r[0][3]) / dt
            inv = max(0, r[-1][3] - r[0][3]) / dt
            print(f'{base:34s} {proc:10s} {cores:6.2f} {csw:8.0f} {inv:8.0f} {r[-1][4]:7d}')
        print(f'{base:34s} {"total":10s} {total:6.2f}')
    return 0


ap = argparse.ArgumentParser()
sub = ap.add_subparsers(dest='cmd', required=True)
r = sub.add_parser('record')
r.add_argument('out')
r.add_argument('--pid', type=int, required=True)
r.add_argument('--interval', type=float, default=1.0)
p = sub.add_parser('report')
p.add_argument('runs', nargs='+')
p.add_argument('--from', dest='t_from', type=float, default=5.0)
p.add_argument('--to', dest='t_to', type=float, default=80.0)
a = ap.parse_args()
sys.exit(cmd_record(a) if a.cmd == 'record' else cmd_report(a))
