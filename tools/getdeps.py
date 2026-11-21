#!/usr/bin/env python3
"""Download what the test VMs need besides Windows itself, from Microsoft only.

    python3 -I tools/getdeps.py [out/apps/dl/deps]

Writes <dir>/_files (each file once), and per system <dir>/<vista|win7>-<x86|x64>/:
  plan.txt      the install steps, in order, for tests/vm/deps.cmd:
                "id|kind|file|arguments|what" - kind: msu, cab (DISM), exe,
                fallback, dx, roots, tls, netfx3, restart ("-": none)
  MANIFEST.txt  every file: source URL, size, SHA-256
  the files themselves (hard links), roots\\ and disallowed\\ (certificates)
Catalog files are checked against the SHA-1 in their names.
"""
import concurrent.futures, hashlib, os, re, shutil, struct, subprocess, sys, urllib.request

sys.path.insert(0, os.path.dirname(__file__))
import mscatalog

OUT = sys.argv[1] if len(sys.argv) > 1 else 'out/apps/dl/deps'
FILES = os.path.join(OUT, '_files')
UA = {'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'}
CTL = 'http://ctldl.windowsupdate.com/msdownload/update/v3/static/trustedr/en/'

def catalog(query, pattern):
    """the first file of a catalog search whose name matches pattern"""
    for uid, title in mscatalog.search(query):
        for u in mscatalog.files(uid):
            if re.search(pattern, u.rsplit('/', 1)[-1], re.I):
                return u
    raise SystemExit('not found in the catalog: %s / %s' % (query, pattern))

