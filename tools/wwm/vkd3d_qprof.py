#!/usr/bin/env python3
"""Summarize a vkd3d-proton VKD3D_QUEUE_PROFILE trace over the teleport load window.

usage: tools/wwm/vkd3d_qprof.py telemetry/vkd3d_queue_<run>.json telemetry/abrun_<run>.markers

Trace timestamps are microseconds since vkd3d device creation. wwm_ab_run.sh SIGTERMs
the game right after the world_visible keypress and presents keep flowing until then,
so the last trace event ~= world_visible; that anchors trace time to wall time.
"""
import json, re, statistics, sys
from collections import defaultdict

trace, markers = sys.argv[1], sys.argv[2]
m = {}
src = {}
for line in open(markers):
    k, v = line.split(None, 1)
    try:
        m[k] = float(v)
    except ValueError:
        src[k] = v.strip()  # descriptive markers such as teleport_source
if 'teleport_start' not in m or not ({'world_visible', 'quit_at'} & set(m)):
    sys.exit('markers lack teleport_start and world_visible/quit_at')
# The game is SIGTERMed right after world_visible (keypress runs) or quit_at (AUTO=1 runs),
# and presents keep flowing until then, so the trace's last event anchors trace time.
anchor = m['quit_at'] if 'quit_at' in m else m['world_visible']

ev = []
for line in open(trace):
    line = line.strip().rstrip(',')
    if line.startswith('{'):
        try:
            ev.append(json.loads(line))
        except json.JSONDecodeError:
            pass  # torn last line from the kill

last = max(e['ts'] + e.get('dur', 0) for e in ev) / 1e6
off = anchor - last                          # wall = trace + off
w0 = (m['teleport_start'] - off) * 1e6

# Streaming bursts: 2-s bins (from the load start) with >= BURST copy-queue submissions.
BURST = 10
allcp = sorted(e['ts'] for e in ev if 'COPY' in str(e.get('tid')) and e['name'].startswith('SUBMIT') and e['ts'] >= w0)
bins = {}
for t in allcp:
    bins.setdefault(int((t - w0) / 2e6), []).append(t)
burst_bins = sorted(k for k, v in bins.items() if len(v) >= BURST)
runs = []
for k in burst_bins:
    if runs and k == runs[-1][1] + 1:
        runs[-1][1] = k
    else:
        runs.append([k, k])
if runs:
    load_end = (max(bins[runs[-1][1]]) - w0) / 1e6
    spans = ', '.join(f"{(min(bins[a]) - w0) / 1e6:.1f}-{(max(bins[b]) - w0) / 1e6:.1f}s" for a, b in runs)
    gaps = [((min(bins[runs[i + 1][0]]) - max(bins[runs[i][1]])) / 1e6) for i in range(len(runs) - 1)]
    print(f"{trace}: upload bursts at {spans}; longest pause between bursts "
          f"{max(gaps) if gaps else 0:.1f}s; load end (last burst) {load_end:.1f}s after teleport")
else:
    load_end = None
    print(f"{trace}: no upload bursts found")
w1 = (m['world_visible'] - off) * 1e6 if 'world_visible' in m else (w0 + load_end * 1e6 if load_end else last * 1e6)
win = w1 - w0
wv_src = src.get('world_visible_source', 'keypress')
print(f"{trace}: load window {win/1e6:.1f}s ({wv_src if 'world_visible' in m else 'last upload burst'})")

def lane(e):
    pid, tid, name = str(e.get('pid')), str(e.get('tid')), e.get('name', '')
    if pid == 'pso':
        return 'pso ' + name.split(' ', 1)[1]
    if tid.startswith('family'):
        q = tid.split(',')[1].strip()
        return f'gpu {q} ' + ('PRESENT' if name.startswith('PRESENT') else name.split()[0])
    if tid in ('regions', 'submit', 'overhead', 'event', 'cpu'):
        return f'{tid} ' + re.sub(r'\s*[0-9a-f]{16}.*|\s*#\d+.*|\s*\(id.*', '', name)
    return f'{pid} {tid}'[:40]

stats = defaultdict(list)
for e in ev:
    ts, d = e['ts'], e.get('dur', 0.0)
    if ts + d < w0 or ts > w1:
        continue
    stats[lane(e)].append(d)

print(f"  {'lane':34s} {'n':>6s} {'n/s':>6s} {'sum ms':>8s} {'%win':>5s} {'p50us':>7s} {'max ms':>7s}")
for k in sorted(stats, key=lambda k: -sum(stats[k]))[:12]:
    v = sorted(stats[k])
    print(f"  {k:34s} {len(v):6d} {len(v)/win*1e6:6.1f} {sum(v)/1e3:8.1f} {sum(v)/win*100:5.1f} "
          f"{statistics.median(v):7.1f} {v[-1]/1e3:7.1f}")

# Copy-queue submissions are the asset uploads: their cadence shows streaming progress.
cp = sorted(e['ts'] for e in ev if 'COPY' in str(e.get('tid')) and e['name'].startswith('SUBMIT') and w0 <= e['ts'] <= w1)
nb = int(win / 2e6) + 1
h = [0] * nb
for t in cp:
    h[int((t - w0) / 2e6)] += 1
print(f"  copy submits per 2s ({len(cp)} total): " + ' '.join(map(str, h)))
gaps = [(b - a) / 1e3 for a, b in zip(cp, cp[1:])]
timerish = [g for g in gaps if abs(g - 600) < 3 or abs(g - 1000) < 3]
print(f"  gaps within 3 ms of 600/1000 ms: {len(timerish)} of {len(gaps)}")
