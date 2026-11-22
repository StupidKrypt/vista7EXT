/* kernelvx - files, app model (packages), locale, time zones, console. */
#include "../common/vx.h"

/* ---- shared memory ----------------------------------------------------------
 * Windows before 8.1 ignores the access list (DACL) of an UNNAMED section.
 * Programs that protect shared memory with an empty DACL, so that a read-only
 * handle can never be made writable again (Chromium, Electron: Brave,
 * Discord, ...), check this and stop ("CHECK" crash) when it can. Chromium
 * itself used to name such sections on Windows 7 for exactly this reason,
 * but skips that when it believes it runs on Windows 10. So: an unnamed
 * section created with a DACL gets an unguessable random name, which makes
 * Vista/7 apply the DACL. Everything else is passed through unchanged.
 * The cost: about 15 µs per such call instead of under 1 µs (creating a
 * named object; measured on Windows 7, one slow processor). There is no
 * cheaper way to get the DACL applied on Vista/7, and programs make these a
 * few hundred times a second at most. */
typedef LONG (WINAPI *rtl_dacl_fn)(PSECURITY_DESCRIPTOR, PBOOLEAN, PACL *, PBOOLEAN);
typedef BOOLEAN (WINAPI *gen_random_fn)(PVOID, ULONG);

static BOOL has_dacl(LPSECURITY_ATTRIBUTES sa)
{
    static rtl_dacl_fn get;
    BOOLEAN present = FALSE, def = FALSE;
    PACL dacl = NULL;
    if (!sa || !sa->lpSecurityDescriptor) return FALSE;
    if (!get) get = (rtl_dacl_fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetDaclSecurityDescriptor");
    if (!get || get(sa->lpSecurityDescriptor, &present, &dacl, &def) < 0) return FALSE;
    return present && dacl;
}

/* "VxSharedMem_" + 32 hex digits: a per-process secret from the system's
 * random generator (asked once: it is a device call, and Chromium makes such
 * sections all the time) mixed with a counter and the clock. The name only
 * has to be unique and unguessable in advance; the DACL does the protecting. */
static void random_name(WCHAR *out)
{
    static DWORD secret[4];
    static volatile LONG state;              /* 0 none, 1 being made, 2 ready */
    static volatile LONG counter;
    DWORD r[4];
    LARGE_INTEGER t;
    int i, j;
    if (state != 2 && InterlockedCompareExchange(&state, 1, 0) == 0) {
        gen_random_fn gen = (gen_random_fn)(void *)vx_native(L"advapi32.dll", "SystemFunction036");   /* RtlGenRandom */
        QueryPerformanceCounter(&t);
        secret[0] = t.LowPart ^ GetCurrentProcessId();
        secret[1] = t.HighPart ^ GetTickCount();
        secret[2] = (DWORD)(ULONG_PTR)&t;
        secret[3] = GetCurrentThreadId() * 0x9E3779B1u;
        if (gen) {
            DWORD x[4];
            if (gen(x, sizeof(x))) for (i = 0; i < 4; i++) secret[i] ^= x[i];
        }
        InterlockedExchange(&state, 2);
    }
    while (state != 2) SwitchToThread();
    /* per name: the counter (unique in this process) and the interrupt clock,
     * a plain read of the shared system page. Not the performance counter:
     * where it is a hardware timer (HPET/ACPI, virtual machines) each read
     * costs microseconds, which made every such section ~15 us instead of
     * ~1 us (measured in the test VMs). */
    r[0] = secret[0] ^ (DWORD)InterlockedIncrement(&counter);
    r[1] = secret[1] ^ (DWORD)VX_KUSER_INTERRUPT_TIME[0];
    r[2] = secret[2] ^ GetCurrentProcessId();
    r[3] = secret[3];
    /* mix, so that consecutive names share no visible pattern */
    for (i = 0; i < 4; i++) {
        r[0] += r[1]; r[1] = (r[1] << 7 | r[1] >> 25) ^ r[0];
        r[2] += r[3]; r[3] = (r[3] << 13 | r[3] >> 19) ^ r[2];
        r[0] ^= r[3] * 0x85EBCA6Bu; r[2] ^= r[1] * 0xC2B2AE35u;
    }
    vx_wcscpyn(out, L"VxSharedMem_", 16);
    for (i = 0; i < 4; i++)
        for (j = 7; j >= 0; j--) out[12 + i * 8 + (7 - j)] = L"0123456789abcdef"[(r[i] >> (j * 4)) & 15];
    out[44] = 0;
}

static HANDLE create_mapping(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCWSTR name,
                             DWORD numa, BOOL use_numa)
{
    typedef HANDLE (WINAPI *numa_fn)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR, DWORD);
    static numa_fn numa_create;
    WCHAR rn[48];
    HANDLE h;
    int tries;
    if (use_numa && !numa_create) numa_create = (numa_fn)(void *)GetProcAddress(GetModuleHandleW(K32), "CreateFileMappingNumaW");
    if (name || !has_dacl(sa)) {
        if (use_numa && numa_create) return numa_create(file, sa, prot, hi, lo, name, numa);
        return CreateFileMappingW(file, sa, prot, hi, lo, name);
    }
    for (tries = 0; tries < 8; tries++) {
        random_name(rn);
        h = use_numa && numa_create ? numa_create(file, sa, prot, hi, lo, rn, numa) : CreateFileMappingW(file, sa, prot, hi, lo, rn);
        if (!h) {
            /* a name not allowed here (e.g. a sandboxed process): unnamed, as asked */
            if (GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_PATH_NOT_FOUND ||
                GetLastError() == ERROR_BAD_PATHNAME || GetLastError() == ERROR_INVALID_HANDLE) break;
            return NULL;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) { SetLastError(ERROR_SUCCESS); return h; }
        CloseHandle(h);                         /* someone else's object: never hand that out */
    }
    return use_numa && numa_create ? numa_create(file, sa, prot, hi, lo, NULL, numa) : CreateFileMappingW(file, sa, prot, hi, lo, NULL);
}

VX_EXPORT HANDLE WINAPI vx_CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCWSTR name)
{
    return create_mapping(file, sa, prot, hi, lo, name, 0, FALSE);
}

VX_EXPORT HANDLE WINAPI vx_CreateFileMappingNumaW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo,
                                                  LPCWSTR name, DWORD numa)
{
    return create_mapping(file, sa, prot, hi, lo, name, numa, TRUE);
}

