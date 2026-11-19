"""Minimal PE reader: export and import tables. No third-party deps."""
import struct


class PE:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        e = struct.unpack_from('<I', d, 0x3C)[0]
        if d[e:e + 4] != b'PE\0\0':
            raise ValueError(path + ': not a PE file')
        self.machine, nsec = struct.unpack_from('<HH', d, e + 4)
        optsz = struct.unpack_from('<H', d, e + 20)[0]
        opt = e + 24
        self.is64 = struct.unpack_from('<H', d, opt)[0] == 0x20B
        dd = opt + (112 if self.is64 else 96)
        self.dirs = [struct.unpack_from('<II', d, dd + 8 * i) for i in range(16)]
        self.secs = []
        s = opt + optsz
        for i in range(nsec):
            va, vs, raw, rs = (struct.unpack_from('<I', d, s + 40 * i + 12)[0],
                               struct.unpack_from('<I', d, s + 40 * i + 8)[0],
                               struct.unpack_from('<I', d, s + 40 * i + 20)[0],
                               struct.unpack_from('<I', d, s + 40 * i + 16)[0])
            self.secs.append((va, max(vs, rs), raw))

    def off(self, rva):
        for va, sz, raw in self.secs:
            if va <= rva < va + sz:
                return rva - va + raw
        raise ValueError('bad rva %x' % rva)

    def cstr(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.index(b'\0', o)].decode('latin1')

    def exports(self):
        """Return dict name -> forwarder string (or None)."""
        rva, size = self.dirs[0]
        out = {}
        if not rva:
            return out
        o = self.off(rva)
        nfun, nnames, afun, anames, aords = struct.unpack_from('<IIIII', self.d, o + 20)
        for i in range(nnames):
            nrva = struct.unpack_from('<I', self.d, self.off(anames) + 4 * i)[0]
            ordi = struct.unpack_from('<H', self.d, self.off(aords) + 2 * i)[0]
            frva = struct.unpack_from('<I', self.d, self.off(afun) + 4 * ordi)[0]
            fwd = self.cstr(frva) if rva <= frva < rva + size else None
            out[self.cstr(nrva)] = fwd
        return out

    def imports(self):
        """Return list of (dllname, [names or '#ord'])."""
        rva, _ = self.dirs[1]
        res = []
        if not rva:
            return res
        o = self.off(rva)
        tsz, fmt, flag = (8, '<Q', 1 << 63) if self.is64 else (4, '<I', 1 << 31)
        while True:
            oft, ts, fc, name, ft = struct.unpack_from('<IIIII', self.d, o)
            if not name:
                break
            t = oft or ft
            names = []
            to = self.off(t)
            while True:
                v = struct.unpack_from(fmt, self.d, to)[0]
                if not v:
                    break
                names.append('#%d' % (v & 0xFFFF) if v & flag else self.cstr((v & 0x7FFFFFFF) + 2))
                to += tsz
            res.append((self.cstr(name), names))
            o += 20
        return res


if __name__ == '__main__':
    import sys
    p = PE(sys.argv[1])
    if len(sys.argv) > 2 and sys.argv[2] == 'imports':
        for dll, names in p.imports():
            print(dll, len(names))
            for n in names:
                print('   ', n)
    else:
        for n, f in sorted(p.exports().items()):
            print(n, f or '')
