#!/usr/bin/env bash
# Assemble the release folder out/dist/VistaExt and VistaExt.zip (run build.sh first).
set -euo pipefail
cd "$(dirname "$0")"
D=out/dist/VistaExt
rm -rf out/dist; mkdir -p "$D/webview2"
# one build per Windows and architecture; install.cmd picks the right one
for os in vista win7 win8; do
    [ -d out/$os-x86 ] || { [ $os = win8 ] && echo "note: no Windows 8 build (build.sh skipped it): not packaged" && continue; }
    for a in x86 x64; do
        mkdir -p "$D/$os/$a/apisets"
        cp out/$os-$a/*.dll out/$os-$a/*.exe "$D/$os/$a/"
        cp out/$os-$a/apisets/*.dll "$D/$os/$a/apisets/"
    done
done
cp installer/install.cmd installer/uninstall.cmd installer/vx-diagnostics.cmd "$D/"
cp webview2/vx-webview2-setup.cmd "$D/webview2/"
cp README.txt LICENSE.txt KNOWN_RISKS.md "$D/" 2>/dev/null || true
# Windows batch files and text files need CRLF line endings
for f in "$D"/*.cmd "$D"/webview2/*.cmd "$D"/*.txt; do [ -f "$f" ] && sed -i 's/\r*$/\r/' "$f"; done
(cd out/dist && rm -f VistaExt.zip && python3 -I -m zipfile -c VistaExt.zip VistaExt)
# the one-file installer: the release folder packed onto the setup program
python3 -I tools/mkpayload.py out/obj/VistaExtSetup-stub.exe "$D" out/dist/VistaExtSetup.exe
echo "out/dist/VistaExt.zip: $(du -h out/dist/VistaExt.zip | cut -f1), $(find "$D" -type f | wc -l) files"
