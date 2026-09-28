#!/usr/bin/env python3
"""In-world frame-time statistics from the vkd3d-proton queue traces of AUTO runs.

usage: tools/wwm/frametimes.py [--from 5] [--to 40] telemetry/abrun_<run> [...]

For each run: the present timestamps ("PRESENT (id = N)" events on the IDXGISwapChain::Present() lane) in the
window from --from to --to seconds after world_visible (the screen-measured world HUD), and
from them the frame rate, median / 95th / 99th percentile frame time, the worst frame, and the
number of frames longer than 25 ms and 50 ms. Trace time is anchored to wall time as in
vkd3d_qprof.py (the game is killed at quit_at, so the trace's last event ~= quit_at).
"""
import argparse, json, os, statistics

ap = argparse.ArgumentParser()
ap.add_argument('runs', nargs='+')
ap.add_argument('--from', dest='t_from', type=float, default=5.0)
ap.add_argument('--to', dest='t_to', type=float, default=40.0)
a = ap.parse_args()


def markers(path):
    m = {}
    for line in open(path):
        k, v = line.split(None, 1)
        try:
            m[k] = float(v)
        except ValueError:
            pass
    return m


print(f"{'run':34s} {'fps':>6s} {'p50':>6s} {'p95':>6s} {'p99':>6s} {'max':>7s} {'>25ms':>6s} {'>50ms':>6s}  (ms, window world+{a.t_from:g}..+{a.t_to:g}s)")
for run in a.runs:
    base = os.path.basename(run)[len('abrun_'):]
    trace = os.path.join(os.path.dirname(run), f'vkd3d_queue_{base}.json')
    m = markers(run + '.markers')
    if 'world_visible' not in m or not os.path.exists(trace):
        print(f'{base:34s} (no world_visible marker or no trace)')
        continue
    ev = []
    last = 0.0
    for line in open(trace):
        line = line.strip().rstrip(',')
        if not line.startswith('{'):
            continue
        try:
            e = json.loads(line)
        except json.JSONDecodeError:
            continue
        end = (e['ts'] + e.get('dur', 0)) / 1e6
        last = max(last, end)
        if str(e.get('pid')) == 'IDXGISwapChain::Present()' and e.get('name', '').startswith('PRESENT'):
            ev.append(e['ts'] / 1e6)
    anchor = m.get('quit_at')
    if anchor is None:
        print(f'{base:34s} (no quit_at)')
        continue
    off = anchor - last
    lo, hi = m['world_visible'] + a.t_from, m['world_visible'] + a.t_to
    ts = sorted(t + off for t in ev if lo <= t + off <= hi)
    if len(ts) < 10:
        print(f'{base:34s} (only {len(ts)} presents in window)')
        continue
    ft = [(b - c) * 1e3 for c, b in zip(ts, ts[1:])]
    q = statistics.quantiles(ft, n=100)
    fps = (len(ts) - 1) / (ts[-1] - ts[0])
    print(f'{base:34s} {fps:6.1f} {statistics.median(ft):6.2f} {q[94]:6.2f} {q[98]:6.2f} {max(ft):7.1f} '
          f'{sum(f > 25 for f in ft):6d} {sum(f > 50 for f in ft):6d}')
