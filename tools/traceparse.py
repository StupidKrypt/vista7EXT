"""Summarise traced program runs (tests/vm/apps.cmd :trace, vxdbg output).

    python3 tools/traceparse.py <results folder> [<results folder> ...] [--json out.json]

Each folder is a VM's results (results.txt + <name>-run.txt). Per program:
  - running or not after a minute (results.txt);
  - lookups VistaExt could not answer ("vx: GetProcAddress X: NOT FOUND");
  - DLLs the program tried to load at run time that this Windows lacks (the
    loader's own messages: LdrLoadDll ... RETURN 0xc0000135 for the name the
    same thread asked for);
  - procedures the loader could not find (0xc0000139 / 0xc0000138);
  - crashes: unhandled exceptions with the module and offset.
"""
import json
import os
import re
import sys
from collections import Counter, defaultdict

LINE = re.compile(r'^\s*\[(\d+) ([^\]]+)\] (.*)$')
SNAP = re.compile(r'^([0-9a-f]+:[0-9a-f]+) @ [0-9a-f]+ - (\w+) - (ENTER|RETURN|INFO|ERROR|WARNING): (.*)$')


def parse_run(path):
    out = {'notfound': Counter(), 'dllmissing': Counter(), 'procmissing': Counter(), 'crashes': Counter(),
           'exits': Counter(), 'processes': set()}
    asked = {}                                    # thread -> last DLL name LdrLoadDll was asked for
    proc_asked = {}
    for raw in open(path, encoding='utf-8', errors='replace'):
        v = re.match(r'^\[vxdbg: (\d+) process\(es\), (\d+) exception report\(s\); first process (still running|exit (0x[0-9A-Fa-f]+))', raw)
        if v:
            # vxdbg's verdict after its time limit: the program was still
            # running (it then stopped it), or how its first process ended
            out['verdict'] = 'still running' if v.group(3) == 'still running' else 'exit ' + v.group(4)
            continue
        m = LINE.match(raw)
        if not m:
            continue
        pid, img, msg = m.groups()
        out['processes'].add(img)
        if 'GetProcAddress' in msg and 'NOT FOUND' in msg:
            out['notfound'][msg.split('GetProcAddress', 1)[1].split(':')[0].strip()] += 1
            continue
        if msg.startswith('UNHANDLED exception'):
            out['crashes'][msg.replace('UNHANDLED exception ', '')] += 1
            continue
        if msg.startswith('exited '):
            out['exits'][msg.split()[1]] += 1
            continue
        s = SNAP.match(msg)
        if not s:
            continue
        th, fn, kind, rest = s.groups()
        # the loader's own report of an import it could not bind (static or
        # LoadLibrary-time imports of a DLL): procedure and the DLL it named
        mm = re.match(r'Procedure "([^"]+)" could not be located in DLL "([^"]+)"', rest)
        if fn == 'LdrpSnapThunk' and mm:
            out['procmissing']['%s!%s' % (mm.group(2).lower(), mm.group(1))] += 1
            continue
        mm = re.match(r'Ordinal "?(\d+)"? could not be located in DLL "([^"]+)"', rest)
        if fn == 'LdrpSnapThunk' and mm:
            out['procmissing']['%s!#%s' % (mm.group(2).lower(), mm.group(1))] += 1
            continue
        key = (pid, th)
        if fn == 'LdrLoadDll' and kind == 'ENTER':
            mm = re.match(r'DLL name: (\S+)', rest)
            if mm:
                asked[key] = mm.group(1)
        elif fn == 'LdrLoadDll' and kind == 'RETURN' and '0xc0000135' in rest.lower() and key in asked:
            # the loader retries a bare name with the full system path: count the name once
            n = asked.pop(key).lower().replace('/', '\\').rsplit('\\', 1)[-1]
            if not n.endswith('.dll') and '.' not in n:
                n += '.dll'
            out['dllmissing'][n] += 1
        elif fn in ('LdrGetProcedureAddress', 'LdrGetProcedureAddressEx', 'LdrpGetProcedureAddress') and kind == 'ENTER':
            mm = re.search(r'[Pp]rocedure name: (\S+)', rest)
            if mm:
                proc_asked[key] = mm.group(1)
        elif fn in ('LdrGetProcedureAddress', 'LdrGetProcedureAddressEx') and kind == 'RETURN' and \
                re.search(r'0xc000013[89]', rest.lower()) and key in proc_asked:
            out['procmissing'][proc_asked.pop(key)] += 1
    out['processes'] = sorted(out['processes'])
    return out


def parse_folder(folder):
    res = {}
    txt = os.path.join(folder, 'results.txt')
    lines = open(txt, encoding='utf-8', errors='replace').read().splitlines() if os.path.exists(txt) else []
    win = next((l.split(':', 1)[1].strip() for l in lines if l.startswith('Windows:')), os.path.basename(folder))
    for l in lines:
        m = re.match(r'^(\S+): (running after a minute|NOT running after a minute)', l.strip())
        if m:
            res.setdefault(m.group(1), {})['running'] = m.group(2).startswith('running')
    for f in os.listdir(folder):
        if f.endswith('-run.txt'):
            name = f[:-8]
            res.setdefault(name, {}).update(parse_run(os.path.join(folder, f)))
    return win, res


if __name__ == '__main__':
    args = [a for a in sys.argv[1:]]
    dest = None
    if '--json' in args:
        i = args.index('--json'); dest = args[i + 1]; del args[i:i + 2]
    allres = {}
    for folder in args:
        win, res = parse_folder(folder)
        # several folders of one Windows (runs in phases): one entry, merged
        allres.setdefault(win, {}).update(res)
        print('=== %s' % win)
        for name in sorted(res):
            r = res[name]
            print('%-12s %s' % (name, r.get('verdict', 'no vxdbg verdict')))
            for k, label in (('crashes', 'crash'), ('dllmissing', 'DLL not found'), ('notfound', 'lookup not found'),
                             ('procmissing', 'procedure not found')):
                if r.get(k):
                    print('    %-20s %s' % (label + ':', ', '.join('%s (%d)' % (x, n) for x, n in r[k].most_common(12))))
    if dest:
        json.dump(allres, open(dest, 'w'), indent=1, default=list)
