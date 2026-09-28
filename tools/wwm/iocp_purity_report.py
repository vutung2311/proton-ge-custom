#!/usr/bin/env python3
"""Classify the game's completion ports from a gen_iocp_purity.sh capture.

usage: tools/wwm/iocp_purity_report.py <bpftrace.json>

A port is "pure" (eligible for an in-process queue with one-way fallback) when, over the whole
capture, it had no file/socket bound, no job object, no duplication, no alertable dequeue and
no direct wait. Ports are identified by handle value; a handle closed and re-created shows up as
separate lifetimes in the event list.
"""
import json, sys
from collections import Counter, defaultdict

src = sys.argv[1]
ports = {}                       # handle -> info
events = defaultdict(list)       # handle -> [(kind, detail)]
tot = defaultdict(Counter)       # map name -> handle(/key) -> count
for line in open(src):
    try:
        o = json.loads(line)
    except json.JSONDecodeError:
        continue
    if o.get('type') == 'printf':
        for ln in o['data'].splitlines():
            f = ln.split()
            if not f or f[0].startswith('#'):
                continue
            k = f[0]
            if k in ('C', 'O'):
                h = int(f[3], 16)
                ports.setdefault(h, {'created': 0})
                ports[h]['created'] += 1
                events[h].append(('create' if k == 'C' else 'open', f[4] if k == 'C' else ''))
            elif k == 'B':
                events[int(f[4], 16)].append(('bind', f'file {f[3]} key {f[5]}'))
            elif k == 'R':
                events[int(f[4], 16)].append(('replace', f'file {f[3]}'))
            elif k == 'J':
                events[int(f[4], 16)].append(('job', f'job {f[3]}'))
            elif k == 'D':
                events[int(f[3], 16)].append(('dup', f'src {f[4]} dst {f[5]} options {f[6]}'))
            elif k == 'X':
                events[int(f[3], 16)].append(('close', ''))
    elif o.get('type') == 'map':
        for name, d in o['data'].items():
            for key, v in d.items():
                tot[name][key] += v


def hint(h):
    return str(h)


rows = []
for h in sorted(set(ports) | {int(k.split(',')[0]) for k in tot['@set']} | {int(k.split(',')[0]) for k in tot['@rm']}):
    posts = tot['@set'].get(str(h), 0) + tot['@setx'].get(str(h), 0)
    rm = tot['@rm'].get(str(h), 0)
    ex_na = tot['@ex'].get(f'{h}, 0', 0) + tot['@ex'].get(f'{h},0', 0)
    ex_al = sum(v for k, v in tot['@ex'].items() if k.replace(' ', '').startswith(f'{h},') and not k.replace(' ', '').endswith(',0'))
    waits = tot['@wso'].get(str(h), 0) + tot['@wmo'].get(str(h), 0)
    kinds = Counter(k for k, _ in events[h])
    impure = []
    if kinds['bind'] or kinds['replace']: impure.append(f"{kinds['bind'] + kinds['replace']} file/socket bindings")
    if kinds['job']: impure.append('job object')
    if kinds['dup']: impure.append(f"{kinds['dup']} duplications")
    if ex_al: impure.append(f'{ex_al} alertable dequeues')
    if waits: impure.append(f'{waits} direct waits')
    known = h in ports
    rows.append((posts + rm + ex_na + ex_al, h, known, posts, rm, ex_na, ex_al, impure, kinds))

total_ops = sum(r[0] for r in rows)
pure_ops = sum(r[0] for r in rows if not r[7] and r[2])
print(f'{src}: {len(ports)} ports created/opened; completion-port operations {total_ops}, on pure ports {pure_ops} '
      f'({100 * pure_ops / total_ops if total_ops else 0:.1f}%)')
print(f"  {'port':>8s} {'posts':>9s} {'dequeue':>9s} {'deqEx':>8s} {'deqEx-alrt':>10s}  class")
for ops, h, known, posts, rm, ex_na, ex_al, impure, kinds in sorted(rows, reverse=True)[:25]:
    cls = 'PURE' if known and not impure else ('not created while traced' if not known else 'impure: ' + ', '.join(impure))
    print(f'  {h:#8x} {posts:9d} {rm:9d} {ex_na:8d} {ex_al:10d}  {cls}')
