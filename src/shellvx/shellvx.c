/* shellvx.dll - shell32 companion for Windows Vista (taskbar app IDs and
 * known-folder helpers from Windows 7). */
#include "../common/vx.h"
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>

static WCHAR g_appid[256];

/* ---- starting programs ---------------------------------------------------------
 * A setup that restarts itself with administrator rights, or starts its
 * helper, through ShellExecute: VistaExt's service is asked first, like for
 * CreateProcess (kernelvx VxPrepareChild), so the program it starts can run
 * on this Windows too. Documents and other files are not looked at. */
static void prepare(LPCWSTR file, LPCWSTR dir)
{
    static void (WINAPI *p)(LPCWSTR, LPCWSTR);
    SIZE_T n;
    DWORD e = GetLastError();
    if (!file || !(n = vx_wcslen(file)) || n >= MAX_PATH) return;
    if (n > 4 && file[n - 4] == '.' && vx_wcsicmp(file + n - 4, L".exe")) return;      /* a document, .msi, ... */
    if (!p) {
        HMODULE k = GetModuleHandleW(L"kernelvx.dll");
        p = k ? (void (WINAPI *)(LPCWSTR, LPCWSTR))(void *)GetProcAddress(k, "VxPrepareChild") : NULL;
    }
    if (p) {
        WCHAR q[MAX_PATH + 2];
        wsprintfW(q, L"\"%s\"", file);                /* one token, even with spaces */
        p(q, dir);
    }
    SetLastError(e);
}

VX_EXPORT BOOL WINAPI vx_ShellExecuteExW(SHELLEXECUTEINFOW *i)
{
    if (i && i->cbSize >= sizeof(*i) && !(i->fMask & SEE_MASK_INVOKEIDLIST) && !i->lpIDList) prepare(i->lpFile, i->lpDirectory);
    return ShellExecuteExW(i);
}

VX_EXPORT BOOL WINAPI vx_ShellExecuteExA(SHELLEXECUTEINFOA *i)
{
    WCHAR f[MAX_PATH], d[MAX_PATH];
    if (i && i->cbSize >= sizeof(*i) && !(i->fMask & SEE_MASK_INVOKEIDLIST) && !i->lpIDList && i->lpFile &&
        MultiByteToWideChar(CP_ACP, 0, i->lpFile, -1, f, MAX_PATH))
        prepare(f, i->lpDirectory && MultiByteToWideChar(CP_ACP, 0, i->lpDirectory, -1, d, MAX_PATH) ? d : NULL);
    return ShellExecuteExA(i);
}

VX_EXPORT HINSTANCE WINAPI vx_ShellExecuteW(HWND h, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show)
{
    prepare(file, dir);
    return ShellExecuteW(h, op, file, params, dir, show);
}

VX_EXPORT HINSTANCE WINAPI vx_ShellExecuteA(HWND h, LPCSTR op, LPCSTR file, LPCSTR params, LPCSTR dir, INT show)
{
    WCHAR f[MAX_PATH], d[MAX_PATH];
    if (file && MultiByteToWideChar(CP_ACP, 0, file, -1, f, MAX_PATH))
        prepare(f, dir && MultiByteToWideChar(CP_ACP, 0, dir, -1, d, MAX_PATH) ? d : NULL);
    return ShellExecuteA(h, op, file, params, dir, show);
}

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT HRESULT WINAPI vx_SetCurrentProcessExplicitAppUserModelID(PCWSTR id)
{ VX_NATIVE(L"shell32.dll", SetCurrentProcessExplicitAppUserModelID, HRESULT, (PCWSTR id), (id));
    if (!id || vx_wcslen(id) >= 128) return E_INVALIDARG;
    vx_wcscpyn(g_appid, id, 256);
    return S_OK;
}
#endif

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT HRESULT WINAPI vx_GetCurrentProcessExplicitAppUserModelID(PWSTR *id)
{ VX_NATIVE(L"shell32.dll", GetCurrentProcessExplicitAppUserModelID, HRESULT, (PWSTR *id), (id));
    SIZE_T n;
    *id = NULL;
    if (!g_appid[0]) return E_FAIL;
    n = (vx_wcslen(g_appid) + 1) * sizeof(WCHAR);
    *id = CoTaskMemAlloc(n);
    if (!*id) return E_OUTOFMEMORY;
    vx_memcpy(*id, g_appid, n);
    return S_OK;
}
#endif

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT HRESULT WINAPI vx_SHGetPropertyStoreForWindow(HWND h, REFIID riid, void **ppv)
{ VX_NATIVE(L"shell32.dll", SHGetPropertyStoreForWindow, HRESULT, (HWND h, REFIID riid, void **ppv), (h, riid, ppv));
    *ppv = NULL;
    return E_NOTIMPL;      /* taskbar window properties: Windows 7 taskbar only */
}
#endif

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT HRESULT WINAPI vx_Shell_NotifyIconGetRect(const void *id, RECT *rc) { VX_NATIVE(L"shell32.dll", Shell_NotifyIconGetRect, HRESULT, (const void *id, RECT *rc), (id, rc)); return E_NOTIMPL; }
#endif

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT HRESULT WINAPI vx_SHGetKnownFolderItem(REFKNOWNFOLDERID id, DWORD flags, HANDLE tok, REFIID riid, void **ppv)
{ VX_NATIVE(L"shell32.dll", SHGetKnownFolderItem, HRESULT, (REFKNOWNFOLDERID id, DWORD flags, HANDLE tok, REFIID riid, void **ppv), (id, flags, tok, riid, ppv));
    PWSTR path = NULL;
    HRESULT hr = SHGetKnownFolderPath(id, flags, tok, &path);
    *ppv = NULL;
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(path, NULL, riid, ppv);
    CoTaskMemFree(path);
    return hr;
}
#endif

VX_EXPORT HRESULT WINAPI vx_SHCreateItemInKnownFolder(REFKNOWNFOLDERID id, DWORD flags, PCWSTR item, REFIID riid, void **ppv)
{ VX_NATIVE(L"shell32.dll", SHCreateItemInKnownFolder, HRESULT, (REFKNOWNFOLDERID id, DWORD flags, PCWSTR item, REFIID riid, void **ppv), (id, flags, item, riid, ppv));
    PWSTR path = NULL;
    WCHAR full[MAX_PATH * 2];
    HRESULT hr = SHGetKnownFolderPath(id, flags, NULL, &path);
    *ppv = NULL;
    if (FAILED(hr)) return hr;
    vx_wcscpyn(full, path, MAX_PATH * 2);
    CoTaskMemFree(path);
    if (item && *item) {
        vx_wcscatn(full, L"\\", MAX_PATH * 2);
        vx_wcscatn(full, item, MAX_PATH * 2);
    }
    return SHCreateItemFromParsingName(full, NULL, riid, ppv);
}

VX_EXPORT HRESULT WINAPI vx_SHGetKnownFolderIDList(REFKNOWNFOLDERID id, DWORD flags, HANDLE tok, PIDLIST_ABSOLUTE *pidl)
{ VX_NATIVE(L"shell32.dll", SHGetKnownFolderIDList, HRESULT, (REFKNOWNFOLDERID id, DWORD flags, HANDLE tok, PIDLIST_ABSOLUTE *pidl), (id, flags, tok, pidl));
    PWSTR path = NULL;
    HRESULT hr = SHGetKnownFolderPath(id, flags, tok, &path);
    *pidl = NULL;
    if (SUCCEEDED(hr)) hr = SHParseDisplayName(path, NULL, pidl, 0, NULL);
    CoTaskMemFree(path);
    return hr;
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}
