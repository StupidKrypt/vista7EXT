/* vxpatch - command line front end for the VistaExt patch library. */
#include "patchlib.h"
#include "../common/console.h"

static void usage(void)
{
    con_write(
L"VistaExt vxpatch - make programs run on Windows Vista and Windows 7\r\n"
L"\r\n"
L"  vxpatch [options] <program.exe | folder> ...\r\n"
L"\r\n"
L"  /check          only report what is missing; change nothing\r\n"
L"  /undo           put back the original files (<file>.vxorig)\r\n"
L"  /undoall        put back every file VistaExt has patched on this PC\r\n"
L"  /winver:<v>     tell the program it runs on another Windows version:\r\n"
L"                  VISTA, WIN7, WIN8, WIN81, WIN10, WIN11, or OFF\r\n"
L"  /args:\"...\"     extra command line arguments to add when it starts\r\n"
L"  /webview2       use the WebView2 runtime set up by vx-webview2-setup\r\n"
L"  /force          load VistaExt even if nothing is missing\r\n"
L"  /memory         redirect the imports in memory while the program runs\r\n"
L"                  (its files are not changed; the default when vxload.dll\r\n"
L"                  is installed)\r\n"
L"  /file           rewrite the imports in the files instead\r\n"
L"  /auto           choose the settings itself (Windows 10 and the Chromium\r\n"
L"                  options for Chromium/Electron programs)\r\n"
L"  /wv2runtime:x64 register <folder> as the WebView2 runtime (x86 or x64)\r\n"
L"                  and patch it; used by vx-webview2-setup.cmd\r\n"
L"  /q              list only files that need or got changes\r\n"
L"\r\n"
L"Folders are processed with every .exe and .dll inside them. Settings for a\r\n"
L"folder apply to every program in it. Changing settings and patching files in\r\n"
L"Program Files needs an administrator command prompt.\r\n");
}

#define WV2_ARGS L"--no-sandbox --disable-gpu --disable-features=RendererCodeIntegrity"

static int wv2_register(const WCHAR *arch, const WCHAR *folder)
{
    HKEY k;
    LONG r;
    DWORD type, cb;
    WCHAR full[MAX_PATH], exe[MAX_PATH + 32];
    const WCHAR *value = !vx_wcsicmp(arch, L"x64") ? L"RuntimeFolder64" : !vx_wcsicmp(arch, L"x86") ? L"RuntimeFolder32" : NULL;
    if (!value) { con_print(L"Use /wv2runtime:x86 or /wv2runtime:x64\r\n"); return 2; }
    if (!GetFullPathNameW(folder, MAX_PATH, full, NULL)) return 2;
    vx_wcscpyn(exe, full, MAX_PATH);
    vx_wcscatn(exe, L"\\msedgewebview2.exe", MAX_PATH + 32);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        con_print(L"%s does not contain msedgewebview2.exe. Point to the extracted Fixed Version runtime folder.\r\n", full);
        return 2;
    }
    if ((r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, VX_REG_WEBVIEW2, 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE | VX_REG_VIEW, NULL, &k, NULL))) {
        con_print(L"Could not write the settings (error %d). Run as administrator.\r\n", (int)r);
        return 2;
    }
    RegSetValueExW(k, value, 0, REG_SZ, (const BYTE *)full, (DWORD)((vx_wcslen(full) + 1) * sizeof(WCHAR)));
    if (RegQueryValueExW(k, L"BrowserArguments", NULL, &type, NULL, &cb))
        RegSetValueExW(k, L"BrowserArguments", 0, REG_SZ, (const BYTE *)WV2_ARGS, sizeof(WV2_ARGS));
    RegCloseKey(k);
    con_print(L"WebView2 runtime (%s) registered: %s\r\n", arch, full);
    return 0;
}

