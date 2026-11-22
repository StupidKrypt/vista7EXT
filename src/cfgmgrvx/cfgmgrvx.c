/* cfgmgrvx.dll - cfgmgr32 companion for Windows Vista and 7.
 *
 * CM_Register_Notification (Windows 8): Vista and 7 report the same events
 * to windows (RegisterDeviceNotification, WM_DEVICECHANGE). So the callback
 * registrations share one hidden window on a thread of its own
 * (src/common/notifywnd.c), which calls them as Windows 8 calls them from
 * one of its threads:
 *  - device interfaces (a class, or all classes): arrival and removal. The
 *    window takes all interface classes once and hands each event to the
 *    registrations it matches, so each gets every event once;
 *  - a device handle: query-remove (a callback answering ERROR_CANCELLED
 *    vetoes it, as on Windows 8), remove failed, remove pending, remove
 *    complete, and the device's custom events;
 *  - device instances (enumerated/started/removed): Vista and 7 have no such
 *    notification, so that filter fails with CR_CALL_NOT_IMPLEMENTED.
 * Unregistering runs on the window's thread, so no callback of it is running
 * or still to come once it returns. */
#include "../common/vx.h"

typedef DWORD CONFIGRET;
#define CFG L"cfgmgr32.dll"
#define CR_SUCCESS               0x00
#define CR_OUT_OF_MEMORY         0x02
#define CR_INVALID_POINTER       0x03
#define CR_INVALID_FLAG          0x04
#define CR_INVALID_DEVNODE       0x05
#define CR_INVALID_DATA          0x1F
#define CR_BUFFER_SMALL          0x1A
#define CR_NO_SUCH_DEVNODE       0x0D
#define CR_NO_SUCH_VALUE         0x25
#define CR_ACCESS_DENIED         0x33
#define CR_CALL_NOT_IMPLEMENTED  0x34
#define CR_INVALID_PROPERTY      0x35

#define CR_FAILURE               0x13

/* CM_NOTIFY_FILTER / CM_NOTIFY_EVENT_DATA (cfgmgr32.h, Windows 8) */
#define VX_FILTER_INTERFACE 0
#define VX_FILTER_HANDLE    1
#define VX_FILTER_INSTANCE  2
#define VX_ALL_INTERFACE_CLASSES 0x1
#define VX_ALL_DEVICE_INSTANCES  0x2
typedef struct {
    DWORD cbSize, Flags;
    int FilterType;
    DWORD Reserved;
    union { GUID ClassGuid; HANDLE hTarget; WCHAR InstanceId[200]; } u;
} VX_CM_NOTIFY_FILTER;
typedef DWORD (CALLBACK *cm_cb)(HANDLE notify, PVOID ctx, int action, PVOID data, DWORD size);
/* actions */
#define A_IF_ARRIVAL 0
#define A_IF_REMOVAL 1
#define A_QUERYREMOVE 2
#define A_QUERYREMOVEFAILED 3
#define A_REMOVEPENDING 4
#define A_REMOVECOMPLETE 5
#define A_CUSTOMEVENT 6

/* WM_DEVICECHANGE (dbt.h) */
#define DBT_ARRIVAL          0x8000
#define DBT_QUERYREMOVE      0x8001
#define DBT_QUERYREMOVEFAIL  0x8002
#define DBT_REMOVEPENDING    0x8003
#define DBT_REMOVECOMPLETE   0x8004
#define DBT_CUSTOM           0x8006
#define DBT_TYP_INTERFACE    5
#define DBT_TYP_HANDLE       6
#define BROADCAST_QUERY_DENY 0x424D5144
typedef struct { DWORD size, type, reserved; GUID cls; WCHAR name[1]; } VX_DEV_IF;
typedef struct { DWORD size, type, reserved; HANDLE handle; HANDLE hdevnotify; GUID event; LONG nameoff; BYTE data[1]; } VX_DEV_HANDLE;

typedef struct vx_creg {
    struct vx_creg *next, *dead;
    DWORD magic;
    int type;
    BOOL all;
    GUID cls;
    HANDLE target;
    HANDLE hdn;                     /* Windows' registration (device handles) */
    cm_cb cb;
    PVOID ctx;
} vx_creg;
#define VX_CREG_MAGIC 0x47455243u   /* "CREG" */

