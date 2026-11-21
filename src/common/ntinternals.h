/* ntinternals.h - the few undocumented Windows layouts VistaExt relies on,
 * in one place. Each has been the same from Vista to Windows 11 (they are
 * part of what old programs and debuggers read directly, so Windows keeps
 * them); the offsets were taken from the public symbols (ntdll/ntoskrnl PDBs)
 * and are checked on all four builds by tests/vm (vxtest /expectall).
 *
 * Header only, no imports: usable in vxload (ntdll only, loaded before
 * kernel32 is ready) as well as in the DLLs and tools. */
#ifndef VX_NTINTERNALS_H
#define VX_NTINTERNALS_H

/* ---- PEB: the Windows version a program is told -----------------------------
 * GetVersionEx, RtlGetVersion and VerifyVersionInfo answer from these fields;
 * kernelvx writes them when a version is set for the program. */
#ifdef _WIN64
#define VX_TEB_PEB          0x60
#define VX_PEB_MAJOR        0x118   /* ULONG  OSMajorVersion */
#define VX_PEB_MINOR        0x11C   /* ULONG  OSMinorVersion */
#define VX_PEB_BUILD        0x120   /* USHORT OSBuildNumber */
#define VX_PEB_CSDNUM       0x122   /* USHORT OSCSDVersion (service pack << 8) */
#define VX_PEB_CSDSTR       0x2E8   /* UNICODE_STRING CSDVersion */
#define VX_TEB_DEALLOCSTACK 0x1478  /* PVOID DeallocationStack: the stack's lowest address */
#else
#define VX_TEB_PEB          0x30
#define VX_PEB_MAJOR        0xA4
#define VX_PEB_MINOR        0xA8
#define VX_PEB_BUILD        0xAC
#define VX_PEB_CSDNUM       0xAE
#define VX_PEB_CSDSTR       0x1F0
#define VX_TEB_DEALLOCSTACK 0xE0C
#endif

static __inline BYTE *vx_peb(void) { return *(BYTE **)((BYTE *)NtCurrentTeb() + VX_TEB_PEB); }

/* the version in the PEB (what the program is told) */
static __inline void vx_peb_version(ULONG *maj, ULONG *min, ULONG *bld)
{
    BYTE *p = vx_peb();
    if (maj) *maj = *(ULONG *)(p + VX_PEB_MAJOR);
    if (min) *min = *(ULONG *)(p + VX_PEB_MINOR);
    if (bld) *bld = *(USHORT *)(p + VX_PEB_BUILD);
}

/* ---- KUSER_SHARED_DATA: the page the kernel maps read-only into every
 * process at 0x7FFE0000 (its address is fixed by the architecture) ---------- */
#define VX_KUSER                0x7FFE0000
#define VX_KUSER_INTERRUPT_TIME ((volatile LONG *)(ULONG_PTR)(VX_KUSER + 0x008))   /* KSYSTEM_TIME, 3 LONGs */
#define VX_KUSER_SYSTEM_ROOT    ((const WCHAR *)(ULONG_PTR)(VX_KUSER + 0x030))     /* WCHAR[260] NtSystemRoot */
#define VX_KUSER_NT_MAJOR       (*(volatile ULONG *)(ULONG_PTR)(VX_KUSER + 0x26C)) /* the real version: */
#define VX_KUSER_NT_MINOR       (*(volatile ULONG *)(ULONG_PTR)(VX_KUSER + 0x270)) /* never changed per program */
#define VX_KUSER_SAFE_BOOT      (*(volatile BYTE *)(ULONG_PTR)(VX_KUSER + 0x2EC))  /* SafeBootMode */

#endif