# (id, systems, kind, source, arguments, description)
# source: ('cat', query, filename pattern with {a} = x86/x64) or ('url', url, file name)
W7, VI = ('win7',), ('vista',)
BOTH = W7 + VI
STEPS = [
    # --- Step 1: servicing, SHA-2, certificates, TLS
    ('kb4490628', W7, 'msu', ('cat', 'KB4490628', r'windows6\.1-kb4490628-{a}_'), '', 'Servicing stack update 2019-03 (needed by SHA-2 support)'),
    ('kb4474419', W7, 'msu', ('cat', 'KB4474419', r'windows6\.1-kb4474419-v3-{a}_'), '', 'SHA-2 code signing support (KB4474419 v3)'),
    ('restart1', W7, 'restart', None, '', 'restart for the servicing stack and SHA-2'),
    ('roots', BOTH, 'roots', None, '', "trusted and disallowed root certificates from Microsoft's current list"),
    ('kb3140245', W7, 'msu', ('cat', 'KB3140245', r'windows6\.1-kb3140245-{a}_'), '', 'TLS 1.1/1.2 as default for WinHTTP (KB3140245)'),
    ('tls', W7, 'tls', None, '', 'TLS 1.1/1.2 enabled: SChannel, WinHTTP, Internet Explorer, .NET'),
    # --- Step 3 (needed early: IE11 and others build on them)
    ('kb3063858', VI, 'msu', ('cat', 'KB3063858', r'windows6\.0-kb3063858-{a}_'), '', 'DLL loading: SetDefaultDllDirectories & co. (KB3063858, replaces KB2533623)'),
    ('kb2533623', W7, 'cab', ('cat', 'KB2841134', r'wu-windows6\.1-kb2533623-{a}_'), '', 'DLL loading: SetDefaultDllDirectories & co. (KB2533623)'),
    ('kb971513', VI, 'msu', ('cat', 'KB971513', r'windows6\.0-kb971513-{a}_'), '', 'Platform Update part: Windows Automation API (KB971513)'),
    ('kb971514', VI, 'msu', ('cat', 'KB971514', r'windows6\.0-kb971514-{a}_'), '', 'Platform Update part (KB971514)'),
    ('kb960362', VI, 'msu', ('cat', 'KB960362', r'windows6\.0-kb960362-{a}_'), '', 'Platform Update part (KB960362)'),
    ('kb2117917', VI, 'msu', ('cat', 'KB2117917', r'windows6\.0-kb2117917-{a}_'), '', 'Platform Update Supplement (KB2117917)'),
    ('kb2670838', W7, 'msu', ('cat', 'KB2670838', r'windows6\.1-kb2670838-{a}_'), '', 'Platform Update for Windows 7 (KB2670838)'),
    ('kb2729094', W7, 'msu', ('cat', 'KB2729094', r'windows6\.1-kb2729094-v2-{a}_'), '', 'Segoe UI Symbol font update (KB2729094)'),
    ('kb2834140', W7, 'msu', ('cat', 'KB2834140', r'windows6\.1-kb2834140-v2-{a}_'), '', 'fix for the Platform Update (KB2834140, IE11 prerequisite)'),
    ('kb2731771', W7, 'cab', ('cat', 'KB2841134', r'wu-windows6\.1-kb2731771-{a}_'), '', 'IE11 prerequisite (KB2731771)'),
    ('kb2639308', W7, 'cab', ('cat', 'KB2841134', r'wu-windows6\.1-kb2639308-{a}_'), '', 'IE11 prerequisite (KB2639308)'),
    ('kb2786081', W7, 'cab', ('cat', 'KB2841134', r'wu-windows6\.1-kb2786081-{a}_'), '', 'IE11 prerequisite (KB2786081)'),
    ('kb2882822', W7, 'cab', ('cat', 'KB2841134', r'wu-windows6\.1-kb2882822-{a}_'), '', 'IE11 prerequisite (KB2882822)'),
    ('kb2888049', W7, 'msu', ('cat', 'KB2888049', r'windows6\.1-kb2888049-{a}_'), '', 'IE11 prerequisite (KB2888049)'),
    ('kb4019990', W7, 'msu', ('cat', 'KB4019990', r'windows6\.1-kb4019990-{a}_'), '', 'D3DCompiler_47 (KB4019990)'),
    ('restart2', BOTH, 'restart', None, '', 'restart for the platform updates'),
    ('ie11', W7, 'exe', ('cat', 'KB2841134', r'^ie11-windows6\.1-{a}-en-us_'), '/quiet /update-no /norestart', 'Internet Explorer 11 (English)'),
    ('restart3', W7, 'restart', None, '', 'restart for Internet Explorer 11'),
    # --- Step 2: .NET
    ('netfx3', W7, 'netfx3', None, '', '.NET Framework 3.5.1 (a Windows 7 feature): make sure it is on'),
    ('dotnetfx35', VI, 'exe', ('url', 'https://download.microsoft.com/download/2/0/E/20E90413-712F-438C-988E-FDAA79A8AC3D/dotnetfx35.exe', 'dotnetfx35.exe'), '/q /norestart', '.NET Framework 3.5 SP1 (includes 2.0 SP2 and 3.0 SP2)'),
    ('ndp48', W7, 'exe', ('url', 'https://go.microsoft.com/fwlink/?linkid=2088631', 'ndp48-x86-x64-allos-enu.exe'), '/q /norestart', '.NET Framework 4.8'),
    ('ndp46', VI, 'exe', ('url', 'https://download.microsoft.com/download/6/F/9/6F9673B1-87D1-46C4-BF04-95F24C3EB9DA/enu_netfx/NDP46-KB3045557-x86-x64-AllOS-ENU_exe/NDP46-KB3045557-x86-x64-AllOS-ENU.exe', 'NDP46-KB3045557-x86-x64-AllOS-ENU.exe'), '/q /norestart', '.NET Framework 4.6'),
    ('restart4', BOTH, 'restart', None, '', 'restart for .NET'),
    ('kb4532945', W7, 'msu', ('cat', 'KB4535102', r'windows6\.1-kb4532945-{a}_'), '', '.NET Framework 3.5.1 rollup 2020-01 (KB4532945): TLS 1.2 for .NET 2.0/3.5 programs'),
    # --- Step 3: runtimes
    ('vc2005-x86', BOTH, 'exe', ('url', 'https://download.microsoft.com/download/8/B/4/8B42259F-5D70-43F4-AC2E-4B208FD8D66A/vcredist_x86.EXE', 'vc2005_x86.exe'), '/q', 'Visual C++ 2005 SP1 (8.0.61001, MFC security update) x86'),
    ('vc2005-x64', BOTH, 'exe64', ('url', 'https://download.microsoft.com/download/8/B/4/8B42259F-5D70-43F4-AC2E-4B208FD8D66A/vcredist_x64.EXE', 'vc2005_x64.exe'), '/q', 'Visual C++ 2005 SP1 (8.0.61001) x64'),
    ('vc2008-x86', BOTH, 'exe', ('url', 'https://download.microsoft.com/download/5/D/8/5D8C65CB-C849-4025-8E95-C3966CAFD8AE/vcredist_x86.exe', 'vc2008_x86.exe'), '/q', 'Visual C++ 2008 SP1 (9.0.30729.6161) x86'),
    ('vc2008-x64', BOTH, 'exe64', ('url', 'https://download.microsoft.com/download/5/D/8/5D8C65CB-C849-4025-8E95-C3966CAFD8AE/vcredist_x64.exe', 'vc2008_x64.exe'), '/q', 'Visual C++ 2008 SP1 (9.0.30729.6161) x64'),
    ('vc2010-x86', BOTH, 'exe', ('url', 'https://download.microsoft.com/download/1/6/5/165255E7-1014-4D0A-B094-B6A430A6BFFC/vcredist_x86.exe', 'vc2010_x86.exe'), '/q /norestart', 'Visual C++ 2010 SP1 (10.0.40219.325) x86'),
    ('vc2010-x64', BOTH, 'exe64', ('url', 'https://download.microsoft.com/download/1/6/5/165255E7-1014-4D0A-B094-B6A430A6BFFC/vcredist_x64.exe', 'vc2010_x64.exe'), '/q /norestart', 'Visual C++ 2010 SP1 (10.0.40219.325) x64'),
    ('vc2012-x86', BOTH, 'exe', ('url', 'https://download.microsoft.com/download/1/6/B/16B06F60-3B20-4FF2-B699-5E9B7962F9AE/VSU_4/vcredist_x86.exe', 'vc2012_x86.exe'), '/install /quiet /norestart', 'Visual C++ 2012 Update 4 (11.0.61030) x86'),
    ('vc2012-x64', BOTH, 'exe64', ('url', 'https://download.microsoft.com/download/1/6/B/16B06F60-3B20-4FF2-B699-5E9B7962F9AE/VSU_4/vcredist_x64.exe', 'vc2012_x64.exe'), '/install /quiet /norestart', 'Visual C++ 2012 Update 4 (11.0.61030) x64'),
    ('vc2013-x86', BOTH, 'exe', ('url', 'https://aka.ms/highdpimfc2013x86enu', 'vc2013_x86.exe'), '/install /quiet /norestart', 'Visual C++ 2013 (12.0.40664) x86'),
    ('vc2013-x64', BOTH, 'exe64', ('url', 'https://aka.ms/highdpimfc2013x64enu', 'vc2013_x64.exe'), '/install /quiet /norestart', 'Visual C++ 2013 (12.0.40664) x64'),
    # 2015-2022: the newest first; if it refuses this Windows, the newest of an older series
    ('vc14-x86', BOTH, 'exe', ('url', 'https://aka.ms/vs/17/release/vc_redist.x86.exe', 'vc2015-2022_x86.exe'), '/install /quiet /norestart', 'Visual C++ 2015-2022 (newest) x86'),
    ('vc14-x64', BOTH, 'exe64', ('url', 'https://aka.ms/vs/17/release/vc_redist.x64.exe', 'vc2015-2022_x64.exe'), '/install /quiet /norestart', 'Visual C++ 2015-2022 (newest) x64'),
    ('vc14b-x86', W7, 'fallback', ('url', 'https://aka.ms/vs/16/release/vc_redist.x86.exe', 'vc2015-2019_x86.exe'), '/install /quiet /norestart', 'Visual C++ 2015-2019 x86 (if the newest refuses)'),
    ('vc14b-x64', W7, 'fallback64', ('url', 'https://aka.ms/vs/16/release/vc_redist.x64.exe', 'vc2015-2019_x64.exe'), '/install /quiet /norestart', 'Visual C++ 2015-2019 x64 (if the newest refuses)'),
    ('vc14c-x86', VI, 'fallback', ('url', 'https://aka.ms/vs/15/release/vc_redist.x86.exe', 'vc2015-2017_x86.exe'), '/install /quiet /norestart', 'Visual C++ 2015-2017 x86 (if the newest refuses)'),
    ('vc14c-x64', VI, 'fallback64', ('url', 'https://aka.ms/vs/15/release/vc_redist.x64.exe', 'vc2015-2017_x64.exe'), '/install /quiet /norestart', 'Visual C++ 2015-2017 x64 (if the newest refuses)'),
    ('directx', BOTH, 'dx', ('url', 'https://download.microsoft.com/download/8/4/A/84A35BF1-DAFE-4AE8-82AF-AD2AE20B6B14/directx_Jun2010_redist.exe', 'directx_Jun2010_redist.exe'), '', 'DirectX End-User Runtimes (June 2010)'),
    ('restart5', BOTH, 'restart', None, '', 'final restart'),
]

