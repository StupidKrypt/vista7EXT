/* diag.h - the diagnostics folder: %ProgramData%\VistaExt Diagnostics.
 *
 * Only VistaExt's service and its tools run as administrator write there,
 * so the folder gets a protected access list: SYSTEM and Administrators may
 * change it, every user may read and copy it. (A folder every user could
 * write to would let one plant a link there that the service, running as
 * SYSTEM, would then write through.) Uninstalling keeps it.
 *   events.log         the service: Windows' own error reports (a program
 *                      crashed, hung, or "could not start"), with whether
 *                      VistaExt runs that program and whether one of its
 *                      files is the faulting module
 *   decisions.log      the service: which programs it set up or left alone
 *   report-<date>\     "Save diagnostics" (VistaExt's window, the tray
 *                      icon): a snapshot - system, settings, logs (also each
 *                      user's crash logs), Windows' error log, crash dumps
 * Crashes of programs VistaExt runs are written by the crash window, as the
 * user, to that user's %LOCALAPPDATA%\VistaExt (crashes.log, one line each;
 * crash-details.log); the snapshot copies them.
 * Text files are UTF-8 with a byte-order mark (Notepad reads them). */
#ifndef VX_DIAG_H
#define VX_DIAG_H
#include <windows.h>
/* the folder, created and protected if needed (needs administrator rights);
 * FALSE if it cannot be used */
BOOL vx_diag_dir(WCHAR *out /* MAX_PATH */);
/* one entry at the end of <name> in that folder (started afresh above 2 MB) */
void vx_diag_append(const WCHAR *name, const WCHAR *text);
/* the same for any file (the user's own crash log): UTF-8, a byte-order
 * mark when new, started afresh above 2 MB (one older file kept) */
void vx_append_utf8(const WCHAR *path, const WCHAR *text);
#endif
