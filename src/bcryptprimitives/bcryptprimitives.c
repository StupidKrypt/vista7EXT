/* bcryptprimitives.dll for Windows Vista, and bcryptprimvx.dll (built from
 * this file with VX_COMPANION) for Windows 7, whose own bcryptprimitives.dll
 * lacks ProcessPrng (added in Windows 10). Patched programs import
 * bcryptprimvx.dll, which uses the system ProcessPrng when there is one. Rust's standard library and
 * Chromium 120+ get random numbers from ProcessPrng. Vista's RtlGenRandom
 * (advapi32!SystemFunction036) is the same system CSPRNG behind CryptGenRandom. */
#include "../common/vx.h"

BOOLEAN WINAPI SystemFunction036(PVOID buf, ULONG len);

VX_EXPORT BOOL WINAPI vx_ProcessPrng(PBYTE buf, SIZE_T len)
{
#ifdef VX_COMPANION
    VX_NATIVE(L"bcryptprimitives.dll", ProcessPrng, BOOL, (PBYTE, SIZE_T), (buf, len));
#endif
    while (len) {
        ULONG n = len > 0x10000000 ? 0x10000000 : (ULONG)len;
        if (!SystemFunction036(buf, n)) {
            /* ProcessPrng is documented to never fail; there is no safe
             * fallback for missing randomness, so stop the process. */
            TerminateProcess(GetCurrentProcess(), 0xC0000001);
        }
        buf += n;
        len -= n;
    }
    return TRUE;
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE h, DWORD r, LPVOID p)
{
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        vx_pin((void *)DllMainCRTStartup);
    }
    return TRUE;
}
