"""What do real programs import that a plain Windows Vista, 7 or 8 lacks?

    python3 tools/gapscan.py [--sys vista=vista64-deps,win7=win7x64-deps,win8=win8x64] \
        <out.json> <app name>=<folder or file> ...

Uses copies of the real system folders in out/apps/sys/<os>x64/Windows
(System32 = 64-bit, SysWOW64 = 32-bit) and, for 7 and 8, their API-set
schema (tools/apiset.py). Each import of each program file is resolved the
way the loader does it:
  - api-ms-* / ext-ms-*: through the schema (Vista has none); a name the
    schema lacks is searched as a file (the program's own copy counts);
  - other DLLs: the program's own folder, then the system folder;
  - functions: the DLL's exports, following forwarders (also through the
    schema).
Missing items are recorded per Windows and architecture, split into imports
the program needs to start at all ("static") and those loaded when used
("delay"). The result also says whether the VistaExt build for that Windows
(out/<os>-<arch>: companions and API-set copies) already supplies the item.
"""
import json
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(__file__))
from peexp import PE
import apiset

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYS = os.path.join(ROOT, 'out', 'apps', 'sys')
OSES = [('vista', 'vista64'), ('win7', 'win7x64'), ('win8', 'win8x64')]
# the C/C++ runtime: Microsoft's own redistributable and update (KB2999226)
# supply these on every one of these Windows; listed apart, not as gaps
RUNTIME = ('api-ms-win-crt-', 'ucrtbase.dll', 'vcruntime140', 'msvcp140', 'concrt140', 'vccorlib140')

# what VistaExt's companions stand in for (the program's import is switched)
COMPANION = {'kernel32.dll': 'kernelvx.dll', 'kernelbase.dll': 'kernelvx.dll', 'user32.dll': 'uservx.dll',
             'advapi32.dll': 'advapivx.dll', 'shell32.dll': 'shellvx.dll',
             'bcryptprimitives.dll': 'bcryptprimvx.dll', 'ntdll.dll': 'ntvx.dll', 'ws2_32.dll': 'ws2vx.dll',
             'cfgmgr32.dll': 'cfgmgrvx.dll', 'ole32.dll': 'olevx.dll', 'version.dll': 'versvx.dll'}


def delay_imports(pe):
    rva, _ = pe.dirs[13]
    out = []
    if not rva:
        return out
    import struct
    try:
        o = pe.off(rva)
        tsz, fmt, flag = (8, '<Q', 1 << 63) if pe.is64 else (4, '<I', 1 << 31)
        while True:
            attrs, name, _, _, iat, intr, _, _ = struct.unpack_from('<8I', pe.d, o)
            if not name:
                break
            if not attrs & 1:          # old VA form: skip
                o += 32
                continue
            names = []
            to = pe.off(intr)
            while True:
                v = struct.unpack_from(fmt, pe.d, to)[0]
                if not v:
                    break
                names.append('#%d' % (v & 0xFFFF) if v & flag else pe.cstr((v & 0x7FFFFFFF) + 2))
                to += tsz
            out.append((pe.cstr(name), names))
            o += 32
    except (ValueError, Exception):
        pass
    return out


class OSView:
    """one Windows, one architecture"""

    def __init__(self, os_, sysname, bits):
        base = os.path.join(SYS, sysname, 'Windows')
        self.dir = os.path.join(base, 'System32' if bits == 64 else 'SysWOW64')
        self.files = {f.lower(): os.path.join(self.dir, f) for f in os.listdir(self.dir)}
        sch = os.path.join(base, 'System32', 'apisetschema.dll')
        self.schema = apiset.schema(sch) if os_ != 'vista' and os.path.exists(sch) else {}
        self.exp = {}

    def exports(self, dll):
        dll = dll.lower()
        if dll not in self.exp:
            p = self.files.get(dll)
            try:
                self.exp[dll] = PE(p).exports() if p else None
            except Exception:
                self.exp[dll] = None
        return self.exp[dll]

    def host(self, dll, local):
        """the file an import name loads on this Windows, or None"""
        d = dll.lower()
        if not d.endswith('.dll') and '.' not in d:
            d += '.dll'
        if d.startswith(('api-', 'ext-')):
            h = apiset.lookup(self.schema, d)
            if h:
                return ('sys', h)
            if h == '' and d.startswith('ext-'):
                return None                      # an ext- set Windows knows but has no host for
        if d in local:
            return ('local', d)
        if d in self.files:
            return ('sys', d)
        return None

    def has(self, dll, func, local, depth=0):
        """does the function resolve (following forwarders)?"""
        h = self.host(dll, local)
        if not h:
            return False
        if h[0] == 'local':
            return True                          # the program's own DLL: its own business
        if func.startswith('#'):
            return True                          # by number: not checked
        ex = self.exports(h[1])
        if ex is None or func not in ex:
            return False
        fwd = ex[func]
        if fwd and depth < 6 and '.' in fwd:
            fd, ff = fwd.rsplit('.', 1)
            return self.has(fd + '.dll', ff, local, depth + 1)
        return True