VX_EXPORT HANDLE WINAPI vx_CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name)
{
    if (name || !has_dacl(sa)) return CreateFileMappingA(file, sa, prot, hi, lo, name);
    return create_mapping(file, sa, prot, hi, lo, NULL, 0, FALSE);
}

/* Windows 8 / 10 forms of the same */
VX_EXPORT HANDLE WINAPI vx_CreateFileMappingFromApp(HANDLE file, LPSECURITY_ATTRIBUTES sa, ULONG prot, ULONG64 max, LPCWSTR name)
{
    VX_NATIVE(K32, CreateFileMappingFromApp, HANDLE, (HANDLE file, LPSECURITY_ATTRIBUTES sa, ULONG prot, ULONG64 max, LPCWSTR name),
              (file, sa, prot, max, name));
    return create_mapping(file, sa, prot, (DWORD)(max >> 32), (DWORD)max, name, 0, FALSE);
}

VX_EXPORT HANDLE WINAPI vx_OpenFileMappingFromApp(ULONG access, BOOL inherit, LPCWSTR name)
{
    VX_NATIVE(K32, OpenFileMappingFromApp, HANDLE, (ULONG access, BOOL inherit, LPCWSTR name), (access, inherit, name));
    return OpenFileMappingW(access, inherit, name);
}

/* extended parameters (NUMA node) are ignored; access is what Vista grants */
VX_EXPORT HANDLE WINAPI vx_CreateFileMapping2(HANDLE file, LPSECURITY_ATTRIBUTES sa, ULONG access, ULONG prot, ULONG attrs,
                                              ULONG64 max, PCWSTR name, PVOID ext, ULONG next)
{
    VX_NATIVE(K32, CreateFileMapping2, HANDLE, (HANDLE file, LPSECURITY_ATTRIBUTES sa, ULONG access, ULONG prot, ULONG attrs,
              ULONG64 max, PCWSTR name, PVOID ext, ULONG next), (file, sa, access, prot, attrs, max, name, ext, next));
    return create_mapping(file, sa, prot | attrs, (DWORD)(max >> 32), (DWORD)max, name, 0, FALSE);
}

/* ---- files ----------------------------------------------------------------- */
typedef struct {
    DWORD dwSize, dwFileAttributes, dwFileFlags, dwSecurityQosFlags;
    LPSECURITY_ATTRIBUTES lpSecurityAttributes;
    HANDLE hTemplateFile;
} VX_CREATEFILE2_EXTENDED_PARAMETERS;

