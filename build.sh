#!/usr/bin/env bash
# Build VistaExt: one build per Windows (Vista, 7, 8 - which 8.1 uses too),
# each for 32-bit (x86) and 64-bit (x64) Windows. The code is shared; what
# differs per Windows is chosen here, at build time (-DVX_OS=600 / 601 / 602,
# gen.py --os), never by a version check at run time. See docs/LAYOUT.md.
# The Windows 8 build needs Windows 8's export lists in third_party/winexports
# (read from an installation with tools/peexp.py); without them it is skipped.
# Needs: clang, lld, llvm-nm, llvm-windres, python3, and Wine's Windows
# headers + import libraries (/usr/include/wine, /usr/lib/wine/*-windows).
# Output: out/<vista|win7|win8>-<x86|x64>.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
WINEINC=${WINEINC:-/usr/include/wine}
ONLY=${1:-all}     # "dlls" builds just the DLLs

build_arch() {
    local os=$1 arch=$2 winarch=$3 archname=$4
    local OUT=$ROOT/out/$os-$archname OBJ=$ROOT/out/obj/$os-$archname
    local vxos; case $os in vista) vxos=600 ;; win7) vxos=601 ;; win8) vxos=602 ;; esac
    local LIB=/usr/lib/wine/$winarch-windows
    local entry=DllMainCRTStartup cmain=mainCRTStartup wmain=wWinMainCRTStartup
    [ "$arch" = i686 ] && entry=DllMainCRTStartup@12
    mkdir -p "$OUT" "$OBJ"
    local CFLAGS=(--target=$arch-w64-windows-gnu -O2 -Wall -Wno-unused-parameter -Wno-unused-variable
        -Wno-missing-braces -Wno-unused-function -fno-builtin -ffreestanding -fno-stack-protector
        -mno-stack-arg-probe -ffunction-sections -fdata-sections
        -I"$WINEINC/windows" -I"$WINEINC/msvcrt" -D_ACRTIMP= -D_ACRTIMP_ALT= -DVX_OS=$vxos)
    # programs: commit 256 KB of stack up front (no stack probes are compiled
    # in, so a function with a frame over 4 KB must not skip the guard page)
    local EXEFLAGS=(-Xlinker --stack -Xlinker 0x100000,0x40000)
    local LDFLAGS=(--target=$arch-w64-windows-gnu -fuse-ld=lld -nostdlib -L"$LIB"
        -Wl,--major-subsystem-version,6 -Wl,--minor-subsystem-version,0
        -Wl,--major-os-version,6 -Wl,--minor-os-version,0 -Wl,--dynamicbase,--nxcompat
        -Wl,--gc-sections)    # only the code a DLL uses (the API-set stubs: forwarders, and DllMain)
    [ "$arch" = x86_64 ] && LDFLAGS+=(-Wl,--high-entropy-va)

    clang "${CFLAGS[@]}" -c src/common/common.c -o "$OBJ/common.o"
    clang "${CFLAGS[@]}" -c src/common/pin.c -o "$OBJ/pin.o"
    # the shared code as a library: each program or DLL takes only the parts it uses
    rm -f "$OBJ/libvxcommon.a"
    llvm-ar rcs "$OBJ/libvxcommon.a" "$OBJ/common.o" "$OBJ/pin.o"
    # version info + ownership marker for every DLL (see src/common/dll.rc)
    llvm-windres --target=$arch-w64-mingw32 -I"$WINEINC/windows" src/common/dll.rc -o "$OBJ/dll.res.o"

    # name : sources : extra cflags : libs
    local comps=(
        "kernelvx:src/kernelvx/main.c src/kernelvx/synch.c src/kernelvx/sysinfo.c src/kernelvx/thread.c src/kernelvx/misc.c src/kernelvx/path.c src/kernelvx/power.c::-lkernel32 -ladvapi32 -lntdll"
        "uservx:src/uservx/uservx.c::-luser32 -lgdi32 -lkernel32 -ladvapi32"
        "advapivx:src/advapivx/advapivx.c::-ladvapi32 -lkernel32"
        "shellvx:src/shellvx/shellvx.c::-lshell32 -lole32 -luser32 -lkernel32 -ladvapi32"
        "combase:src/combase/combase.c::-lole32 -lkernel32 -ladvapi32 -luuid"
        "shcore:src/shcore/shcore.c::-luser32 -lgdi32 -lkernel32 -ladvapi32"
        "ntvx:src/ntvx/ntvx.c::-lntdll -lkernel32 -ladvapi32"
        "ws2vx:src/ws2vx/ws2vx.c::-lws2_32 -lkernel32 -ladvapi32"
        "cfgmgrvx:src/cfgmgrvx/cfgmgrvx.c::-lkernel32 -ladvapi32"
        "olevx:src/olevx/olevx.c src/olevx/wmi.c::-lole32 -lkernel32 -ladvapi32 -luuid -lwbemuuid"
        "versvx:src/versvx/versvx.c::-lversion -lkernel32 -ladvapi32"
    )
    if [ "$os" = vista ]; then
        # DLLs Vista lacks entirely: kernelbase (forwards to kernelvx) and
        # bcryptprimitives (programs use VistaExt's directly)
        comps+=("bcryptprimitives:src/bcryptprimitives/bcryptprimitives.c::-ladvapi32 -lkernel32"
                "kernelbase:src/kernelbase/kernelbase.c::-lkernel32 -ladvapi32")
    else
        # Windows 7's bcryptprimitives lacks ProcessPrng: a companion adds it
        comps+=("bcryptprimvx:src/bcryptprimitives/bcryptprimitives.c:-DVX_COMPANION:-ladvapi32 -lkernel32")
    fi
    for c in "${comps[@]}"; do
        IFS=: read -r name srcs extra libs <<<"$c"
        local objs=()
        for s in $srcs; do
            local o="$OBJ/${name}_$(basename "${s%.c}").o"
            clang "${CFLAGS[@]}" $extra -c "$s" -o "$o"
            objs+=("$o")
        done
        python3 -I tools/gen.py component --os $os --arch $arch --comp $name --out "$OBJ/$name.def" --outdir "$OUT" "${objs[@]}"
        clang "${LDFLAGS[@]}" -shared -o "$OUT/$name.dll" "${objs[@]}" "$OBJ/libvxcommon.a" "$OBJ/dll.res.o" "$OBJ/$name.def" \
            -Wl,--entry=$entry $libs
    done

    # api-set forwarder DLLs
    mkdir -p "$OUT/apisets" "$OBJ/apisets"
    python3 -I tools/gen.py apisets --os $os --arch $arch --outdir "$OBJ/apisets" --bindir "$OUT"
    clang "${CFLAGS[@]}" -c src/kernelbase/kernelbase.c -o "$OBJ/apistub.o"
    for def in "$OBJ"/apisets/*.def; do
        local n=$(basename "${def%.def}")
        clang "${LDFLAGS[@]}" -shared -o "$OUT/apisets/$n.dll" "$OBJ/apistub.o" "$OBJ/libvxcommon.a" "$OBJ/dll.res.o" "$def" \
            -Wl,--entry=$entry -lkernel32 -ladvapi32
    done

    # VistaExt's API sets under names Windows' API-set table does not
    # intercept, for the in-memory mode (vxload): api-ms-X -> vxs-ms-X, ext-ms-X -> vxe-ms-X
    for f in "$OUT"/apisets/api-*.dll; do cp "$f" "$OUT/apisets/vxs-$(basename "$f" | cut -c5-)"; done
    for f in "$OUT"/apisets/ext-*.dll; do cp "$f" "$OUT/apisets/vxe-$(basename "$f" | cut -c5-)"; done

    # the in-memory mode's loader hook: a verifier provider, ntdll only
    clang "${CFLAGS[@]}" -c src/vxload/vxload.c -o "$OBJ/vxload.o"
    clang "${LDFLAGS[@]}" -shared -o "$OUT/vxload.dll" "$OBJ/vxload.o" "$OBJ/dll.res.o" -Wl,--entry=$entry -lntdll

    [ "$ONLY" = dlls ] && return

    # tools
    local main=mainCRTStartup wmainw=WinMainCRTStartup
    clang "${CFLAGS[@]}" -c src/vxpatch/patchlib.c -o "$OBJ/patchlib.o"
    clang "${CFLAGS[@]}" -c src/common/console.c -o "$OBJ/console.o"
    for t in vxpatch msifix vxauto; do
        llvm-windres --target=$arch-w64-mingw32 -I"$WINEINC/windows" src/$t/$t.rc -o "$OBJ/$t.res.o"
        clang "${CFLAGS[@]}" -c src/$t/$t.c -o "$OBJ/$t.o"
    done
    clang "${CFLAGS[@]}" -c src/common/userrun.c -o "$OBJ/userrun.o"
    llvm-windres --target=$arch-w64-mingw32 -I"$WINEINC/windows" src/vxcenter/vxcenter.rc -o "$OBJ/vxcenter.res.o"
    clang "${CFLAGS[@]}" -c src/vxcenter/vxcenter.c -o "$OBJ/vxcenter.o"

    clang "${LDFLAGS[@]}" "${EXEFLAGS[@]}" -o "$OUT/vxpatch.exe" "$OBJ/vxpatch.o" "$OBJ/vxpatch.res.o" "$OBJ/patchlib.o" "$OBJ/console.o" \
        "$OBJ/libvxcommon.a" -Wl,--entry=$main -Wl,--subsystem,console:6.0 -ladvapi32 -lkernel32 -lshell32 -luser32 -lwtsapi32 -luserenv
    clang "${LDFLAGS[@]}" "${EXEFLAGS[@]}" -o "$OUT/msifix.exe" "$OBJ/msifix.o" "$OBJ/msifix.res.o" "$OBJ/patchlib.o" "$OBJ/console.o" \
        "$OBJ/libvxcommon.a" -Wl,--entry=$main -Wl,--subsystem,windows:6.0 -lmsi -ladvapi32 -lkernel32 -lshell32 -luser32
    clang "${LDFLAGS[@]}" "${EXEFLAGS[@]}" -o "$OUT/vxauto.exe" "$OBJ/vxauto.o" "$OBJ/vxauto.res.o" "$OBJ/patchlib.o" "$OBJ/console.o" \
        "$OBJ/libvxcommon.a" -Wl,--entry=$main -Wl,--subsystem,console:6.0 -ladvapi32 -lkernel32 -lshell32 -luser32 -lwtsapi32 -luserenv \
        -lcrypt32 -lwintrust -lversion -lole32 -luuid
    # the setup program: one for all four builds (32-bit, so it runs on every
    # Vista/7); it picks the build to install (src/setup/vxsetup.c)
    if [ "$arch" = i686 ] && [ "$os" = vista ]; then
        llvm-windres --target=$arch-w64-mingw32 -I"$WINEINC/windows" src/setup/vxsetup.rc -o "$OBJ/vxsetup.res.o"
        clang "${CFLAGS[@]}" -c src/setup/vxsetup.c -o "$OBJ/vxsetup.o"
        clang "${LDFLAGS[@]}" "${EXEFLAGS[@]}" -o "$ROOT/out/obj/VistaExtSetup-stub.exe" "$OBJ/vxsetup.o" "$OBJ/vxsetup.res.o" "$OBJ/libvxcommon.a" "$OBJ/userrun.o" \
            -Wl,--entry=$wmainw -Wl,--subsystem,windows:6.0 -lcomctl32 -lshell32 -lole32 -loleaut32 -luuid -luser32 -lgdi32 \
            -ladvapi32 -lkernel32
    fi
    # the crash window (installed next to the DLLs; kernelvx starts it)
    clang "${CFLAGS[@]}" -c src/vxwatch/vxwatch.c -o "$OBJ/vxwatch.o"
    clang "${LDFLAGS[@]}" "${EXEFLAGS[@]}" -o "$OUT/vxwatch.exe" "$OBJ/vxwatch.o" "$OBJ/libvxcommon.a" "$OBJ/dll.res.o" \
        -Wl,--entry=$wmainw -Wl,--subsystem,windows:6.0 -ladvapi32 -luser32 -lgdi32 -lshell32 -lkernel32
    # the tray icon (the only visible part while VistaExt works)
    llvm-windres --target=$arch-w64-mingw32 -I"$WINEINC/windows" src/vxtray/vxtray.rc -o "$OBJ/vxtray.res.o"
    clang "${CFLAGS[@]}" -c src/vxtray/vxtray.c -o "$OBJ/vxtray.o"
    clang "${LDFLAGS[@]}" "${EXEFLAGS[@]}" -o "$OUT/vxtray.exe" "$OBJ/vxtray.o" "$OBJ/vxtray.res.o" "$OBJ/libvxcommon.a" \
        -Wl,--entry=$wmainw -Wl,--subsystem,windows:6.0 -lshell32 -luser32 -lgdi32 -ladvapi32 -lkernel32
    clang "${LDFLAGS[@]}" "${EXEFLAGS[@]}" -o "$OUT/vxcenter.exe" "$OBJ/vxcenter.o" "$OBJ/vxcenter.res.o" "$OBJ/patchlib.o" \
        "$OBJ/libvxcommon.a" "$OBJ/userrun.o" -Wl,--entry=$wmainw -Wl,--subsystem,windows:6.0 -lcomctl32 -lcomdlg32 -lshell32 -lole32 \
        -luser32 -lgdi32 -ladvapi32 -lkernel32 -loleaut32 -luuid
}

# one build per Windows and architecture; within each, the API sets come
# last (gen.py reads that build's kernelvx.dll)
OSES="vista win7"
if python3 -I -c 'import sys; sys.path.insert(0, "tools"); import gen; sys.exit(0 if gen.has_os_data("win8") else 1)'; then
    OSES="$OSES win8"
else
    echo "note: no Windows 8 export lists in third_party/winexports yet: the Windows 8 build is skipped"
fi
for os in $OSES; do
    build_arch $os i686 i386 x86
    build_arch $os x86_64 x86_64 x64
done

# DLLs run on their host program's threads, whose stack VistaExt doesn't
# control. Without stack probes a frame of 4 KB or more can skip the guard
# page and crash the program: refuse to build such a DLL.
big=0
for f in $(for os in $OSES; do echo out/$os-{x86,x64}/*.dll; done); do
    for h in $(llvm-objdump -d --no-show-raw-insn "$f" | grep -oE 'sub[lq][[:space:]]+\$0x[0-9a-f]+, %(r|e)sp' | grep -oE '0x[0-9a-f]+'); do
        if [ $((h)) -ge 4096 ]; then echo "error: $f has a $h-byte stack frame (limit 4 KB; use the heap)"; big=1; fi
    done
done
[ $big = 0 ] || exit 1

# Wine's import libraries put some functions in the DLL newer Windows has
# them in (OpenThreadToken: kernel32 on 8+, advapi32 on Vista/7). Check every
# program against the real Vista and 7 export lists.
python3 -I tools/vistacheck.py $(for os in $OSES; do echo out/$os-{x86,x64}/*.exe out/$os-{x86,x64}/vxload.dll; done) \
    out/obj/VistaExtSetup-stub.exe || {
    echo "error: a program imports something Windows Vista or 7 lacks (link order?)"; exit 1; }
echo "done: $(for os in $OSES; do echo -n "out/$os-x86 out/$os-x64 "; done)"
