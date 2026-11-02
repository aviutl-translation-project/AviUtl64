#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include "aviutl.h"
#include "debug_console.h"

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

#define CMD_QUEUE_CAP 16
#define CMD_MAX_LEN 256

static volatile LONG g_cs_state = 0; // 0=uninit, 1=initing, 2=ready
static CRITICAL_SECTION g_log_cs;
static BOOL g_initialized = FALSE;
static BOOL g_is_bridge = FALSE;
static BOOL g_owns_console = FALSE;
static volatile LONG g_exfunc_trace = 0;

// interactive console command queue & reader thread
static HANDLE g_stop_ev = NULL;
static HANDLE g_input_thread = NULL;
static HWND g_notify_hwnd = NULL;
static char g_cmd_queue[CMD_QUEUE_CAP][CMD_MAX_LEN];
static int g_cmd_head = 0;
static int g_cmd_tail = 0;
static int g_cmd_count = 0;

static void ensure_log_lock(void)
{
    for (;;)
    {
        LONG st = InterlockedCompareExchange(&g_cs_state, 1, 0);
        if (st == 2)
            return;
        if (st == 0)
        {
            InitializeCriticalSection(&g_log_cs);
            InterlockedExchange(&g_cs_state, 2);
            return;
        }
        Sleep(0);
    }
}

static BOOL enable_vt_on_handle(HANDLE h)
{
    DWORD mode = 0;
    if (!h || h == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!GetConsoleMode(h, &mode))
        return FALSE;
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
        return TRUE;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type)
{
    // ignore Ctrl+C / Ctrl+Break in console so gui can close cleanly
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT)
    {
        return TRUE;
    }
    return FALSE;
}

static const char *exception_code_name(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION:
        return "FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:
        return "FLT_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:
        return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
        return "INT_OVERFLOW";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:
        return "STACK_OVERFLOW";
    case 0xE06D7363u:
        return "CPP_EXCEPTION";
    default:
        return "UNHANDLED_EXCEPTION";
    }
}

static void crash_write_str(HANDLE hErr, const char *s)
{
    DWORD mode = 0;
    DWORD written = 0;
    wchar_t wbuf[1024];
    char u8buf[1536];

    if (!hErr || hErr == INVALID_HANDLE_VALUE || !s || !s[0])
        return;
    if (MultiByteToWideChar(932, 0, s, -1, wbuf, 1024) <= 0)
    {
        MultiByteToWideChar(CP_ACP, 0, s, -1, wbuf, 1024);
    }
    wbuf[1023] = L'\0';
    if (GetConsoleMode(hErr, &mode))
    {
        WriteConsoleW(hErr, wbuf, (DWORD)wcslen(wbuf), &written, NULL);
    }
    else
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, u8buf, (int)sizeof(u8buf), NULL, NULL);
        if (n > 1)
        {
            WriteFile(hErr, u8buf, (DWORD)(n - 1), &written, NULL);
        }
        else
        {
            WriteFile(hErr, s, (DWORD)lstrlenA(s), &written, NULL);
        }
    }
}

