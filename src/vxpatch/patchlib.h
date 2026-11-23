/* VistaExt patch library: checks a program's imports against this Windows
 * and points the ones that need it at the VistaExt companion DLLs. */
#ifndef PATCHLIB_H
#define PATCHLIB_H
#include "../common/vx.h"

#define VXP_FORCE     0x1   /* load kernelvx even if nothing is missing (needed for
                               the Windows version setting and extra arguments) */
#define VXP_DRYRUN    0x2   /* report only, change nothing */
#define VXP_NOBACKUP  0x4   /* do not keep a .vxorig copy (used for temp files) */
#define VXP_QUIET     0x8   /* only log files that need or got changes */
#define VXP_MEMORY    0x20  /* in-memory mode: the program's imports are redirected while it
                               loads (vxload.dll), its files are not rewritten; only a header
                               that asks for a newer Windows is still lowered on disk */
#define VXP_REGALL    0x40  /* in-memory mode: register every program file, also one that needs
                               nothing itself (a DLL next to it does) */
#define VXP_SETUP     0x80  /* in-memory mode: registered as a setup program (its new helper
                               programs are told the same version, see vxauto) */
#define VXP_USER      0x100 /* in-memory mode: the user chose this program (right-click, VistaExt's
                               window, vxpatch): never taken back because it would run without */
#define VXP_LAYER     0x10  /* a Windows version setting is in effect: version.dll
                               imports go to versvx in every file (programs that
                               read kernel32's file version) */

typedef void (*vxp_log_fn)(void *ctx, const WCHAR *line);

typedef struct {
    int files;        /* PE files looked at */
    int patched;      /* files changed (or that would be, with VXP_DRYRUN) */
    int failed;       /* files that could not be read or written */
    int unsupported;  /* functions still missing after VistaExt */
} vxp_stats;

/* Returns 1 if patched, 0 if nothing to do, -1 on error. */
int  vxp_process_file(const WCHAR *path, DWORD flags, vxp_log_fn log, void *ctx, vxp_stats *st);
/* Every .exe and .dll under dir (recursive). Returns number patched. */
int  vxp_process_dir(const WCHAR *dir, DWORD flags, vxp_log_fn log, void *ctx, vxp_stats *st);
/* Put back the original from <file>.vxorig. Returns 1 if restored, 0 if
 * there was nothing to restore (no backup, or the program was updated or
 * removed since it was patched: then the outdated backup is deleted), -1 if
 * the file could not be replaced (program running?). */
int  vxp_restore_file(const WCHAR *path, vxp_log_fn log, void *ctx);
/* Every .vxorig under dir. Returns the number restored; *failed (optional)
 * gets the number that could not be. */
int  vxp_restore_dir(const WCHAR *dir, vxp_log_fn log, void *ctx, int *failed);
BOOL vxp_has_backup(const WCHAR *path);
/* Restore every file VistaExt has patched on this PC (uninstall). If any of
 * them is in use, none is restored and *failed gets their number; files that
 * still cannot be restored stay listed and are counted in *failed. */
int  vxp_restore_all(vxp_log_fn log, void *ctx, int *failed);

/* Recommended setting (layer string) for a program file or folder: Chromium
 * and Electron programs get "WIN10 ARGS:<VXP_CHROMIUM_ARGS>", programs whose
 * header requires Windows 10 get "WIN10", others "". Returns TRUE if the
 * folder is a Chromium/Electron program. */
#define VXP_CHROMIUM_ARGS L"--no-sandbox --disable-gpu --disable-gpu-compositing"
BOOL vxp_recommend(const WCHAR *path, WCHAR *layer, SIZE_T cch);
DWORD vxp_flags_for_layer(const WCHAR *layer);
/* stop / resume impersonating a user on this thread (vxauto) */
HANDLE vxp_unimpersonate(void);
/* in-memory mode: is vxload.dll installed; the mode to use by default
 * (VXP_MEMORY unless HKLM\SOFTWARE\VistaExt\Mode = "file"); is this
 * program registered */
BOOL  vxp_memory_available(void);
DWORD vxp_default_mode(void);
BOOL  vxp_memory_registered(const WCHAR *path);
BOOL  vxp_folder_needs(const WCHAR *file);
/* Can this program start on this Windows without VistaExt? Its header, and
 * every function it and the DLLs it loads at start (from its own folder)
 * import, not counting imports loaded on demand (delay-loaded: programs
 * check those first) or VistaExt's own files. FALSE: why[] says what is
 * missing. A program that runs by itself is never changed automatically. */
BOOL  vxp_runs_natively(const WCHAR *exe, WCHAR *why, SIZE_T cch);
/* registered for the in-memory mode by the user's choice (VXP_USER) */
BOOL  vxp_user_registered(const WCHAR *path);
/* the mode for this program or folder (file mode for sandboxed Gecko programs) */
DWORD vxp_mode_for(const WCHAR *path);
void vxp_reimpersonate(HANDLE token);
/* Explorer entries and .msi double-click (dir: VistaExt's tool folder, with a
 * trailing backslash); off puts the .msi command found before back */
void vxp_shell_entries(BOOL on, const WCHAR *dir);
/* the on/off switch (HKLM\SOFTWARE\VistaExt "Enabled") */
BOOL vxp_enabled(void);
/* Image File Execution Options for every registered program, on or off */
int  vxp_memory_switch(BOOL on, vxp_log_fn log, void *ctx);
/* Windows has a program of this file name (never registered by name) */
BOOL vxp_windows_name(const WCHAR *name);
/* a removed program: its registration, backup, list entry and setting */
int  vxp_forget(const WCHAR *path, vxp_log_fn log, void *ctx);
/* every list entry whose file or folder no longer exists */
int  vxp_cleanup_missing(vxp_log_fn log, void *ctx);
/* the patched files, as "a\0b\0\0" (returns how many) */
int  vxp_patched_list(WCHAR *buf, DWORD cch);
void vxp_unlist_patched(const WCHAR *path);

#endif
