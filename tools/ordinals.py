"""Record the export numbers (ordinals) of Windows system DLLs.

    python3 tools/ordinals.py <dll> vista-32=<dir> vista-64=<dir> win7-32=<dir> win7-64=<dir>

Reads <dir>/<dll>.dll from copies of Vista's and 7's System32 and writes
third_party/winexports/<dll>.ords: one line per ordinal, with the name it
has on each Windows (or "-" for an export that only has a number). Only
numbers and names, no code. tools/gen.py uses it so that each VistaExt
companion exports every ordinal of the DLL it stands in for, forwarded by
number: a program that imports shell32 #155 then gets what #155 is on that
Windows, as it would without VistaExt.
"""
import os
import struct
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def ordinals(path):
    """{ordinal: name or None} of a DLL's export table."""
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    magic = struct.unpack_from('<H', data, pe + 24)[0]
    dd = pe + 24 + (112 if magic == 0x20b else 96)
    erva, esz = struct.unpack_from('<II', data, dd)
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt = struct.unpack_from('<H', data, pe + 20)[0]
    secs = [struct.unpack_from('<IIII', data, pe + 24 + opt + 40 * i + 8) for i in range(nsec)]

    def off(r):
        for vs, va, rs, ra in secs:
            if va <= r < va + max(vs, rs):
                return r - va + ra
        raise ValueError(hex(r))
    e = off(erva)
    base, nf, nn, af, an, ao = struct.unpack_from('<IIIIII', data, e + 16)
    names = {}
    for i in range(nn):
        nr = struct.unpack_from('<I', data, off(an) + 4 * i)[0]
        idx = struct.unpack_from('<H', data, off(ao) + 2 * i)[0]
        names[idx] = data[off(nr):off(nr) + 256].split(b'\0')[0].decode('latin1')
    out = {}
    for i in range(nf):
        if struct.unpack_from('<I', data, off(af) + 4 * i)[0]:
            out[base + i] = names.get(i)
    return out


def main():
    dll = sys.argv[1].lower()
    seen = defaultdict(dict)        # ordinal -> {where: name}
    wheres = []
    for arg in sys.argv[2:]:
        where, d = arg.split('=', 1)
        p = os.path.join(d, dll + '.dll')
        if not os.path.exists(p):
            continue
        wheres.append(where)
        for o, n in ordinals(p).items():
            seen[o][where] = n or '-'
    out = os.path.join(ROOT, 'third_party', 'winexports', dll + '.ords')
    with open(out, 'w') as f:
        f.write('# %s.dll export ordinals: %s.\n' % (dll, ', '.join(wheres)))
        f.write('# Each line: ordinal, then <windows>:<name> ("-" = exported by number only).\n')
        for o in sorted(seen):
            f.write('%d %s\n' % (o, ' '.join('%s:%s' % (w, seen[o][w]) for w in wheres if w in seen[o])))
    print('%s: %d ordinals' % (out, len(seen)))


if __name__ == '__main__':
    main()
