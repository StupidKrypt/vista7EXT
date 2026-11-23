#!/usr/bin/env bash
# Test VistaExt on real Windows Vista / 7 in a headless QEMU/KVM virtual machine.
#
#   tests/vm/vm.sh install <vm> <windows.iso> "<edition>" [amd64|x86]
#       One-time unattended install of Windows into a clean base disk
#       (20-40 minutes). <edition> is the image name on the disc, e.g.
#       "Windows Vista ULTIMATE" or "Windows 7 ULTIMATE".
#       Windows 8 ("Windows 8") also needs VX_WIN_KEY=<product key>.
#   tests/vm/vm.sh test <vm>
#       Boots a throw-away copy of the base disk with a test CD: the VM
#       installs VistaExt, runs the tests, uninstalls it and shuts down.
#       The results are copied back and printed (a few minutes).
#       Run ./build.sh, ./package.sh and tests/run_wine.sh first.
#   tests/vm/vm.sh testcd
#       Only make the test CD (out/vm/test.iso), e.g. for another VM program
#       or a real PC: run \vxvm\run.cmd on it as administrator.
#   tests/vm/vm.sh apps <vm>
#       Real programs: boots a throw-away copy with an apps CD made from
#       out/apps (see tests/vm/apps.cmd), with network access, installs
#       VistaExt, and for each program patches it, starts it and takes a
#       screenshot (out/vm/<vm>/results/shots).
#   tests/vm/vm.sh video <vm>
#       Video playback: Brave and Firefox (VX_VIDEO_APPS="... edge" adds
#       Edge; all from out/apps) open the
#       browser-independent test page tests/vm/video.html (a 4K YouTube
#       video) in in-memory and in file mode; results in
#       out/vm/<vm>/results/video.
#   tests/vm/vm.sh prepare <vm>
#       Once per VM: installs Microsoft's updates from out/apps/dl/prereq and
#       keeps the result as prepared.qcow2. Every later run starts from it
#       (VX_VM_CLEAN=1: from the bare base.qcow2 instead).
#   tests/vm/vm.sh shot <vm> [out.png]
#       Screenshot of a running VM.
#
# The VMs have no network and use a blank Administrator password; they are
# for testing only. The disk of the last run is kept as run.qcow2 (replaced
# by the next run; VX_VM_KEEP=0 deletes it) for a look afterwards. Files: out/vm/<vm> (or $VX_VM_DIR/<vm>). While a VM runs,
# its screen is on VNC at 127.0.0.1 (the port is printed) and in
# out/vm/<vm>/screen.png (updated every 30 s).
set -uo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
VMS=${VX_VM_DIR:-$ROOT/out/vm}
H=$ROOT/tests/vm
WINEINC=/usr/include/wine

die() { echo "error: $*" >&2; exit 2; }
crlf() { sed 's/\r*$/\r/' "$1" > "$2"; }
qmp() { python3 -I "$H/qmp.py" "$@"; }
# QMP socket: a short path (unix sockets are limited to 107 characters)
sock() { echo "${XDG_RUNTIME_DIR:-/tmp}/vistaext-$(basename "$1").qmp"; }

shot() {   # shot <vm dir> <out.png>
    qmp "$(sock "$1")" shot "$1/shot.ppm" 2>/dev/null && magick "$1/shot.ppm" "$2" 2>/dev/null
    rm -f "$1/shot.ppm"
}

# run_qemu <vm dir> <time limit in s> <drive options...>
# Returns when Windows shuts down (0), or stops the VM at the time limit (124).
# VX_VM_NET=1 gives the VM network access (NAT, outgoing only). A line
# "SHOT <name>" from the VM on COM1 saves a screenshot as shots/<name>.png;
# "KEYS <key>..." presses keys and "TYPE <text>" types text on the VM's
# keyboard (QEMU key names, see qmp.py), for tests that drive a program.
run_qemu() {
    local D=$1 limit=$2; shift 2
    local arch mem=3072 n vnc=0 pid t=0 off=0 nic="-nic none" lines l smp=${VX_VM_SMP:-2}
    arch=$(cat "$D/arch")
    [ "$arch" = x86 ] && mem=2048
    [ "${VX_VM_MEM:-}" ] && mem=$VX_VM_MEM
    # VX_VM_SLOW=1: an old PC - one processor, 1 GB of memory (and a slow disk, see boot_cd)
    [ "${VX_VM_SLOW:-0}" = 1 ] && { smp=1; mem=1024; }
    if [ -f "$D/vnc" ]; then vnc=$(cat "$D/vnc")
    else for n in $(seq 1 30); do ss -ltn 2>/dev/null | grep -q "127.0.0.1:$((5900 + n)) " || { vnc=$n; break; }; done; fi
    [ "${VX_VM_NET:-0}" = 1 ] && nic="-nic user,model=e1000"
    rm -f "$(sock "$D")"
    : > "$D/serial.log"
    qemu-system-x86_64 -name "vistaext-$(basename "$D")" \
        -machine pc,accel=kvm -cpu host -smp $smp,sockets=1,cores=$smp -m $mem -rtc base=utc \
        -vga std -display none -vnc 127.0.0.1:$vnc $nic -usb -device usb-tablet \
        -serial file:"$D/serial.log" -qmp unix:"$(sock "$D")",server=on,wait=off \
        "$@" &
    pid=$!
    echo "VM $(basename "$D") started (VNC 127.0.0.1:$((5900 + vnc)), screen: $D/screen.png)"
    while kill -0 $pid 2>/dev/null; do
        if [ $t -ge "$limit" ]; then
            shot "$D" "$D/timeout.png"
            echo "VM still running after $limit s; stopped it (last screen: $D/timeout.png)"
            kill $pid; wait $pid 2>/dev/null
            return 124
        fi
        sleep 2; t=$((t + 2))
        lines=$(tail -c +$((off + 1)) "$D/serial.log" 2>/dev/null)
        off=$(stat -c %s "$D/serial.log" 2>/dev/null || echo 0)
        while IFS= read -r l; do
            l=${l%$'\r'}
            case $l in
                "SHOT "*) mkdir -p "$D/shots"; shot "$D" "$D/shots/${l#SHOT }.png" ;;
                "KEYS "*) qmp "$(sock "$D")" keys ${l#KEYS } >/dev/null 2>&1 ;;
                "TYPE "*) qmp "$(sock "$D")" type "${l#TYPE }" >/dev/null 2>&1 ;;
            esac
        done <<<"$lines"
        [ $((t % 30)) = 0 ] && kill -0 $pid 2>/dev/null && shot "$D" "$D/screen.png"
    done
    wait $pid
}

