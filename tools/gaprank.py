"""Merge the evidence of what programs miss on each Windows and rank it by
how many programs need each item.

    python3 tools/gaprank.py [--md out.md] [--json out.json]

Reads (made by tools/rescan.sh and tools/traceparse.py):
  out/gaps/scan2.json   imports of every file of each program (static)
  out/gaps/start.json   imports along each main program's start chain
  out/gaps/trace2.json  what traced runs in the test VMs looked for and missed

Leaves out what the current build supplies (tools/gapscan.vistaext_supplies,
the same rule the scans use) and Microsoft's C runtime (its own installer).

Evidence, strongest first:
  START  imported by the main program or a DLL it loads at start: the
         program cannot start without it (unless it is delay-loaded)
  RUN    a traced run failed on it (procedure or DLL not found at load)
  LOOKUP a traced run asked for it at run time and did not get it (the
         program may cope: many such lookups are probes)
  FILES  imported by some file of the program (a feature that is used later,
         a helper, a plugin)
"""
import argparse
import json
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gapscan  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
G = os.path.join(ROOT, 'out', 'gaps')
STRENGTH = {'START': 0, 'RUN': 1, 'LOOKUP': 2, 'FILES': 3}


def trace_os(header):
    os_ = 'vista' if 'Vista' in header else 'win7' if 'Windows 7' in header else 'win8' if 'Windows 8' in header else None
    return os_, 64 if header.rstrip().endswith('x64') else 32


def app_name(n):
    """trace names of 32-bit runs (edge32) are the same program"""
    return n[:-2] if n.endswith('32') and len(n) > 2 else n


def supplied_anywhere(os_, bits, func):
    """for a run-time lookup (no DLL known): does any of this build's DLLs export it?"""
    arch = 'x64' if bits == 64 else 'x86'
    b = os.path.join(ROOT, 'out', '%s-%s' % (os_, arch))
    if not os.path.isdir(b):
        return False
    for f in os.listdir(b):
        if f.lower().endswith('.dll'):
            try:
                if func in gapscan.PE(os.path.join(b, f)).exports():
                    return True
            except Exception:
                pass
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--md')
    ap.add_argument('--json')
    a = ap.parse_args()
    # (os, item) -> {'dlls': set, 'apps': {app: strongest evidence}}
    items = defaultdict(lambda: {'dlls': set(), 'apps': {}})

    def add(os_, dll, func, app, ev):
        key = (os_, func if func else dll.lower())
        it = items[key]
        if dll:
            it['dlls'].add(dll.lower())
        it['func'] = func
        old = it['apps'].get(app)
        if old is None or STRENGTH[ev] < STRENGTH[old]:
            it['apps'][app] = ev

    for x in json.load(open(os.path.join(G, 'scan2.json'))):
        if x['vistaext'] or x['runtime']:
            continue
        for app in x['apps']:
            add(x['os'], x['dll'], x['func'], app, 'FILES')
    for x in json.load(open(os.path.join(G, 'start.json'))):
        if x['vistaext'] or x['runtime']:
            continue
        add(x['os'], x['dll'], x['func'], x['app'], 'START')
    tp = os.path.join(G, 'trace2.json')
    if os.path.exists(tp):
        for header, progs in json.load(open(tp)).items():
            os_, bits = trace_os(header)
            if not os_:
                continue
            for prog, r in progs.items():
                app = app_name(prog)
                for k in r.get('procmissing', {}):
                    m = re.match(r'^(.+?)!(.+)$', k)
                    if not m:
                        continue
                    dll, func = m.group(1), m.group(2)
                    if func.startswith('#') or gapscan.vistaext_supplies(os_, bits, dll, func):
                        continue
                    add(os_, dll, func, app, 'RUN')
                for dll in r.get('dllmissing', {}):
                    if gapscan.vistaext_supplies(os_, bits, dll, None):
                        continue
                    add(os_, dll, None, app, 'LOOKUP')
                for func in r.get('notfound', {}):
                    if func.startswith('wine_') or supplied_anywhere(os_, bits, func):
                        continue
                    add(os_, '', func, app, 'LOOKUP')

    out = {}
    for (os_, name), it in items.items():
        apps = it['apps']
        best = min(apps.values(), key=lambda e: STRENGTH[e])
        out.setdefault(os_, []).append({
            'item': name, 'func': it.get('func'), 'dlls': sorted(it['dlls']), 'count': len(apps),
            'strongest': best, 'apps': {k: apps[k] for k in sorted(apps)},
            'blocking': sorted(k for k, e in apps.items() if e in ('START', 'RUN'))})
    for os_ in out:
        out[os_].sort(key=lambda r: (-len(r['blocking']), -r['count'], STRENGTH[r['strongest']], r['item'].lower()))
    if a.json:
        json.dump(out, open(a.json, 'w'), indent=1)
    lines = []
    for os_ in ('vista', 'win7', 'win8'):
        rows = out.get(os_, [])
        lines.append('## %s: %d items' % ({'vista': 'Windows Vista', 'win7': 'Windows 7', 'win8': 'Windows 8'}[os_], len(rows)))
        lines.append('')
        lines.append('| # | missing | in | programs | stops start/run | evidence |')
        lines.append('|---|---|---|---|---|---|')
        for i, r in enumerate(rows, 1):
            ev = ', '.join('%s (%s)' % (k, e) for k, e in r['apps'].items())
            lines.append('| %d | `%s` | %s | %d | %s | %s |' % (
                i, r['item'], ', '.join(r['dlls']) or '(looked up)', r['count'],
                ', '.join(r['blocking']) or '-', ev))
        lines.append('')
    text = '\n'.join(lines)
    if a.md:
        open(a.md, 'w').write(text)
    for os_ in ('vista', 'win7', 'win8'):
        rows = out.get(os_, [])
        print('%s: %d items, %d stop a program at start or during the traced run' % (
            os_, len(rows), sum(1 for r in rows if r['blocking'])))


if __name__ == '__main__':
    main()
