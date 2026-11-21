"""Generate .def files for VistaExt DLLs.

Companion DLLs (kernelvx, uservx, ...) export:
  * every vx_<Name> function compiled into them, as <Name>
  * every other name the original DLL is known to export, forwarded to the
    original Vista DLL. Name lists come from the real Windows Vista and 7 DLLs
    (third_party/winexports), Wine's builds of those DLLs and the names
    One-Core-API's api-set specs reference.
api-set DLLs forward each function to the companion or system DLL that
provides it on Vista.
"""
import argparse, glob, os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(__file__))
from peexp import PE

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPECS = os.path.join(ROOT, 'third_party', 'oca-apisets')
WINEXPORTS = os.path.join(ROOT, 'third_party', 'winexports')
WINEDIR = {'i686': '/usr/lib/wine/i386-windows', 'x86_64': '/usr/lib/wine/x86_64-windows'}
SPEC_ARCH = {'i686': 'i386', 'x86_64': 'x86_64'}

# where api-set forwards point on Vista
TARGET_MAP = {'kernel32': 'kernelvx', 'kernelex': 'kernelvx', 'kernelbase': 'kernelvx',
              'advapi32': 'advapivx', 'advapinew': 'advapivx',
              'user32': 'uservx', 'shell32': 'shellvx', 'd3dkmt': 'gdi32',
              'ntdll': 'ntvx', 'ws2_32': 'ws2vx', 'cfgmgr32': 'cfgmgrvx', 'ole32': 'olevx'}
# functions One-Core-API's specs leave out of a contract
APISET_EXTRA = {
    'api-ms-win-core-winrt-l1-1-0': {'RoRevokeActivationFactories': 'combase', 'RoUnregisterForApartmentShutdown': 'combase',
                                     'RoRegisterForApartmentShutdown': 'combase'},
}
# Newer contracts One-Core-API has no spec for: (the contract it extends, added functions)
APISET_NEW = {
    'api-ms-win-power-setting-l1-1-1': ('api-ms-win-power-setting-l1-1-0',
                                        {'PowerRegisterForEffectivePowerModeNotifications': 'kernelvx',
                                         'PowerUnregisterFromEffectivePowerModeNotifications': 'kernelvx'}),
}
APISET_EXTRA['api-ms-win-core-psm-appnotify-l1-1-0'] = {'RegisterAppStateChangeNotification': 'kernelvx',
                                                        'UnregisterAppStateChangeNotification': 'kernelvx'}
SKIP_TARGETS = {'ole'}
COMPANIONS = set(TARGET_MAP.values()) | {'combase', 'shcore', 'gdi32'}

LINE = re.compile(r'^\s*(@|\d+)\s+(\w+)\s+(.*)$')


def spec_entries(path, arch):
    """Yield (export_name, target_dll, target_func) from a Wine-style .spec."""
    for raw in open(path, encoding='latin1'):
        line = raw.split('#')[0].split(';')[0].strip()
        m = LINE.match(line)
        if not m or m.group(2) in ('stub', 'extern', 'variable'):
            continue
        rest = m.group(3).split()
        flags = [r for r in rest if r.startswith('-')]
        rest = [r for r in rest if not r.startswith('-')]
        if any(f == '-noname' for f in flags):
            continue
        ok = True
        for f in flags:
            if f.startswith('-arch='):
                archs = f[6:].split(',')
                neg = [a[1:] for a in archs if a.startswith('!')]
                pos = [a for a in archs if not a.startswith('!')]
                sa = SPEC_ARCH[arch]
                aliases = {sa, 'win32' if arch == 'i686' else 'win64'}
                if neg and aliases & set(neg):
                    ok = False
                if pos and not aliases & set(pos):
                    ok = False
        if not ok or not rest:
            continue
        name = rest[0].split('(')[0]
        fwd = rest[-1] if '(' in rest[0] and ')' in ' '.join(rest) else None
        # the forward target is the token after the closing paren
        joined = ' '.join(rest)
        after = joined[joined.find(')') + 1:].strip() if ')' in joined else ''
        if not after or '.' not in after:
            continue
        dll, func = after.split('.', 1)
        yield name, dll.lower(), func


def oca_refs(dlls, arch):
    names = set()
    for p in glob.glob(os.path.join(SPECS, '*.spec')):
        for _, dll, func in spec_entries(p, arch):
            if dll in dlls:
                names.add(func)
    return names


