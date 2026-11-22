/* dnsvx.dll - dnsapi companion for Windows Vista and 7. Added: DnsQueryEx and
 * DnsCancelQuery (Windows 8), built on DnsQuery_W (Vista).
 *
 * Microsoft Learn (windns.h): DnsQueryEx takes a DNS_QUERY_REQUEST (version
 * 1) and a DNS_QUERY_RESULT (version 1, other members NULL). Without a
 * completion callback it is synchronous; with one it returns
 * DNS_REQUEST_PENDING and calls back with the results, which the caller
 * frees with DnsRecordListFree. Errors in the request are returned at once
 * (ERROR_INVALID_PARAMETER for a wrong version). DnsCancelQuery does not
 * wait: the callback still comes, here with ERROR_CANCELLED.
 *
 * Vista's DnsQuery_W does the query, on a thread-pool thread for the
 * asynchronous form. What it cannot do fails cleanly with ERROR_NOT_SUPPORTED
 * instead of being quietly ignored: a query on one network interface only
 * (InterfaceIndex not 0) and IPv6 DNS servers (DnsQuery_W takes IPv4 ones). */
#include "../common/vx.h"
#include <windns.h>

#define DNSAPI L"dnsapi.dll"
#define VX_DNS_REQUEST_PENDING 9506
#ifndef AF_INET
#define AF_INET 2
#endif

typedef VOID (WINAPI *vx_dns_cb)(PVOID ctx, PVOID results);
typedef struct {
    ULONG Version;
    PCWSTR QueryName;
    WORD QueryType;
    ULONG64 QueryOptions;
    PVOID pDnsServerList;           /* DNS_ADDR_ARRAY */
    ULONG InterfaceIndex;
    vx_dns_cb pQueryCompletionCallback;
    PVOID pQueryContext;
} VX_DNS_QUERY_REQUEST;
typedef struct {
    ULONG Version;
    DNS_STATUS QueryStatus;
    ULONG64 QueryOptions;
    PDNS_RECORD pQueryRecords;
    PVOID Reserved;
} VX_DNS_QUERY_RESULT;
typedef struct { CHAR Reserved[32]; } VX_DNS_QUERY_CANCEL;

/* DNS_ADDR_ARRAY: a 32-byte header, then 64-byte DNS_ADDRs (a sockaddr first) */
#define ADDR_HDR 32
#define ADDR_SIZE 64

typedef struct dns_job {
    struct dns_job *next;
    LONG id;
    volatile LONG cancelled;
    WCHAR *name;
    WORD type;
    ULONG64 options;
    IP4_ARRAY *servers;
    VX_DNS_QUERY_RESULT *res;
    vx_dns_cb cb;
    PVOID ctx;
} dns_job;

static dns_job *g_jobs;
static SRWLOCK g_jlock = SRWLOCK_INIT;
static volatile LONG g_next_id;
#define CANCEL_MAGIC 0x584E4456u         /* "VDNX" */

/* the IPv4 servers of a DNS_ADDR_ARRAY (NULL: none given); FALSE: not possible here */
static BOOL servers_of(const BYTE *arr, IP4_ARRAY **out)
{
    DWORD n, i;
    *out = NULL;
    if (!arr || !(n = ((const DWORD *)arr)[1])) return TRUE;               /* AddrCount */
    if (!(*out = HeapAlloc(GetProcessHeap(), 0, sizeof(IP4_ARRAY) + n * sizeof(IP4_ADDRESS)))) return FALSE;
    (*out)->AddrCount = n;
    for (i = 0; i < n; i++) {
        const BYTE *sa = arr + ADDR_HDR + i * ADDR_SIZE;
        if (*(const WORD *)sa != AF_INET) { HeapFree(GetProcessHeap(), 0, *out); *out = NULL; return FALSE; }
        (*out)->AddrArray[i] = *(const IP4_ADDRESS *)(sa + 4);              /* sin_addr */
    }
    return TRUE;
}

static DNS_STATUS run(const WCHAR *name, WORD type, ULONG64 opts, IP4_ARRAY *servers, PDNS_RECORD *recs)
{
    *recs = NULL;
    return DnsQuery_W(name, type, (DWORD)opts, servers, recs, NULL);
}

static DWORD WINAPI job_proc(LPVOID p)
{
    dns_job *j = p, **pp;
    PDNS_RECORD recs;
    DNS_STATUS st = run(j->name, j->type, j->options, j->servers, &recs);
    AcquireSRWLockExclusive(&g_jlock);
    for (pp = &g_jobs; *pp; pp = &(*pp)->next) if (*pp == j) { *pp = j->next; break; }
    ReleaseSRWLockExclusive(&g_jlock);
    if (j->cancelled) {
        if (recs) DnsRecordListFree(recs, DnsFreeRecordList);
        recs = NULL;
        st = ERROR_CANCELLED;
    }
    j->res->QueryStatus = st;
    j->res->QueryOptions = j->options;
    j->res->pQueryRecords = recs;
    j->cb(j->ctx, j->res);
    HeapFree(GetProcessHeap(), 0, j->name);
    if (j->servers) HeapFree(GetProcessHeap(), 0, j->servers);
    HeapFree(GetProcessHeap(), 0, j);
    return 0;
}

