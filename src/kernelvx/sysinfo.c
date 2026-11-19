/* kernelvx - system information: processor groups, timers, firmware. */
#include "../common/vx.h"

/* ---- time ------------------------------------------------------------------
 * GetSystemTimePreciseAsFileTime (Windows 8). The normal clock only moves
 * once per timer tick (every 15.6 ms, or 1 ms while a program asked for a
 * finer timer), which makes time measured with it jump. Between ticks the
 * performance counter interpolates: each new tick value is paired with the
 * counter reading at the moment it was first seen, and the result is never
 * allowed to run past the next tick, so it stays within a tick of the
 * normal clock and follows it when the clock is set.
 *
 * Programs call this often and expect it to be cheap (on Windows 8 it is a
 * few nanoseconds). Where the performance counter is a slow hardware timer
 * (HPET or ACPI on many Vista-era PCs, and in virtual machines: measured
 * 6 us per call) interpolating would make every call that slow, so there it
 * returns the normal clock, which is what these programs get on Windows 7. */
static struct {
    volatile LONG seq;              /* odd while being updated */
    ULONGLONG tick;                 /* system time of the current tick */
    LONGLONG qpc;                   /* counter when that tick was first seen */
} g_clk;
static LONGLONG g_qpf;
static volatile LONG g_clk_mode;    /* 0 not decided, 1 interpolate, 2 normal clock only */

static LONG decide_clock(void)
{
    LARGE_INTEGER f, a, b;
    int i;
    LONG mode = 2;
    if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) {
        g_qpf = f.QuadPart;
        QueryPerformanceCounter(&a);
        for (i = 0; i < 16; i++) QueryPerformanceCounter(&b);
        /* 16 calls in under 16 * 300 ns: cheap enough */
        if ((b.QuadPart - a.QuadPart) * 10000000 < f.QuadPart * 48) mode = 1;
    }
    InterlockedExchange(&g_clk_mode, mode);
    return mode;
}

static ULONGLONG ft64(const FILETIME *f) { return ((ULONGLONG)f->dwHighDateTime << 32) | f->dwLowDateTime; }

/* counter ticks -> 100 ns units, for spans below a few seconds */
static ULONGLONG to_100ns(ULONGLONG d)
{
    ULONGLONG f = (ULONGLONG)g_qpf;
    if (d >= f) return 10000000;    /* a second or more: clamped by the caller anyway */
#ifdef _WIN64
    return d * 10000000 / f;        /* d < f: no overflow below 1.8 THz */
#else
    while (f > 0x7FFFFFFF) { f >>= 1; d >>= 1; }     /* MulDiv takes ints (no 64-bit division here) */
    return (ULONGLONG)(DWORD)MulDiv((int)d, 10000000, (int)f);
#endif
}

VX_EXPORT void WINAPI vx_GetSystemTimePreciseAsFileTime(LPFILETIME ft)
{ VX_NATIVE_VOID(K32, GetSystemTimePreciseAsFileTime, (LPFILETIME ft), (ft));
    FILETIME c;
    LARGE_INTEGER q;
    ULONGLONG coarse, tick, est;
    LONGLONG qpc;
    LONG s, mode = g_clk_mode;
    if (!mode) mode = decide_clock();
    if (mode != 1) { GetSystemTimeAsFileTime(ft); return; }
    GetSystemTimeAsFileTime(&c);
    QueryPerformanceCounter(&q);
    coarse = ft64(&c);
    s = g_clk.seq;
    MemoryBarrier();
    tick = g_clk.tick;
    qpc = g_clk.qpc;
    MemoryBarrier();
    if ((s & 1) || g_clk.seq != s || tick != coarse || q.QuadPart < qpc) {
        /* a new tick (or a first call, or another thread updating): this
         * call returns the tick itself and, if nobody else is at it, records
         * when it was first seen */
        if (!(s & 1) && tick != coarse && InterlockedCompareExchange(&g_clk.seq, s + 1, s) == s) {
            g_clk.tick = coarse;
            g_clk.qpc = q.QuadPart;
            InterlockedExchange(&g_clk.seq, s + 2);
        }
        *ft = c;
        return;
    }
    est = coarse + to_100ns((ULONGLONG)(q.QuadPart - qpc));
    /* never past the next tick (the largest tick Windows uses is 15.6 ms) */
    if (est > coarse + 156249) est = coarse + 156249;
    ft->dwLowDateTime = (DWORD)est;
    ft->dwHighDateTime = (DWORD)(est >> 32);
}