static LRESULT CALLBACK cwnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static vx_notifywnd g_cw = { L"VistaExtDeviceNotify", cwnd_proc, NULL, FALSE };
static vx_creg *g_cregs, *g_cdead;   /* touched only on the window's thread */
static int g_cdelivering;
static HANDLE g_all_if;             /* the window's all-interface-classes registration */
#define WM_VX_ADD    (WM_APP + 1)
#define WM_VX_REMOVE (WM_APP + 2)

typedef HANDLE (WINAPI *rdn_fn)(HANDLE, LPVOID, DWORD);
typedef BOOL (WINAPI *udn_fn)(HANDLE);

static BOOL same_guid(const GUID *a, const GUID *b)
{
    const BYTE *x = (const BYTE *)a, *y = (const BYTE *)b;
    int i;
    for (i = 0; i < (int)sizeof(GUID); i++) if (x[i] != y[i]) return FALSE;
    return TRUE;
}

static void reap(void)
{
    vx_creg *r;
    if (!--g_cdelivering)
        while (g_cdead) { r = g_cdead; g_cdead = r->dead; HeapFree(GetProcessHeap(), 0, r); }
}

/* an interface event to every matching registration */
static void deliver_interface(int action, const VX_DEV_IF *d)
{
    SIZE_T n = 0, size;
    BYTE *ev;
    vx_creg *r;
    while ((SIZE_T)((const BYTE *)&d->name[n] - (const BYTE *)d) < d->size && d->name[n]) n++;
    size = 8 + sizeof(GUID) + (n + 1) * sizeof(WCHAR);
    if (!(ev = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size))) return;
    *(DWORD *)ev = VX_FILTER_INTERFACE;                   /* FilterType, Reserved */
    vx_memcpy(ev + 8, &d->cls, sizeof(GUID));             /* ClassGuid */
    vx_memcpy(ev + 8 + sizeof(GUID), d->name, n * sizeof(WCHAR));   /* SymbolicLink */
    g_cdelivering++;
    for (r = g_cregs; r; r = r->next)
        if (r->magic == VX_CREG_MAGIC && r->type == VX_FILTER_INTERFACE && (r->all || same_guid(&r->cls, &d->cls)))
            r->cb((HANDLE)r, r->ctx, action, ev, (DWORD)size);
    reap();
    HeapFree(GetProcessHeap(), 0, ev);
}

/* a device-handle event to its registration; FALSE = vetoed */
static BOOL deliver_handle(int action, const VX_DEV_HANDLE *d)
{
    vx_creg *r;
    BOOL ok = TRUE;
    BYTE *ev = NULL;
    DWORD size = 8, datasize = 0;
    if (action == A_CUSTOMEVENT) {
        /* EventGuid, NameOffset, DataSize, Data[] */
        datasize = d->size > (DWORD)((const BYTE *)d->data - (const BYTE *)d) ? d->size - (DWORD)((const BYTE *)d->data - (const BYTE *)d) : 0;
        size = 8 + sizeof(GUID) + 8 + datasize;
    }
    if (!(ev = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size < 8 + sizeof(GUID) + 8 ? 8 + sizeof(GUID) + 8 : size))) return TRUE;
    *(DWORD *)ev = VX_FILTER_HANDLE;
    if (action == A_CUSTOMEVENT) {
        vx_memcpy(ev + 8, &d->event, sizeof(GUID));
        *(LONG *)(ev + 8 + sizeof(GUID)) = d->nameoff;
        *(DWORD *)(ev + 12 + sizeof(GUID)) = datasize;
        if (datasize) vx_memcpy(ev + 16 + sizeof(GUID), d->data, datasize);
    }
    g_cdelivering++;
    for (r = g_cregs; r; r = r->next)
        if (r->magic == VX_CREG_MAGIC && r->type == VX_FILTER_HANDLE && r->hdn == d->hdevnotify)
            if (r->cb((HANDLE)r, r->ctx, action, ev, size) == ERROR_CANCELLED && action == A_QUERYREMOVE) ok = FALSE;
    reap();
    HeapFree(GetProcessHeap(), 0, ev);
    return ok;
}