def vistaext_supplies(os_, bits, dll, func):
    """does VistaExt's build for this Windows supply it (companion or API-set copy)?"""
    arch = 'x64' if bits == 64 else 'x86'
    b = os.path.join(ROOT, 'out', '%s-%s' % (os_, arch))
    if not os.path.isdir(b):
        return False
    d = dll.lower()
    cands = []
    if d in COMPANION:
        cands.append(os.path.join(b, COMPANION[d]))
    cands += [os.path.join(b, d), os.path.join(b, 'apisets', d)]
    for c in cands:
        if os.path.exists(c):
            try:
                ex = PE(c).exports()
            except Exception:
                continue
            if func is None or func in ex:
                return True
    return False


def scan(apps, views):
    items = defaultdict(lambda: {'apps': set(), 'files': set(), 'static': False})
    for app, root in apps:
        paths = []
        if os.path.isfile(root):
            paths = [root]
        else:
            for dp, _, fs in os.walk(root):
                paths += [os.path.join(dp, f) for f in fs if f.lower().endswith(('.exe', '.dll'))]
        # the program's own DLLs, wherever they are in its folders (a
        # launcher and its DLLs can sit in different subfolders)
        own = {os.path.basename(p).lower() for p in paths}
        for p in paths:
            try:
                pe = PE(p)
            except Exception:
                continue
            if pe.machine not in (0x14C, 0x8664):
                continue
            bits = 64 if pe.is64 else 32
            local = own | {f.lower() for f in os.listdir(os.path.dirname(p))}
            stat = pe.imports()
            if any(d.lower() == 'vertdll.dll' for d, _ in stat):
                continue                    # an enclave image: never loaded in a normal process
            for kind, imps in (('static', stat), ('delay', delay_imports(pe))):
                for dll, names in imps:
                    for os_, v in views[bits].items():
                        h = v.host(dll, local)
                        if not h:
                            key = (os_, bits, dll.lower(), None)
                            it = items[key]
                            it['apps'].add(app); it['files'].add(os.path.relpath(p, root) if p != root else os.path.basename(p))
                            it['static'] |= kind == 'static'
                            continue
                        for n in names:
                            if not v.has(dll, n, local):
                                key = (os_, bits, dll.lower(), n)
                                it = items[key]
                                it['apps'].add(app); it['files'].add(os.path.relpath(p, root) if p != root else os.path.basename(p))
                                it['static'] |= kind == 'static'
    out = []
    for (os_, bits, dll, fn), it in items.items():
        out.append({'os': os_, 'bits': bits, 'dll': dll, 'func': fn, 'apps': sorted(it['apps']),
                    'runtime': dll.startswith(RUNTIME),
                    'files': sorted(it['files'])[:8], 'static': it['static'],
                    'vistaext': vistaext_supplies(os_, bits, dll, fn)})
    return out


if __name__ == '__main__':
    args = sys.argv[1:]
    if args and args[0] == '--sys':
        OSES = [tuple(x.split('=', 1)) for x in args[1].split(',')]
        args = args[2:]
    dest = args[0]
    apps = [a.split('=', 1) for a in args[1:]]
    views = {bits: {os_: OSView(os_, s, bits) for os_, s in OSES} for bits in (32, 64)}
    res = scan(apps, views)
    json.dump(res, open(dest, 'w'), indent=1)
    print('%d missing items written to %s' % (len(res), dest))