VX_EXPORT HANDLE WINAPI vx_CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disp,
        VX_CREATEFILE2_EXTENDED_PARAMETERS *p)
{ VX_NATIVE(K32, CreateFile2, HANDLE, (LPCWSTR name, DWORD access, DWORD share, DWORD disp,         VX_CREATEFILE2_EXTENDED_PARAMETERS *p), (name, access, share, disp, p));
    if (!p) return CreateFileW(name, access, share, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    return CreateFileW(name, access, share, p->lpSecurityAttributes, disp,
                       p->dwFileAttributes | p->dwFileFlags | p->dwSecurityQosFlags, p->hTemplateFile);
}

typedef struct {
    DWORD dwSize, dwCopyFlags;
    BOOL *pfCancel;
    PVOID pProgressRoutine, pvCallbackContext;
} VX_COPYFILE2_EXTENDED_PARAMETERS;

VX_EXPORT HRESULT WINAPI vx_CopyFile2(PCWSTR src, PCWSTR dst, VX_COPYFILE2_EXTENDED_PARAMETERS *p)
{ VX_NATIVE(K32, CopyFile2, HRESULT, (PCWSTR src, PCWSTR dst, VX_COPYFILE2_EXTENDED_PARAMETERS *p), (src, dst, p));
    DWORD flags = p ? p->dwCopyFlags & (COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_RESTARTABLE |
                  COPY_FILE_OPEN_SOURCE_FOR_WRITE | COPY_FILE_ALLOW_DECRYPTED_DESTINATION |
                  COPY_FILE_COPY_SYMLINK | COPY_FILE_NO_BUFFERING) : 0;
    if (CopyFileExW(src, dst, NULL, NULL, p ? p->pfCancel : NULL, flags)) return S_OK;
    return HRESULT_FROM_WIN32(GetLastError());
}

VX_EXPORT DWORD WINAPI vx_GetTempPath2W(DWORD n, LPWSTR buf) { VX_NATIVE(K32, GetTempPath2W, DWORD, (DWORD n, LPWSTR buf), (n, buf)); return GetTempPathW(n, buf); }
VX_EXPORT DWORD WINAPI vx_GetTempPath2A(DWORD n, LPSTR buf) { VX_NATIVE(K32, GetTempPath2A, DWORD, (DWORD n, LPSTR buf), (n, buf)); return GetTempPathA(n, buf); }

/* ---- FileStorageInfo from what Vista can tell ---------------------------------
 * Logical sector size: the volume's (NtQueryVolumeInformationFile,
 * FileFsSizeInformation). Physical sector size and the sector offset: the
 * disk's access-alignment descriptor (IOCTL_STORAGE_QUERY_PROPERTY,
 * StorageAccessAlignmentProperty, Vista and later), asked through the
 * file's volume. A disk or driver that doesn't report it gets the logical
 * size as physical, as Windows 8 does then. The partition's offset is not
 * looked up: reported as unknown (STORAGE_INFO_OFFSET_UNKNOWN). */
typedef struct { LONG_PTR Status; ULONG_PTR Information; } IO_STATUS_BLOCK_;
LONG WINAPI NtQueryInformationFile(HANDLE, IO_STATUS_BLOCK_ *, PVOID, ULONG, int);
LONG WINAPI NtQueryVolumeInformationFile(HANDLE, IO_STATUS_BLOCK_ *, PVOID, ULONG, int);
ULONG WINAPI RtlNtStatusToDosError(LONG);

static BOOL storage_info(HANDLE h, ULONG *u)
{
    struct { LARGE_INTEGER total, avail; ULONG spc, bps; } fs;     /* FILE_FS_SIZE_INFORMATION */
    struct { DWORD ver, size, cacheline, cacheoff, logical, physical, secoff; } ad;   /* STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR */
    struct { DWORD prop, type; BYTE extra[4]; } q = { 6 /* StorageAccessAlignmentProperty */, 0 /* standard query */ };
    IO_STATUS_BLOCK_ iosb;
    WCHAR path[MAX_PATH + 8];
    ULONG logical, physical, secoff = 0xFFFFFFFF;
    DWORD got, n, i;
    LONG st;
    HANDLE vol;
    if ((st = NtQueryVolumeInformationFile(h, &iosb, &fs, sizeof(fs), 3 /* FileFsSizeInformation */)) < 0) {
        SetLastError(RtlNtStatusToDosError(st));
        return FALSE;
    }
    logical = physical = fs.bps;
    /* "\\?\Volume{...}\dir\file" -> "\\?\Volume{...}": the volume, opened
     * with no access rights (enough for this query) */
    n = GetFinalPathNameByHandleW(h, path, MAX_PATH, 1 /* VOLUME_NAME_GUID */);
    if (n && n < MAX_PATH && !vx_wcsnicmp(path, L"\\\\?\\Volume{", 11)) {
        for (i = 11; path[i] && path[i] != '}'; i++) ;
        if (path[i] == '}') {
            path[i + 1] = 0;
            vol = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
            if (vol != INVALID_HANDLE_VALUE) {
                vx_memset(&ad, 0, sizeof(ad));
                if (DeviceIoControl(vol, 0x2D1400 /* IOCTL_STORAGE_QUERY_PROPERTY */, &q, sizeof(q), &ad, sizeof(ad), &got, NULL) &&
                    got >= sizeof(ad) && ad.logical == logical && ad.physical >= logical) {
                    physical = ad.physical;
                    secoff = ad.secoff;
                }
                CloseHandle(vol);
            }
        }
    }
    u[0] = logical;                       /* LogicalBytesPerSector */
    u[1] = physical;                      /* PhysicalBytesPerSectorForAtomicity */
    u[2] = physical;                      /* PhysicalBytesPerSectorForPerformance */
    u[3] = physical;                      /* FileSystemEffectivePhysicalBytesPerSectorForAtomicity */
    u[4] = secoff == 0 ? 1 : 0;           /* Flags: STORAGE_INFO_FLAGS_ALIGNED_DEVICE */
    u[5] = secoff;                        /* ByteOffsetForSectorAlignment */
    u[6] = 0xFFFFFFFF;                    /* ByteOffsetForPartitionAlignment: unknown */
    return TRUE;
}

/* GetFileInformationByHandleEx: Vista knows classes up to 11. Windows 8 added
 * FileIdInfo (18), FileStorageInfo (16) and FileAlignmentInfo (17), which
 * Rust, Python and Chromium use. */
VX_EXPORT BOOL WINAPI vx_GetFileInformationByHandleEx(HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, LPVOID buf, DWORD size)
{
    BY_HANDLE_FILE_INFORMATION bhi;
    DWORD err;
    if (GetFileInformationByHandleEx(h, cls, buf, size)) return TRUE;
    err = GetLastError();
    if (err != ERROR_INVALID_PARAMETER && err != ERROR_NOT_SUPPORTED && err != ERROR_INVALID_FUNCTION &&
        err != ERROR_CALL_NOT_IMPLEMENTED) return FALSE;
    switch ((int)cls) {
    case 18: { /* FileIdInfo: ULONGLONG VolumeSerialNumber; BYTE FileId[16] */
        BYTE *b = buf;
        if (size < 24) { SetLastError(ERROR_BAD_LENGTH); return FALSE; }
        if (!GetFileInformationByHandle(h, &bhi)) return FALSE;
        vx_memset(b, 0, 24);
        *(ULONGLONG *)b = bhi.dwVolumeSerialNumber;
        *(DWORD *)(b + 8) = bhi.nFileIndexLow;
        *(DWORD *)(b + 12) = bhi.nFileIndexHigh;
        return TRUE;
    }
    case 16: /* FileStorageInfo: 7 ULONGs */
        if (size < 28) { SetLastError(ERROR_BAD_LENGTH); return FALSE; }
        return storage_info(h, buf);
    case 17: { /* FileAlignmentInfo = ntdll's FileAlignmentInformation (17), which every NT has */
        IO_STATUS_BLOCK_ iosb;
        ULONG a;
        LONG st;
        if (size < 4) { SetLastError(ERROR_BAD_LENGTH); return FALSE; }
        if ((st = NtQueryInformationFile(h, &iosb, &a, sizeof(a), 17)) < 0) { SetLastError(RtlNtStatusToDosError(st)); return FALSE; }
        *(ULONG *)buf = a;
        return TRUE;
    }
    }
    SetLastError(err);
    return FALSE;
}

/* InitializeCriticalSectionEx: Windows 8 accepts more flags in the high byte
 * (FORCE_DEBUG_INFO and others) than Vista/7, which refuse the call. Programs
 * told they run on Windows 8+ pass them (Chromium's crash reporter does). */
VX_EXPORT BOOL WINAPI vx_InitializeCriticalSectionEx(LPCRITICAL_SECTION cs, DWORD spin, DWORD flags)
{
    if (InitializeCriticalSectionEx(cs, spin, flags)) return TRUE;
    if (GetLastError() != ERROR_INVALID_PARAMETER || !(flags & 0xFF000000)) return FALSE;
    if (InitializeCriticalSectionEx(cs, spin, flags & (0x00FFFFFF | 0x01000000))) return TRUE;  /* keep NO_DEBUG_INFO */
    return InitializeCriticalSectionEx(cs, spin, flags & 0x00FFFFFF);
}

/* Windows 11 24H2: FlsGetValue without touching the last error (Microsoft's
 * newer C runtime uses it when GetProcAddress finds it) */
VX_EXPORT PVOID WINAPI vx_FlsGetValue2(DWORD index)
{
    DWORD e;
    PVOID v;
    VX_NATIVE(K32, FlsGetValue2, PVOID, (DWORD index), (index));
    e = GetLastError();
    v = FlsGetValue(index);
    SetLastError(e);
    return v;
}

/* ---- app model: a Vista process is never a packaged (Store) app ---------- */
#define NO_PACKAGE 15700L   /* APPMODEL_ERROR_NO_PACKAGE */
VX_EXPORT LONG WINAPI vx_GetCurrentPackageId(UINT32 *len, BYTE *buf) { VX_NATIVE(K32, GetCurrentPackageId, LONG, (UINT32 *len, BYTE *buf), (len, buf)); return NO_PACKAGE; }
VX_EXPORT LONG WINAPI vx_GetCurrentPackageFullName(UINT32 *len, PWSTR buf) { VX_NATIVE(K32, GetCurrentPackageFullName, LONG, (UINT32 *len, PWSTR buf), (len, buf)); return NO_PACKAGE; }
VX_EXPORT LONG WINAPI vx_GetCurrentPackageFamilyName(UINT32 *len, PWSTR buf) { VX_NATIVE(K32, GetCurrentPackageFamilyName, LONG, (UINT32 *len, PWSTR buf), (len, buf)); return NO_PACKAGE; }
VX_EXPORT LONG WINAPI vx_GetCurrentPackagePath(UINT32 *len, PWSTR buf) { VX_NATIVE(K32, GetCurrentPackagePath, LONG, (UINT32 *len, PWSTR buf), (len, buf)); return NO_PACKAGE; }
VX_EXPORT LONG WINAPI vx_GetCurrentPackageInfo(UINT32 f, UINT32 *len, BYTE *buf, UINT32 *count) { VX_NATIVE(K32, GetCurrentPackageInfo, LONG, (UINT32 f, UINT32 *len, BYTE *buf, UINT32 *count), (f, len, buf, count)); return NO_PACKAGE; }
VX_EXPORT LONG WINAPI vx_GetCurrentApplicationUserModelId(UINT32 *len, PWSTR buf) { VX_NATIVE(K32, GetCurrentApplicationUserModelId, LONG, (UINT32 *len, PWSTR buf), (len, buf)); return 15703L; /* NO_APPLICATION */ }
VX_EXPORT LONG WINAPI vx_GetPackageFamilyName(HANDLE p, UINT32 *len, PWSTR buf) { VX_NATIVE(K32, GetPackageFamilyName, LONG, (HANDLE p, UINT32 *len, PWSTR buf), (p, len, buf)); return NO_PACKAGE; }
VX_EXPORT LONG WINAPI vx_GetPackageFullName(HANDLE p, UINT32 *len, PWSTR buf) { VX_NATIVE(K32, GetPackageFullName, LONG, (HANDLE p, UINT32 *len, PWSTR buf), (p, len, buf)); return NO_PACKAGE; }
VX_EXPORT LONG WINAPI vx_GetApplicationUserModelId(HANDLE p, UINT32 *len, PWSTR buf) { VX_NATIVE(K32, GetApplicationUserModelId, LONG, (HANDLE p, UINT32 *len, PWSTR buf), (p, len, buf)); return 15703L; }
VX_EXPORT LONG WINAPI vx_GetPackagesByPackageFamily(PCWSTR fam, UINT32 *count, PWSTR *names, UINT32 *len, WCHAR *buf)
{ VX_NATIVE(K32, GetPackagesByPackageFamily, LONG, (PCWSTR fam, UINT32 *count, PWSTR *names, UINT32 *len, WCHAR *buf), (fam, count, names, len, buf));
    if (count) *count = 0;
    if (len) *len = 0;
    return ERROR_SUCCESS;
}
/* Windows 10: the same question with filters (Edge imports it); no packages here either */
VX_EXPORT LONG WINAPI vx_FindPackagesByPackageFamily(PCWSTR fam, UINT32 flags, UINT32 *count, PWSTR *names, UINT32 *len, WCHAR *buf, UINT32 *props)
{ VX_NATIVE(K32, FindPackagesByPackageFamily, LONG, (PCWSTR fam, UINT32 flags, UINT32 *count, PWSTR *names, UINT32 *len, WCHAR *buf, UINT32 *props), (fam, flags, count, names, len, buf, props));
    if (!fam || !count || !len) return ERROR_INVALID_PARAMETER;
    *count = 0;
    *len = 0;
    return ERROR_SUCCESS;
}
/* takes a reference from OpenPackageInfoByFullName, which never gives one here */
VX_EXPORT LONG WINAPI vx_GetPackageApplicationIds(PVOID ref, UINT32 *len, BYTE *buf, UINT32 *count)
{ VX_NATIVE(K32, GetPackageApplicationIds, LONG, (PVOID ref, UINT32 *len, BYTE *buf, UINT32 *count), (ref, len, buf, count));
    if (count) *count = 0;
    return ERROR_INVALID_PARAMETER;
}

VX_EXPORT LONG WINAPI vx_GetPackagePathByFullName(PCWSTR name, UINT32 *len, PWSTR path) { VX_NATIVE(K32, GetPackagePathByFullName, LONG, (PCWSTR name, UINT32 *len, PWSTR path), (name, len, path)); return ERROR_NOT_FOUND; }
VX_EXPORT LONG WINAPI vx_GetStagedPackagePathByFullName(PCWSTR name, UINT32 *len, PWSTR path) { VX_NATIVE(K32, GetStagedPackagePathByFullName, LONG, (PCWSTR name, UINT32 *len, PWSTR path), (name, len, path)); return ERROR_NOT_FOUND; }
VX_EXPORT LONG WINAPI vx_GetPackagePath(const void *id, UINT32 r, UINT32 *len, PWSTR path) { VX_NATIVE(K32, GetPackagePath, LONG, (const void *id, UINT32 r, UINT32 *len, PWSTR path), (id, r, len, path)); return ERROR_NOT_FOUND; }
VX_EXPORT LONG WINAPI vx_OpenPackageInfoByFullName(PCWSTR name, UINT32 r, PVOID *ref) { VX_NATIVE(K32, OpenPackageInfoByFullName, LONG, (PCWSTR name, UINT32 r, PVOID *ref), (name, r, ref)); if (ref) *ref = NULL; return ERROR_NOT_FOUND; }
VX_EXPORT LONG WINAPI vx_ClosePackageInfo(PVOID ref) { VX_NATIVE(K32, ClosePackageInfo, LONG, (PVOID ref), (ref)); return ERROR_SUCCESS; }

/* AppPolicy* (Windows 10): the answers a classic desktop program gets. */
VX_EXPORT LONG WINAPI vx_AppPolicyGetProcessTerminationMethod(HANDLE t, int *p) { VX_NATIVE(K32, AppPolicyGetProcessTerminationMethod, LONG, (HANDLE t, int *p), (t, p)); *p = 0; return ERROR_SUCCESS; }
VX_EXPORT LONG WINAPI vx_AppPolicyGetThreadInitializationType(HANDLE t, int *p) { VX_NATIVE(K32, AppPolicyGetThreadInitializationType, LONG, (HANDLE t, int *p), (t, p)); *p = 0; return ERROR_SUCCESS; }
VX_EXPORT LONG WINAPI vx_AppPolicyGetWindowingModel(HANDLE t, int *p) { VX_NATIVE(K32, AppPolicyGetWindowingModel, LONG, (HANDLE t, int *p), (t, p)); *p = 2; /* ClassicDesktop */ return ERROR_SUCCESS; }
VX_EXPORT LONG WINAPI vx_AppPolicyGetShowDeveloperDiagnostic(HANDLE t, int *p) { VX_NATIVE(K32, AppPolicyGetShowDeveloperDiagnostic, LONG, (HANDLE t, int *p), (t, p)); *p = 1; return ERROR_SUCCESS; }
VX_EXPORT LONG WINAPI vx_AppPolicyGetMediaFoundationCodecLoading(HANDLE t, int *p) { VX_NATIVE(K32, AppPolicyGetMediaFoundationCodecLoading, LONG, (HANDLE t, int *p), (t, p)); *p = 0; return ERROR_SUCCESS; }
VX_EXPORT LONG WINAPI vx_AppPolicyGetCreateFileAccess(HANDLE t, int *p) { VX_NATIVE(K32, AppPolicyGetCreateFileAccess, LONG, (HANDLE t, int *p), (t, p)); *p = 0; return ERROR_SUCCESS; }
VX_EXPORT LONG WINAPI vx_AppPolicyGetClrCompat(HANDLE t, int *p) { VX_NATIVE(K32, AppPolicyGetClrCompat, LONG, (HANDLE t, int *p), (t, p)); *p = 1; return ERROR_SUCCESS; }
VX_EXPORT LONG WINAPI vx_AppPolicyGetLifecycleManagement(HANDLE t, int *p) { VX_NATIVE(K32, AppPolicyGetLifecycleManagement, LONG, (HANDLE t, int *p), (t, p)); *p = 0; return ERROR_SUCCESS; }

/* ---- locale ---------------------------------------------------------------- */
#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
/* ResolveLocaleName (Windows 7): the specific locale that best matches a
 * name. Vista's LocaleNameToLCID knows specific names ("de-AT") but not
 * every language-only one ("de"), nor names with unknown parts ("en-XX").
 * So: drop parts from the end until a name is known; if only the language
 * is left and Vista doesn't know it alone, find the language among the
 * installed locales. Then ConvertDefaultLocale (Vista) turns a language into
 * its default locale ("de" -> "de-DE", "en" -> "en-US"), as Windows 7 does. */
typedef struct { const WCHAR *lang; SIZE_T n; LCID lcid; } lang_find;

static BOOL CALLBACK find_lang(LPWSTR name, DWORD flags, LPARAM lp)
{
    lang_find *f = (lang_find *)lp;
    if (!vx_wcsnicmp(name, f->lang, f->n) && (name[f->n] == '-' || !name[f->n])) {
        f->lcid = LocaleNameToLCID(name, 0);
        if (f->lcid) return FALSE;                    /* found: stop */
    }
    return TRUE;
}

VX_EXPORT int WINAPI vx_ResolveLocaleName(LPCWSTR name, LPWSTR out, int cch)
{ VX_NATIVE(K32, ResolveLocaleName, int, (LPCWSTR name, LPWSTR out, int cch), (name, out, cch));
    WCHAR tmp[LOCALE_NAME_MAX_LENGTH];
    LCID lcid = 0;
    SIZE_T n;
    if (cch < 0 || (cch && !out)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (!name) {                                       /* the user's default locale */
        if (!GetUserDefaultLocaleName(tmp, LOCALE_NAME_MAX_LENGTH)) return 0;
        name = tmp;
    }
    if (!name[0]) {                                    /* the invariant locale: "" */
        if (!cch) return 1;
        out[0] = 0;
        return 1;
    }
    n = vx_wcslen(name);
    if (n >= LOCALE_NAME_MAX_LENGTH) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    if (name != tmp) vx_wcscpyn(tmp, name, LOCALE_NAME_MAX_LENGTH);
    for (;;) {
        if ((lcid = LocaleNameToLCID(tmp, 0))) break;
        while (n && tmp[n - 1] != '-') n--;            /* drop the last "-part" */
        if (!n) break;
        tmp[--n] = 0;
    }
    if (!lcid) {
        lang_find f;
        SIZE_T k = 0;
        while (name[k] && name[k] != '-') k++;
        f.lang = name; f.n = k; f.lcid = 0;
        if (k) EnumSystemLocalesEx(find_lang, 1 /* LOCALE_WINDOWS */, (LPARAM)&f, NULL);
        /* only the language is known to match: its default locale, not
         * whichever of its locales was listed first ("en-029") */
        if (f.lcid) lcid = MAKELCID(MAKELANGID(PRIMARYLANGID(LANGIDFROMLCID(f.lcid)), SUBLANG_NEUTRAL), SORT_DEFAULT);
    }
    if (!lcid) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    /* a language alone ("de", sublanguage neutral) -> its default locale */
    if (SUBLANGID(LANGIDFROMLCID(lcid)) == SUBLANG_NEUTRAL)
        lcid = ConvertDefaultLocale(MAKELCID(MAKELANGID(PRIMARYLANGID(LANGIDFROMLCID(lcid)), SUBLANG_NEUTRAL), SORT_DEFAULT));
    return LCIDToLocaleName(lcid, out, cch, 0);
}
#endif

VX_EXPORT int WINAPI vx_GetUserDefaultGeoName(LPWSTR buf, int cch)
{ VX_NATIVE(K32, GetUserDefaultGeoName, int, (LPWSTR buf, int cch), (buf, cch));
    int n = GetGeoInfoW(GetUserGeoID(GEOCLASS_NATION), GEO_ISO2, buf, cch, 0);
    if (!n) {
        if (buf && cch >= 4) { vx_wcscpyn(buf, L"001", cch); return 4; }
        if (!buf || !cch) return 4;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
    }
    return n;
}

/* FindStringOrdinal (Windows 7) */
#define VX_FIND_STARTSWITH 0x00100000
#define VX_FIND_ENDSWITH   0x00200000
#define VX_FIND_FROMSTART  0x00400000
#define VX_FIND_FROMEND    0x00800000

#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT int WINAPI vx_FindStringOrdinal(DWORD flags, LPCWSTR src, int cs, LPCWSTR val, int cv, BOOL ic)
{ VX_NATIVE(K32, FindStringOrdinal, int, (DWORD flags, LPCWSTR src, int cs, LPCWSTR val, int cv, BOOL ic), (flags, src, cs, val, cv, ic));
    int i;
    if (!src || !val) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }
    if (cs < 0) cs = (int)vx_wcslen(src);
    if (cv < 0) cv = (int)vx_wcslen(val);
    SetLastError(ERROR_SUCCESS);
    if (cv > cs) return -1;
    switch (flags) {
    case VX_FIND_STARTSWITH:
        return CompareStringOrdinal(src, cv, val, cv, ic) == CSTR_EQUAL ? 0 : -1;
    case VX_FIND_ENDSWITH:
        return CompareStringOrdinal(src + cs - cv, cv, val, cv, ic) == CSTR_EQUAL ? cs - cv : -1;
    case VX_FIND_FROMSTART:
        for (i = 0; i <= cs - cv; i++)
            if (CompareStringOrdinal(src + i, cv, val, cv, ic) == CSTR_EQUAL) return i;
        return -1;
    case VX_FIND_FROMEND:
        for (i = cs - cv; i >= 0; i--)
            if (CompareStringOrdinal(src + i, cv, val, cv, ic) == CSTR_EQUAL) return i;
        return -1;
    }
    SetLastError(ERROR_INVALID_FLAGS);
    return -1;
}
#endif

/* ---- time zones (Windows 8): enumerate the registry database ------------- */
typedef struct { LONG Bias, StandardBias, DaylightBias; SYSTEMTIME StandardDate, DaylightDate; } VX_REG_TZI;

static void read_sz(HKEY k, const WCHAR *v, WCHAR *out, DWORD cch)
{
    DWORD cb = (cch - 1) * sizeof(WCHAR), type;
    out[0] = 0;
    if (!RegQueryValueExW(k, v, NULL, &type, (BYTE *)out, &cb) && type == REG_SZ)
        out[cb / sizeof(WCHAR)] = 0;
    else out[0] = 0;
}

VX_EXPORT DWORD WINAPI vx_EnumDynamicTimeZoneInformation(DWORD index, PDYNAMIC_TIME_ZONE_INFORMATION tz)
{ VX_NATIVE(K32, EnumDynamicTimeZoneInformation, DWORD, (DWORD index, PDYNAMIC_TIME_ZONE_INFORMATION tz), (index, tz));
    HKEY root, k;
    WCHAR name[128];
    DWORD cch = 128, cb, type;
    VX_REG_TZI tzi;
    LONG r;
    if (!tz) return ERROR_INVALID_PARAMETER;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones",
                      0, KEY_READ, &root)) return ERROR_NO_MORE_ITEMS;
    r = RegEnumKeyExW(root, index, name, &cch, NULL, NULL, NULL, NULL);
    if (r) { RegCloseKey(root); return ERROR_NO_MORE_ITEMS; }
    r = RegOpenKeyExW(root, name, 0, KEY_READ, &k);
    RegCloseKey(root);
    if (r) return r;
    vx_memset(tz, 0, sizeof(*tz));
    vx_wcscpyn(tz->TimeZoneKeyName, name, 128);
    read_sz(k, L"Std", tz->StandardName, 32);
    read_sz(k, L"Dlt", tz->DaylightName, 32);
    cb = sizeof(tzi);
    if (!RegQueryValueExW(k, L"TZI", NULL, &type, (BYTE *)&tzi, &cb) && cb == sizeof(tzi)) {
        tz->Bias = tzi.Bias;
        tz->StandardBias = tzi.StandardBias;
        tz->DaylightBias = tzi.DaylightBias;
        tz->StandardDate = tzi.StandardDate;
        tz->DaylightDate = tzi.DaylightDate;
    }
    RegCloseKey(k);
    return ERROR_SUCCESS;
}