/* KUSER_SHARED_DATA.InterruptTime, readable from user mode on every NT. */
static ULONGLONG interrupt_time(void)
{
    volatile LONG *p = (volatile LONG *)(ULONG_PTR)0x7FFE0008;
    LONG hi1, hi2;
    ULONG lo;
    do { hi1 = p[1]; lo = (ULONG)p[0]; hi2 = p[2]; } while (hi1 != hi2);
    return ((ULONGLONG)(ULONG)hi1 << 32) | lo;
}

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT BOOL WINAPI vx_QueryUnbiasedInterruptTime(PULONGLONG t)
{ VX_NATIVE(K32, QueryUnbiasedInterruptTime, BOOL, (PULONGLONG t), (t));
    if (!t) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *t = interrupt_time();
    return TRUE;
}
#endif
VX_EXPORT void WINAPI vx_QueryInterruptTime(PULONGLONG t) { VX_NATIVE_VOID(K32, QueryInterruptTime, (PULONGLONG t), (t)); *t = interrupt_time(); }
VX_EXPORT void WINAPI vx_QueryInterruptTimePrecise(PULONGLONG t) { VX_NATIVE_VOID(K32, QueryInterruptTimePrecise, (PULONGLONG t), (t)); *t = interrupt_time(); }
VX_EXPORT void WINAPI vx_QueryUnbiasedInterruptTimePrecise(PULONGLONG t) { VX_NATIVE_VOID(K32, QueryUnbiasedInterruptTimePrecise, (PULONGLONG t), (t)); *t = interrupt_time(); }

/* ---- processor groups (Windows 7). Vista always has exactly one group. -- */
static DWORD ncpu(void) { SYSTEM_INFO si; GetSystemInfo(&si); return si.dwNumberOfProcessors; }

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT DWORD WINAPI vx_GetActiveProcessorCount(WORD group)
{ VX_NATIVE(K32, GetActiveProcessorCount, DWORD, (WORD group), (group));
    if (group && group != ALL_PROCESSOR_GROUPS) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    return ncpu();
}
VX_EXPORT DWORD WINAPI vx_GetMaximumProcessorCount(WORD group) { VX_NATIVE(K32, GetMaximumProcessorCount, DWORD, (WORD group), (group)); return vx_GetActiveProcessorCount(group); }
VX_EXPORT WORD WINAPI vx_GetActiveProcessorGroupCount(void) { VX_NATIVE(K32, GetActiveProcessorGroupCount, WORD, (void), ()); return 1; }
VX_EXPORT WORD WINAPI vx_GetMaximumProcessorGroupCount(void) { VX_NATIVE(K32, GetMaximumProcessorGroupCount, WORD, (void), ()); return 1; }
#endif

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT void WINAPI vx_GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER p)
{ VX_NATIVE_VOID(K32, GetCurrentProcessorNumberEx, (PPROCESSOR_NUMBER p), (p));
    p->Group = 0;
    p->Number = (BYTE)GetCurrentProcessorNumber();
    p->Reserved = 0;
}
#endif

