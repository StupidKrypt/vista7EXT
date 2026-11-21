#!/usr/bin/env python3
"""Compare two tests/vm/perf.cmd results files (before / after).

usage: perfcompare.py <before.txt> <after.txt> [--md]

Every PERF and BENCH line is keyed by the section it is in ("### ..." or
"--- ...") and its label (the text before the colon); each number in it is
compared. Lower is better for every measure perf.cmd takes (time, memory,
CPU, wake-ups, threads, handles), except counts of processes."""
import re
import sys

UNIT_ONLY = re.compile(r'^(\d+)\s*(ns|us|ms|s|KB|MB)$')        # "22 ns"
PIECE_VALUE_FIRST = re.compile(r'^(\d+)\s+(.+)$')               # "5 threads"
PIECE_NAME_FIRST = re.compile(r'^(.*?)\s*(\d+)\s*([A-Za-z%]*)$')  # "working set 4500 KB", "CPU 10 ms"


def parse(path):
    out, order = {}, []
    section = ''
    for raw in open(path, encoding='latin-1'):
        line = raw.strip()
        if line.startswith('### ') or line.startswith('--- '):
            section = line[4:]
            continue
        m = re.match(r'^(PERF|BENCH) (.+?): (.+)$', line)
        if not m:
            continue
        label = m.group(2)
        for piece in m.group(3).split(', '):
            piece = piece.strip()
            u = UNIT_ONLY.match(piece)
            a = PIECE_VALUE_FIRST.match(piece)
            b = PIECE_NAME_FIRST.match(piece)
            if u:
                name, value, unit = 'value', int(u.group(1)), u.group(2)
            elif a:
                name, value, unit = a.group(2), int(a.group(1)), ''
            elif b:
                name, value, unit = b.group(1) or 'value', int(b.group(2)), b.group(3)
            else:
                continue
            key = (section, label, name)
            if key not in out:
                order.append(key)
            out[key] = (value, unit)
    return out, order


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 2:
        sys.exit(__doc__)
    before, order = parse(args[0])
    after, order2 = parse(args[1])
    for k in order2:
        if k not in before:
            order.append(k)
    last = None
    print('| measure | before | after | change |')
    print('|---|---:|---:|---:|')
    for k in order:
        section, label, name = k
        if section != last:
            print('| **%s** | | | |' % section.replace('|', '/'))
            last = section
        b, a = before.get(k), after.get(k)
        unit = (b or a)[1]
        bs = '%d %s' % (b[0], unit) if b else '-'
        as_ = '%d %s' % (a[0], unit) if a else '-'
        ch = ''
        if a and b and b[0]:
            ch = '%+.0f%%' % ((a[0] - b[0]) * 100.0 / b[0])
        elif a and b:
            ch = '0' if a[0] == 0 else 'new'
        what = label if name in ('value', '') else '%s: %s' % (label, name)
        print('| %s | %s | %s | %s |' % (what.replace('|', '/'), bs.strip(), as_.strip(), ch))


if __name__ == '__main__':
    main()