/* ---- pseudo console (Windows 10 1809): not possible on Vista ------------ */
VX_EXPORT HRESULT WINAPI vx_CreatePseudoConsole(COORD size, HANDLE in, HANDLE out, DWORD flags, PVOID *pc)
{ VX_NATIVE(K32, CreatePseudoConsole, HRESULT, (COORD size, HANDLE in, HANDLE out, DWORD flags, PVOID *pc), (size, in, out, flags, pc));
    if (pc) *pc = NULL;
    return E_NOTIMPL;
}
VX_EXPORT HRESULT WINAPI vx_ResizePseudoConsole(PVOID pc, COORD size) { VX_NATIVE(K32, ResizePseudoConsole, HRESULT, (PVOID pc, COORD size), (pc, size)); return E_NOTIMPL; }
VX_EXPORT void WINAPI vx_ClosePseudoConsole(PVOID pc) { VX_NATIVE_VOID(K32, ClosePseudoConsole, (PVOID pc), (pc)); }

/* ---- error reporting (Windows 7+) ---------------------------------------- */
#if VX_OS < 601   /* Windows 7 and later have it: their .def forwards there */
VX_EXPORT HRESULT WINAPI vx_WerRegisterRuntimeExceptionModule(PCWSTR dll, PVOID ctx) { VX_NATIVE(K32, WerRegisterRuntimeExceptionModule, HRESULT, (PCWSTR dll, PVOID ctx), (dll, ctx)); return S_OK; }
VX_EXPORT HRESULT WINAPI vx_WerUnregisterRuntimeExceptionModule(PCWSTR dll, PVOID ctx) { VX_NATIVE(K32, WerUnregisterRuntimeExceptionModule, HRESULT, (PCWSTR dll, PVOID ctx), (dll, ctx)); return S_OK; }
#endif
VX_EXPORT HRESULT WINAPI vx_WerRegisterExcludedMemoryBlock(const void *a, DWORD s) { VX_NATIVE(K32, WerRegisterExcludedMemoryBlock, HRESULT, (const void *a, DWORD s), (a, s)); return S_OK; }
VX_EXPORT HRESULT WINAPI vx_WerUnregisterExcludedMemoryBlock(const void *a) { VX_NATIVE(K32, WerUnregisterExcludedMemoryBlock, HRESULT, (const void *a), (a)); return S_OK; }
VX_EXPORT HRESULT WINAPI vx_WerRegisterCustomMetadata(PCWSTR k, PCWSTR v) { VX_NATIVE(K32, WerRegisterCustomMetadata, HRESULT, (PCWSTR k, PCWSTR v), (k, v)); return S_OK; }
VX_EXPORT HRESULT WINAPI vx_WerUnregisterCustomMetadata(PCWSTR k) { VX_NATIVE(K32, WerUnregisterCustomMetadata, HRESULT, (PCWSTR k), (k)); return S_OK; }

