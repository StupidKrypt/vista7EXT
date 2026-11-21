/* userrun.h - starting a program as the signed-in user from a program that
 * runs as administrator (VistaExt Setup, VistaExt Center). */
#ifndef VX_USERRUN_H
#define VX_USERRUN_H
/* FALSE: Explorer is not running (or refused); start it some other way */
BOOL vx_run_as_user(const WCHAR *file, const WCHAR *args, const WCHAR *dir);
#endif
