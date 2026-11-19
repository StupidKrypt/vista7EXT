#!/usr/bin/env bash
# Exercise VistaExt under Wine. Wine is not Vista, so this checks the
# mechanics (patching, forwarding, the added functions, version setting),
# not real-Vista behaviour.
#
# Each of the four builds (vista-x86, vista-x64, win7-x86, win7-x64) is
# tested against a fake system folder for its Windows: kernel32/user32 lack
# every function that build's kernelvx/uservx implements. For the Vista
# build that is all of VistaExt's additions; for the Windows 7 build only the
# Windows 8+ ones, so the functions Windows 7 has come from "Windows" (Wine)
# through the build's forwarders. vxpatch analyses programs against it
# (VX_SYSDIR).
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
T=${TESTDIR:-$ROOT/out/test}
# Always the test's own prefix (never the user's ~/.wine). winedbg.exe=d stops
# Wine from starting its debugger / crash dialog when a test program crashes:
# the crash still shows up as a non-zero exit code.
export WINEPREFIX=${VX_TEST_PREFIX:-$T/prefix} WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winedbg.exe=d"
WINEINC=/usr/include/wine
mkdir -p "$T"
[ -d "$WINEPREFIX/drive_c" ] || wineboot -i >/dev/null 2>&1
# no crash dialog (it would wait forever with nobody to close it)
command wine reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
WINEBIN=$(command -v wine)
wine() { timeout 120 "$WINEBIN" "$@"; }

total_fail=0
note() { printf '\n### %s\n' "$*"; }
expect() {   # expect <name> <expected rc> <actual rc>
    if [ "$2" = "$3" ]; then echo "PASS: $1"; else echo "FAIL: $1 (exit $3, wanted $2)"; total_fail=$((total_fail+1)); fi
}

for B in vista:x86_64:x86_64:x64 vista:i686:i386:x86 win7:x86_64:x86_64:x64 win7:i686:i386:x86; do
    IFS=: read os arch winarch archname <<<"$B"
    out=$os-$archname
    O=$ROOT/out/$out; W=$T/$out; LIB=/usr/lib/wine/$winarch-windows
    rm -rf "$W"; mkdir -p "$W/app" "$W/fakesys" "$W/obj"
    CF=(--target=$arch-w64-windows-gnu -O2 -ffreestanding -fno-builtin -mno-stack-arg-probe
        -I"$WINEINC/windows" -I"$WINEINC/msvcrt" -D_ACRTIMP= -D_ACRTIMP_ALT=)
    LF=(--target=$arch-w64-windows-gnu -fuse-ld=lld -nostdlib -L"$LIB")
    entry=DllMainCRTStartup; [ $arch = i686 ] && entry=DllMainCRTStartup@12

    # --- test program, importing the newer functions from kernel32/user32
    clang "${CF[@]}" -c tests/vxtest.c -o "$W/obj/vxtest.o"
    clang "${CF[@]}" -c src/common/console.c -o "$W/obj/console.o"
    clang "${CF[@]}" -c src/common/common.c -o "$W/obj/common.o"
    python3 -I - "$W" "$arch" <<'PY'
import subprocess, sys, re
W, arch = sys.argv[1], sys.argv[2]
want = {'kernel32': ['GetSystemTimePreciseAsFileTime','WaitOnAddress','WakeByAddressAll','WakeByAddressSingle','GetActiveProcessorCount',
        'GetLogicalProcessorInformationEx','TryAcquireSRWLockExclusive','GetCurrentPackageFullName','FindStringOrdinal',
        'GetCurrentThreadStackLimits','InitializeSynchronizationBarrier','EnterSynchronizationBarrier',
        'DeleteSynchronizationBarrier'], 'user32': ['GetDpiForSystem','GetSystemMetricsForDpi']}