typedef struct {
    LONG ExitStatus; PVOID TebBaseAddress; PVOID UniqueProcess, UniqueThread;
    ULONG_PTR AffinityMask; LONG Priority, BasePriority;
} VX_THREAD_BASIC_INFORMATION;
LONG WINAPI NtQueryInformationThread(HANDLE, int, PVOID, ULONG, PULONG);

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT BOOL WINAPI vx_GetThreadGroupAffinity(HANDLE h, PGROUP_AFFINITY ga)
{ VX_NATIVE(K32, GetThreadGroupAffinity, BOOL, (HANDLE h, PGROUP_AFFINITY ga), (h, ga));
    VX_THREAD_BASIC_INFORMATION tbi;
    if (NtQueryInformationThread(h, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), NULL) < 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    vx_memset(ga, 0, sizeof(*ga));
    ga->Mask = tbi.AffinityMask;
    return TRUE;
}
#endif

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT BOOL WINAPI vx_SetThreadGroupAffinity(HANDLE h, const GROUP_AFFINITY *ga, PGROUP_AFFINITY prev)
{ VX_NATIVE(K32, SetThreadGroupAffinity, BOOL, (HANDLE h, const GROUP_AFFINITY *ga, PGROUP_AFFINITY prev), (h, ga, prev));
    DWORD_PTR old;
    if (!ga || ga->Group) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    old = SetThreadAffinityMask(h, ga->Mask);
    if (!old) return FALSE;
    if (prev) { vx_memset(prev, 0, sizeof(*prev)); prev->Mask = old; }
    return TRUE;
}
#endif

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT BOOL WINAPI vx_GetProcessGroupAffinity(HANDLE h, PUSHORT count, PUSHORT groups)
{ VX_NATIVE(K32, GetProcessGroupAffinity, BOOL, (HANDLE h, PUSHORT count, PUSHORT groups), (h, count, groups));
    if (!count) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (*count < 1 || !groups) { *count = 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    *count = 1;
    groups[0] = 0;
    return TRUE;
}
#endif

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT BOOL WINAPI vx_GetNumaNodeProcessorMaskEx(USHORT node, PGROUP_AFFINITY ga)
{ VX_NATIVE(K32, GetNumaNodeProcessorMaskEx, BOOL, (USHORT node, PGROUP_AFFINITY ga), (node, ga));
    ULONGLONG m;
    if (!GetNumaNodeProcessorMask((UCHAR)node, &m)) return FALSE;
    vx_memset(ga, 0, sizeof(*ga));
    ga->Mask = (KAFFINITY)m;
    return TRUE;
}
#endif

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT BOOL WINAPI vx_GetNumaProximityNodeEx(ULONG prox, PUSHORT node)
{ VX_NATIVE(K32, GetNumaProximityNodeEx, BOOL, (ULONG prox, PUSHORT node), (prox, node));
    if (prox) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *node = 0;
    return TRUE;
}
#endif

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT BOOL WINAPI vx_GetNumaProcessorNodeEx(PPROCESSOR_NUMBER p, PUSHORT node)
{ VX_NATIVE(K32, GetNumaProcessorNodeEx, BOOL, (PPROCESSOR_NUMBER p, PUSHORT node), (p, node));
    UCHAR n = 0;
    if (!p || !node || p->Group) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!GetNumaProcessorNode(p->Number, &n)) { *node = 0xFFFF; return FALSE; }
    *node = n;
    return TRUE;
}
#endif

/* SetThreadIdealProcessor(h, MAXIMUM_PROCESSORS) reads the current one */
#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT BOOL WINAPI vx_GetThreadIdealProcessorEx(HANDLE h, PPROCESSOR_NUMBER p)
{ VX_NATIVE(K32, GetThreadIdealProcessorEx, BOOL, (HANDLE h, PPROCESSOR_NUMBER p), (h, p));
    DWORD cur = SetThreadIdealProcessor(h, MAXIMUM_PROCESSORS);
    if (cur == (DWORD)-1 || !p) return FALSE;
    p->Group = 0; p->Number = (BYTE)cur; p->Reserved = 0;
    return TRUE;
}
#endif

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT BOOL WINAPI vx_SetThreadIdealProcessorEx(HANDLE h, PPROCESSOR_NUMBER ideal, PPROCESSOR_NUMBER prev)
{ VX_NATIVE(K32, SetThreadIdealProcessorEx, BOOL, (HANDLE h, PPROCESSOR_NUMBER ideal, PPROCESSOR_NUMBER prev), (h, ideal, prev));
    DWORD old;
    if (!ideal || ideal->Group) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if ((old = SetThreadIdealProcessor(h, ideal->Number)) == (DWORD)-1) return FALSE;
    if (prev) { prev->Group = 0; prev->Number = (BYTE)old; prev->Reserved = 0; }
    return TRUE;
}
#endif

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT BOOL WINAPI vx_GetNumaNodeNumberFromHandle(HANDLE h, PUSHORT node) { VX_NATIVE(K32, GetNumaNodeNumberFromHandle, BOOL, (HANDLE h, PUSHORT node), (h, node)); *node = 0; return TRUE; }
#endif

static BYTE popcount(ULONG_PTR m) { BYTE n = 0; while (m) { n += (BYTE)(m & 1); m >>= 1; } return n; }

/* GetLogicalProcessorInformationEx (Windows 7), built from the Vista-era
 * GetLogicalProcessorInformation. Callers walk the result by each entry's
 * Size, so a fixed entry size is fine. */