# Vista's product key page, answered as a person would without a key: Next,
# then "No" to "Do you want to enter your product key now?". The page is
# recognised by the yellow key sticker picture in the middle of it.
vista_key_page() {
    local D=$1 i rgb r g b
    for i in $(seq 1 120); do
        sleep 5
        qmp "$(sock "$D")" shot "$D/key.ppm" 2>/dev/null || continue
        rgb=$(magick "$D/key.ppm" -crop 200x56+134+237 +repage -scale 1x1! -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info: 2>/dev/null)
        read -r r g b <<<"$rgb"
        if [ "${r:-0}" -gt 190 ] && [ "${g:-0}" -gt 170 ] && [ "${b:-255}" -lt 160 ]; then
            qmp "$(sock "$D")" keys alt-n
            sleep 4
            qmp "$(sock "$D")" keys tab ret
            echo "Answered Vista's product key page (no key, 30-day trial)."
            break
        fi
    done
    rm -f "$D/key.ppm"
}

cmd_install() {
    local name=${1:?vm name} iso=${2:?windows iso} image=${3:?edition name} arch=${4:-amd64}
    local D=$VMS/$name key=""
    [ -f "$iso" ] || die "no such file: $iso"
    case $arch in amd64) echo x64 ;; x86) echo x86 ;; *) die "arch is amd64 or x86" ;; esac > /dev/null
    [ -f "$D/base.qcow2" ] && die "$D/base.qcow2 already exists (delete it to reinstall)"
    mkdir -p "$D/setup"
    [ $arch = amd64 ] && echo x64 > "$D/arch" || echo x86 > "$D/arch"
    # its own VNC display number, so several VMs can run at the same time
    local used n
    used=$(cat "$VMS"/*/vnc 2>/dev/null)
    for n in $(seq 1 30); do grep -qx "$n" <<<"$used" || { echo $n > "$D/vnc"; break; }; done
    # No product key: Windows runs as a 30-day trial. Windows 7 asks for a key
    # in OOBE, which the answer file skips. Vista's setup always shows its key
    # page (an empty key with WillShowUI=Never is rejected as invalid), so
    # vista_key_page answers it.
    # Windows 8 setup cannot go on without a key; it comes from VX_WIN_KEY
    # (your own key, or Microsoft's generic install key for the edition) and
    # is only written into this VM's setup CD, never into the repository.
    case $image in
        *Vista*) key=$'\n        <ProductKey>\n          <Key></Key>\n          <WillShowUI>OnError</WillShowUI>\n        </ProductKey>' ;;
        *"Windows 8"*)
            [[ ${VX_WIN_KEY:-} =~ ^[A-Z0-9]{5}(-[A-Z0-9]{5}){4}$ ]] ||
                die "Windows 8 setup needs a product key: VX_WIN_KEY=XXXXX-XXXXX-XXXXX-XXXXX-XXXXX"
            key=$'\n        <ProductKey>\n          <Key>'"$VX_WIN_KEY"$'</Key>\n          <WillShowUI>OnError</WillShowUI>\n        </ProductKey>' ;;
    esac
    ARCH=$arch IMAGE=$image KEY=$key python3 -I -c '
import os, sys
t = open(sys.argv[1]).read()
for k, v in (("@ARCH@", "ARCH"), ("@IMAGE@", "IMAGE"), ("@PRODUCTKEY@", "KEY")):
    t = t.replace(k, os.environ[v])
open(sys.argv[2], "w", newline="\r\n").write(t)' "$H/autounattend.xml" "$D/setup/autounattend.xml"
    crlf "$H/vxsetup.cmd" "$D/setup/vxsetup.cmd"
    crlf "$H/vxboot.cmd" "$D/setup/vxboot.cmd"
    genisoimage -quiet -J -r -V VXSETUP -o "$D/setup.iso" "$D/setup" || die "genisoimage failed"
    qemu-img create -q -f qcow2 "$D/base.qcow2" 40G || die "qemu-img failed"
    echo "$iso" > "$D/iso"
    echo "Installing $image ($arch) into $D/base.qcow2; this takes 20-40 minutes."
    case $image in *Vista*) vista_key_page "$D" & ;; esac
    run_qemu "$D" 5400 \
        -drive file="$D/base.qcow2",if=ide,index=0,media=disk,format=qcow2 \
        -drive file="$iso",if=ide,index=2,media=cdrom,readonly=on \
        -drive file="$D/setup.iso",if=ide,index=3,media=cdrom,readonly=on \
        -boot once=d
    if grep -q 'VXSETUP done' "$D/serial.log"; then
        chmod a-w "$D/base.qcow2"    # the clean base; test runs use copies
        echo "Windows installed and prepared: $D/base.qcow2"
    else
        echo "Windows setup did not finish (serial log: $D/serial.log; last screen: $D/screen.png)"
        return 1
    fi
}

build_tools() {   # the in-VM helpers, both architectures, into $VMS/bin
    local A arch winarch out O c t
    for A in x86_64:x86_64:x64 i686:i386:x86; do
        IFS=: read -r arch winarch out <<<"$A"
        O=$VMS/bin/$out; mkdir -p "$O/obj"
        local CF=(--target=$arch-w64-windows-gnu -O2 -ffreestanding -fno-builtin -mno-stack-arg-probe
                  -I"$WINEINC/windows" -I"$WINEINC/msvcrt" -D_ACRTIMP= -D_ACRTIMP_ALT=)
        local LF=(--target=$arch-w64-windows-gnu -fuse-ld=lld -nostdlib -L/usr/lib/wine/$winarch-windows
                  -Wl,--entry=mainCRTStartup -Wl,--subsystem,console:6.0 -Xlinker --stack -Xlinker 0x100000,0x40000)
        for c in src/common/console.c src/common/common.c src/common/pin.c tests/vm/nofault.c tests/guishot.c tests/msitest.c tests/vm/apitest.c tests/vm/vxdbg.c tests/vm/probe.c tests/vm/mitigtest.c; do
            clang "${CF[@]}" -c "$c" -o "$O/obj/$(basename "${c%.c}").o" || return 1
        done
        for t in nofault guishot msitest vxdbg probe mitigtest; do
            clang "${LF[@]}" -o "$O/$t.exe" "$O/obj/$t.o" "$O/obj/console.o" "$O/obj/common.o" "$O/obj/pin.o" \
                -lmsi -lkernel32 -luser32 -lgdi32 -lshell32 -ladvapi32 || return 1
        done
        # apitest imports through API-set DLL names: import libraries for them
        # (SetThreadDescription is Windows 10's, through a contract Vista, 7 and
        # 8 lack: so apitest needs VistaExt on all of them)
        local m=i386:x86-64 d1=WaitOnAddress d2=WakeByAddressAll d3=GetSystemTimePreciseAsFileTime d4=WakeByAddressSingle d5=SetThreadDescription
        [ "$arch" = i686 ] && { m=i386; d1=WaitOnAddress@16; d2=WakeByAddressAll@4; d3=GetSystemTimePreciseAsFileTime@4; d4=WakeByAddressSingle@4; d5=SetThreadDescription@8; }
        printf 'LIBRARY api-ms-win-core-synch-l1-2-0.dll\nEXPORTS\n  %s\n  %s\n' $d1 $d2 > "$O/obj/synch.def"
        printf 'LIBRARY api-ms-win-core-sysinfo-l1-2-0.dll\nEXPORTS\n  %s\n' $d3 > "$O/obj/sysinfo.def"
        llvm-dlltool -m $m -k -d "$O/obj/synch.def" -l "$O/obj/libsynch.a" || return 1
        printf 'LIBRARY api-ms-win-core-processthreads-l1-1-3.dll\nEXPORTS\n  %s\n' $d5 > "$O/obj/pt13.def"
        llvm-dlltool -m $m -k -d "$O/obj/sysinfo.def" -l "$O/obj/libsysinfo.a" || return 1
        llvm-dlltool -m $m -k -d "$O/obj/pt13.def" -l "$O/obj/libpt13.a" || return 1
        clang "${LF[@]}" -o "$O/apitest.exe" "$O/obj/apitest.o" "$O/obj/console.o" "$O/obj/common.o" "$O/obj/pin.o" \
            "$O/obj/libsynch.a" "$O/obj/libsysinfo.a" "$O/obj/libpt13.a" -lkernel32 -luser32 -lshell32 -ladvapi32 || return 1
        python3 -I tools/peexp.py "$O/apitest.exe" imports | grep -q '^api-ms-win-core-synch-l1-2-0.dll' ||
            { echo "apitest.exe does not import through the API set"; return 1; }
        # in-memory mode: a DLL needing Windows 8 functions from kernel32, a
        # program loading it at start and later, and a windowed program that crashes
        printf 'LIBRARY kernel32.dll\nEXPORTS\n  %s\n  %s\n  %s\n  %s\n' $d1 $d2 $d3 $d4 > "$O/obj/k32new.def"
        llvm-dlltool -m $m -k -d "$O/obj/k32new.def" -l "$O/obj/libk32new.a" || return 1
        for c in memdll memhost crashtest spawntest; do clang "${CF[@]}" -c tests/vm/$c.c -o "$O/obj/$c.o" || return 1; done
        local dentry=DllMainCRTStartup; [ "$arch" = i686 ] && dentry=DllMainCRTStartup@12
        clang "${LF[@]}" -shared -Wl,--entry=$dentry -o "$O/memdll.dll" "$O/obj/memdll.o" \
            -Wl,--out-implib,"$O/obj/libmemdll.a" "$O/obj/libk32new.a" -lkernel32 || return 1
        clang "${LF[@]}" -o "$O/memhost.exe" "$O/obj/memhost.o" "$O/obj/console.o" "$O/obj/common.o" "$O/obj/pin.o" \
            "$O/obj/libmemdll.a" -lkernel32 -luser32 -lshell32 -ladvapi32 || return 1
        clang "${LF[@]}" -Wl,--entry=WinMainCRTStartup -Wl,--subsystem,windows:6.0 -o "$O/crashtest.exe" \
            "$O/obj/crashtest.o" "$O/obj/libk32new.a" -lkernel32 -luser32 || return 1
        clang "${CF[@]}" -c tests/vm/procstat.c -o "$O/obj/procstat.o" || return 1
        clang "${LF[@]}" -o "$O/procstat.exe" "$O/obj/procstat.o" "$O/obj/console.o" "$O/obj/common.o" "$O/obj/pin.o" \
            -lntdll -lkernel32 -luser32 -lshell32 -ladvapi32 || return 1
        clang "${LF[@]}" -o "$O/spawntest.exe" "$O/obj/spawntest.o" "$O/obj/console.o" "$O/obj/common.o" "$O/obj/pin.o" \
            "$O/obj/libk32new.a" -lkernel32 -luser32 -lshell32 -ladvapi32 || return 1
        clang "${CF[@]}" -c tests/vm/nettest.c -o "$O/obj/nettest.o" || return 1
        clang "${LF[@]}" -o "$O/nettest.exe" "$O/obj/nettest.o" "$O/obj/console.o" "$O/obj/common.o" "$O/obj/pin.o" \
            -lwinhttp -lwininet -lkernel32 -luser32 -lshell32 -ladvapi32 || return 1
        clang "${CF[@]}" -c tests/vm/vercheck.c -o "$O/obj/vercheck.o" || return 1
        clang "${LF[@]}" -o "$O/vercheck.exe" "$O/obj/vercheck.o" "$O/obj/console.o" "$O/obj/common.o" "$O/obj/pin.o" \
            -lkernel32 -luser32 -lshell32 -ladvapi32 || return 1
        # vxbench: VistaExt's cost (vxbench-native.exe is the plain-Windows baseline)
        clang "${CF[@]}" -c tests/vm/vxbench.c -o "$O/obj/vxbench.o" || return 1
        clang "${CF[@]}" -DVX_BENCH_NATIVE -c tests/vm/vxbench.c -o "$O/obj/vxbench-native.o" || return 1
        clang "${CF[@]}" -c tests/vm/benchdll.c -o "$O/obj/benchdll.o" || return 1
        clang "${LF[@]}" -o "$O/vxbench.exe" "$O/obj/vxbench.o" "$O/obj/console.o" "$O/obj/common.o" "$O/obj/pin.o" \
            "$O/obj/libk32new.a" -lkernel32 -luser32 -lshell32 -ladvapi32 || return 1
        clang "${LF[@]}" -o "$O/vxbench-native.exe" "$O/obj/vxbench-native.o" "$O/obj/console.o" "$O/obj/common.o" "$O/obj/pin.o" \
            -lkernel32 -luser32 -lshell32 -ladvapi32 || return 1
        clang "${LF[@]}" -shared -Wl,--entry=$dentry -o "$O/benchdll.dll" "$O/obj/benchdll.o" -lkernel32 || return 1
        # the test program from run_wine.sh, which leaves it unpatched
        python3 -I tools/peexp.py "out/test/vista-$out/app/vxtest.exe" imports | grep -q '^kernel32.dll' ||
            { echo "out/test/vista-$out/app/vxtest.exe missing or patched: run tests/run_wine.sh"; return 1; }
        cp "out/test/vista-$out/app/vxtest.exe" "$O/"   # the same program for both builds
    done
}

# The test CD (out/vm/test.iso): \vxvm\run.cmd, the release folder and the
# test programs. It also works by hand on any Vista/7 PC or VM: insert it and
# run \vxvm\run.cmd as administrator (results go to the desktop).
make_testcd() {
    local S=$VMS/testcd
    [ -f out/dist/VistaExt/install.cmd ] || { echo "run ./package.sh first"; return 1; }
    build_tools || { echo "could not build the test tools"; return 1; }
    rm -rf "$S"; mkdir -p "$S/vxvm/bin"
    # VX_DIST: another release folder (measuring an older build)
    cp -r "${VX_DIST:-out/dist}/VistaExt" "$S/vxvm/VistaExt"
    cp -r "$VMS/bin/x64" "$VMS/bin/x86" "$S/vxvm/bin/"
    rm -rf "$S/vxvm/bin/x64/obj" "$S/vxvm/bin/x86/obj"
    crlf "$H/run.cmd" "$S/vxvm/run.cmd"
    # VX_VM_ONLY=bench: install, measure performance, nothing else
    [ "${VX_VM_ONLY:-}" = bench ] && echo bench > "$S/vxvm/only-bench.txt"
    # VX_VM_ONLY=diag: install, then the version/helper diagnostics under vxdbg
    [ "${VX_VM_ONLY:-}" = diag ] && echo diag > "$S/vxvm/only-diag.txt"
    # VX_VM_ONLY=mitig: only mitigtest (how this Windows treats mitigation
    # policies it does not know), no install
    [ "${VX_VM_ONLY:-}" = mitig ] && echo mitig > "$S/vxvm/only-mitig.txt"
    # VX_VM_ONLY=shellx: install, then the ShellExecute experiment
    [ "${VX_VM_ONLY:-}" = shellx ] && echo shellx > "$S/vxvm/only-shellx.txt"
    # VX_VM_ONLY=watch: install, then which crash watchers of a 32-bit program exist when
    [ "${VX_VM_ONLY:-}" = watch ] && echo watch > "$S/vxvm/only-watch.txt"
    # VX_VM_ONLY=uninst: uninstalling with VistaExtSetup.exe while a program uses VistaExt (one restart)
    [ "${VX_VM_ONLY:-}" = uninst ] && echo uninst > "$S/vxvm/only-uninst.txt" && cp out/dist/VistaExtSetup.exe "$S/vxvm/"
    # VX_VM_ONLY=upgrade: an earlier release (VX_OLD_SETUP, default the released
    # Beta 1.1 kept in out/old) installed and used, this one over it, then removed
    if [ "${VX_VM_ONLY:-}" = upgrade ]; then
        echo upgrade > "$S/vxvm/only-upgrade.txt"
        cp out/dist/VistaExtSetup.exe "$S/vxvm/"
        cp "${VX_OLD_SETUP:-out/old/beta1.1/VistaExtSetup.exe}" "$S/vxvm/VistaExtSetup-old.exe" || return 1
    fi
    genisoimage -quiet -J -r -V VXTEST -o "$VMS/test.iso" "$S" || return 1
    echo "test CD: $VMS/test.iso"
}

# boot_cd <vm> <cd image> <time limit>: a throw-away copy of the base disk,
# the CD and an empty results disk; prints the results afterwards.
boot_cd() {
    local name=$1 iso=$2 limit=$3
    local D=$VMS/$name R rc
    R=$D/results.img
    rm -f "$R"; truncate -s 512M "$R"
    printf 'label: dos\nstart=2048, type=6\n' | sfdisk -q "$R" || die "sfdisk failed"
    mkfs.fat -F 16 --offset 2048 -n VXRESULTS "$R" >/dev/null || die "mkfs.fat failed"
    echo marker > "$D/marker"; mcopy -i "$R@@1M" "$D/marker" ::vxresults.marker
    # VX_VM_EXTRA: files the VM's script reads from the results disk
    for f in ${VX_VM_EXTRA:-}; do mcopy -i "$R@@1M" "$f" "::$(basename "$f")"; done

    rm -f "$D/run.qcow2"; rm -rf "$D/shots"
    local back="$D/base.qcow2"
    [ -f "$D/prepared.qcow2" ] && [ "${VX_VM_CLEAN:-0}" != 1 ] && back="$D/prepared.qcow2"
    # the dependency baseline (cmd_deps), unless a layer is asked for by name
    [ -f "$D/deps.qcow2" ] && [ "${VX_VM_CLEAN:-0}" != 1 ] && back="$D/deps.qcow2"
    [ -n "${VX_VM_BACK:-}" ] && back="$D/$VX_VM_BACK"
    echo "VM $name starts from $(basename "$back")"
    # the backing file by name only (relative to the VM's folder): the VM
    # folders keep working when the project is moved or copied
    qemu-img create -q -f qcow2 -b "$(basename "$back")" -F qcow2 "$D/run.qcow2" || die "qemu-img failed"
    # VX_VM_SLOW=1: a laptop hard disk - 20 MB/s, 100 operations per second
    local slow=""
    [ "${VX_VM_SLOW:-0}" = 1 ] && slow=",throttling.bps-total=20971520,throttling.iops-total=100"
    run_qemu "$D" "$limit" \
        -drive file="$D/run.qcow2",if=ide,index=0,media=disk,format=qcow2$slow \
        -drive file="$R",if=ide,index=1,media=disk,format=raw \
        -drive file="$iso",if=ide,index=2,media=cdrom,readonly=on
    rc=$?

    rm -rf "$D/results"; mkdir -p "$D/results"
    mcopy -s -n -i "$R@@1M" '::*' "$D/results/" 2>/dev/null
    [ -d "$D/shots" ] && mv "$D/shots" "$D/results/shots"
    for b in "$D"/results/*.bmp; do [ -f "$b" ] && magick "$b" -alpha off "${b%.bmp}.png" && rm -f "$b"; done
    [ "${VX_VM_KEEP:-1}" = 1 ] || rm -f "$D/run.qcow2"
    echo
    if [ -f "$D/results/results.txt" ]; then
        tr -d '\r' < "$D/results/results.txt"
    else
        echo "no results (VM exit $rc); serial log:"; tr -d '\r' < "$D/serial.log"
        return 1
    fi
    echo; echo "All files: $D/results"
    grep -q '^RESULT: [0-9]* passed, 0 failed' <(tr -d '\r' < "$D/results/results.txt")
}

cmd_test() {
    local name=${1:?vm name}
    local D=$VMS/$name
    [ -f "$D/base.qcow2" ] || die "no VM $name: run '$0 install $name ...' first"
    # each run boots its own copy of the CD, so VMs can be tested side by side
    mkdir -p "$VMS"
    ( flock 9; make_testcd && cp "$VMS/test.iso" "$D/test.iso" ) 9>"$VMS/.testcd.lock" || die "could not make the test CD"
    echo "Testing VistaExt on $name ..."
    boot_cd "$name" "$D/test.iso" 3000
}

# The apps CD: the test CD's tools plus the programs in out/apps/x (unpacked
# by hand, see tests/vm/apps.cmd for the layout) and \vxvm\run.cmd = apps.cmd.
make_appscd() {
    local S=$VMS/appscd A=out/apps
    [ -d $A/x ] || { echo "no programs in $A/x"; return 1; }
    make_testcd >/dev/null || return 1
    rm -rf "$S"; mkdir -p "$S/apps"
    cp -r "$VMS/testcd/vxvm" "$S/vxvm"
    crlf "$H/apps.cmd" "$S/vxvm/run.cmd"
    [ -f out/dist/VistaExtSetup.exe ] || { echo "run ./package.sh first (no VistaExtSetup.exe)"; return 1; }
    cp out/dist/VistaExtSetup.exe "$S/vxvm/"
    # Microsoft's C runtime update (KB3118401 .msu from catalog.update.microsoft.com)
    if ls $A/dl/prereq/*.msu >/dev/null 2>&1; then mkdir -p "$S/prereq"; cp $A/dl/prereq/*.msu "$S/prereq/"; fi
    # VX_APPS="brave discord": only these programs (default: all)
    printf '%s\r\n' ${VX_APPS:-brave discord firefox epic steam} > "$S/vxvm/apps.txt"
    # VX_APPS="trace": the programs of tests/vm/trace-apps.txt for this VM's
    # architecture (VX_TRACE_ARCH, set by cmd_apps), each run traced;
    # VX_TRACE_ONLY="obs zoom": only these names from the list
    case " ${VX_APPS:-} " in *" trace "*)
        : > "$S/vxvm/trace.txt"
        grep -v '^#' "$H/trace-apps.txt" | while IFS='|' read -r n a d e args; do
            [ -n "$n" ] && [ "$a" = "${VX_TRACE_ARCH:-x64}" ] && [ -d "$d" ] || continue
            [ -z "${VX_TRACE_ONLY:-}" ] || case " $VX_TRACE_ONLY " in *" $n "*) ;; *) continue ;; esac
            # -L: links (a macOS app bundle inside a game folder) as the files they name
            cp -rL "$d" "$S/apps/trace-$n"
            printf '%s|%s|%s\r\n' "$n" "$e" "$args" >> "$S/vxvm/trace.txt"
        done
        echo "traced programs: $(grep -c . "$S/vxvm/trace.txt") ($(du -sh "$S/apps" | cut -f1))"
        genisoimage -quiet -udf -J -joliet-long -r -V VXAPPS -o "$VMS/apps.iso" "$S" || return 1
        echo "apps CD: $VMS/apps.iso ($(du -h "$VMS/apps.iso" | cut -f1))"
        return 0 ;; esac
    [ -d $A/x/brave ] && cp -r $A/x/brave "$S/apps/brave"
    [ -d $A/x/firefox/core ] && cp -r $A/x/firefox/core "$S/apps/firefox"
    if [ -d $A/x/discord/pkg/lib/net45 ]; then
        mkdir -p "$S/apps/discord"
        cp $A/x/discord/Update.exe "$S/apps/discord/"
        cp -r $A/x/discord/pkg/lib/net45 "$S/apps/discord/app"
    fi
    if [ -f $A/x/steam/Steam.exe ]; then
        mkdir -p "$S/apps/steam"
        cp -r $A/x/steam/Steam.exe $A/x/steam/bin $A/x/steam/public "$S/apps/steam/"
    fi
    [ -f $A/dl/epic/EpicInstaller.msi ] && mkdir -p "$S/apps/epic" && cp $A/dl/epic/EpicInstaller.msi "$S/apps/epic/"
    # VX_APPS="setups": the real setup programs, run as a user runs them
    case " ${VX_APPS:-} " in *" setups "*)
        mkdir -p "$S/setups"
        for f in $A/dl/spotify/SpotifySetup.exe $A/dl/discord/DiscordSetup.exe $A/dl/steam/SteamSetup.exe; do
            [ -f "$f" ] && cp "$f" "$S/setups/"
        done ;; esac
    # Edge (tools/unpack-omaha-msi.sh): 1.3 GB, only when asked for
    case " ${VX_APPS:-} " in *" edge "*)
        for a in x64 x86; do [ -d $A/x/edge-$a ] && cp -r $A/x/edge-$a "$S/apps/edge-$a"; done ;; esac
    genisoimage -quiet -udf -J -joliet-long -r -V VXAPPS -o "$VMS/apps.iso" "$S" || return 1
    echo "apps CD: $VMS/apps.iso ($(du -h "$VMS/apps.iso" | cut -f1))"
}

# prepare: Microsoft's updates installed once, kept as prepared.qcow2
cmd_prepare() {
    local name=${1:?vm name}
    local D=$VMS/$name S=$VMS/prepcd
    [ -f "$D/base.qcow2" ] || die "no VM $name: run '$0 install $name ...' first"
    ls out/apps/dl/prereq/*.msu >/dev/null 2>&1 || die "no updates in out/apps/dl/prereq"
    ( flock 9
      rm -rf "$S"; mkdir -p "$S/vxvm" "$S/prereq"
      crlf "$H/prepare.cmd" "$S/vxvm/run.cmd"
      cp out/apps/dl/prereq/*.msu "$S/prereq/"
      genisoimage -quiet -udf -J -joliet-long -r -V VXPREP -o "$D/prep.iso" "$S" ) 9>"$VMS/.testcd.lock" || die "could not make the CD"
    chmod u+w "$D/prepared.qcow2" 2>/dev/null; rm -f "$D/prepared.qcow2"
    VX_VM_KEEP=1 VX_VM_CLEAN=1 boot_cd "$name" "$D/prep.iso" 2400
    if grep -q "PREPARED ok" <(tr -d '\r' < "$D/results/results.txt" 2>/dev/null); then
        mv "$D/run.qcow2" "$D/prepared.qcow2" && chmod a-w "$D/prepared.qcow2"
        echo "prepared: $D/prepared.qcow2 (later runs start from it)"
    else
        echo "preparing $name failed"; return 1
    fi
}

# perf: VistaExt's cost on a slow PC (tests/vm/perf.cmd); VX_DIST picks the
# build (default out/dist). Results: results/perf.txt
cmd_perf() {
    local name=${1:?vm name}
    local D=$VMS/$name S=$VMS/perfcd
    [ -f "$D/base.qcow2" ] || die "no VM $name: run '$0 install $name ...' first"
    ( flock 9
      make_testcd >/dev/null || exit 1
      rm -rf "$S"; cp -r "$VMS/testcd" "$S"
      crlf "$H/perf.cmd" "$S/vxvm/run.cmd"
      [ -n "${VX_PERF_ONLY:-}" ] && echo x > "$S/vxvm/only-${VX_PERF_ONLY}.txt"
      genisoimage -quiet -J -r -V VXPERF -o "$D/perf.iso" "$S" ) 9>"$VMS/.testcd.lock" || die "could not make the CD"
    VX_VM_SLOW=${VX_VM_SLOW:-1} boot_cd "$name" "$D/perf.iso" 7200
}

# deps: the runtimes and updates programs expect (tests/vm/deps.cmd), from
# out/apps/dl/deps/<vista|win7>-<x86|x64>/ (see deps.cmd), installed on top of
# prepared.qcow2 into deps.qcow2; later runs start from deps.qcow2.
# VX_DEPS_ONLY=survey: only record the VM's state (results/survey-before.txt).
cmd_deps() {
    local name=${1:?vm name}
    local D=$VMS/$name S=$VMS/depscd arch os back
    [ -f "$D/base.qcow2" ] || die "no VM $name: run '$0 install $name ...' first"
    arch=$(cat "$D/arch"); case $name in vista*) os=vista ;; *) os=win7 ;; esac
    ( flock 9
      build_tools >/dev/null || exit 1
      rm -rf "$S"; mkdir -p "$S/vxvm/bin"
      crlf "$H/deps.cmd" "$S/vxvm/run.cmd"
      cp -r "$VMS/bin/x64" "$VMS/bin/x86" "$S/vxvm/bin/"; rm -rf "$S/vxvm/bin/x64/obj" "$S/vxvm/bin/x86/obj"
      [ "${VX_DEPS_ONLY:-}" = survey ] && echo survey > "$S/vxvm/only-survey.txt"
      if [ -d "out/apps/dl/deps/$os-$arch" ] && [ "${VX_DEPS_ONLY:-}" != survey ]; then
          mkdir -p "$S/deps"; cp -r "out/apps/dl/deps/$os-$arch/." "$S/deps/"; fi
      genisoimage -quiet -udf -J -joliet-long -r -V VXDEPS -o "$D/deps.iso" "$S" ) 9>"$VMS/.testcd.lock" || die "could not make the CD"
    if [ "${VX_DEPS_ONLY:-}" = survey ]; then
        VX_VM_NET=1 boot_cd "$name" "$D/deps.iso" 1200
        return
    fi
    # on top of prepared.qcow2 (kept as it is: the snapshot to go back to)
    back=prepared.qcow2; [ -f "$D/$back" ] || back=base.qcow2
    chmod u+w "$D/deps.qcow2" 2>/dev/null; rm -f "$D/deps.qcow2"
    VX_VM_NET=1 VX_VM_KEEP=1 VX_VM_BACK=$back boot_cd "$name" "$D/deps.iso" 10800
    if grep -q "DEPS ok" <(tr -d '\r' < "$D/results/install.txt" 2>/dev/null); then
        mv "$D/run.qcow2" "$D/deps.qcow2" && chmod a-w "$D/deps.qcow2"
        echo "baseline: $D/deps.qcow2 (later runs start from it; delete it to go back to $back)"
    else
        echo "installing the dependencies on $name did not finish: see $D/results/install.txt"; return 1
    fi
}

# The video CD: the apps CD's Brave and Firefox, with video.cmd as run.cmd
make_videocd() {
    local S=$VMS/videocd
    VX_APPS="${VX_VIDEO_APPS:-brave firefox}" make_appscd >/dev/null || return 1
    rm -rf "$S"; cp -r "$VMS/appscd" "$S"
    rm -rf "$S/apps/discord" "$S/apps/steam" "$S/apps/epic"
    crlf "$H/video.cmd" "$S/vxvm/run.cmd"
    # VX_VIDEO_APPS="firefox edge": these browsers (default: brave firefox)
    printf '%s\r\n' ${VX_VIDEO_APPS:-brave firefox} > "$S/vxvm/video-apps.txt"
    # VX_VIDEO_LENIENT=1: VX_LENIENT_CHILDREN=1 for the browsers (see misc.c)
    [ "${VX_VIDEO_LENIENT:-0}" = 1 ] && echo on > "$S/vxvm/lenient.txt"
    genisoimage -quiet -udf -J -joliet-long -r -V VXVIDEO -o "$VMS/video.iso" "$S" || return 1
    echo "video CD: $VMS/video.iso ($(du -h "$VMS/video.iso" | cut -f1))"
}

# Video playback: the VM opens http://10.0.2.2:<port>/video.html (served here
# by videoserve.py, which also collects the page's reports) in each browser
cmd_video() {
    local name=${1:?vm name}
    local D=$VMS/$name port srv
    [ -f "$D/base.qcow2" ] || die "no VM $name: run '$0 install $name ...' first"
    mkdir -p "$VMS"
    ( flock 9; make_videocd && cp "$VMS/video.iso" "$D/video.iso" ) 9>"$VMS/.testcd.lock" || die "could not make the video CD"
    port=$((28000 + $(cat "$D/vnc" 2>/dev/null || echo 0)))
    rm -rf "$D/video"
    python3 -I "$H/videoserve.py" $port "$D/video" > "$D/videoserve.log" 2>&1 &
    srv=$!
    printf 'http://10.0.2.2:%s/video.html\r\n' $port > "$D/video-url.txt"
    echo "Video playback on $name (network, ${VX_VM_SMP:-8} processors, page server on port $port) ..."
    VX_VM_EXTRA="$D/video-url.txt" VX_VM_NET=1 VX_VM_SMP=${VX_VM_SMP:-8} VX_VM_MEM=${VX_VM_MEM:-6144} \
        boot_cd "$name" "$D/video.iso" 5400
    kill $srv 2>/dev/null
    [ -d "$D/video" ] && mv "$D/video" "$D/results/video"
    cat "$D/videoserve.log"
}

cmd_apps() {
    local name=${1:?vm name}
    local D=$VMS/$name
    [ -f "$D/base.qcow2" ] || die "no VM $name: run '$0 install $name ...' first"
    mkdir -p "$VMS"
    ( flock 9; VX_TRACE_ARCH=$(cat "$D/arch") make_appscd && cp "$VMS/apps.iso" "$D/apps.iso" ) 9>"$VMS/.testcd.lock" || die "could not make the apps CD"
    echo "Testing programs on $name (with network access) ..."
    VX_VM_NET=1 VX_VM_MEM=${VX_VM_MEM:-4096} boot_cd "$name" "$D/apps.iso" "${VX_VM_TIMEOUT:-5400}"
}

# one run per VM at a time (they share the VM's disk files)
lock_vm() {
    mkdir -p "$VMS/$1"
    exec 8>"$VMS/$1/.lock"
    flock -n 8 || die "VM $1 is already running"
}

case ${1:-} in
    install) shift; lock_vm "${1:?vm name}"; cmd_install "$@" ;;
    test) shift; lock_vm "${1:?vm name}"; cmd_test "$@" ;;
    apps) shift; lock_vm "${1:?vm name}"; cmd_apps "$@" ;;
    testcd) mkdir -p "$VMS"; make_testcd ;;
    video) shift; lock_vm "${1:?vm name}"; cmd_video "$@" ;;
    prepare) shift; lock_vm "${1:?vm name}"; cmd_prepare "$@" ;;
    deps) shift; lock_vm "${1:?vm name}"; cmd_deps "$@" ;;
    perf) shift; lock_vm "${1:?vm name}"; cmd_perf "$@" ;;
    shot) D=$VMS/${2:?vm name}; shot "$D" "${3:-$D/screen.png}" && echo "${3:-$D/screen.png}" ;;
    *) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