und = subprocess.run(['llvm-nm','-u',W+'/obj/vxtest.o'],capture_output=True,text=True).stdout.split()
for dll, names in want.items():
    lines = []
    for n in names:
        if arch == 'i686':
            m = [u for u in und if re.fullmatch(r'__imp__%s@\d+' % n, u)]
            lines.append(m[0][len('__imp__'):] if m else n)
        else:
            lines.append(n)
    open('%s/obj/test_%s.def' % (W, dll), 'w').write('LIBRARY %s.dll\nEXPORTS\n%s\n' % (dll, '\n'.join('  '+l for l in lines)))
PY
    m=i386:x86-64; [ $arch = i686 ] && m=i386
    for d in kernel32 user32; do llvm-dlltool -m $m -k -d "$W/obj/test_$d.def" -l "$W/obj/libtest_$d.a"; done
    clang "${LF[@]}" -o "$W/app/vxtest.exe" "$W/obj/vxtest.o" "$W/obj/console.o" "$W/obj/common.o" \
        -Wl,--entry=mainCRTStartup -Wl,--subsystem,console:6.0 "$W/obj/libtest_kernel32.a" "$W/obj/libtest_user32.a" \
        -lkernel32 -luser32 -lshell32 -ladvapi32 -lversion -lntdll -lshlwapi -lole32 -loleaut32 -luuid -lwbemuuid

    # --- fake system folder for this build's Windows: Wine's DLLs (for their
    #     export lists), with kernel32/user32 stubs that lack what this
    #     build's companions implement
    for d in shell32 advapi32 gdi32 ole32 oleaut32 shlwapi version ntdll psapi msi msvcrt; do cp "$LIB/$d.dll" "$W/fakesys/"; done
    cp "$O"/*.dll "$W/fakesys/"
    python3 -I - "$W" "$O" "$LIB" <<'PY'
import sys
sys.path.insert(0, 'tools')
from peexp import PE
W, O, LIB = sys.argv[1:]
for dll, comp in (('kernel32', 'kernelvx'), ('user32', 'uservx')):
    added = {n for n, f in PE('%s/%s.dll' % (O, comp)).exports().items() if f is None}
    names = sorted(set(PE('%s/%s.dll' % (LIB, dll)).exports()) - added)
    open('%s/obj/fake_%s.def' % (W, dll), 'w').write('LIBRARY %s.dll\nEXPORTS\n%s\n' % (dll, '\n'.join('  %s = wine.%s' % (n, n) for n in names)))
    print('fake %s: %d exports (%d this build implements removed)' % (dll, len(names), len(added)))
PY
    clang "${CF[@]}" -c src/kernelbase/kernelbase.c -o "$W/obj/stub.o"
    for d in kernel32 user32; do
        clang "${LF[@]}" -shared -o "$W/fakesys/$d.dll" "$W/obj/stub.o" "$W/obj/common.o" "$W/obj/fake_$d.def" \
            -Wl,--entry=$entry -lkernel32 -ladvapi32
    done

    APP=$(winepath -w "$W/app/vxtest.exe" 2>/dev/null)
    export VX_SYSDIR=$(winepath -w "$W/fakesys" 2>/dev/null)

    note "$out: unpatched program must fail (Vista refuses to load it; Wine crashes on the first missing call, headless)"
    wine "$W/app/vxtest.exe" >"$W/unpatched.txt" 2>&1; rc=$?
    head -3 "$W/unpatched.txt"
    [ -f "$W/app/vxtest.exe" ] && [ $rc -ne 0 ] && expect "$out unpatched program fails to load" x x || expect "$out unpatched program fails to load" fail ok

    note "$out: vxpatch /check"
    wine "$O/vxpatch.exe" /check "$APP"; echo "(exit $?)"

    note "$out: vxpatch"
    wine "$O/vxpatch.exe" "$APP"; rc=$?
    expect "$out vxpatch patches the program" 0 $rc
    python3 -I tools/peexp.py "$W/app/vxtest.exe" imports | grep -v '^    '
    # Wine searches the program folder first, so the companions go there
    cp "$O"/kernelvx.dll "$O"/uservx.dll "$O"/advapivx.dll "$O"/shellvx.dll "$O"/versvx.dll "$O"/ntvx.dll "$W/app/"

    note "$out: patched program, VistaExt's own versions also of functions an update can add (VX_NO_NATIVE=1)"
    VX_NO_NATIVE=1 wine "$W/app/vxtest.exe"; expect "$out patched program, VistaExt code" 0 $?

    note "$out: patched program, Windows' own versions where an update added them"
    wine "$W/app/vxtest.exe"; expect "$out patched program, native-first" 0 $?

    note "$out: Windows version setting = Windows 11, chosen with vxpatch /winver"
    wine "$O/vxpatch.exe" /q /winver:WIN11 "$APP" >/dev/null 2>&1
    wine "$W/app/vxtest.exe" /expect:10.0.22631 | grep -E 'info  (Get|kernel32)|chosen|FAIL'; expect "$out reports Windows 11" 0 ${PIPESTATUS[0]}
    note "$out: Windows version setting = Windows 10, plus extra arguments"
    wine reg add 'HKLM\SOFTWARE\VistaExt\Layers' /v "$APP" /d 'WIN10 ARGS:--hello-from-vistaext' /f /reg:64 >/dev/null 2>&1
    wine "$W/app/vxtest.exe" /expect:10.0.19045 | grep -E 'info|chosen|passed|FAIL'; expect "$out reports Windows 10" 0 ${PIPESTATUS[0]}
    wine reg add 'HKLM\SOFTWARE\VistaExt\Layers' /v "$APP" /d 'WIN7' /f /reg:64 >/dev/null 2>&1
    wine "$W/app/vxtest.exe" /expect:6.1.7601 | grep -E 'info|chosen|FAIL'; expect "$out reports Windows 7" 0 ${PIPESTATUS[0]}
    note "$out: folder setting applies to programs inside it"
    FOLDER=$(winepath -w "$W/app" 2>/dev/null)
    wine reg delete 'HKLM\SOFTWARE\VistaExt\Layers' /v "$APP" /f /reg:64 >/dev/null 2>&1
    wine reg add 'HKLM\SOFTWARE\VistaExt\Layers' /v "$FOLDER\\" /d 'WIN81' /f /reg:64 >/dev/null 2>&1
    wine "$W/app/vxtest.exe" /expect:6.3.9600 | grep -E 'info  Get|chosen|FAIL'; expect "$out folder setting reports Windows 8.1" 0 ${PIPESTATUS[0]}
    wine reg delete 'HKLM\SOFTWARE\VistaExt\Layers' /v "$FOLDER\\" /f /reg:64 >/dev/null 2>&1

    note "$out: WebView2 runtime registration reaches patched programs"
    mkdir -p "$W/wv2rt"; cp "$W/app/vxtest.exe" "$W/wv2rt/msedgewebview2.exe"
    wine "$O/vxpatch.exe" /q /wv2runtime:$archname "$(winepath -w "$W/wv2rt" 2>/dev/null)" | grep -E 'registered|patched|error'
    wine "$W/app/vxtest.exe" | grep WEBVIEW2 | tee "$W/wv2.txt"
    grep -qi 'wv2rt' "$W/wv2.txt"; expect "$out patched program gets the WebView2 runtime folder" 0 $?
    wine reg delete 'HKLM\SOFTWARE\VistaExt\WebView2' /f /reg:64 >/dev/null 2>&1

    note "$out: vxpatch /undo"
    wine "$O/vxpatch.exe" /undo "$APP"
    python3 -I tools/peexp.py "$W/app/vxtest.exe" imports | grep -q '^kernel32.dll' ; expect "$out undo restores kernel32 import" 0 $?
    unset VX_SYSDIR
    wineserver -k 2>/dev/null; sleep 1
done

echo
[ $total_fail = 0 ] && echo "ALL WINE TESTS PASSED" || echo "$total_fail WINE TEST(S) FAILED"
exit $total_fail
