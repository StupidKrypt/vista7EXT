"""Can a program's main executable start on Vista, 7 and 8 (with or without
VistaExt)? Follows its static imports through its own DLLs, transitively, and
lists what is missing on that chain - the imports the loader needs before the
program runs a single instruction. Delay-loaded imports and other executables
(helpers, updaters) are not on the chain.

    python3 tools/startcheck.py [--sys ...] <out.json> <app>=<main exe> ...
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from peexp import PE
import gapscan


def own_dlls(root, exe_dir):
    """the program's DLLs the loader can find: in the program's own folder,
    and in folders that carry a side-by-side manifest (Chromium's version
    folder: its launcher reaches chrome_elf.dll through that assembly)"""
    out = {}
    for dp, _, fs in os.walk(root):
        if os.path.normpath(dp) != os.path.normpath(exe_dir) and not any(f.lower().endswith('.manifest') for f in fs):
            continue
        for f in fs:
            if f.lower().endswith('.dll'):
                out.setdefault(f.lower(), os.path.join(dp, f))
    return out


def chain(exe, app_root, view):
    """missing (dll, func) on the start chain of exe for one Windows view"""
    own = own_dlls(app_root, os.path.dirname(exe))
    seen, todo, missing = set(), [exe], set()
    local = set(own)
    while todo:
        p = todo.pop()
        if p in seen:
            continue
        seen.add(p)
        try:
            pe = PE(p)
        except Exception:
            continue
        for dll, names in pe.imports():
            d = dll.lower()
            h = view.host(d, local)
            if not h:
                missing.add((d, None))
                continue
            if h[0] == 'local' and d in own:
                todo.append(own[d])
                continue
            for n in names:
                if not view.has(d, n, local):
                    missing.add((d, n))
    return missing


if __name__ == '__main__':
    args = sys.argv[1:]
    if args and args[0] == '--sys':
        gapscan.OSES = [tuple(x.split('=', 1)) for x in args[1].split(',')]
        args = args[2:]
    dest = args[0]
    views = {bits: {os_: gapscan.OSView(os_, s, bits) for os_, s in gapscan.OSES} for bits in (32, 64)}
    res = []
    for a in args[1:]:
        app, exe = a.split('=', 1)
        root = exe
        # the app's folder: up to the level the caller passed as "app/..."
        if '::' in exe:
            root, exe = exe.split('::', 1)
            exe = os.path.join(root, exe)
        else:
            root = os.path.dirname(exe)
        bits = 64 if PE(exe).is64 else 32
        for os_, v in views[bits].items():
            for d, n in sorted(chain(exe, root, v), key=lambda x: (x[0], x[1] or '')):
                res.append({'app': app, 'exe': os.path.relpath(exe, root), 'os': os_, 'bits': bits, 'dll': d, 'func': n,
                            'runtime': d.startswith(gapscan.RUNTIME),
                            'vistaext': gapscan.vistaext_supplies(os_, bits, d, n)})
    json.dump(res, open(dest, 'w'), indent=1)
    print('%d start-chain gaps written to %s' % (len(res), dest))