VX_EXPORT DNS_STATUS WINAPI vx_DnsQueryEx(VX_DNS_QUERY_REQUEST *req, VX_DNS_QUERY_RESULT *res, VX_DNS_QUERY_CANCEL *cancel)
{ VX_NATIVE(DNSAPI, DnsQueryEx, DNS_STATUS, (VX_DNS_QUERY_REQUEST *req, VX_DNS_QUERY_RESULT *res, VX_DNS_QUERY_CANCEL *cancel), (req, res, cancel));
    WCHAR local[256];
    const WCHAR *name;
    IP4_ARRAY *servers;
    dns_job *j;
    SIZE_T n;
    if (!req || !res || req->Version != 1 || res->Version != 1) return ERROR_INVALID_PARAMETER;
    res->pQueryRecords = NULL;
    if (req->InterfaceIndex) return ERROR_NOT_SUPPORTED;          /* one interface only: not on Vista/7 */
    if (!servers_of(req->pDnsServerList, &servers)) return ERROR_NOT_SUPPORTED;   /* an IPv6 server */
    name = req->QueryName;
    if (!name) {                                                   /* the local machine */
        DWORD cch = 256;
        if (!GetComputerNameExW(ComputerNameDnsFullyQualified, local, &cch)) {
            if (servers) HeapFree(GetProcessHeap(), 0, servers);
            return GetLastError();
        }
        name = local;
    }
    if (!req->pQueryCompletionCallback) {                          /* synchronous */
        PDNS_RECORD recs;
        DNS_STATUS st = run(name, req->QueryType, req->QueryOptions, servers, &recs);
        if (servers) HeapFree(GetProcessHeap(), 0, servers);
        res->QueryStatus = st;
        res->QueryOptions = req->QueryOptions;
        res->pQueryRecords = recs;
        return st;
    }
    n = vx_wcslen(name) + 1;
    if (!(j = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*j))) ||
        !(j->name = HeapAlloc(GetProcessHeap(), 0, n * sizeof(WCHAR)))) {
        if (j) HeapFree(GetProcessHeap(), 0, j);
        if (servers) HeapFree(GetProcessHeap(), 0, servers);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    vx_memcpy(j->name, name, n * sizeof(WCHAR));
    j->type = req->QueryType;
    j->options = req->QueryOptions;
    j->servers = servers;
    j->res = res;
    j->cb = req->pQueryCompletionCallback;
    j->ctx = req->pQueryContext;
    j->id = InterlockedIncrement(&g_next_id);
    res->QueryStatus = VX_DNS_REQUEST_PENDING;
    if (cancel) {
        vx_memset(cancel, 0, sizeof(*cancel));
        ((DWORD *)cancel->Reserved)[0] = CANCEL_MAGIC;
        ((LONG *)cancel->Reserved)[1] = j->id;
    }
    AcquireSRWLockExclusive(&g_jlock);
    j->next = g_jobs;
    g_jobs = j;
    ReleaseSRWLockExclusive(&g_jlock);
    if (!QueueUserWorkItem(job_proc, j, WT_EXECUTELONGFUNCTION)) {
        dns_job **pp;
        DWORD e = GetLastError();
        AcquireSRWLockExclusive(&g_jlock);
        for (pp = &g_jobs; *pp; pp = &(*pp)->next) if (*pp == j) { *pp = j->next; break; }
        ReleaseSRWLockExclusive(&g_jlock);
        HeapFree(GetProcessHeap(), 0, j->name);
        if (servers) HeapFree(GetProcessHeap(), 0, servers);
        HeapFree(GetProcessHeap(), 0, j);
        res->QueryStatus = e;
        return e;
    }
    return VX_DNS_REQUEST_PENDING;
}

VX_EXPORT DNS_STATUS WINAPI vx_DnsCancelQuery(VX_DNS_QUERY_CANCEL *cancel)
{ VX_NATIVE(DNSAPI, DnsCancelQuery, DNS_STATUS, (VX_DNS_QUERY_CANCEL *cancel), (cancel));
    dns_job *j;
    LONG id;
    if (!cancel || ((DWORD *)cancel->Reserved)[0] != CANCEL_MAGIC) return ERROR_INVALID_PARAMETER;
    id = ((LONG *)cancel->Reserved)[1];
    AcquireSRWLockExclusive(&g_jlock);
    for (j = g_jobs; j && j->id != id; j = j->next) ;
    if (j) InterlockedExchange(&j->cancelled, 1);
    ReleaseSRWLockExclusive(&g_jlock);
    return j ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;         /* finished already, or never started */
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}
