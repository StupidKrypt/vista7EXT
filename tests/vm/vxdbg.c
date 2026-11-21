/* vxdbg <seconds> <program> [arguments]
 * Runs a program (and every process it starts) under a minimal debugger for
 * at most <seconds>, then stops them all. Prints, for diagnosing programs
 * that do not run:
 *   - each process started and its exit code
 *   - crashes: exception code and where (module + offset), with the module
 *     list of that process (an unhandled exception, or a breakpoint the
 *     program did not expect: Chromium's CHECK failures)
 *   - the program's debug output (OutputDebugString), shortened
 * Error boxes are switched off as in nofault. Returns the first process's
 * exit code, or 258 (WAIT_TIMEOUT) if it was still running. */
#include "../../src/common/vx.h"
#include "../../src/common/console.h"

#define MAXP 64
#define MAXM 4096
#define MAXT 1024

#define RING 60
typedef struct { DWORD pid; HANDLE h; WCHAR name[80]; BOOL first_bp; DWORD exitcode; BOOL done;
                 WCHAR ring[RING][160]; int nring; } proc;
typedef struct { DWORD pid; ULONG_PTR base, size; WCHAR name[64]; } mod;

static struct { DWORD tid; HANDLE h; } g_t[MAXT];
static int g_nt;
static proc g_p[MAXP];
static int g_np;
static mod g_m[MAXM];
static int g_nm;
static int g_lines;

static proc *find_proc(DWORD pid)
{
    int i;
    for (i = 0; i < g_np; i++) if (g_p[i].pid == pid) return &g_p[i];
    return NULL;
}

static void base_name(HANDLE file, WCHAR *out, DWORD cch)
{
    WCHAR path[MAX_PATH] = L"?";
    DWORD n = file ? GetFinalPathNameByHandleW(file, path, MAX_PATH, 0) : 0;
    const WCHAR *b = path + (n && n < MAX_PATH ? n : 1);
    if (!n || n >= MAX_PATH) { vx_wcscpyn(out, L"?", cch); return; }
    while (b > path && b[-1] != '\\') b--;
    vx_wcscpyn(out, b, cch);
}