def fetch(url, path):
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return
    tmp = path + '.part'
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=300) as r, open(tmp, 'wb') as f:
        shutil.copyfileobj(r, f, 1 << 20)
    m = re.search(r'_([0-9a-f]{40})\.(msu|exe)$', url)
    if m:
        h = hashlib.sha1(open(tmp, 'rb').read()).hexdigest()
        if h != m.group(1):
            os.remove(tmp)
            raise SystemExit('SHA-1 mismatch for %s' % url)
    os.rename(tmp, path)

def carve_cab(path):
    d = open(path, 'rb').read()
    i = d.find(b'MSCF')
    while i >= 0:
        size = struct.unpack_from('<I', d, i + 8)[0]
        if 0 < size <= len(d) - i and d[i + 4:i + 8] == b'\0\0\0\0':
            name = os.path.basename(path).replace('wu-', '').rsplit('_', 1)[0] + '.cab'
            out = os.path.join(FILES, name)
            open(out, 'wb').write(d[i:i + size])
            if subprocess.run(['cabextract', '-t', '-q', out]).returncode == 0:
                return out, name
        i = d.find(b'MSCF', i + 4)
    raise SystemExit('no update package inside ' + path)

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()

def roots(target):
    """Microsoft's current trusted and disallowed root lists, and their certificates"""
    for stl, sub in (('authrootstl.cab', 'roots'), ('disallowedcertstl.cab', 'disallowed')):
        d = os.path.join(FILES, sub)
        os.makedirs(d, exist_ok=True)
        cab = os.path.join(FILES, stl)
        fetch(CTL + stl, cab)
        subprocess.run(['cabextract', '-q', '-d', d, cab], check=True)
        stlfile = [f for f in os.listdir(d) if f.endswith('.stl')][0]
        der = open(os.path.join(d, stlfile), 'rb').read()
        # every 20-byte OCTET STRING is a candidate thumbprint; only real
        # certificates exist on Microsoft's server (others: 404)
        cands = sorted({der[i + 2:i + 22].hex().upper() for i in range(len(der) - 22) if der[i] == 0x04 and der[i + 1] == 0x14})
        def one(t):
            p = os.path.join(d, t + '.crt')
            if os.path.exists(p):
                return 1
            try:
                fetch(CTL + t + '.crt', p)
                return 1
            except Exception:
                return 0
        with concurrent.futures.ThreadPoolExecutor(8) as ex:
            n = sum(ex.map(one, cands))
        print('%s: %d certificates' % (sub, n))
        dst = os.path.join(target, sub)
        if os.path.exists(dst):
            shutil.rmtree(dst)
        shutil.copytree(d, dst, copy_function=os.link)