def real_exports(dll, where=None):
    """Names the real Windows DLL exports; where = 'vista' or 'win7' to limit."""
    p = os.path.join(WINEXPORTS, dll + '.txt')
    out = set()
    if os.path.exists(p):
        for line in open(p):
            if line.startswith('#') or not line.strip():
                continue
            parts = line.split()
            name, on = parts[0], parts[1] if len(parts) > 1 else ''
            if not where or where in on:
                out.add(name)
    return out


def wine_exports(dll, arch):
    p = os.path.join(WINEDIR[arch], dll + '.dll')
    if not os.path.exists(p):
        return set()
    return {n for n in PE(p).exports() if not n.startswith(('wine_', '__wine', 'Wine'))}


def impl_symbols(objs, arch):
    """vx_ functions defined in the objects: {Name: linker symbol}."""
    out = {}
    nm = subprocess.run(['llvm-nm', '--defined-only', '-g'] + objs, capture_output=True, text=True, check=True).stdout
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) < 3 or parts[1] not in ('T', 't'):
            continue
        sym = parts[2]
        if arch == 'i686':
            m = re.match(r'^_vx_(\w+?)@(\d+)$', sym)
            if m:
                out[m.group(1)] = 'vx_%s@%s' % (m.group(1), m.group(2))
        else:
            m = re.match(r'^vx_(\w+)$', sym)
            if m:
                out[m.group(1)] = sym
    return out


def other_os_only(dll, os_):
    """Names the real DLL has only on the other Windows (Vista vs 7): the
    companion for this Windows must not forward them (they'd point at
    nothing). Names in no list (from Wine or One-Core-API) are kept."""
    if not os_:
        return set()
    return real_exports(dll) - real_exports(dll, os_)


# companion -> the Windows DLL it stands in for
BASE = {'kernelvx': 'kernel32', 'uservx': 'user32', 'advapivx': 'advapi32', 'shellvx': 'shell32',
        'bcryptprimvx': 'bcryptprimitives', 'ntvx': 'ntdll', 'ws2vx': 'ws2_32', 'cfgmgrvx': 'cfgmgr32',
        'olevx': 'ole32', 'versvx': 'version'}
# the oldest Windows VistaExt supports: a function it implements that this
# Windows has too is a deliberate wrapper (version reporting, GetProcAddress,
# CreateProcess...), kept on every build
OLDEST = 'vista'


def has_os_data(os_):
    """third_party/winexports lists this Windows (read from an installation)."""
    return bool(real_exports('kernel32', os_))


def native_newer(comp, os_):
    """{name: 'dll.name'}: what this build's Windows has but the oldest lacks.
    The companion forwards these to Windows instead of exporting its own
    (which then is not linked in at all: --gc-sections)."""
    out = {}
    if not os_ or os_ == OLDEST or comp not in BASE:
        return out
    dlls = [BASE[comp]] + (['kernelbase'] if comp == 'kernelvx' else [])
    for dll in dlls:
        for n in real_exports(dll, os_) - real_exports(dll, OLDEST):
            out.setdefault(n, '%s.%s' % (dll, n))
    return out


def forwards_for(comp, arch, outdir, os_=None):
    """Return {name: 'dll.func'} forwarders for a component."""
    f = forwards_all(comp, arch, outdir)
    base = BASE.get(comp)
    if base:
        for n in other_os_only(base, os_):
            if f.get(n, '').split('.', 1)[0] == base:
                del f[n]
    return f