int wmain(int argc, WCHAR **argv)
{
    DWORD flags = 0;
    BOOL undo = FALSE, set = FALSE, webview2 = FALSE, any = FALSE, autoset = FALSE, mode_chosen = FALSE;
    const vx_osver *ver = NULL;
    const WCHAR *args = L"";
    vxp_stats st = { 0 };
    int i;

    for (i = 1; i < argc; i++) {
        WCHAR *a = argv[i];
        if (a[0] != '/' && a[0] != '-') continue;
        a++;
        if (!vx_wcsicmp(a, L"undoall")) {
            int failed, n = vxp_restore_all(con_log, NULL, &failed);
            con_print(L"%d patched file(s) restored.\r\n", n);
            if (failed) {
                con_print(L"%d file(s) could not be restored. Close those programs and run this again.\r\n", failed);
                return 3;
            }
            return 0;
        }
        if (!vx_wcsicmp(a, L"check")) flags |= VXP_DRYRUN;
        else if (!vx_wcsicmp(a, L"memory")) { flags |= VXP_MEMORY; mode_chosen = TRUE; }
        else if (!vx_wcsicmp(a, L"file")) { flags &= ~VXP_MEMORY; mode_chosen = TRUE; }
        else if (!vx_wcsicmp(a, L"undo")) undo = TRUE;
        else if (!vx_wcsicmp(a, L"force")) flags |= VXP_FORCE;
        else if (!vx_wcsicmp(a, L"auto")) autoset = TRUE;
        else if (!vx_wcsicmp(a, L"q")) flags |= VXP_QUIET;
        else if (!vx_wcsicmp(a, L"webview2")) { webview2 = set = TRUE; }
        else if (!vx_wcsnicmp(a, L"winver:", 7)) {
            set = TRUE;
            if (vx_wcsicmp(a + 7, L"off") && !(ver = vx_find_osver(a + 7))) {
                con_print(L"Unknown Windows version '%s'.\r\n", a + 7);
                return 2;
            }
        } else if (!vx_wcsnicmp(a, L"args:", 5)) { args = a + 5; set = TRUE; }
        else if (!vx_wcsnicmp(a, L"wv2runtime:", 11)) {
            int j, r;
            for (j = i + 1; j < argc && (argv[j][0] == '/' || argv[j][0] == '-'); j++) ;
            if (j >= argc) { usage(); return 2; }
            if ((r = wv2_register(a + 11, argv[j]))) return r;
        }
        else if (!vx_wcsicmp(a, L"?") || !vx_wcsicmp(a, L"help")) { usage(); return 0; }
        else { con_print(L"Unknown option /%s\r\n", a); return 2; }
    }
    if (!mode_chosen) flags |= vxp_default_mode();     /* per program below: vxp_mode_for */
    if ((flags & VXP_MEMORY) && !vxp_memory_available()) {
        con_print(L"vxload.dll is not installed: /memory needs VistaExt installed (install.cmd).\r\n");
        return 2;
    }
    if (set && (ver || webview2 || *args)) flags |= VXP_FORCE;
    if (ver) flags |= VXP_LAYER;

    for (i = 1; i < argc; i++) {
        WCHAR full[MAX_PATH];
        DWORD attr;
        if (argv[i][0] == '/' || argv[i][0] == '-') continue;
        any = TRUE;
        if (!GetFullPathNameW(argv[i], MAX_PATH, full, NULL)) { con_print(L"%s: bad path\r\n", argv[i]); return 2; }
        attr = GetFileAttributesW(full);
        if (attr == INVALID_FILE_ATTRIBUTES) { con_print(L"%s: not found\r\n", full); st.failed++; continue; }
        if (autoset && !undo) {
            /* the recommended settings for this program, as if given */
            static WCHAR rec[600];
            static vx_layer l;              /* "args" points into it after this block */
            vxp_recommend(full, rec, 600);
            vx_parse_layer(rec, &l);
            ver = l.ver;
            args = l.args[0] ? l.args : L"";
            set = TRUE;
            flags &= ~(VXP_FORCE | VXP_LAYER);
            if (ver || *args) flags |= VXP_FORCE;
            if (ver) flags |= VXP_LAYER;
            con_print(L"%s: %s\r\n", full, rec[0] ? rec : L"no special settings needed");
        }
        if (!mode_chosen) flags = (flags & ~VXP_MEMORY) | vxp_mode_for(full);
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            SIZE_T n = vx_wcslen(full);
            if (n && full[n - 1] == '\\') full[--n] = 0;
            if (undo) {
                int failed, n = vxp_restore_dir(full, con_log, NULL, &failed);
                con_print(L"%d file(s) restored in %s\r\n", n, full);
                st.failed += failed;
                vx_wcscatn(full, L"\\", MAX_PATH);
                vx_set_layer(full, L"OFF");   /* automatic mode leaves it alone */
            } else {
                vxp_process_dir(full, flags, con_log, NULL, &st);
                if (set && !(flags & VXP_DRYRUN)) {
                    WCHAR layer[600];
                    LONG r;
                    vx_make_layer(layer, 600, ver, webview2, args);
                    vx_wcscatn(full, L"\\", MAX_PATH);
                    if ((r = vx_set_layer(full, layer))) con_print(L"Could not save settings (error %d). Run as administrator.\r\n", (int)r);
                    else con_print(L"Settings for every program in %s: %s\r\n", full, layer[0] ? layer : L"(default)");
                }
            }
        } else {
            if (undo) {
                BOOL had = vxp_has_backup(full);
                int r = vxp_restore_file(full, con_log, NULL);
                if (r < 0) st.failed++;
                else if (!r && !had) con_print(L"%s: no backup found\r\n", full);
                vx_set_layer(full, L"OFF");   /* automatic mode leaves it alone */
            } else {
                DWORD f = flags;
                /* in memory: also when only a DLL next to it needs VistaExt */
                if ((f & VXP_MEMORY) && !(f & VXP_DRYRUN) && vxp_folder_needs(full)) f |= VXP_REGALL;
                vxp_process_file(full, f, con_log, NULL, &st);
                if (set && !(flags & VXP_DRYRUN)) {
                    WCHAR layer[600];
                    LONG r;
                    vx_make_layer(layer, 600, ver, webview2, args);
                    if ((r = vx_set_layer(full, layer))) con_print(L"Could not save settings (error %d). Run as administrator.\r\n", (int)r);
                    else con_print(L"Settings saved: %s\r\n", layer[0] ? layer : L"(default)");
                }
            }
        }
    }
    if (!any) { usage(); return 2; }
    if (undo && st.failed) con_print(L"%d file(s) could not be restored. Close the program and try again.\r\n", st.failed);
    if (!undo)
        con_print(L"\r\n%d file(s) checked, %d %s, %d function(s) still missing, %d error(s)\r\n",
                  st.files, st.patched, (flags & VXP_DRYRUN) ? L"would be patched" : L"patched",
                  st.unsupported, st.failed);
    return st.failed ? 2 : st.unsupported ? 1 : 0;
}