static ULONG_PTR image_size(HANDLE proc, ULONG_PTR base)
{
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS nt;
    SIZE_T got;
    if (!ReadProcessMemory(proc, (void *)base, &dos, sizeof(dos), &got) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    if (!ReadProcessMemory(proc, (BYTE *)base + dos.e_lfanew, &nt, sizeof(nt), &got)) return 0;
    return nt.OptionalHeader.SizeOfImage;
}

static void add_mod(DWORD pid, ULONG_PTR base, HANDLE proc, HANDLE file)
{
    mod *m;
    if (g_nm >= MAXM) return;
    m = &g_m[g_nm++];
    m->pid = pid;
    m->base = base;
    m->size = image_size(proc, base);
    base_name(file, m->name, 64);
}

static void where(DWORD pid, ULONG_PTR addr, WCHAR *out)
{
    int i;
    for (i = g_nm - 1; i >= 0; i--)
        if (g_m[i].pid == pid && addr >= g_m[i].base && addr < g_m[i].base + g_m[i].size) {
            wsprintfW(out, L"%s+0x%lx", g_m[i].name, (unsigned long)(addr - g_m[i].base));
            return;
        }
    wsprintfW(out, L"%p (no module)", (void *)addr);
}

static void debug_string(proc *p, const OUTPUT_DEBUG_STRING_INFO *o)
{
    char a[400];
    WCHAR w[400];
    SIZE_T got = 0, n = o->nDebugStringLength;
    if (!p) return;
    if (n > 398) n = 398;
    if (o->fUnicode) {
        if (!ReadProcessMemory(p->h, o->lpDebugStringData, w, n * sizeof(WCHAR), &got)) return;
        w[got / sizeof(WCHAR) < 399 ? got / sizeof(WCHAR) : 398] = 0;
    } else {
        if (!ReadProcessMemory(p->h, o->lpDebugStringData, a, n, &got)) return;
        a[got < 399 ? got : 398] = 0;
        MultiByteToWideChar(CP_ACP, 0, a, -1, w, 400);
    }
    {   /* one line, no trailing newline */
        SIZE_T l = vx_wcslen(w);
        while (l && (w[l - 1] == '\n' || w[l - 1] == '\r')) w[--l] = 0;
        if (!l) return;
    }
    vx_wcscpyn(p->ring[p->nring++ % RING], w, 160);
    if (++g_lines > 40000) return;
    con_print(L"  [%u %s] %s\r\n", p->pid, p->name, w);
}

/* A crude stack trace: values on the thread's stack that point into a
 * module (return addresses, mostly), innermost first. 64-bit processes. */
static void stack_scan(proc *p, DWORD tid)
{
#ifdef _WIN64
    CONTEXT c;
    ULONG_PTR st[256];
    SIZE_T got = 0;
    HANDLE th = NULL;
    int i, n = 0;
    BOOL wow = FALSE;
    for (i = 0; i < g_nt; i++) if (g_t[i].tid == tid) th = g_t[i].h;
    if (!th || (IsWow64Process(p->h, &wow) && wow)) return;
    vx_memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(th, &c)) return;
    {   /* the thread's last error and last NTSTATUS (TEB LastErrorValue at
         * 0x68, LastStatusValue at 0x1250 on x64): a program that stops on
         * a failed call (V8's CHECK_EQ(ERROR_..., GetLastError())) has the
         * value there */
        typedef LONG (WINAPI *qit_fn)(HANDLE, int, PVOID, ULONG, PULONG);
        static qit_fn qit;
        struct { LONG exit; PVOID teb; ULONG_PTR cid[2]; ULONG_PTR aff; LONG pri, base; } tbi;
        DWORD le = 0, ls = 0;
        if (!qit) qit = (qit_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
        if (qit && !qit(th, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), NULL) && tbi.teb &&
            ReadProcessMemory(p->h, (BYTE *)tbi.teb + 0x68, &le, sizeof(le), &got) &&
            ReadProcessMemory(p->h, (BYTE *)tbi.teb + 0x1250, &ls, sizeof(ls), &got))
            con_print(L"    thread's last error: %u, last status: 0x%08X\r\n", le, ls);
        got = 0;
    }
    ReadProcessMemory(p->h, (void *)c.Rsp, st, sizeof(st), &got);
    con_print(L"    stack (rip, then values that point into modules):\r\n");
    {
        WCHAR w[120];
        where(p->pid, c.Rip, w);
        con_print(L"      %s\r\n", w);
    }
    for (i = 0; i < (int)(got / sizeof(ULONG_PTR)) && n < 24; i++) {
        WCHAR w[120];
        int k, hit = 0;
        for (k = g_nm - 1; k >= 0; k--)
            if (g_m[k].pid == p->pid && st[i] >= g_m[k].base && st[i] < g_m[k].base + g_m[k].size) { hit = 1; break; }
        if (!hit) continue;
        where(p->pid, st[i], w);
        con_print(L"      %s\r\n", w);
        n++;
    }
    {   /* readable text near the stack pointer: a program that stops on
         * purpose (Chromium's LOG(FATAL), assertions) has just formatted its
         * reason there */
        static char sb[16384];
        SIZE_T sg = 0;
        int run = 0, shown = 0;
        ReadProcessMemory(p->h, (void *)c.Rsp, sb, sizeof(sb), &sg);
        for (i = 0; i <= (int)sg && shown < 10; i++) {
            char ch = i < (int)sg ? sb[i] : 0;
            if (ch >= 0x20 && ch < 0x7f) { run++; continue; }
            if (run >= 16) {
                WCHAR w[300];
                int k, s = i - run, len = run > 290 ? 290 : run;
                for (k = 0; k < len; k++) w[k] = (WCHAR)(unsigned char)sb[s + k];
                w[len] = 0;
                con_print(L"    text on stack: %s\r\n", w);
                shown++;
            }
            run = 0;
        }
    }
#endif
}

static void last_lines(proc *p)
{
    int i, n = p->nring < RING ? p->nring : RING;
    if (!n) return;
    con_print(L"    last debug output of this process:\r\n");
    for (i = p->nring - n; i < p->nring; i++) con_print(L"      %s\r\n", p->ring[i % RING]);
}