def forwards_all(comp, arch, outdir):
    f = {}
    simple = {'ntvx': 'ntdll', 'ws2vx': 'ws2_32', 'cfgmgrvx': 'cfgmgr32', 'olevx': 'ole32', 'versvx': 'version'}
    if comp in simple:
        dll = simple[comp]
        for n in wine_exports(dll, arch) | real_exports(dll):
            f[n] = '%s.%s' % (dll, n)
    elif comp == 'kernelvx':
        for n in wine_exports('kernel32', arch) | oca_refs({'kernel32', 'kernelex', 'kernelbase'}, arch) | real_exports('kernel32'):
            f[n] = 'kernel32.' + n
        for n in list(f):
            if n.startswith('K32'):
                f[n] = 'psapi.' + n[3:]
    elif comp == 'uservx':
        for n in wine_exports('user32', arch) | oca_refs({'user32'}, arch) | real_exports('user32'):
            f[n] = 'user32.' + n
    elif comp == 'advapivx':
        for n in wine_exports('advapi32', arch) | oca_refs({'advapi32', 'advapinew'}, arch) | real_exports('advapi32'):
            f[n] = 'advapi32.' + n
    elif comp == 'shellvx':
        for n in wine_exports('shell32', arch) | oca_refs({'shell32'}, arch) | real_exports('shell32'):
            f[n] = 'shell32.' + n
    elif comp == 'bcryptprimvx':
        for n in wine_exports('bcryptprimitives', arch) | real_exports('bcryptprimitives'):
            f[n] = 'bcryptprimitives.' + n
    elif comp == 'kernelbase':
        for n in PE(os.path.join(outdir, 'kernelvx.dll')).exports():
            f[n] = 'kernelvx.' + n
    elif comp == 'combase':
        ole = wine_exports('ole32', arch)
        for n in (wine_exports('combase', arch) | oca_refs({'combase'}, arch)) & ole:
            f[n] = 'ole32.' + n
    elif comp == 'shcore':
        shl = wine_exports('shlwapi', arch)
        for n in (wine_exports('shcore', arch) | oca_refs({'shcore'}, arch)) & shl:
            f[n] = 'shlwapi.' + n
    return f


def fixed_ordinals(dll):
    """{name: ordinal} a companion must keep (programs import these by number)."""
    p = os.path.join(WINEXPORTS, dll + '.ord')
    out = {}
    if os.path.exists(p):
        for line in open(p):
            if line.strip() and not line.startswith('#'):
                o, n = line.split()
                out[n] = int(o)
    return out


# companion -> the DLL whose export numbers it keeps (kernelvx stands in for
# both kernel32 and kernelbase, whose numbers differ; ws2vx has its own list)
ORDINAL_DLL = {'uservx': 'user32', 'shellvx': 'shell32', 'advapivx': 'advapi32', 'olevx': 'ole32',
               'versvx': 'version', 'cfgmgrvx': 'cfgmgr32', 'ntvx': 'ntdll', 'bcryptprimvx': 'bcryptprimitives'}


def system_ordinals(dll, arch, os_=None):
    """{ordinal: name, or None when the name differs between the Windows
    versions looked at or the export has no name} for one architecture and
    (os_ = 'vista' / 'win7') one Windows, from <dll>.ords."""
    p = os.path.join(WINEXPORTS, dll + '.ords')
    bits = '64' if arch == 'x86_64' else '32'
    out = {}
    if not os.path.exists(p):
        return out
    for line in open(p):
        if not line.strip() or line.startswith('#'):
            continue
        o, *where = line.split()
        names = {w.split(':', 1)[1] for w in where
                 if w.split(':', 1)[0].endswith('-' + bits) and (not os_ or w.startswith(os_ + '-'))}
        if names:
            out[int(o)] = names.pop() if len(names) == 1 and '-' not in names else None
    return out


def write_def(path, libname, impl, fwd, ords=None, by_number=None):
    """by_number: {ordinal: 'dll.#ordinal'} exported without a name."""
    ords = ords or {}
    with open(path, 'w') as o:
        o.write('LIBRARY %s\nEXPORTS\n' % libname)
        for n in sorted(impl):
            o.write('  %s=%s%s\n' % (n, impl[n], ' @%d' % ords[n] if n in ords else ''))
        for n in sorted(fwd):
            if n not in impl:
                o.write('  %s = %s%s\n' % (n, fwd[n], ' @%d' % ords[n] if n in ords else ''))
        for num in sorted(by_number or {}):
            o.write('  __vxord_%d = %s @%d NONAME\n' % (num, by_number[num], num))


def keep_ordinals(comp, arch, impl, fwd, os_=None):
    """Every ordinal of the original DLL: a name that is the same on Vista
    and 7 and that the companion only forwards keeps its number; any other
    number is forwarded by number, so it means what it means on the running
    Windows. Returns (ords, by_number)."""
    dll = ORDINAL_DLL.get(comp)
    if not dll:
        return {}, {}
    ords, by_number = {}, {}
    for num, name in system_ordinals(dll, arch, os_).items():
        if name and name in fwd and name not in impl and fwd[name] == '%s.%s' % (dll, name):
            ords[name] = num
        else:
            by_number[num] = '%s.#%d' % (dll, num)
    return ords, by_number


