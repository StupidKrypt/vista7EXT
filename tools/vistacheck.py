"""Fail if a VistaExt program imports a name that the DLL it names lacks on
Windows Vista SP2 (or 7 SP1), according to third_party/winexports.

    python3 tools/vistacheck.py <file.exe> ...
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from peexp import PE

W = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'third_party', 'winexports')


def table(dll):
    p = os.path.join(W, dll.lower().replace('.dll', '') + '.txt')
    if not os.path.exists(p):
        return None
    t = {}
    for l in open(p):
        if l.startswith('#') or not l.strip():
            continue
        parts = l.split()
        t[parts[0]] = parts[1].split(',') if len(parts) > 1 else []
    return t


bad = 0
for f in sys.argv[1:]:
    pe = PE(f)
    bits = '64' if pe.is64 else '32'
    for dll, names in pe.imports():
        t = table(dll)
        if t is None:
            continue
        for n in names:
            if n.startswith('#'):
                continue
            for osn in ('vista', 'win7'):
                # "vista" without "-32"/"-64": both (lists made from one architecture's names)
                if n not in t or ('%s-%s' % (osn, bits) not in t[n] and osn not in t[n]):
                    print('%s: %s!%s is missing on %s %s-bit' % (f, dll, n, osn, bits))
                    bad += 1
sys.exit(1 if bad else 0)
