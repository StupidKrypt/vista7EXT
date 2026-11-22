/* kernelvx - CONTEXT helpers for extended processor state (Windows 7 SP1),
 * and InitializeContext2 (Windows 10 build 20348).
 *
 * Vista saves no extended state (AVX and later) for threads, and VistaExt's
 * GetEnabledXStateFeatures reports x87 and SSE only. So on Vista:
 *  - InitializeContext asked for CONTEXT_XSTATE fails with ERROR_NOT_SUPPORTED
 *    (.NET and others ask only when GetEnabledXStateFeatures reports AVX);
 *    otherwise it lays out a CONTEXT in the caller's buffer, aligned as the
 *    processor needs, or says how big the buffer must be
 *    (ERROR_INSUFFICIENT_BUFFER with the size in *ContextLength).
 *  - CopyContext copies the register groups the flags name and both contexts
 *    hold. (Microsoft documents the flags as a subset of the destination's;
 *    Windows 7's own CopyContext does not refuse more, measured in the test
 *    VMs, so neither does this one.)
 *  - LocateXStateFeature: NULL without CONTEXT_XSTATE (Microsoft: "No
 *    additional error information"); with it, x87 (0) and SSE (1) live in
 *    the legacy save area of the CONTEXT itself.
 * On Windows 7 and 8 InitializeContext2 is InitializeContext: the compaction
 * mask applies only where Windows compacts extended state, which 7 and 8
 * never do. Sources: Microsoft Learn, winbase.h pages of each function. */
#include "../common/vx.h"

#ifdef _WIN64
#define VX_CTX_ARCH  0x00100000
#define VX_CTX_ALIGN 16
#else
#define VX_CTX_ARCH  0x00010000
#define VX_CTX_ALIGN 4
#endif
#define VX_CTX_XSTATE_BIT 0x40