/* ---- starting child processes -----------------------------------------------
 * A program told it runs on Windows 10 asks CreateProcess for attributes that
 * only Windows 8/10 know (job lists, child-process policy, desktop-app policy,
 * the 64-bit mitigation-policy mask, ...). Vista/7 refuse them, the child is
 * never started, and programs like Chromium give up ("GPU process isn't
 * usable"). Such attributes are left out instead: the child starts without
 * those extra restrictions, as it would on this Windows anyway. With
 * VX_TRACE=1 each change is reported. */
#define VX_ATTR_MITIGATION 0x00020007

static void trace_hex(const char *what, DWORD v)
{
    char b[96];
    int n = 0, i;
    while (*what && n < 80) b[n++] = *what++;
    b[n++] = ' '; b[n++] = '0'; b[n++] = 'x';
    for (i = 7; i >= 0; i--) b[n++] = "0123456789abcdef"[(v >> (i * 4)) & 15];
    b[n] = 0;
    vx_trace(b);
}

/* the attributes this build's Windows knows (Vista: the first two; Windows
 * 7 adds processor groups, NUMA, UMS and the mitigation policy; 8 adds
 * AppContainer security capabilities) */
static BOOL attr_known_here(DWORD_PTR attr)
{
    switch (attr) {
    case 0x00020000:            /* PARENT_PROCESS */
    case 0x00020002:            /* HANDLE_LIST */
        return TRUE;
    case 0x00030003:            /* GROUP_AFFINITY */
    case 0x00020004:            /* PREFERRED_NODE */
    case 0x00030005:            /* IDEAL_PROCESSOR */
    case 0x00030006:            /* UMS_THREAD */
    case VX_ATTR_MITIGATION:
        return VX_OS >= 601;
    case 0x00020009:            /* SECURITY_CAPABILITIES */
        return VX_OS >= 602;
    default:
        return FALSE;
    }
}

