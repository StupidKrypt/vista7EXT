#!/usr/bin/env bash
# Test msifix under Wine's msi.dll: the Vista build's msifix and the Windows
# 7 build's, each against its fake system folder. Run tests/run_wine.sh
# first (it builds those folders and the unpatched test program used as a
# custom action).
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
T=$ROOT/out/test
W=$T/vista-x64
# test prefix only; no debugger or crash dialog on a crash (see run_wine.sh)
export WINEPREFIX=${VX_TEST_PREFIX:-$T/prefix} WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winedbg.exe=d"
WINEBIN=$(command -v wine)
wine() { timeout 120 "$WINEBIN" "$@"; }
"$WINEBIN" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
WINEINC=/usr/include/wine
fails=0
check() { if grep -qF -- "$2" "$3"; then echo "PASS: $1"; else echo "FAIL: $1 (wanted: $2)"; fails=$((fails+1)); fi; }
nocheck() { if grep -qF -- "$2" "$3"; then echo "FAIL: $1 (found: $2)"; fails=$((fails+1)); else echo "PASS: $1"; fi; }

[ -f "$W/app/vxtest.exe" ] || { echo "run tests/run_wine.sh first"; exit 2; }
mkdir -p "$T/msi"
CF=(--target=x86_64-w64-windows-gnu -O2 -ffreestanding -fno-builtin -mno-stack-arg-probe
    -I"$WINEINC/windows" -I"$WINEINC/msvcrt" -D_ACRTIMP= -D_ACRTIMP_ALT=)
clang "${CF[@]}" -c tests/msitest.c -o "$T/msi/msitest.o"
clang --target=x86_64-w64-windows-gnu -fuse-ld=lld -nostdlib -L/usr/lib/wine/x86_64-windows -o "$T/msi/msitest.exe" \
    "$T/msi/msitest.o" "$W/obj/console.o" "$W/obj/common.o" -Wl,--entry=mainCRTStartup -Wl,--subsystem,console \
    -lmsi -lkernel32 -luser32 -lshell32 -ladvapi32

wp() { winepath -w "$1" 2>/dev/null; }
rm -f "$T/msi/"*.msi
wine "$T/msi/msitest.exe" create "$(wp "$T/msi/modern.msi")" "$(wp "$W/app/vxtest.exe")"
for ver in 600 601; do
    name=$([ $ver = 600 ] && echo Vista || echo "Windows 7")
    os=$([ $ver = 600 ] && echo vista || echo win7)
    export VX_SYSDIR=$(wp "$T/$os-x64/fakesys")
    echo; echo "### msifix of the $name build"
    wine "$ROOT/out/$os-x64/msifix.exe" "$(wp "$T/msi/modern.msi")" "$(wp "$T/msi/fixed$ver.msi")" | tee "$T/msi/fix$ver.txt"
    wine "$T/msi/msitest.exe" dump "$(wp "$T/msi/fixed$ver.msi")" "$(wp "$T/msi/ca$ver.exe")" | tee "$T/msi/dump$ver.txt"
    python3 -I tools/peexp.py "$T/msi/ca$ver.exe" imports | grep -v '^    ' > "$T/msi/caimports$ver.txt"
    D=$T/msi/dump$ver.txt
    check "$name: Windows 8.1 check lowered" "[Installed OR VersionNT >= $ver]" "$D"
    check "$name: 64-bit check kept" "[VersionNT64]" "$D"
    if [ $ver = 600 ]; then check "$name: build check lowered" "WindowsBuild >= 6002 AND VersionNT >= 600" "$D"; else check "$name: build check lowered, 7 already meets VersionNT>600" "WindowsBuild >= 7601 AND VersionNT>600" "$D"; fi
    check "$name: XP check untouched" "[VersionNT >= 501]" "$D"
    check "$name: negated check left as it is" "[NOT (VersionNT >= 1000)]" "$D"
    check "$name: package platform kept" "template=x64;1033" "$D"
    check "$name: package code kept" "package code={11111111-2222-3333-4444-555555555555}" "$D"
    check "$name: install-step condition untouched without /all" "[InstallFiles] [VersionNT >= 1000]" "$D"
    # Wine's msi.dll accepts a replaced Binary stream but never saves it (all
    # calls return success), so only msifix's own side can be checked here.
    check "$name: custom action patched and written back" "custom action ModernCA: patched to use VistaExt" "$T/msi/fix$ver.txt"
    if [ $ver = 600 ]; then
        check "Vista: schema lowered to 405" "schema=405" "$D"
        check "Vista: 5.0 table removed" "MsiServiceConfig table: absent" "$D"
        nocheck "Vista: 5.0 action removed" "[MsiConfigureServices]" "$D"
    else
        check "Windows 7: schema 500 kept (has Installer 5.0)" "schema=500" "$D"
        check "Windows 7: 5.0 table kept" "MsiServiceConfig table: present" "$D"
        check "Windows 7: 5.0 action kept" "[MsiConfigureServices]" "$D"
    fi
done

echo; echo "### msifix /all, Vista build"
export VX_SYSDIR=$(wp "$T/vista-x64/fakesys")
wine "$ROOT/out/vista-x64/msifix.exe" /all "$(wp "$T/msi/modern.msi")" "$(wp "$T/msi/fixedall.msi")" > /dev/null
wine "$T/msi/msitest.exe" dump "$(wp "$T/msi/fixedall.msi")" "$(wp "$T/msi/caall.exe")" > "$T/msi/dumpall.txt"
check "/all: install-step condition lowered" "[InstallFiles] [VersionNT >= 600]" "$T/msi/dumpall.txt"

echo; check "original package untouched" "schema=500" <(wine "$T/msi/msitest.exe" dump "$(wp "$T/msi/modern.msi")" "$(wp "$T/msi/caorig.exe")")
echo
[ $fails = 0 ] && echo "ALL MSI TESTS PASSED" || echo "$fails MSI TEST(S) FAILED"
exit $fails
