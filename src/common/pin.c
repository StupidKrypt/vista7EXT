/* pin.c - vx_pin, on its own: the API-set DLLs need only this from the
 * shared code. The shared code is linked as a library (libvxcommon.a), so a
 * DLL gets only the files it uses, and with them only their imports (lld
 * keeps the imports of every function in a file it links, used or not). */
#include "vx.h"

void vx_pin(void *addr)
{
    HMODULE h;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)addr, &h);
}