LPWSTR WINAPI vx_GetCommandLineW(void);          /* main.c: with the extra start-up options */

/* Only a program that runs its children without a sandbox ("--no-sandbox",
 * which VistaExt gives Chromium/Electron programs) gets the lenient
 * handling: for it these attributes are only extra hardening of children
 * that must start. A program WITH a sandbox relies on them: told "done"
 * without them, its sandboxed child notices and stops (Firefox's content
 * processes did), so there they fail exactly as on this Windows. */
static BOOL lenient_attributes(void)
{
    static int on = -1;
    if (on < 0) {
        const WCHAR *c = vx_GetCommandLineW();
        /* VX_LENIENT_CHILDREN=1 asks for it for any program (and its children) */
        on = GetEnvironmentVariableW(L"VX_LENIENT_CHILDREN", NULL, 0) != 0;
        for (; !on && c && *c; c++) if (!vx_wcsnicmp(c, L"--no-sandbox", 12)) { on = 1; break; }
    }
    return on;
}

VX_EXPORT BOOL WINAPI vx_UpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST list, DWORD flags, DWORD_PTR attr,
                                                   PVOID value, SIZE_T size, PVOID prev, PSIZE_T retsize)
{
    if (UpdateProcThreadAttribute(list, flags, attr, value, size, prev, retsize)) return TRUE;
    if (!lenient_attributes()) return FALSE;
#if VX_WIN7
    if (attr == VX_ATTR_MITIGATION && value && size > sizeof(DWORD)) {
        /* Windows 7 takes a 32-bit mask with DEP / ATL thunk / SEHOP only. The
         * list keeps a pointer to the value, so it must live on: lists are
         * few and small. */
        DWORD *keep = HeapAlloc(GetProcessHeap(), 0, sizeof(DWORD));
        if (keep) {
            *keep = (DWORD)(*(ULONGLONG *)value & 0x7);
            trace_hex("UpdateProcThreadAttribute: mitigation mask reduced to Windows 7's, was", (DWORD)*(ULONGLONG *)value);
            if (!*keep) { SetLastError(ERROR_SUCCESS); return TRUE; }
            if (UpdateProcThreadAttribute(list, flags, attr, keep, sizeof(DWORD), NULL, NULL)) return TRUE;
        }
    }
#elif VX_WIN8
    if (attr == VX_ATTR_MITIGATION && value && size >= sizeof(ULONGLONG)) {
        /* Windows 8 takes a 64-bit mask with its own policies: DEP, ATL
         * thunks, SEHOP (0x7), forced relocation, heap termination, bottom-up
         * and high-entropy ASLR, strict handle checks, win32k calls and
         * extension points (2 bits each from bit 8 to 33). Later ones (8.1's
         * dynamic code and CFG, Windows 10's) are left out. Not yet verified
         * on Windows 8. */
        ULONGLONG *keep = HeapAlloc(GetProcessHeap(), 0, sizeof(ULONGLONG));
        if (keep) {
            *keep = *(ULONGLONG *)value & 0x333333307ULL;
            trace_hex("UpdateProcThreadAttribute: mitigation mask reduced to Windows 8's, was", (DWORD)*(ULONGLONG *)value);
            if (!*keep) { SetLastError(ERROR_SUCCESS); return TRUE; }
            if (UpdateProcThreadAttribute(list, flags, attr, keep, sizeof(ULONGLONG), NULL, NULL)) return TRUE;
        }
    }
#endif
    if (!attr_known_here(attr) || attr == VX_ATTR_MITIGATION) {
        trace_hex("UpdateProcThreadAttribute: left out attribute this Windows lacks:", (DWORD)attr);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    return FALSE;
}

/* ---- a program about to start another one ------------------------------------
 * A setup run by VistaExt often unpacks its real installer or a helper into
 * Temp and starts it right away; an updater starts the new version it just
 * put in place. Before the child starts, VistaExt's service is asked
 * (\\.\pipe\VistaExt "PREPARE <path>") to look at it: if it cannot run on
 * this Windows by itself, it is registered for VistaExt like its parent was,
 * before Windows loads it. Nothing is asked for Windows' own programs, for
 * programs already registered, or when the service is not there. */
static BOOL resolve_child(LPCWSTR file, LPCWSTR dir, WCHAR *out)
{
    WCHAR tok[MAX_PATH], tmp[MAX_PATH];
    SIZE_T n = 0;
    BOOL quoted;
    WCHAR *fp;
    if (!file) return FALSE;
    while (*file == ' ' || *file == '\t') file++;
    quoted = *file == '"';
    if (quoted) file++;
    while (*file && n < MAX_PATH - 5 && (quoted ? *file != '"' : (*file != ' ' && *file != '\t'))) tok[n++] = *file++;
    tok[n] = 0;
    if (!n) return FALSE;
    {   /* no extension: Windows tries ".exe" */
        SIZE_T i = n;
        while (i && tok[i - 1] != '.' && tok[i - 1] != '\\' && tok[i - 1] != '/') i--;
        if (!i || tok[i - 1] != '.') vx_wcscatn(tok, L".exe", MAX_PATH);
    }
    if (tok[1] == ':' || (tok[0] == '\\' && tok[1] == '\\')) {
        if (!GetFullPathNameW(tok, MAX_PATH, tmp, &fp)) return FALSE;
    } else if (dir && dir[0] && (tok[0] != '\\')) {
        WCHAR both[MAX_PATH * 2];            /* no wsprintf: kernelvx does not load user32 */
        vx_wcscpyn(both, dir, MAX_PATH * 2);
        vx_wcscatn(both, L"\\", MAX_PATH * 2);
        vx_wcscatn(both, tok, MAX_PATH * 2);
        if (GetFileAttributesW(both) == INVALID_FILE_ATTRIBUTES || !GetFullPathNameW(both, MAX_PATH, tmp, &fp))
            if (!SearchPathW(NULL, tok, NULL, MAX_PATH, tmp, &fp)) return FALSE;
    } else if (!SearchPathW(NULL, tok, NULL, MAX_PATH, tmp, &fp)) return FALSE;
    if (!GetLongPathNameW(tmp, out, MAX_PATH)) vx_wcscpyn(out, tmp, MAX_PATH);
    return TRUE;
}

static BOOL reg_has(const WCHAR *key, const WCHAR *value, DWORD *dw)
{
    HKEY k;
    DWORD cb = sizeof(*dw), type;
    LONG r;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) return FALSE;
    r = RegQueryValueExW(k, value, NULL, &type, dw ? (BYTE *)dw : NULL, dw ? &cb : NULL);
    RegCloseKey(k);
    return !r;
}