int wmain(int argc, WCHAR **argv)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DEBUG_EVENT ev;
    WCHAR *cmd = GetCommandLineW(), buf[200];
    DWORD secs = 0, i, t0, first = 0, firstcode = WAIT_TIMEOUT, crashes = 0;
    BOOL q = FALSE;
    if (argc < 3) { con_print(L"usage: vxdbg <seconds> <program> [arguments]\r\n"); return 2; }
    for (i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; i++) secs = secs * 10 + (argv[1][i] - '0');
    /* skip our name and the seconds */
    for (i = 0; i < 2; i++) {
        while (*cmd && (q || (*cmd != ' ' && *cmd != '\t'))) { if (*cmd == '"') q = !q; cmd++; }
        while (*cmd == ' ' || *cmd == '\t') cmd++;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    vx_memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, DEBUG_PROCESS, NULL, NULL, &si, &pi)) {
        con_print(L"[vxdbg: could not start, error %u]\r\n", GetLastError());
        return 9009;
    }
    first = pi.dwProcessId;
    t0 = GetTickCount();
    for (;;) {
        DWORD cont = DBG_CONTINUE, left;
        proc *p;
        DWORD el = GetTickCount() - t0;
        if (el >= secs * 1000) break;
        left = secs * 1000 - el;
        if (!WaitForDebugEvent(&ev, left > 500 ? 500 : left)) continue;
        p = find_proc(ev.dwProcessId);
        switch (ev.dwDebugEventCode) {
        case CREATE_THREAD_DEBUG_EVENT:
            if (g_nt < MAXT) { g_t[g_nt].tid = ev.dwThreadId; g_t[g_nt].h = ev.u.CreateThread.hThread; g_nt++; }
            break;
        case CREATE_PROCESS_DEBUG_EVENT:
            if (g_nt < MAXT) { g_t[g_nt].tid = ev.dwThreadId; g_t[g_nt].h = ev.u.CreateProcessInfo.hThread; g_nt++; }
            if (g_np < MAXP) {
                p = &g_p[g_np++];
                vx_memset(p, 0, sizeof(*p));
                p->pid = ev.dwProcessId;
                p->h = ev.u.CreateProcessInfo.hProcess;
                base_name(ev.u.CreateProcessInfo.hFile, p->name, 80);
                con_print(L"  [%u %s] started\r\n", p->pid, p->name);
                add_mod(p->pid, (ULONG_PTR)ev.u.CreateProcessInfo.lpBaseOfImage, p->h, ev.u.CreateProcessInfo.hFile);
            }
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
            break;
        case UNLOAD_DLL_DEBUG_EVENT: {
            int k;
            for (k = g_nm - 1; k >= 0; k--)
                if (g_m[k].pid == ev.dwProcessId && g_m[k].base == (ULONG_PTR)ev.u.UnloadDll.lpBaseOfDll) {
                    g_m[k].pid = 0;             /* gone: no longer matches addresses */
                    break;
                }
            break;
        }
        case LOAD_DLL_DEBUG_EVENT:
            if (p) add_mod(p->pid, (ULONG_PTR)ev.u.LoadDll.lpBaseOfDll, p->h, ev.u.LoadDll.hFile);
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
            break;
        case OUTPUT_DEBUG_STRING_EVENT:
            debug_string(p, &ev.u.DebugString);
            break;
        case EXCEPTION_DEBUG_EVENT: {
            EXCEPTION_RECORD *r = &ev.u.Exception.ExceptionRecord;
            BOOL bp = r->ExceptionCode == EXCEPTION_BREAKPOINT || r->ExceptionCode == 0x4000001F;  /* WOW64 breakpoint */
            if (bp && p && !p->first_bp) { p->first_bp = TRUE; break; }       /* the loader's breakpoint */
            if (r->ExceptionCode == 0x406D1388) break;                         /* thread naming */
            cont = DBG_EXCEPTION_NOT_HANDLED;
            /* also serious exceptions the program catches itself (Unreal and
             * others turn them into their own crash report and exit) */
            if (!ev.u.Exception.dwFirstChance || bp || r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                r->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION || r->ExceptionCode == EXCEPTION_STACK_OVERFLOW ||
                r->ExceptionCode == 0xC0000409 /* fast fail / stack buffer overrun */) {
                int k;
                where(ev.dwProcessId, (ULONG_PTR)r->ExceptionAddress, buf);
                crashes++;
                con_print(L"  [%u %s] %s exception 0x%08X at %s",
                          ev.dwProcessId, p ? p->name : L"?", ev.u.Exception.dwFirstChance ? L"first-chance" : L"UNHANDLED",
                          r->ExceptionCode, buf);
                if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
                    WCHAR at[120];
                    where(ev.dwProcessId, (ULONG_PTR)r->ExceptionInformation[1], at);
                    con_print(L" (%s %s)", r->ExceptionInformation[0] == 8 ? L"executing (DEP)" :
                              r->ExceptionInformation[0] ? L"writing" : L"reading", at);
                }
                con_print(L"\r\n");
                if (crashes < 6 && p) stack_scan(p, ev.dwThreadId);
                if (crashes < 6) {
                    con_print(L"    modules:");
                    for (k = 0; k < g_nm; k++) if (g_m[k].pid == ev.dwProcessId) con_print(L" %s@%p", g_m[k].name, (void *)g_m[k].base);
                    con_print(L"\r\n");
                    if (p) last_lines(p);
                }
            }
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            if (p) {
                p->done = TRUE;
                p->exitcode = ev.u.ExitProcess.dwExitCode;
                con_print(L"  [%u %s] exited 0x%08X\r\n", p->pid, p->name, p->exitcode);
            }
            if (ev.dwProcessId == first) firstcode = ev.u.ExitProcess.dwExitCode;
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
        if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            int k, alive = 0;
            for (k = 0; k < g_np; k++) if (!g_p[k].done) alive++;
            if (!alive) break;
        }
    }
    for (i = 0; i < (DWORD)g_np; i++) if (!g_p[i].done) TerminateProcess(g_p[i].h, WAIT_TIMEOUT);
    con_print(L"[vxdbg: %u process(es), %u exception report(s); first process %s 0x%08X]\r\n", (DWORD)g_np, crashes,
              firstcode == WAIT_TIMEOUT ? L"still running, stopped" : L"exit", firstcode);
    return (int)firstcode;
}