static void write_seh_report(EXCEPTION_POINTERS *ep, const char *plugin_name,
                             const char *stage, BOOL isolated)
{
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    char buf[1024];
    char mod_path[MAX_PATH] = "(unknown module)";
    const char *mod_base_name = mod_path;
    HMODULE hMod = NULL;
    uintptr_t fault_addr = 0;
    uintptr_t base_addr = 0;
    uintptr_t exe_base = (uintptr_t)GetModuleHandleA(NULL);

    if (!hErr || hErr == INVALID_HANDLE_VALUE)
    {
        hErr = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (!ep || !ep->ExceptionRecord)
        return;

    fault_addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)fault_addr, &hMod) &&
        hMod)
    {
        base_addr = (uintptr_t)hMod;
        if (GetModuleFileNameA(hMod, mod_path, MAX_PATH) > 0)
        {
            const char *slash = strrchr(mod_path, '\\');
            mod_base_name = slash ? (slash + 1) : mod_path;
        }
    }

    crash_write_str(hErr,
                    "\r\n------------------------------------------------------------------------\r\n");
    if (isolated)
    {
        snprintf(buf, sizeof(buf),
                 " [plugin crash] exception in '%s' (%s), plugin disabled\r\n",
                 (plugin_name && plugin_name[0]) ? plugin_name : "plugin",
                 (stage && stage[0]) ? stage : "callback");
        crash_write_str(hErr, buf);
    }
    else
    {
        crash_write_str(hErr, " [crash] unhandled exception in ");
        crash_write_str(hErr, g_is_bridge ? "reaviutl_bridge32.exe (32-bit plugin host)\r\n"
                                          : (REAVIUTL_X64 ? "reaviutl64.exe (64-bit host)\r\n"
                                                          : "reaviutl.exe (32-bit host)\r\n"));
    }
    crash_write_str(hErr,
                    "------------------------------------------------------------------------\r\n");

    snprintf(buf, sizeof(buf),
             "  PID / TID  : %lu / %lu  (HostExeBase = 0x%p)\r\n"
             "  Exception  : 0x%08lX (%s)\r\n"
             "  Address    : 0x%p  [%s + 0x%lX] (ModuleBase = 0x%p)\r\n",
             (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetCurrentThreadId(),
             (void *)exe_base,
             (unsigned long)ep->ExceptionRecord->ExceptionCode,
             exception_code_name(ep->ExceptionRecord->ExceptionCode),
             (void *)fault_addr,
             mod_base_name,
             (unsigned long)(base_addr ? (fault_addr - base_addr) : 0),
             (void *)base_addr);
    crash_write_str(hErr, buf);

    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2)
    {
        ULONG_PTR op = ep->ExceptionRecord->ExceptionInformation[0];
        uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        const char *op_str = (op == 0) ? "READ from" : ((op == 1) ? "WRITE to" : "EXECUTE at");
        if (exe_base && addr >= exe_base && (addr - exe_base) < 0x01000000u)
        {
            snprintf(buf, sizeof(buf),
                     "  Violation  : %s memory at 0x%p  (HostExeBase + 0x%lX)\r\n",
                     op_str, (void *)addr, (unsigned long)(addr - exe_base));
        }
        else
        {
            snprintf(buf, sizeof(buf),
                     "  Violation  : %s memory at 0x%p\r\n",
                     op_str, (void *)addr);
        }
        crash_write_str(hErr, buf);
    }

    if (ep->ContextRecord)
    {
        CONTEXT *c = ep->ContextRecord;
#if defined(_M_X64) || defined(__x86_64__)
        snprintf(buf, sizeof(buf),
                 "  RIP=%016llX RSP=%016llX RBP=%016llX\r\n"
                 "  RAX=%016llX RBX=%016llX RCX=%016llX\r\n"
                 "  RDX=%016llX RSI=%016llX RDI=%016llX\r\n"
                 "  R8 =%016llX R9 =%016llX R10=%016llX\r\n"
                 "  R11=%016llX R12=%016llX R13=%016llX\r\n",
                 (unsigned long long)c->Rip, (unsigned long long)c->Rsp, (unsigned long long)c->Rbp,
                 (unsigned long long)c->Rax, (unsigned long long)c->Rbx, (unsigned long long)c->Rcx,
                 (unsigned long long)c->Rdx, (unsigned long long)c->Rsi, (unsigned long long)c->Rdi,
                 (unsigned long long)c->R8, (unsigned long long)c->R9, (unsigned long long)c->R10,
                 (unsigned long long)c->R11, (unsigned long long)c->R12, (unsigned long long)c->R13);
        crash_write_str(hErr, buf);
#elif defined(_M_IX86) || defined(__i386__)
        snprintf(buf, sizeof(buf),
                 "  EIP=%08lX ESP=%08lX EBP=%08lX EFLAGS=%08lX\r\n"
                 "  EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX\r\n",
                 (unsigned long)c->Eip, (unsigned long)c->Esp,
                 (unsigned long)c->Ebp, (unsigned long)c->EFlags,
                 (unsigned long)c->Eax, (unsigned long)c->Ebx,
                 (unsigned long)c->Ecx, (unsigned long)c->Edx,
                 (unsigned long)c->Esi, (unsigned long)c->Edi);
        crash_write_str(hErr, buf);
#endif
    }

    crash_write_str(hErr,
                    "------------------------------------------------------------------------\r\n");
}

