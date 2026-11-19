#!/usr/bin/env bash
# Unpack a Chromium browser's offline MSI that wraps an Omaha (Google Update /
# Edge Update) installer, without running anything, into the layout the
# browser's own setup would install:
#     <out>/<browser>.exe  <out>/<version>/...
# Used for the VM app tests (tests/vm/apps.cmd), e.g. for Microsoft Edge:
#     tools/unpack-omaha-msi.sh out/apps/dl/edge/MicrosoftEdgeEnterpriseX64.msi out/apps/x/edge-x64
#
# The chain: MSI binary table -> Omaha metainstaller (its largest resource is
# LZMA, with a BCJ2 x86 filter: tools/bcj2dec.c) -> tar -> the browser's
# mini installer -> a .7z resource -> Chrome-bin/<version>/.
# Needs 7z, python3, cc and tar.
set -euo pipefail
msi=${1:?msi} out=${2:?output folder}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
[ -x "$root/out/bcj2dec" ] || cc -O2 -o "$root/out/bcj2dec" "$here/bcj2dec.c"

7z x -y -o"$tmp/msi" "$msi" >/dev/null
meta=$(ls -S "$tmp"/msi/Binary.* | sed -n 1p)
python3 -I - "$meta" "$tmp/payload.lzma" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec, opt = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
magic = struct.unpack_from('<H', d, pe + 24)[0]
rva = struct.unpack_from('<I', d, pe + 24 + (112 if magic == 0x20b else 96) + 16)[0]
secs = [struct.unpack_from('<IIII', d, pe + 24 + opt + 40 * i + 8) for i in range(nsec)]
def off(r):
    for vs, va, rs, ra in secs:
        if va <= r < va + max(vs, rs):
            return r - va + ra
base = off(rva)
best = (0, 0)
def walk(p):
    global best
    nn, ni = struct.unpack_from('<HH', d, p + 12)
    for i in range(nn + ni):
        t = struct.unpack_from('<I', d, p + 16 + 8 * i + 4)[0]
        if t & 0x80000000:
            walk(base + (t & 0x7fffffff))
        else:
            r, sz = struct.unpack_from('<II', d, base + t)
            if sz > best[1]:
                best = (off(r), sz)
walk(base)
open(sys.argv[2], 'wb').write(d[best[0]:best[0] + best[1]])
PY
python3 -I -c "
import lzma, sys
open(sys.argv[2], 'wb').write(lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(open(sys.argv[1], 'rb').read()))
" "$tmp/payload.lzma" "$tmp/payload.bcj2"
"$root/out/bcj2dec" "$tmp/payload.bcj2" "$tmp/payload.tar"
mkdir -p "$tmp/tar"
tar -xf "$tmp/payload.tar" -C "$tmp/tar"
setup=$(ls -S "$tmp"/tar/* | sed -n 1p)
mv "$setup" "$tmp/setup.exe"    # its name ends in {GUID}, which 7z reads as a pattern
7z x -y -o"$tmp/res" "$tmp/setup.exe" >/dev/null
arc=$(find "$tmp/res" -iname '*.7z' -size +1M | sed -n 1p)
[ -n "$arc" ] || { echo "no browser archive in $(basename "$setup")" >&2; exit 1; }
7z x -y -o"$tmp/app" "$arc" >/dev/null
ver=$(ls "$tmp/app/Chrome-bin" | grep -E '^[0-9.]+$' | sed -n 1p)
rm -rf "$out"; mkdir -p "$out"
mv "$tmp/app/Chrome-bin/$ver" "$out/"
# the browser's setup puts the main program (the .exe that has a .sig file
# beside it and isn't a WebView) next to the version folder
for f in "$out/$ver"/*.exe; do
    b=$(basename "$f")
    [ -f "$f.sig" ] && [[ $b != *webview* ]] && cp "$f" "$out/$b"
done
cp "$tmp/app/Chrome-bin/"*.exe "$out/" 2>/dev/null || true
echo "$out: version $ver, $(du -sh "$out" | cut -f1); programs: $(cd "$out" && ls *.exe | tr '\n' ' ')"