def cmd_component(a):
    impl = impl_symbols(a.objs, a.arch) if a.objs else {}
    fwd = forwards_for(a.comp, a.arch, a.outdir, a.os)
    newer = native_newer(a.comp, a.os)
    native = sorted(n for n in impl if n in newer)
    for n in native:                    # Windows' own one wins
        del impl[n]
        fwd[n] = newer[n]
    ords = fixed_ordinals('ws2_32') if a.comp == 'ws2vx' else {}
    for n in ords:
        fwd.setdefault(n, 'ws2_32.' + n)
    by_number = {}
    if a.comp in ORDINAL_DLL:
        ords, by_number = keep_ordinals(a.comp, a.arch, impl, fwd, a.os)
    write_def(a.out, a.comp + '.dll', impl, fwd, ords, by_number)
    print('%s-%s/%s: %d implemented, %d forwarded (%d of them Windows\' own instead of VistaExt\'s), '
          '%d export numbers kept' % (a.os, a.arch, a.comp, len(impl), len(set(fwd) - set(impl)), len(native),
                                      len(ords) + len(by_number)))


def kernel_host(func, kvx, os_='vista'):
    """Where an api-set function that One-Core-API puts in kernel32/kernelbase
    really is on Vista: kernelvx implements it or kernel32 has it, else
    another system DLL (OpenProcessToken is advapi32's on Vista)."""
    if kvx.get(func, 'x') is None or func in real_exports('kernel32', os_):
        return 'kernelvx'
    for dll, comp in (('advapi32', 'advapivx'), ('user32', 'uservx'), ('ntdll', 'ntvx'), ('ole32', 'olevx'),
                      ('shlwapi', 'shlwapi'), ('ws2_32', 'ws2vx')):
        if func in real_exports(dll, os_):
            return comp
    return 'kernelvx'


def cmd_apisets(a):
    os.makedirs(a.outdir, exist_ok=True)
    made = 0
    outbin = a.bindir
    kvx = PE(os.path.join(outbin, 'kernelvx.dll')).exports()
    sets = {}
    for p in sorted(glob.glob(os.path.join(SPECS, '*.spec'))):
        name = os.path.basename(p)[:-5].lower()
        if name.startswith('api-ms-win-crt-'):
            continue            # shipped by Microsoft's Universal CRT update (KB2999226)
        fwd = {}
        for exp, dll, func in spec_entries(p, a.arch):
            if dll in SKIP_TARGETS:
                continue
            target = TARGET_MAP.get(dll, dll)
            if target == 'kernelvx':
                target = kernel_host(func, kvx, a.os)
            elif target not in COMPANIONS and kvx.get(func, 'x') is None:
                target = 'kernelvx'     # a newer function of another system DLL (powrprof) that kernelvx adds
            fwd[exp] = '%s.%s' % (target, func)
        for exp, dll in APISET_EXTRA.get(name, {}).items():
            fwd.setdefault(exp, '%s.%s' % (dll, exp))
        if not fwd:
            continue
        sets[name] = fwd
    for name, (base, extra) in APISET_NEW.items():
        fwd = dict(sets.get(base, {}))
        for exp, dll in extra.items():
            fwd[exp] = '%s.%s' % (dll, exp)
        sets[name] = fwd
    for name, fwd in sorted(sets.items()):
        write_def(os.path.join(a.outdir, name + '.def'), name + '.dll', {}, fwd)
        made += 1
    print('%s-%s: %d api-set .def files' % (a.os, a.arch, made))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('component')
    c.add_argument('--arch', required=True)
    c.add_argument('--comp', required=True)
    c.add_argument('--out', required=True)
    c.add_argument('--outdir', required=True)
    c.add_argument('--os', required=True, choices=('vista', 'win7', 'win8'))
    c.add_argument('objs', nargs='*')
    s = sub.add_parser('apisets')
    s.add_argument('--arch', required=True)
    s.add_argument('--outdir', required=True)
    s.add_argument('--os', required=True, choices=('vista', 'win7', 'win8'))
    s.add_argument('--bindir', required=True, help='this build\'s folder with kernelvx.dll')
    a = ap.parse_args()
    {'component': cmd_component, 'apisets': cmd_apisets}[a.cmd](a)
