"""Read the API-set schema of a Windows installation (apisetschema.dll,
section .apiset): which api-ms-win-* / ext-ms-win-* names exist on it and
which DLL each one goes to.

    python3 tools/apiset.py <System32>/apisetschema.dll [name]

Formats (no Microsoft documentation exists; layouts as described by Geoff
Chappell's "API Set Schema" study and used by Wine/ReactOS loaders):
  version 2 (Windows 7): ULONG Version, Count; then Count entries
      {NameOffset, NameLength, DataOffset}; names are UTF-16 without the
      "api-" prefix and ".dll"; data {Count; {NameOffset, NameLength,
      ValueOffset, ValueLength}...} (value = host DLL, empty = none)
  version 4 (Windows 8, 8.1): ULONG Version, Size, Flags, Count; entries
      {Flags, NameOffset, NameLength, AliasOffset, AliasLength, DataOffset};
      data {Flags, Count; {Flags, NameOffset, NameLength, ValueOffset,
      ValueLength}...}
Vista has no schema: none of these names exist there.
Returns {name in lower case without ".dll" (e.g. "api-ms-win-core-synch-l1-2-0"): host dll or ""}.
"""
import struct
import sys


def section(path, want=b'.apiset'):
    d = open(path, 'rb').read()
    e = struct.unpack_from('<I', d, 0x3C)[0]
    nsec = struct.unpack_from('<H', d, e + 6)[0]
    optsz = struct.unpack_from('<H', d, e + 20)[0]
    s = e + 24 + optsz
    for i in range(nsec):
        h = s + 40 * i
        if d[h:h + 8].rstrip(b'\0') == want:
            vs, va, rs, raw = struct.unpack_from('<IIII', d, h + 8)
            return d[raw:raw + max(vs, rs)]
    raise ValueError('%s: no %s section' % (path, want))


def u16(d, off, ln):
    return d[off:off + ln].decode('utf-16-le')


def schema(path):
    d = section(path)
    ver = struct.unpack_from('<I', d, 0)[0]
    out = {}
    if ver == 2:
        count = struct.unpack_from('<I', d, 4)[0]
        for i in range(count):
            no, nl, do = struct.unpack_from('<III', d, 8 + 12 * i)
            name = 'api-' + u16(d, no, nl).lower()
            vc = struct.unpack_from('<I', d, do)[0]
            host = ''
            for j in range(vc):
                _, _, vo, vl = struct.unpack_from('<IIII', d, do + 4 + 16 * j)
                if vl:
                    host = u16(d, vo, vl).lower()
            out[name] = host
    elif ver == 4:
        count = struct.unpack_from('<I', d, 12)[0]
        for i in range(count):
            _, no, nl, _, _, do = struct.unpack_from('<IIIIII', d, 16 + 24 * i)
            name = u16(d, no, nl).lower()
            if not name.startswith(('api-', 'ext-')):
                name = 'api-' + name
            vc = struct.unpack_from('<I', d, do + 4)[0]
            host = ''
            for j in range(vc):
                _, _, _, vo, vl = struct.unpack_from('<IIIII', d, do + 8 + 20 * j)
                if vl:
                    host = u16(d, vo, vl).lower()
            out[name] = host
    else:
        raise ValueError('%s: schema version %d not handled' % (path, ver))
    return out


def lookup(sch, dll):
    """host for an imported api-set name ("api-ms-win-core-synch-l1-2-0.dll"),
    None if this Windows has no such set. Windows matches the name without its
    last "-N" (the minor version) up to the version the schema has."""
    n = dll.lower()
    if n.endswith('.dll'):
        n = n[:-4]
    if n in sch:
        return sch[n]
    return None


if __name__ == '__main__':
    s = schema(sys.argv[1])
    if len(sys.argv) > 2:
        print(lookup(s, sys.argv[2]))
    else:
        for k in sorted(s):
            print(k, s[k])
        print(len(s), 'sets', file=sys.stderr)
