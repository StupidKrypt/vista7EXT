"""Pack a folder onto the setup program: VistaExtSetup.exe = stub + entries
(u16 path length in UTF-16 units, UTF-16LE path, u32 size, data), an empty
entry, then the trailer b"VXPAYLD2" + u64 offset of the first entry.
A file identical to an earlier one (the four builds share most files) is
stored as size 0xFFFFFFFF + the earlier file's path (u16 length, UTF-16LE):
setup copies that one (src/setup/vxsetup.c).

    python3 tools/mkpayload.py <stub.exe> <folder> <out.exe>
"""
import hashlib, os, struct, sys

stub, folder, out = sys.argv[1:4]
data = open(stub, 'rb').read()
with open(out, 'wb') as o:
    o.write(data)
    start = len(data)
    n = 0
    seen = {}
    unique = 0
    for root, dirs, files in os.walk(folder):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(root, f)
            rel = os.path.relpath(p, folder).replace('/', '\\')
            enc = rel.encode('utf-16-le')
            body = open(p, 'rb').read()
            h = hashlib.sha256(body).digest()
            if h in seen:
                same = seen[h]
                o.write(struct.pack('<H', len(enc) // 2) + enc + struct.pack('<I', 0xFFFFFFFF) +
                        struct.pack('<H', len(same) // 2) + same)
            else:
                seen[h] = enc
                o.write(struct.pack('<H', len(enc) // 2) + enc + struct.pack('<I', len(body)) + body)
                unique += 1
            n += 1
    o.write(struct.pack('<H', 0))
    o.write(b'VXPAYLD2' + struct.pack('<Q', start))
print('%s: %d files (%d stored, the rest copies), %.1f MB' % (out, n, unique, os.path.getsize(out) / 1e6))