static LONG WINAPI reaviutl_crash_filter(EXCEPTION_POINTERS *ep)
{
    write_seh_report(ep, NULL, NULL, FALSE);
    return EXCEPTION_EXECUTE_HANDLER;
}

int debug_console_seh_filter(EXCEPTION_POINTERS *ep, const char *plugin_name, const char *stage)
{
    write_seh_report(ep, plugin_name, stage, TRUE);
    if (ep && ep->ExceptionRecord &&
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
    {
        _resetstkoflw();
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void debug_console_init(BOOL is_bridge_helper)
{
    char env_buf[16] = {0};
    HWND con_hwnd;

    ensure_log_lock();
    if (g_initialized)
        return;
    g_initialized = TRUE;
    g_is_bridge = is_bridge_helper;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    if (!is_bridge_helper &&
        GetEnvironmentVariableA("REAVIUTL_NO_CONSOLE", env_buf, sizeof(env_buf)) > 0 &&
        lstrcmpA(env_buf, "1") == 0)
    {
        SetUnhandledExceptionFilter(reaviutl_crash_filter);
        return;
    }

    con_hwnd = GetConsoleWindow();
    if (!con_hwnd && !is_bridge_helper)
    {
        if (AllocConsole())
        {
            FILE *fp = NULL;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            freopen_s(&fp, "CONIN$", "r", stdin);
            con_hwnd = GetConsoleWindow();
        }
    }

    // disable CRT stream buffering so plugin/host printf appears immediately
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    enable_vt_on_handle(GetStdHandle(STD_OUTPUT_HANDLE));
    enable_vt_on_handle(GetStdHandle(STD_ERROR_HANDLE));

    if (!is_bridge_helper && con_hwnd)
    {
        DWORD pids[4] = {0};
        DWORD n_procs = GetConsoleProcessList(pids, 4);
        g_owns_console = (n_procs <= 1);

        if (g_owns_console)
        {
            HMENU sysm;
            // set cp932 so shift-jis printf / lua print() renders properly
            SetConsoleOutputCP(932);
            SetConsoleCP(932);

#if REAVIUTL_X64
            SetConsoleTitleW(L"reaviutl console (x64)");
#else
            SetConsoleTitleW(L"reaviutl console (x86)");
#endif
            // disable console close button so closing it does not kill the process
            sysm = GetSystemMenu(con_hwnd, FALSE);
            if (sysm)
            {
                DeleteMenu(sysm, SC_CLOSE, MF_BYCOMMAND);
            }
        }
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    }

    SetUnhandledExceptionFilter(reaviutl_crash_filter);
}

static void enqueue_debug_command(const char *cmd)
{
    const char *s = cmd;
    if (!s)
        return;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s)
        return;

    EnterCriticalSection(&g_log_cs);
    if (g_cmd_count < CMD_QUEUE_CAP)
    {
        lstrcpynA(g_cmd_queue[g_cmd_tail], s, CMD_MAX_LEN);
        g_cmd_tail = (g_cmd_tail + 1) % CMD_QUEUE_CAP;
        g_cmd_count++;
    }
    LeaveCriticalSection(&g_log_cs);

    if (g_notify_hwnd && IsWindow(g_notify_hwnd))
    {
        PostMessageA(g_notify_hwnd, WM_REAVIUTL_DEBUG_CMD, 0, 0);
    }
}

static DWORD WINAPI console_input_thread_proc(LPVOID param)
{
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE waits[2];
    char line[CMD_MAX_LEN];
    int len = 0;

    (void)param;
    if (!hIn || hIn == INVALID_HANDLE_VALUE || !g_stop_ev)
        return 0;

    waits[0] = g_stop_ev;
    waits[1] = hIn;

    for (;;)
    {
        DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        INPUT_RECORD rec;
        DWORD got = 0;

        if (wr != (WAIT_OBJECT_0 + 1))
        {
            break;
        }
        if (!ReadConsoleInputA(hIn, &rec, 1, &got) || got == 0)
        {
            continue;
        }
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
        {
            continue;
        }
        {
            char ch = rec.Event.KeyEvent.uChar.AsciiChar;
            DWORD written = 0;

            if (ch == '\r' || ch == '\n')
            {
                WriteConsoleA(hOut, "\r\n", 2, &written, NULL);
                line[len] = '\0';
                enqueue_debug_command(line);
                len = 0;
            }
            else if (ch == '\b')
            {
                if (len > 0)
                {
                    len--;
                    WriteConsoleA(hOut, "\b \b", 3, &written, NULL);
                }
            }
            else if (ch == 27)
            { // ESC clears current typed input
                while (len > 0)
                {
                    len--;
                    WriteConsoleA(hOut, "\b \b", 3, &written, NULL);
                }
            }
            else if ((unsigned char)ch >= 32 && len < CMD_MAX_LEN - 1)
            {
                line[len++] = ch;
                WriteConsoleA(hOut, &ch, 1, &written, NULL);
            }
        }
    }
    return 0;
}

void debug_console_start_input(HWND notify_hwnd)
{
    HANDLE hIn;
    DWORD mode = 0;

    ensure_log_lock();
    g_notify_hwnd = notify_hwnd;

    if (g_input_thread || g_is_bridge || !g_owns_console)
        return;
    hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (!hIn || hIn == INVALID_HANDLE_VALUE)
        return;
    if (GetFileType(hIn) != FILE_TYPE_CHAR || !GetConsoleMode(hIn, &mode))
        return;

    g_stop_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_stop_ev)
        return;

    g_input_thread = CreateThread(NULL, 0, console_input_thread_proc, NULL, 0, NULL);
    if (!g_input_thread)
    {
        CloseHandle(g_stop_ev);
        g_stop_ev = NULL;
    }
}

BOOL debug_console_pop_command(char *out_buf, int max_len)
{
    BOOL ok = FALSE;
    if (!out_buf || max_len <= 0)
        return FALSE;
    ensure_log_lock();

    EnterCriticalSection(&g_log_cs);
    if (g_cmd_count > 0)
    {
        lstrcpynA(out_buf, g_cmd_queue[g_cmd_head], max_len);
        g_cmd_head = (g_cmd_head + 1) % CMD_QUEUE_CAP;
        g_cmd_count--;
        ok = TRUE;
    }
    LeaveCriticalSection(&g_log_cs);
    return ok;
}

void debug_console_shutdown(void)
{
    if (g_stop_ev)
    {
        SetEvent(g_stop_ev);
    }
    if (g_input_thread)
    {
        WaitForSingleObject(g_input_thread, 1000);
        CloseHandle(g_input_thread);
        g_input_thread = NULL;
    }
    if (g_stop_ev)
    {
        CloseHandle(g_stop_ev);
        g_stop_ev = NULL;
    }
}

BOOL debug_console_is_visible(void)
{
    HWND h = GetConsoleWindow();
    return (h && IsWindowVisible(h)) ? TRUE : FALSE;
}

void debug_console_set_visible(BOOL visible)
{
    HWND h = GetConsoleWindow();
    if (!h)
        return;
    ShowWindow(h, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

BOOL debug_console_toggle_visible(void)
{
    BOOL next = !debug_console_is_visible();
    debug_console_set_visible(next);
    return next;
}

void debug_console_clear(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD count, written;
    COORD home = {0, 0};

    if (!hOut || hOut == INVALID_HANDLE_VALUE)
        return;
    if (!GetConsoleScreenBufferInfo(hOut, &csbi))
        return;
    count = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
    FillConsoleOutputCharacterA(hOut, ' ', count, home, &written);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, count, home, &written);
    SetConsoleCursorPosition(hOut, home);
}

void debug_console_set_exfunc_trace(BOOL enable)
{
    InterlockedExchange(&g_exfunc_trace, enable ? 1 : 0);
}

BOOL debug_console_get_exfunc_trace(void)
{
    return g_exfunc_trace != 0;
}

static const wchar_t *level_ansi_color(int level)
{
    switch (level)
    {
    case DBG_LEVEL_PLUGIN:
        return L"\x1b[36m";
    case DBG_LEVEL_AUP:
        return L"\x1b[32m";
    case DBG_LEVEL_EXFUNC:
        return L"\x1b[35m";
    case DBG_LEVEL_WARN:
        return L"\x1b[33m";
    case DBG_LEVEL_ERROR:
        return L"\x1b[31m";
    default:
        return L"\x1b[90m";
    }
}

void dbg_log_level(int level, const char *tag, const char *fmt, ...)
{
    SYSTEMTIME st;
    char body[1536];
    char line[1700];
    const char *proc_tag;
    HANDLE hOut;
    DWORD mode = 0;
    va_list ap;

    if (!fmt)
        return;
    ensure_log_lock();

    GetLocalTime(&st);
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    body[sizeof(body) - 1] = '\0';

    if (tag && tag[0])
    {
        proc_tag = tag;
    }
    else if (g_is_bridge)
    {
        proc_tag = "BR32";
    }
    else
    {
        proc_tag = REAVIUTL_X64 ? "HOST64" : "HOST32";
    }

    hOut = GetStdHandle((level >= DBG_LEVEL_WARN) ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (!hOut || hOut == INVALID_HANDLE_VALUE)
    {
        hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (!hOut || hOut == INVALID_HANDLE_VALUE)
        return;

    EnterCriticalSection(&g_log_cs);
    {
        wchar_t wbody[1600];
        wchar_t wline[1900];
        wchar_t wtag[32];
        DWORD written = 0;

        if (MultiByteToWideChar(932, 0, body, -1, wbody, 1600) <= 0)
        {
            MultiByteToWideChar(CP_ACP, 0, body, -1, wbody, 1600);
        }
        MultiByteToWideChar(CP_ACP, 0, proc_tag, -1, wtag, 32);

        if (GetConsoleMode(hOut, &mode))
        {
            BOOL use_vt = (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
            if (use_vt)
            {
                _snwprintf(wline, 1900, L"\x1b[90m[%02u:%02u:%02u.%03u]\x1b[0m %s[%-6s]\x1b[0m %s\r\n",
                           st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                           level_ansi_color(level), wtag, wbody);
            }
            else
            {
                _snwprintf(wline, 1900, L"[%02u:%02u:%02u.%03u] [%-6s] %s\r\n",
                           st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                           wtag, wbody);
            }
            wline[1899] = L'\0';
            WriteConsoleW(hOut, wline, (DWORD)wcslen(wline), &written, NULL);
        }
        else
        {
            int u8_len;
            _snwprintf(wline, 1900, L"[%02u:%02u:%02u.%03u] [%-6s] %s\r\n",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                       wtag, wbody);
            wline[1899] = L'\0';
            u8_len = WideCharToMultiByte(CP_UTF8, 0, wline, -1, line, (int)sizeof(line), NULL, NULL);
            if (u8_len > 1)
            {
                WriteFile(hOut, line, (DWORD)(u8_len - 1), &written, NULL);
            }
        }
    }
    LeaveCriticalSection(&g_log_cs);
}