#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT BOOL WINAPI vx_GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP rel,
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX out, PDWORD len)
{ VX_NATIVE(K32, GetLogicalProcessorInformationEx, BOOL, (LOGICAL_PROCESSOR_RELATIONSHIP rel,         PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX out, PDWORD len), (rel, out, len));
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *src = NULL;
    DWORD srclen = 0, need = 0, n, i;
    const DWORD esz = sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX);
    BOOL all = (rel == RelationAll);
    BYTE *w = (BYTE *)out;
    DWORD_PTR pmask, smask;

    if (!len) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    GetLogicalProcessorInformation(NULL, &srclen);
    if (!srclen || !(src = HeapAlloc(GetProcessHeap(), 0, srclen))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    if (!GetLogicalProcessorInformation(src, &srclen)) { HeapFree(GetProcessHeap(), 0, src); return FALSE; }
    n = srclen / sizeof(*src);

    for (i = 0; i < n; i++)
        if (src[i].Relationship <= RelationProcessorPackage && (all || src[i].Relationship == rel)) need += esz;
    if (all || rel == RelationGroup) need += esz;

    if (*len < need || !out) {
        *len = need;
        HeapFree(GetProcessHeap(), 0, src);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    vx_memset(out, 0, need);
    for (i = 0; i < n; i++) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (void *)w;
        LOGICAL_PROCESSOR_RELATIONSHIP r = src[i].Relationship;
        if (r > RelationProcessorPackage || !(all || r == rel)) continue;
        e->Relationship = r;
        e->Size = esz;
        switch (r) {
        case RelationProcessorCore:
        case RelationProcessorPackage:
            e->Processor.Flags = r == RelationProcessorCore ? src[i].ProcessorCore.Flags : 0;
            e->Processor.GroupCount = 1;
            e->Processor.GroupMask[0].Mask = src[i].ProcessorMask;
            break;
        case RelationNumaNode:
            e->NumaNode.NodeNumber = src[i].NumaNode.NodeNumber;
            e->NumaNode.GroupMask.Mask = src[i].ProcessorMask;
            break;
        case RelationCache:
            e->Cache.Level = src[i].Cache.Level;
            e->Cache.Associativity = src[i].Cache.Associativity;
            e->Cache.LineSize = src[i].Cache.LineSize;
            e->Cache.CacheSize = src[i].Cache.Size;
            e->Cache.Type = src[i].Cache.Type;
            e->Cache.GroupMask.Mask = src[i].ProcessorMask;
            break;
        default: break;
        }
        w += esz;
    }
    if (all || rel == RelationGroup) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (void *)w;
        GetProcessAffinityMask(GetCurrentProcess(), &pmask, &smask);
        e->Relationship = RelationGroup;
        e->Size = esz;
        e->Group.MaximumGroupCount = e->Group.ActiveGroupCount = 1;
        e->Group.GroupInfo[0].MaximumProcessorCount = e->Group.GroupInfo[0].ActiveProcessorCount = popcount(smask);
        e->Group.GroupInfo[0].ActiveProcessorMask = smask;
    }
    *len = need;
    HeapFree(GetProcessHeap(), 0, src);
    return TRUE;
}
#endif

VX_EXPORT BOOL WINAPI vx_GetSystemCpuSetInformation(PVOID info, ULONG len, PULONG ret, HANDLE proc, ULONG flags)
{ VX_NATIVE(K32, GetSystemCpuSetInformation, BOOL, (PVOID info, ULONG len, PULONG ret, HANDLE proc, ULONG flags), (info, len, ret, proc, flags));
    if (ret) *ret = 0;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

#if !VX_WIN7     /* Windows 7 has it: its .def forwards there */
VX_EXPORT DWORD64 WINAPI vx_GetEnabledXStateFeatures(void) { VX_NATIVE(K32, GetEnabledXStateFeatures, DWORD64, (void), ()); return 3; /* x87 + SSE */ }
#endif

/* ---- firmware / architecture -------------------------------------------- */
DWORD WINAPI GetFirmwareEnvironmentVariableW(LPCWSTR, LPCWSTR, PVOID, DWORD);
VX_EXPORT BOOL WINAPI vx_GetFirmwareType(PFIRMWARE_TYPE t)
{ VX_NATIVE(K32, GetFirmwareType, BOOL, (PFIRMWARE_TYPE t), (t));
    if (!t) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    /* Documented test: on BIOS systems this fails with ERROR_INVALID_FUNCTION. */
    GetFirmwareEnvironmentVariableW(L"", L"{00000000-0000-0000-0000-000000000000}", NULL, 0);
    *t = GetLastError() == ERROR_INVALID_FUNCTION ? FirmwareTypeBios : FirmwareTypeUefi;
    return TRUE;
}

VX_EXPORT BOOL WINAPI vx_IsWow64Process2(HANDLE h, USHORT *pm, USHORT *nm)
{ VX_NATIVE(K32, IsWow64Process2, BOOL, (HANDLE h, USHORT *pm, USHORT *nm), (h, pm, nm));
    BOOL wow = FALSE;
    SYSTEM_INFO si;
    if (!IsWow64Process(h, &wow)) return FALSE;
    GetNativeSystemInfo(&si);
    *pm = wow ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_UNKNOWN;
    if (nm) *nm = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
    return TRUE;
}
