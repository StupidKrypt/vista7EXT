"""Add one Windows version to the export lists in third_party/winexports.

    python3 tools/addwinexports.py <os> <64-bit System32> <32-bit SysWOW64> [dll ...]

<os> is the tag gen.py and vistacheck.py use ("win8"). For every list
(<dll>.txt: names; <dll>.ords: numbers) the DLL of that Windows is read with
tools/peexp.py / tools/ordinals.py and its tags <os>-32 / <os>-64 are set
on each line (added, or replaced when the tool ran before); names only that
Windows has get new lines. Extra dll names create a new <dll>.txt (for
kernelbase.dll, which Vista lacks). The other Windows' tags are not touched.
Only names and numbers are stored, no code.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
from peexp import PE
from ordinals import ordinals

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W = os.path.join(ROOT, 'third_party', 'winexports')


def find(d, dll):
    for f in os.listdir(d):
        if f.lower() == dll + '.dll':
            return os.path.join(d, f)
    return None


def names_of(path):
    return set(PE(path).exports()) if path else set()


def update_txt(dll, os_, dirs):
    p = os.path.join(W, dll + '.txt')
    head, rows, alltags = [], {}, set()
    if os.path.exists(p):
        for l in open(p):
            if l.startswith('#') or not l.strip():
                head.append(l)
                continue
            parts = l.split()
            tags = parts[1].split(',') if len(parts) > 1 else []
            alltags.update(tags)
            rows[parts[0]] = [t for t in tags if t != os_ and not t.startswith(os_ + '-')]
    else:
        head = ['# %s.dll export names.\n' % dll, '# Each line: name, then where it exists (<windows>-<32/64>).\n']
    # a list without the 32/64 split (version.txt) gets the bare tag
    bare = bool(alltags) and all('-' not in t for t in alltags)
    found = []
    for bits, d in dirs:
        f = find(d, dll)
        if not f:
            continue
        found.append(bits)
        for n in names_of(f):
            rows.setdefault(n, []).append(os_ if bare else '%s-%s' % (os_, bits))
    if not found:
        print('%s: not on %s, unchanged' % (dll, os_))
        return
    head[0] = note(head[0], os_, found)
    with open(p, 'w') as o:
        o.writelines(head)
        for n in sorted(rows):
            if rows[n]:
                o.write('%s %s\n' % (n, ','.join(sorted(set(rows[n]), key=order))))
    print('%s.txt: %d names, %s-%s' % (dll, len(rows), os_, '/'.join(found)))


def order(tag):
    w, b = tag.rsplit('-', 1) if '-' in tag else (tag, '')
    return (['vista', 'win7', 'win8'].index(w) if w in ('vista', 'win7', 'win8') else 9, w, b)


def note(first, os_, found):
    """the header's first line says which Windows the list was read from"""
    label = {'win8': 'Windows 8 RTM (6.2.9200)'}.get(os_, os_)
    if label in first:
        return first
    return first.rstrip('.\n') + '; %s, %s.\n' % (label, ' and '.join('x86' if b == '32' else 'x64' for b in found))


def update_ords(dll, os_, dirs):
    p = os.path.join(W, dll + '.ords')
    if not os.path.exists(p):
        return
    head, rows = [], {}
    for l in open(p):
        if l.startswith('#') or not l.strip():
            head.append(l)
            continue
        o, *where = l.split()
        rows[int(o)] = [w for w in where if not w.startswith(os_ + '-')]
    for bits, d in dirs:
        f = find(d, dll)
        if not f:
            continue
        for o, n in ordinals(f).items():
            rows.setdefault(o, []).append('%s-%s:%s' % (os_, bits, n or '-'))
    head[0] = re.sub(r'\.\n$', '', head[0])
    if (os_ + '-') not in head[0]:
        head[0] += ', %s-32, %s-64' % (os_, os_)
    head[0] += '.\n'
    with open(p, 'w') as out:
        out.writelines(head)
        for o in sorted(rows):
            out.write('%d %s\n' % (o, ' '.join(rows[o])))
    print('%s.ords: %d ordinals' % (dll, len(rows)))


if __name__ == '__main__':
    os_, d64, d32 = sys.argv[1:4]
    dirs = [('32', d32), ('64', d64)]
    dlls = sorted({f[:-4] for f in os.listdir(W) if f.endswith('.txt') and f != 'SOURCE.txt'} | set(sys.argv[4:]))
    for dll in dlls:
        update_txt(dll, os_, dirs)
        update_ords(dll, os_, dirs)