static LRESULT CALLBACK cwnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vx_creg *r, **pp;
    switch (msg) {
    case WM_VX_ADD: {               /* lp: the registration; returns a CONFIGRET */
        rdn_fn rdn = (rdn_fn)(void *)vx_user32("RegisterDeviceNotificationW");
        r = (vx_creg *)lp;
        if (!rdn) return CR_FAILURE;
        if (r->type == VX_FILTER_INTERFACE && !g_all_if) {
            VX_DEV_IF f;
            vx_memset(&f, 0, sizeof(f));
            f.size = sizeof(f);
            f.type = DBT_TYP_INTERFACE;
            if (!(g_all_if = rdn(h, &f, 0 /* WINDOW_HANDLE */ | 4 /* DEVICE_NOTIFY_ALL_INTERFACE_CLASSES */))) return CR_FAILURE;
        } else if (r->type == VX_FILTER_HANDLE) {
            VX_DEV_HANDLE f;
            vx_memset(&f, 0, sizeof(f));
            f.size = sizeof(f);
            f.type = DBT_TYP_HANDLE;
            f.handle = r->target;
            if (!(r->hdn = rdn(h, &f, 0))) return GetLastError() == ERROR_INVALID_HANDLE ? CR_INVALID_DATA : CR_FAILURE;
        }
        r->magic = VX_CREG_MAGIC;
        r->next = g_cregs;
        g_cregs = r;
        return CR_SUCCESS;
    }
    case WM_VX_REMOVE:              /* lp: the registration; returns a CONFIGRET */
        for (pp = &g_cregs; *pp; pp = &(*pp)->next) {
            if (*pp != (vx_creg *)lp) continue;
            r = *pp;
            *pp = r->next;
            if (r->hdn) {
                udn_fn udn = (udn_fn)(void *)vx_user32("UnregisterDeviceNotification");
                if (udn) udn(r->hdn);
            }
            r->magic = 0;
            if (g_cdelivering) { r->dead = g_cdead; g_cdead = r; }
            else HeapFree(GetProcessHeap(), 0, r);
            return CR_SUCCESS;
        }
        return CR_INVALID_DATA;
    case WM_DEVICECHANGE: {
        const DWORD *hdr = (const DWORD *)lp;           /* DEV_BROADCAST_HDR: size, type */
        if (!hdr) break;
        if (hdr[1] == DBT_TYP_INTERFACE && (wp == DBT_ARRIVAL || wp == DBT_REMOVECOMPLETE))
            deliver_interface(wp == DBT_ARRIVAL ? A_IF_ARRIVAL : A_IF_REMOVAL, (const VX_DEV_IF *)lp);
        else if (hdr[1] == DBT_TYP_HANDLE) {
            int a = wp == DBT_QUERYREMOVE ? A_QUERYREMOVE : wp == DBT_QUERYREMOVEFAIL ? A_QUERYREMOVEFAILED :
                    wp == DBT_REMOVEPENDING ? A_REMOVEPENDING : wp == DBT_REMOVECOMPLETE ? A_REMOVECOMPLETE :
                    wp == DBT_CUSTOM ? A_CUSTOMEVENT : -1;
            if (a >= 0 && !deliver_handle(a, (const VX_DEV_HANDLE *)lp)) return BROADCAST_QUERY_DENY;
        }
        return TRUE;
    }
    }
    return vx_defproc(h, msg, wp, lp);
}

