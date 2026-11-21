/* nettest - can this Windows fetch an HTTPS page the way programs do?
 *   nettest <url>...
 * For each URL: WinHTTP with Windows' default protocols (what Spotify's and
 * Steam's downloaders use), and WinINet (Internet Explorer's library). Prints
 * the HTTP status, or the Windows error (12175: a certificate problem,
 * 12157: no protocol in common, i.e. TLS). Exit code: number of failures. */
#include "../../src/common/vx.h"
#include "../../src/common/console.h"
#include <winhttp.h>
/* WinINet (its header clashes with winhttp.h) */
typedef LPVOID HINET;
HINET WINAPI InternetOpenW(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
HINET WINAPI InternetOpenUrlW(HINET, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD_PTR);
BOOL WINAPI HttpQueryInfoW(HINET, DWORD, LPVOID, LPDWORD, LPDWORD);
BOOL WINAPI InternetCloseHandle(HINET);
#define INTERNET_OPEN_TYPE_PRECONFIG 0
#define INTERNET_FLAG_RELOAD 0x80000000
#define INTERNET_FLAG_NO_CACHE_WRITE 0x04000000
#define INTERNET_FLAG_NO_UI 0x00000200
#define HTTP_QUERY_STATUS_CODE 19
#define HTTP_QUERY_FLAG_NUMBER 0x20000000

static int winhttp_get(const WCHAR *url, DWORD *status)
{
    URL_COMPONENTS u;
    WCHAR host[256], path[1024];
    HINTERNET s = NULL, c = NULL, r = NULL;
    DWORD len = sizeof(*status), err = 0;
    vx_memset(&u, 0, sizeof(u));
    u.dwStructSize = sizeof(u);
    u.lpszHostName = host; u.dwHostNameLength = 256;
    u.lpszUrlPath = path; u.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url, 0, 0, &u)) return (int)GetLastError();
    if (!(s = WinHttpOpen(L"VistaExt-nettest/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0)) ||
        !(c = WinHttpConnect(s, host, u.nPort, 0)) ||
        !(r = WinHttpOpenRequest(c, L"GET", path, NULL, NULL, NULL, u.nScheme == 2 /* https */ ? WINHTTP_FLAG_SECURE : 0)) ||
        !WinHttpSendRequest(r, NULL, 0, NULL, 0, 0, 0) || !WinHttpReceiveResponse(r, NULL) ||
        !WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, status, &len, NULL))
        err = GetLastError();
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return (int)err;
}

static int wininet_get(const WCHAR *url, DWORD *status)
{
    HINET s = InternetOpenW(L"VistaExt-nettest/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0), r = NULL;
    DWORD len = sizeof(*status), err = 0;
    if (!s) return (int)GetLastError();
    if (!(r = InternetOpenUrlW(s, url, NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0)) ||
        !HttpQueryInfoW(r, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, status, &len, NULL))
        err = GetLastError();
    if (r) InternetCloseHandle(r);
    InternetCloseHandle(s);
    return (int)err;
}

int wmain(int argc, WCHAR **argv)
{
    int i, bad = 0;
    for (i = 1; i < argc; i++) {
        DWORD st = 0, st2 = 0;
        int e = winhttp_get(argv[i], &st), e2 = wininet_get(argv[i], &st2);
        if (e) con_print(L"  WinHTTP  %s: FAILED, error %d\r\n", argv[i], e);
        else con_print(L"  WinHTTP  %s: ok, HTTP %u\r\n", argv[i], st);
        if (e2) con_print(L"  WinINet  %s: FAILED, error %d\r\n", argv[i], e2);
        else con_print(L"  WinINet  %s: ok, HTTP %u\r\n", argv[i], st2);
        bad += (e != 0) + (e2 != 0);
    }
    return bad;
}
