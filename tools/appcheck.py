"""What would a program still lack on Vista/7 with VistaExt installed?

    python3 tools/appcheck.py <system dir> <program folder> [--files]

<system dir> is a copy of a Vista/7 System32 (or SysWOW64) with VistaExt's
DLLs added the way install.cmd adds them. Follows the same rules as vxpatch
and the Windows loader: forwarders are followed, kernel32 & co. imports are
switched to the companions, API-set imports may be pointed at a companion,
and a program's own DLLs are used before System32's (except Known DLLs).
Prints the missing functions per DLL with the files that need them. Imports
that are only loaded on demand (delay-load) are listed separately: they only
fail when that code runs.
"""
import os
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(__file__))
from peexp import PE

COMPANION = {'kernel32.dll': 'kernelvx.dll', 'kernelbase.dll': 'kernelvx.dll', 'user32.dll': 'uservx.dll',
             'advapi32.dll': 'advapivx.dll', 'shell32.dll': 'shellvx.dll',
             'bcryptprimitives.dll': 'bcryptprimvx.dll', 'ntdll.dll': 'ntvx.dll', 'ws2_32.dll': 'ws2vx.dll',
             'cfgmgr32.dll': 'cfgmgrvx.dll', 'ole32.dll': 'olevx.dll', 'version.dll': 'versvx.dll',
             'dxgi.dll': 'dxvx.dll', 'dnsapi.dll': 'dnsvx.dll', 'winhttp.dll': 'winhtvx.dll', 'userenv.dll': 'uenvvx.dll'}
APISET_HOSTS = ['kernelvx.dll', 'advapivx.dll', 'uservx.dll', 'shellvx.dll', 'combase.dll', 'shcore.dll',
                'olevx.dll', 'ntvx.dll', 'ws2vx.dll', 'cfgmgrvx.dll']
KNOWN = {'kernel32.dll', 'user32.dll', 'advapi32.dll', 'gdi32.dll', 'ole32.dll', 'oleaut32.dll', 'shell32.dll',
         'shlwapi.dll', 'comdlg32.dll', 'msvcrt.dll', 'ws2_32.dll', 'rpcrt4.dll', 'sechost.dll', 'normaliz.dll',
         'nsi.dll', 'psapi.dll', 'setupapi.dll', 'wldap32.dll', 'imagehlp.dll', 'difxapi.dll', 'clbcatq.dll',
         'lpk.dll', 'usp10.dll', 'imm32.dll', 'msctf.dll', 'ntdll.dll', 'kernelbase.dll'}


def delay_imports(pe):
    rva, _ = pe.dirs[13]
    out = []
    if not rva:
        return out
    try:
        o = pe.off(rva)
    except ValueError:
        return out
    tsz, fmt, flag = (8, '<Q', 1 << 63) if pe.is64 else (4, '<I', 1 << 31)
    while True:
        attrs, name, hmod, iat, intab = struct.unpack_from('<IIIII', pe.d, o)
        if not name:
            break
        if not attrs & 1:          # old VA format
            return out
        names = []
        to = pe.off(intab)
        while True:
            v = struct.unpack_from(fmt, pe.d, to)[0]
            if not v:
                break
            names.append('#%d' % (v & 0xFFFF) if v & flag else pe.cstr((v & 0x7FFFFFFF) + 2))
            to += tsz
        out.append((pe.cstr(name), names))
        o += 32
    return out


class System:
    def __init__(self, sysdir):
        self.dir = sysdir
        self.files = {f.lower(): os.path.join(sysdir, f) for f in os.listdir(sysdir)}
        self.cache = {}

    def exports(self, path):
        if path not in self.cache:
            try:
                self.cache[path] = {k: v for k, v in PE(path).exports().items()}
            except Exception:
                self.cache[path] = None
        return self.cache[path]

    def path(self, dll, appfiles):
        d = dll.lower()
        if not d.endswith('.dll') and '.' not in d:
            d += '.dll'
        sysp = self.files.get(d)
        # API sets Windows itself has (not VistaExt's files) win over the program's copy
        if d.startswith(('api-', 'ext-')) and sysp and b'VistaExt-owned-file' not in open(sysp, 'rb').read():
            return sysp
        if d not in KNOWN and d in appfiles:
            return appfiles[d]
        return sysp or appfiles.get(d)

    def resolves(self, dll, name, appfiles, depth=0):
        if depth > 6:
            return True
        p = self.path(dll, appfiles)
        if not p:
            return depth > 0 and dll.lower().startswith(('api-', 'ext-'))
        if name.startswith('#'):
            return True
        ex = self.exports(p)
        if not ex or name not in ex:
            return False
        f = ex[name]
        if not f:
            return True
        tdll, _, tname = f.rpartition('.')
        return self.resolves(tdll, tname, appfiles, depth + 1)


def check(sysdir, folder):
    sysm = System(sysdir)
    missing = defaultdict(lambda: defaultdict(set))     # (dll, delay) -> name -> files
    nodll = defaultdict(set)
    files = 0
    for root, _, fs in os.walk(folder):
        appfiles = {f.lower(): os.path.join(root, f) for f in fs}
        for f in fs:
            if not f.lower().endswith(('.exe', '.dll')):
                continue
            path = os.path.join(root, f)
            try:
                pe = PE(path)
                imps = [(d, n, False) for d, n in pe.imports()] + [(d, n, True) for d, n in delay_imports(pe)]
            except Exception:
                continue
            files += 1
            rel = os.path.relpath(path, folder)
            for dll, names, delay in imps:
                d = dll.lower()
                target = COMPANION.get(d)
                if target and target in sysm.files:
                    check_dll = target
                else:
                    check_dll = d
                if not sysm.path(check_dll, appfiles):
                    nodll[(d, delay)].add(rel)
                    continue
                bad = [n for n in names if not sysm.resolves(check_dll, n, appfiles)]
                if bad and d.startswith(('api-', 'ext-')):
                    for host in APISET_HOSTS:
                        if host in sysm.files and all(sysm.resolves(host, n, appfiles) for n in names):
                            bad = []
                            break
                for n in bad:
                    missing[(d, delay)][n].add(rel)
    return files, missing, nodll


def main():
    sysdir, folder = sys.argv[1], sys.argv[2]
    files, missing, nodll = check(sysdir, folder)
    print('%d program files checked in %s' % (files, folder))
    for delay in (False, True):
        print('\n== %s' % ('loaded on demand (only fails when used)' if delay else 'needed at start-up'))
        for (d, dl), users in sorted(nodll.items()):
            if dl == delay:
                print('  DLL not on this system: %s  (%s)' % (d, ', '.join(sorted(users)[:4]) + (' ...' if len(users) > 4 else '')))
        for (d, dl), names in sorted(missing.items()):
            if dl != delay:
                continue
            print('  %s: %d missing' % (d, len(names)))
            for n, users in sorted(names.items()):
                if '--files' in sys.argv:
                    print('      %-45s %s' % (n, ', '.join(sorted(users)[:3]) + (' ...' if len(users) > 3 else '')))
                else:
                    print('      %s' % n)


if __name__ == '__main__':
    main()