VX_EXPORT CONFIGRET WINAPI vx_CM_Register_Notification(PVOID filter, PVOID ctx, PVOID cb, HANDLE *h)
{ VX_NATIVE(CFG, CM_Register_Notification, CONFIGRET, (PVOID filter, PVOID ctx, PVOID cb, HANDLE *h), (filter, ctx, cb, h));
    VX_CM_NOTIFY_FILTER *f = filter;
    vx_creg *r;
    HWND w;
    CONFIGRET cr;
    if (!filter || !cb || !h) return CR_INVALID_POINTER;
    *h = NULL;
    if (f->cbSize != sizeof(VX_CM_NOTIFY_FILTER) || f->Reserved) return CR_INVALID_DATA;
    switch (f->FilterType) {
    case VX_FILTER_INTERFACE:
        if (f->Flags & ~VX_ALL_INTERFACE_CLASSES) return CR_INVALID_FLAG;
        break;
    case VX_FILTER_HANDLE:
        if (f->Flags || !f->u.hTarget || f->u.hTarget == INVALID_HANDLE_VALUE) return f->Flags ? CR_INVALID_FLAG : CR_INVALID_DATA;
        break;
    case VX_FILTER_INSTANCE:
        VX_TRACE("CM_Register_Notification: device-instance events do not exist on this Windows");
        return CR_CALL_NOT_IMPLEMENTED;
    default:
        return CR_INVALID_DATA;
    }
    if (!g_cw.module) g_cw.module = GetModuleHandleW(L"cfgmgrvx.dll");
    if (!(w = vx_notify_window(&g_cw))) return CR_FAILURE;
    if (!(r = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r)))) return CR_OUT_OF_MEMORY;
    r->type = f->FilterType;
    r->all = f->FilterType == VX_FILTER_INTERFACE && (f->Flags & VX_ALL_INTERFACE_CLASSES);
    if (f->FilterType == VX_FILTER_INTERFACE) r->cls = f->u.ClassGuid;
    else r->target = f->u.hTarget;
    r->cb = (cm_cb)cb;
    r->ctx = ctx;
    if ((cr = (CONFIGRET)vx_send(w, WM_VX_ADD, 0, (LPARAM)r)) != CR_SUCCESS) { HeapFree(GetProcessHeap(), 0, r); return cr; }
    *h = (HANDLE)r;
    return CR_SUCCESS;
}

VX_EXPORT CONFIGRET WINAPI vx_CM_Unregister_Notification(HANDLE h)
{ VX_NATIVE(CFG, CM_Unregister_Notification, CONFIGRET, (HANDLE h), (h));
    if (!h) return CR_INVALID_POINTER;
    if (!g_cw.hwnd) return CR_INVALID_DATA;
    return (CONFIGRET)vx_send(g_cw.hwnd, WM_VX_REMOVE, 0, (LPARAM)h);
}

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT DWORD WINAPI vx_CM_MapCrToWin32Err(CONFIGRET cr, DWORD dflt)
{ VX_NATIVE(CFG, CM_MapCrToWin32Err, DWORD, (CONFIGRET cr, DWORD dflt), (cr, dflt));
    switch (cr) {
    case CR_SUCCESS: return ERROR_SUCCESS;
    case CR_OUT_OF_MEMORY: return ERROR_NOT_ENOUGH_MEMORY;
    case CR_INVALID_POINTER: return ERROR_INVALID_USER_BUFFER;
    case CR_INVALID_FLAG: return ERROR_INVALID_FLAGS;
    case CR_INVALID_DEVNODE: case CR_INVALID_DATA: case CR_INVALID_PROPERTY: return ERROR_INVALID_DATA;
    case CR_BUFFER_SMALL: return ERROR_INSUFFICIENT_BUFFER;
    case CR_NO_SUCH_DEVNODE: case CR_NO_SUCH_VALUE: return ERROR_NOT_FOUND;
    case CR_ACCESS_DENIED: return ERROR_ACCESS_DENIED;
    case CR_CALL_NOT_IMPLEMENTED: return ERROR_CALL_NOT_IMPLEMENTED;
    }
    return dflt;
}
#endif

/* device properties by key (Windows 7): not available on Vista */
#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT CONFIGRET WINAPI vx_CM_Get_DevNode_PropertyW(DWORD dev, const void *key, ULONG *type, BYTE *buf, ULONG *size, ULONG flags)
{ VX_NATIVE(CFG, CM_Get_DevNode_PropertyW, CONFIGRET, (DWORD dev, const void *key, ULONG *type, BYTE *buf, ULONG *size, ULONG flags), (dev, key, type, buf, size, flags));
    return CR_CALL_NOT_IMPLEMENTED;
}
#endif

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}