#if VX_OS < 601   /* Windows 7 and later have them: their .def forwards there */
static BOOL init_context(PVOID buf, DWORD flags, PCONTEXT *ctx, PDWORD len)
{
    DWORD need = sizeof(CONTEXT) + VX_CTX_ALIGN - 1;
    CONTEXT *c;
    if (!len || (flags & VX_CTX_ARCH) != VX_CTX_ARCH) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (flags & VX_CTX_XSTATE_BIT) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
    if (!buf || *len < need) { *len = need; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    c = (CONTEXT *)(((ULONG_PTR)buf + VX_CTX_ALIGN - 1) & ~(ULONG_PTR)(VX_CTX_ALIGN - 1));
    vx_memset(c, 0, sizeof(*c));
    c->ContextFlags = flags;
    if (ctx) *ctx = c;
    return TRUE;
}

VX_EXPORT BOOL WINAPI vx_InitializeContext(PVOID buf, DWORD flags, PCONTEXT *ctx, PDWORD len)
{ VX_NATIVE(K32, InitializeContext, BOOL, (PVOID buf, DWORD flags, PCONTEXT *ctx, PDWORD len), (buf, flags, ctx, len));
    return init_context(buf, flags, ctx, len);
}

VX_EXPORT BOOL WINAPI vx_CopyContext(PCONTEXT dst, DWORD flags, PCONTEXT src)
{ VX_NATIVE(K32, CopyContext, BOOL, (PCONTEXT dst, DWORD flags, PCONTEXT src), (dst, flags, src));
    DWORD f;
    if (!dst || !src || (flags & VX_CTX_ARCH) != VX_CTX_ARCH) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    f = flags & src->ContextFlags & dst->ContextFlags & ~VX_CTX_ARCH;
#ifdef _WIN64
    if (f & 0x1) {   /* CONTEXT_CONTROL */
        dst->SegCs = src->SegCs; dst->SegSs = src->SegSs; dst->EFlags = src->EFlags; dst->Rsp = src->Rsp; dst->Rip = src->Rip;
    }
    if (f & 0x2) {   /* CONTEXT_INTEGER */
        dst->Rax = src->Rax; dst->Rcx = src->Rcx; dst->Rdx = src->Rdx; dst->Rbx = src->Rbx; dst->Rbp = src->Rbp;
        dst->Rsi = src->Rsi; dst->Rdi = src->Rdi; dst->R8 = src->R8; dst->R9 = src->R9; dst->R10 = src->R10;
        dst->R11 = src->R11; dst->R12 = src->R12; dst->R13 = src->R13; dst->R14 = src->R14; dst->R15 = src->R15;
    }
    if (f & 0x4) { dst->SegDs = src->SegDs; dst->SegEs = src->SegEs; dst->SegFs = src->SegFs; dst->SegGs = src->SegGs; }
    if (f & 0x8) { dst->MxCsr = src->MxCsr; vx_memcpy(&dst->FltSave, &src->FltSave, sizeof(dst->FltSave)); }
    if (f & 0x10) {  /* CONTEXT_DEBUG_REGISTERS */
        dst->Dr0 = src->Dr0; dst->Dr1 = src->Dr1; dst->Dr2 = src->Dr2; dst->Dr3 = src->Dr3; dst->Dr6 = src->Dr6; dst->Dr7 = src->Dr7;
    }
#else
    if (f & 0x1) {
        dst->Ebp = src->Ebp; dst->Eip = src->Eip; dst->SegCs = src->SegCs; dst->EFlags = src->EFlags;
        dst->Esp = src->Esp; dst->SegSs = src->SegSs;
    }
    if (f & 0x2) {
        dst->Edi = src->Edi; dst->Esi = src->Esi; dst->Ebx = src->Ebx; dst->Edx = src->Edx; dst->Ecx = src->Ecx; dst->Eax = src->Eax;
    }
    if (f & 0x4) { dst->SegGs = src->SegGs; dst->SegFs = src->SegFs; dst->SegEs = src->SegEs; dst->SegDs = src->SegDs; }
    if (f & 0x8) vx_memcpy(&dst->FloatSave, &src->FloatSave, sizeof(dst->FloatSave));
    if (f & 0x10) {
        dst->Dr0 = src->Dr0; dst->Dr1 = src->Dr1; dst->Dr2 = src->Dr2; dst->Dr3 = src->Dr3; dst->Dr6 = src->Dr6; dst->Dr7 = src->Dr7;
    }
    if (f & 0x20) vx_memcpy(dst->ExtendedRegisters, src->ExtendedRegisters, sizeof(dst->ExtendedRegisters));   /* CONTEXT_EXTENDED_REGISTERS */
#endif
    return TRUE;
}

/* the legacy (FXSAVE) area: x87 in its first 160 bytes, SSE in the next 256 */
static BYTE *legacy_area(PCONTEXT c)
{
#ifdef _WIN64
    return (BYTE *)&c->FltSave;
#else
    return c->ExtendedRegisters;
#endif
}

VX_EXPORT PVOID WINAPI vx_LocateXStateFeature(PCONTEXT c, DWORD id, PDWORD len)
{ VX_NATIVE(K32, LocateXStateFeature, PVOID, (PCONTEXT c, DWORD id, PDWORD len), (c, id, len));
    if (!c || !(c->ContextFlags & VX_CTX_XSTATE_BIT) || id > 1) return NULL;
    if (len) *len = id == 0 ? 160 : 256;
    return legacy_area(c) + (id == 0 ? 0 : 160);
}

VX_EXPORT BOOL WINAPI vx_SetXStateFeaturesMask(PCONTEXT c, DWORD64 mask)
{ VX_NATIVE(K32, SetXStateFeaturesMask, BOOL, (PCONTEXT c, DWORD64 mask), (c, mask));
    if (!c || !(c->ContextFlags & VX_CTX_XSTATE_BIT)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;          /* "silently ignores any feature ... not enabled on the processor" */
}

VX_EXPORT BOOL WINAPI vx_GetXStateFeaturesMask(PCONTEXT c, PDWORD64 mask)
{ VX_NATIVE(K32, GetXStateFeaturesMask, BOOL, (PCONTEXT c, PDWORD64 mask), (c, mask));
    if (!c || !mask || !(c->ContextFlags & VX_CTX_XSTATE_BIT)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *mask = (c->ContextFlags & 0x8) ? 3 : 0;   /* x87 and SSE, when the floating-point group is held */
    return TRUE;
}
#endif

VX_EXPORT BOOL WINAPI vx_InitializeContext2(PVOID buf, DWORD flags, PCONTEXT *ctx, PDWORD len, ULONG64 compaction)
{ VX_NATIVE(K32, InitializeContext2, BOOL, (PVOID buf, DWORD flags, PCONTEXT *ctx, PDWORD len, ULONG64 compaction),
            (buf, flags, ctx, len, compaction));
#if VX_OS < 601
    return init_context(buf, flags, ctx, len);
#else
    return InitializeContext(buf, flags, ctx, len);
#endif
}