/* this program was registered as a setup program (its registration says
 * "setup"): the new helpers it starts are told the same version */
static BOOL i_am_setup(void)
{
    static int r = -1;
    if (r < 0) {
        WCHAR me[MAX_PATH], data[16] = L"";
        HKEY k;
        DWORD cb = sizeof(data) - sizeof(WCHAR), type;
        r = 0;
        if (GetModuleFileNameW(NULL, me, MAX_PATH) && GetLongPathNameW(me, me, MAX_PATH) &&
            !RegOpenKeyExW(HKEY_LOCAL_MACHINE, VX_REG_ROOT L"\\Memory", 0, KEY_QUERY_VALUE | VX_REG_VIEW, &k)) {
            if (!RegQueryValueExW(k, me, NULL, &type, (BYTE *)data, &cb) && type == REG_SZ) r = !vx_wcsicmp(data, L"setup");
            RegCloseKey(k);
        }
    }
    return r;
}

VX_EXPORT void WINAPI vx_VxPrepareChild(LPCWSTR file, LPCWSTR dir)
{
    WCHAR path[MAX_PATH], win[MAX_PATH], req[MAX_PATH + 16], rep[64];
    DWORD got;
    SIZE_T n;
    if (!resolve_child(file, dir, path)) return;
    n = vx_wcslen(path);
    if (n < 5 || vx_wcsnicmp(path + n - 4, L".exe", 4)) return;
    if (GetWindowsDirectoryW(win, MAX_PATH) && !vx_wcsnicmp(path, win, vx_wcslen(win)) && path[vx_wcslen(win)] == '\\') return;
    if (!vx_setting(L"Enabled", 1)) return;
    if (reg_has(VX_REG_ROOT L"\\Memory", path, NULL)) return;     /* already registered */
    vx_wcscpyn(req, i_am_setup() ? L"PREPARE setup " : L"PREPARE ", MAX_PATH + 16);
    vx_wcscatn(req, path, MAX_PATH + 16);
    VX_TRACE("asking VistaExt's service about a program about to start");
    /* the time-out is only for a free pipe instance (the service makes the
     * next one right after each connection); the service bounds its own work
     * (5 s for its lock) and answers "runs as it is" when busy */
    CallNamedPipeW(L"\\\\.\\pipe\\VistaExt", req, (DWORD)((vx_wcslen(req) + 1) * sizeof(WCHAR)), rep, sizeof(rep), &got, 3000);
}

