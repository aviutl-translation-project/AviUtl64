#pragma once

#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

#include <windows.h>

#define WM_REAVIUTL_DEBUG_CMD (WM_APP + 0x60)

enum DebugLogLevel
{
    DBG_LEVEL_INFO = 0,
    DBG_LEVEL_PLUGIN = 1,
    DBG_LEVEL_AUP = 2,
    DBG_LEVEL_EXFUNC = 3,
    DBG_LEVEL_WARN = 4,
    DBG_LEVEL_ERROR = 5
};

void debug_console_init(BOOL is_bridge_helper);
void debug_console_start_input(HWND notify_hwnd);
BOOL debug_console_pop_command(char *out_buf, int max_len);
void debug_console_shutdown(void);

BOOL debug_console_is_visible(void);
void debug_console_set_visible(BOOL visible);
BOOL debug_console_toggle_visible(void);
void debug_console_clear(void);

void debug_console_set_exfunc_trace(BOOL enable);
BOOL debug_console_get_exfunc_trace(void);

int debug_console_seh_filter(EXCEPTION_POINTERS *ep, const char *plugin_name, const char *stage);

void dbg_log_level(int level, const char *tag, const char *fmt, ...);

#define dbg_info(...) dbg_log_level(DBG_LEVEL_INFO, NULL, __VA_ARGS__)
#define dbg_plugin(...) dbg_log_level(DBG_LEVEL_PLUGIN, "PLUGIN", __VA_ARGS__)
#define dbg_aup(...) dbg_log_level(DBG_LEVEL_AUP, "AUP", __VA_ARGS__)
#define dbg_warn(...) dbg_log_level(DBG_LEVEL_WARN, "WARN", __VA_ARGS__)
#define dbg_error(...) dbg_log_level(DBG_LEVEL_ERROR, "ERROR", __VA_ARGS__)
#define dbg_exfunc(...)                                             \
    do                                                              \
    {                                                               \
        if (debug_console_get_exfunc_trace())                       \
        {                                                           \
            dbg_log_level(DBG_LEVEL_EXFUNC, "EXFUNC", __VA_ARGS__); \
        }                                                           \
    } while (0)

#endif
