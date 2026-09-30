#!/usr/bin/env python3
"""In-world frame latency and low-latency pacing from the vkd3d-proton queue traces of AUTO runs.

usage: tools/wwm/latency.py [--from 5] [--to 40] telemetry/abrun_<run> [...]

For each frame presented in the window from --from to --to seconds after world_visible (or,
with --anchor teleport, after teleport_start from the game log, for runs without screen checks):

  queue latency = end of that frame's present blit on the GPU queue ("PRESENT (id = N) (blit)",
                  completed when its fence signals) minus the start of the application's
                  IDXGISwapChain::Present() call for the same N ("PRESENT (id = N)").

It is how far the GPU trails the game's CPU at present time: with frames queued up it is
several frame times, with low-latency pacing close to one GPU frame. It does not include the
scanout wait or input sampling, so it is a lower bound on the latency the layer can remove.

Also reported: the LATENCY SLEEP blocks vkd3d-proton records around vkLatencySleepNV (count per
presented frame and median / 95th percentile duration); none means no low-latency pacing ran.
Trace time is anchored to wall time as in frametimes.py (last trace event ~= quit_at).
"""
import argparse, json, os, re, statistics

ap = argparse.ArgumentParser()
ap.add_argument('runs', nargs='+')
ap.add_argument('--from', dest='t_from', type=float, default=5.0)
ap.add_argument('--to', dest='t_to', type=float, default=40.0)
ap.add_argument('--anchor', choices=('world', 'teleport'), default='world',
                help='window start: world_visible (screen, X11 runs) or teleport_start (game log; also Wayland runs)')
a = ap.parse_args()

ID = re.compile(r'PRESENT \(id = (\d+)\)( \(blit\))?$')


def markers(path):
    m, s = {}, {}
    for line in open(path):
        k, v = line.split(None, 1)
        try:
            m[k] = float(v)
        except ValueError:
            s[k] = v.strip()
    return m, s


def pct(v, p):
    return statistics.quantiles(v, n=100)[p - 1] if len(v) >= 2 else float('nan')


print(f"{'run':30s} {'layer':6s} {'frames':>6s} {'lat p50':>8s} {'p95':>6s} {'p99':>6s} "
      f"{'sleeps/f':>8s} {'sleep p50':>9s} {'p95':>6s}  (ms, window {a.anchor}+{a.t_from:g}..+{a.t_to:g}s)")
for run in a.runs:
    base = os.path.basename(run)[len('abrun_'):]
    trace = os.path.join(os.path.dirname(run), f'vkd3d_queue_{base}.json')
    m, s = markers(run + '.markers')
    layer = s.get('low_latency_layer', '?')
    layer = 'none' if layer == 'none' else ('on' if 'KORTHOS' in layer else '?')
    key = 'world_visible' if a.anchor == 'world' else 'teleport_start'
    if key not in m or 'quit_at' not in m or not os.path.exists(trace):
        print(f'{base:30s} {layer:6s} (no {key}/quit_at marker or no trace)')
        continue
    calls, blits, sleeps = {}, {}, []
    last = 0.0
    for line in open(trace):
        line = line.strip().rstrip(',')
        if not line.startswith('{'):
            continue
        try:
            e = json.loads(line)
        except json.JSONDecodeError:
            continue
        ts, dur = e['ts'] / 1e6, e.get('dur', 0) / 1e6
        last = max(last, ts + dur)
        name = e.get('name', '')
        if name.startswith('LATENCY SLEEP'):
            sleeps.append((ts, dur))
            continue
        mt = ID.match(name)
        if not mt or e.get('ph') != 'X':
            continue
        n = int(mt.group(1))
        if mt.group(2):
            blits[n] = ts + dur
        elif str(e.get('pid')) == 'IDXGISwapChain::Present()':
            calls[n] = ts
    off = m['quit_at'] - last
    lo, hi = m[key] + a.t_from - off, m[key] + a.t_to - off
    lat = [(blits[n] - t) * 1e3 for n, t in calls.items() if lo <= t <= hi and n in blits]
    sl = [d * 1e3 for t, d in sleeps if lo <= t <= hi]
    if len(lat) < 10:
        print(f'{base:30s} {layer:6s} (only {len(lat)} matched frames in window)')
        continue
    print(f'{base:30s} {layer:6s} {len(lat):6d} {statistics.median(lat):8.2f} {pct(lat, 95):6.2f} {pct(lat, 99):6.2f} '
          f'{len(sl) / len(lat):8.2f} {(statistics.median(sl) if sl else float("nan")):9.2f} {pct(sl, 95):6.2f}')