static void prepare_child(LPCWSTR app, LPCWSTR cmd, LPCWSTR dir)
{
    DWORD e = GetLastError();
    vx_VxPrepareChild(app ? app : cmd, app ? NULL : dir);
    SetLastError(e);
}

VX_EXPORT BOOL WINAPI vx_CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                        BOOL inherit, DWORD flags, LPVOID env, LPCSTR dir, LPSTARTUPINFOA si,
                                        LPPROCESS_INFORMATION pi)
{
    WCHAR wa[MAX_PATH], wc[MAX_PATH], wd[MAX_PATH];
    BOOL ha = app && MultiByteToWideChar(CP_ACP, 0, app, -1, wa, MAX_PATH);
    BOOL hc = cmd && MultiByteToWideChar(CP_ACP, 0, cmd, -1, wc, MAX_PATH);
    BOOL hd = dir && MultiByteToWideChar(CP_ACP, 0, dir, -1, wd, MAX_PATH);
    if (!hc) wc[0] = 0;
    prepare_child(ha ? wa : NULL, hc ? wc : NULL, hd ? wd : NULL);
    return CreateProcessA(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
}

VX_EXPORT BOOL WINAPI vx_CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                                        BOOL inherit, DWORD flags, LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si,
                                        LPPROCESS_INFORMATION pi)
{
    BOOL ok;
    prepare_child(app, cmd, dir);
    ok = CreateProcessW(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
    if (!ok) {
        DWORD e = GetLastError();
        trace_hex("CreateProcessW failed, error", e);
        SetLastError(e);
    }
    return ok;
}