def main():
    os.makedirs(FILES, exist_ok=True)
    resolved = {}
    for osn in ('win7', 'vista'):
        for arch in ('x86', 'x64'):
            target = os.path.join(OUT, '%s-%s' % (osn, arch))
            os.makedirs(target, exist_ok=True)
            plan, manifest = [], []
            for sid, systems, kind, src, args, what in STEPS:
                if osn not in systems:
                    continue
                if kind in ('exe64', 'fallback64') and arch == 'x86':
                    continue
                name = ''
                if src:
                    if src[0] == 'cat':
                        key = (src[1], src[2].format(a=arch))
                        if key not in resolved:
                            resolved[key] = catalog(*key)
                        url = resolved[key]
                        name = url.rsplit('/', 1)[-1]
                    else:
                        url, name = src[1], src[2]
                    path = os.path.join(FILES, name)
                    print('  %s-%s %s' % (osn, arch, name), flush=True)
                    fetch(url, path)
                    if kind == 'cab':
                        # Microsoft's IE11 bundle wraps these updates in an
                        # installer (IELangPack.exe) that runs DISM; the update
                        # package (.cab) inside is installed with DISM directly
                        path, name = carve_cab(path)
                    if not os.path.exists(os.path.join(target, name)):
                        os.link(path, os.path.join(target, name))
                    manifest.append('%s  %d  %s  %s  %s' % (name, os.path.getsize(path), sha256(path), url, what))
                # "-" for an empty field: batch's for /f merges empty fields
                plan.append('|'.join((sid, kind.replace('64', ''), name or '-', args or '-', what)))
            if any(p.split('|')[1] == 'roots' for p in plan):
                roots(target)
            open(os.path.join(target, 'plan.txt'), 'w', newline='\r\n').write('\n'.join(plan) + '\n')
            open(os.path.join(target, 'MANIFEST.txt'), 'w', newline='\r\n').write('\n'.join(manifest) + '\n')
            print('%s-%s: %d steps' % (osn, arch, len(plan)))

main()
