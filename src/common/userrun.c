/* userrun.c - starting a program as the signed-in user.
 *
 * A program started directly by one running as administrator runs as
 * administrator too (a browser without its sandbox, no drag and drop from
 * Explorer). Explorer runs as the user, so it is asked to start it: the
 * desktop's shell view gives Shell.Application, whose ShellExecute runs in
 * Explorer's process. This is the way Microsoft documents for it ("Execute
 * In Explorer" sample); Windows has no simpler API for it. The caller has
 * initialized COM (apartment-threaded). */
#include "vx.h"
#include "userrun.h"
#include <shlobj.h>
#include <exdisp.h>
#include <shldisp.h>
#include <servprov.h>
#include <shlguid.h>
#undef ShellExecute          /* shellapi.h's A/W macro would rename the COM method */

BOOL vx_run_as_user(const WCHAR *file, const WCHAR *args, const WCHAR *dir)
{
    IShellWindows *sw = NULL;
    IDispatch *disp = NULL, *bg = NULL, *app = NULL;
    IServiceProvider *sp = NULL;
    IShellBrowser *sb = NULL;
    IShellView *sv = NULL;
    IShellFolderViewDual *fv = NULL;
    IShellDispatch2 *sd = NULL;
    VARIANT loc, none, vargs, vdir, vshow;
    long hwnd = 0;
    BOOL ok = FALSE;
    VariantInit(&loc); VariantInit(&none); VariantInit(&vargs); VariantInit(&vdir); VariantInit(&vshow);
    loc.vt = VT_I4; loc.lVal = CSIDL_DESKTOP;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellWindows, NULL, CLSCTX_LOCAL_SERVER, &IID_IShellWindows, (void **)&sw)) &&
        sw->lpVtbl->FindWindowSW(sw, &loc, &none, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) == S_OK && disp &&
        SUCCEEDED(disp->lpVtbl->QueryInterface(disp, &IID_IServiceProvider, (void **)&sp)) &&
        SUCCEEDED(sp->lpVtbl->QueryService(sp, &SID_STopLevelBrowser, &IID_IShellBrowser, (void **)&sb)) &&
        SUCCEEDED(sb->lpVtbl->QueryActiveShellView(sb, &sv)) &&
        SUCCEEDED(sv->lpVtbl->GetItemObject(sv, SVGIO_BACKGROUND, &IID_IDispatch, (void **)&bg)) &&
        SUCCEEDED(bg->lpVtbl->QueryInterface(bg, &IID_IShellFolderViewDual, (void **)&fv)) &&
        SUCCEEDED(fv->lpVtbl->get_Application(fv, &app)) &&
        SUCCEEDED(app->lpVtbl->QueryInterface(app, &IID_IShellDispatch2, (void **)&sd))) {
        BSTR bfile = SysAllocString(file);
        BOOL good = bfile != NULL;
        if (args && args[0]) { vargs.vt = VT_BSTR; good = good && (vargs.bstrVal = SysAllocString(args)); }
        if (dir && dir[0]) { vdir.vt = VT_BSTR; good = good && (vdir.bstrVal = SysAllocString(dir)); }
        vshow.vt = VT_I4; vshow.lVal = SW_SHOWNORMAL;
        ok = good && SUCCEEDED(sd->lpVtbl->ShellExecute(sd, bfile, vargs, vdir, none, vshow));
        SysFreeString(bfile);
        VariantClear(&vargs);
        VariantClear(&vdir);
    }
    if (sd) sd->lpVtbl->Release(sd);
    if (app) app->lpVtbl->Release(app);
    if (fv) fv->lpVtbl->Release(fv);
    if (bg) bg->lpVtbl->Release(bg);
    if (sv) sv->lpVtbl->Release(sv);
    if (sb) sb->lpVtbl->Release(sb);
    if (sp) sp->lpVtbl->Release(sp);
    if (disp) disp->lpVtbl->Release(disp);
    if (sw) sw->lpVtbl->Release(sw);
    return ok;
}
