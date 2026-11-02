#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input_plugin.h"
#include "plugin_bridge.h"
#include "plugin_cache.h"
#include "debug_console.h"

typedef INPUT_PLUGIN_TABLE *(__stdcall *GetInputPluginTableFn)(void);

typedef struct InputPluginEntry
{
    HMODULE hmod;
    INPUT_PLUGIN_TABLE *table;
    char name[128];
    char filefilter[1024];
    int filefilter_len;
    char information[256];
    char path[MAX_PATH];
    int flag;
    int builtin_kind;
    BOOL has_config;
    int priority;
    BOOL inited;
    BOOL bridged_x86;
    int bridge_aui_idx;
} InputPluginEntry;

struct InputPluginInstance
{
    int plugin_idx;
    INPUT_PLUGIN_TABLE *table;
    INPUT_HANDLE ih;
};

static InputPluginEntry g_inputs[INPUT_PLUGIN_MAX];
static int g_input_count = 0;
static BOOL g_inputs_initialized = FALSE;
static int g_wav_opt_w = 0;
static int g_wav_opt_h = 0;
static InputWavConfigDialogFn g_wav_cfg_fn = NULL;

static void get_ini_path(char *out_path, size_t out_sz)
{
    char mod_path[MAX_PATH];
    char *slash;
    if (GetModuleFileNameA(NULL, mod_path, MAX_PATH) > 0)
    {
        slash = strrchr(mod_path, '\\');
        if (slash)
        {
            *(slash + 1) = '\0';
        }
        else
        {
            mod_path[0] = '\0';
        }
        _snprintf(out_path, out_sz, "%saviutl.ini", mod_path);
        out_path[out_sz - 1] = '\0';
    }
    else
    {
        lstrcpynA(out_path, ".\\aviutl.ini", (int)out_sz);
    }
}

static void ini_write_int(const char *section, const char *key, int val, const char *ini_path)
{
    char buf[32];
    wsprintfA(buf, "%d", val);
    WritePrivateProfileStringA(section, key, buf, ini_path);
}

// 0x004328d0: strip '[' and ']' from plugin name for aviutl.ini section header
static void sanitize_ini_section(const char *name, char *out_sec, size_t out_sz)
{
    const unsigned char *s = (const unsigned char *)(name ? name : "");
    size_t d = 0;

    if (!out_sec || out_sz == 0)
    {
        return;
    }
    while (*s && d + 1 < out_sz)
    {
        if (IsDBCSLeadByteEx(932, *s))
        {
            if (s[1] && d + 2 < out_sz)
            {
                out_sec[d++] = (char)s[0];
                out_sec[d++] = (char)s[1];
                s += 2;
            }
            else
            {
                break;
            }
        }
        else
        {
            if (*s != '[' && *s != ']')
            {
                out_sec[d++] = (char)*s;
            }
            s++;
        }
    }
    out_sec[d] = '\0';
}

static int measure_double_nul(const char *flt, int max_bytes)
{
    int i = 0;
    if (!flt || max_bytes < 2)
    {
        return 0;
    }
    while (i + 1 < max_bytes)
    {
        if (flt[i] == '\0' && flt[i + 1] == '\0')
        {
            return i + 2;
        }
        i++;
    }
    return max_bytes;
}

static void copy_double_nul(char *dst, int dst_cap, const char *src, int *out_len)
{
    int len;
    memset(dst, 0, (size_t)dst_cap);
    if (!src || dst_cap < 2)
    {
        if (out_len)
        {
            *out_len = 0;
        }
        return;
    }
    len = measure_double_nul(src, dst_cap - 2);
    if (len > dst_cap - 2)
    {
        len = dst_cap - 2;
    }
    memcpy(dst, src, (size_t)len);
    dst[dst_cap - 2] = '\0';
    dst[dst_cap - 1] = '\0';
    if (out_len)
    {
        *out_len = measure_double_nul(dst, dst_cap);
    }
}

static BOOL has_aui_path(const char *path)
{
    int i;
    for (i = 0; i < g_input_count; i++)
    {
        if (g_inputs[i].path[0] && lstrcmpiA(g_inputs[i].path, path) == 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL seh_aui_init(INPUT_PLUGIN_TABLE *tbl)
{
    if (!tbl || !tbl->func_init)
    {
        return TRUE;
    }
#ifdef _MSC_VER
    __try
    {
        return tbl->func_init();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        dbg_error(".aui func_init crashed in '%s'", tbl->name ? tbl->name : "?");
        return FALSE;
    }
#else
    return tbl->func_init();
#endif
}

static void seh_aui_exit(INPUT_PLUGIN_TABLE *tbl)
{
    if (!tbl || !tbl->func_exit)
    {
        return;
    }
#ifdef _MSC_VER
    __try
    {
        tbl->func_exit();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        dbg_error(".aui func_exit crashed");
    }
#else
    tbl->func_exit();
#endif
}

static INPUT_HANDLE seh_aui_open(INPUT_PLUGIN_TABLE *tbl, const char *filepath)
{
    if (!tbl || !tbl->func_open || !filepath)
    {
        return NULL;
    }
#ifdef _MSC_VER
    __try
    {
        return tbl->func_open((LPSTR)filepath);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        dbg_error(".aui func_open crashed on '%s'", filepath);
        return NULL;
    }
#else
    return tbl->func_open((LPSTR)filepath);
#endif
}

static void seh_aui_close(INPUT_PLUGIN_TABLE *tbl, INPUT_HANDLE ih)
{
    if (!tbl || !tbl->func_close || !ih)
    {
        return;
    }
#ifdef _MSC_VER
    __try
    {
        tbl->func_close(ih);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
#else
    tbl->func_close(ih);
#endif
}

static BOOL seh_aui_info_get(INPUT_PLUGIN_TABLE *tbl, INPUT_HANDLE ih, INPUT_INFO *iip)
{
    if (!tbl || !tbl->func_info_get || !ih || !iip)
    {
        return FALSE;
    }
#ifdef _MSC_VER
    __try
    {
        return tbl->func_info_get(ih, iip);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return FALSE;
    }
#else
    return tbl->func_info_get(ih, iip);
#endif
}

static int seh_aui_read_video(INPUT_PLUGIN_TABLE *tbl, INPUT_HANDLE ih, int frame, void *buf)
{
    if (!tbl || !tbl->func_read_video || !ih || !buf)
    {
        return 0;
    }
#ifdef _MSC_VER
    __try
    {
        return tbl->func_read_video(ih, frame, buf);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
#else
    return tbl->func_read_video(ih, frame, buf);
#endif
}

static int seh_aui_read_audio(INPUT_PLUGIN_TABLE *tbl, INPUT_HANDLE ih, int start, int length, void *buf)
{
    if (!tbl || !tbl->func_read_audio || !ih || !buf)
    {
        return 0;
    }
#ifdef _MSC_VER
    __try
    {
        return tbl->func_read_audio(ih, start, length, buf);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
#else
    return tbl->func_read_audio(ih, start, length, buf);
#endif
}

static BOOL seh_aui_is_keyframe(INPUT_PLUGIN_TABLE *tbl, INPUT_HANDLE ih, int frame)
{
    if (!tbl || !ih)
    {
        return TRUE;
    }
    if (!tbl->func_is_keyframe)
    {
        return TRUE;
    }
#ifdef _MSC_VER
    __try
    {
        return tbl->func_is_keyframe(ih, frame);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return TRUE;
    }
#else
    return tbl->func_is_keyframe(ih, frame);
#endif
}

static BOOL seh_aui_config(INPUT_PLUGIN_TABLE *tbl, HWND hwnd, HINSTANCE hinst)
{
    if (!tbl || !tbl->func_config)
    {
        return FALSE;
    }
#ifdef _MSC_VER
    __try
    {
        return tbl->func_config(hwnd, hinst);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        dbg_error(".aui func_config crashed");
        return FALSE;
    }
#else
    return tbl->func_config(hwnd, hinst);
#endif
}

// 0x00426360: ensure external .aui dll is loaded and initialized before use
static BOOL ensure_aui_loaded(InputPluginEntry *e)
{
    GetInputPluginTableFn fn;
    INPUT_PLUGIN_TABLE *tbl = NULL;

    if (!e || e->builtin_kind != BUILTIN_INPUT_NONE || e->bridged_x86)
    {
        return FALSE;
    }
    if (e->hmod && e->table && e->inited)
    {
        return TRUE;
    }

    if (!e->hmod)
    {
        e->hmod = LoadLibraryA(e->path);
        if (!e->hmod)
        {
            return FALSE;
        }
    }

    fn = (GetInputPluginTableFn)GetProcAddress(e->hmod, "GetInputPluginTable");
    if (!fn)
    {
        fn = (GetInputPluginTableFn)GetProcAddress(e->hmod, "_GetInputPluginTable@0");
    }
    if (fn)
    {
#ifdef _MSC_VER
        __try
        {
            tbl = fn();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            tbl = NULL;
        }
#else
        tbl = fn();
#endif
    }
    if (!tbl)
    {
        FreeLibrary(e->hmod);
        e->hmod = NULL;
        return FALSE;
    }

    e->table = tbl;
    if (!e->inited)
    {
        if (!seh_aui_init(tbl))
        {
            FreeLibrary(e->hmod);
            e->hmod = NULL;
            e->table = NULL;
            return FALSE;
        }
        e->inited = TRUE;
    }
    return TRUE;
}

static void add_builtin_reader(const char *name, const char *flt, int flt_len,
                               int flag, int builtin_kind, BOOL has_cfg)
{
    InputPluginEntry *e;
    if (g_input_count >= INPUT_PLUGIN_MAX)
    {
        return;
    }
    e = &g_inputs[g_input_count];
    memset(e, 0, sizeof(*e));
    lstrcpynA(e->name, name, sizeof(e->name));
    lstrcpynA(e->information, name, sizeof(e->information));
    if (flt && flt_len > 0 && flt_len <= (int)sizeof(e->filefilter))
    {
        memcpy(e->filefilter, flt, (size_t)flt_len);
        e->filefilter_len = flt_len;
    }
    e->flag = flag;
    e->builtin_kind = builtin_kind;
    e->has_config = has_cfg;
    e->priority = (flag & INPUT_PLUGIN_FLAG_HIDE_LIST) ? 9999 : g_input_count;
    e->bridge_aui_idx = -1;
    g_input_count++;
}

static void scan_single_aui(const char *full_path, const WIN32_FIND_DATAA *fd, const char *ini_path)
{
    HMODULE hmod;
    GetInputPluginTableFn fn;
    INPUT_PLUGIN_TABLE *tbl = NULL;
    InputPluginEntry *e;
    int host_arch = REAVIUTL_X64 ? 64 : 32;
    int cached_flag = 0, cached_flt_len = 0;
    char cached_name[128] = {0};
    char cached_flt[1024] = {0};
    char cached_info[256] = {0};
    PluginCacheLookup clk;

    if (!full_path || !full_path[0] || g_input_count >= INPUT_PLUGIN_MAX || has_aui_path(full_path))
    {
        return;
    }

    clk = plugin_cache_try_load(ini_path, full_path, fd, host_arch,
                                &cached_flag, cached_name, sizeof(cached_name),
                                cached_flt, sizeof(cached_flt), &cached_flt_len,
                                cached_info, sizeof(cached_info));
    if (clk == PLUGIN_CACHE_HIT_OTHER_ARCH)
    {
        return;
    }
    if (clk == PLUGIN_CACHE_HIT_NATIVE && cached_name[0])
    {
        e = &g_inputs[g_input_count++];
        memset(e, 0, sizeof(*e));
        lstrcpynA(e->name, cached_name, sizeof(e->name));
        memcpy(e->filefilter, cached_flt, sizeof(e->filefilter));
        e->filefilter_len = cached_flt_len;
        lstrcpynA(e->information, cached_info[0] ? cached_info : e->name, sizeof(e->information));
        lstrcpynA(e->path, full_path, sizeof(e->path));
        e->flag = cached_flag & ~(INPUT_PLUGIN_FLAG_BUILTIN | INPUT_PLUGIN_FLAG_HIDE_LIST | 0x100);
        e->builtin_kind = BUILTIN_INPUT_NONE;
        e->has_config = (cached_flag & 0x100) != 0;
        e->priority = g_input_count - 1;
        e->bridge_aui_idx = -1;
        dbg_plugin("Cached .aui input plugin: '%s' (%s)", e->name, full_path);
        return;
    }

    hmod = LoadLibraryA(full_path);
    if (!hmod)
    {
        return;
    }

    fn = (GetInputPluginTableFn)GetProcAddress(hmod, "GetInputPluginTable");
    if (!fn)
    {
        fn = (GetInputPluginTableFn)GetProcAddress(hmod, "_GetInputPluginTable@0");
    }
    if (fn)
    {
#ifdef _MSC_VER
        __try
        {
            tbl = fn();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            tbl = NULL;
        }
#else
        tbl = fn();
#endif
    }

    if (!tbl || !tbl->func_open || !tbl->func_close || !tbl->func_info_get)
    {
        FreeLibrary(hmod);
        return;
    }

    e = &g_inputs[g_input_count];
    memset(e, 0, sizeof(*e));
    lstrcpynA(e->name, tbl->name ? tbl->name : "InputPlugin", sizeof(e->name));
    copy_double_nul(e->filefilter, (int)sizeof(e->filefilter), tbl->filefilter, &e->filefilter_len);
    lstrcpynA(e->information,
              (tbl->information && tbl->information[0]) ? tbl->information : e->name,
              sizeof(e->information));
    lstrcpynA(e->path, full_path, sizeof(e->path));
    e->flag = tbl->flag & ~(INPUT_PLUGIN_FLAG_BUILTIN | INPUT_PLUGIN_FLAG_HIDE_LIST);
    e->builtin_kind = BUILTIN_INPUT_NONE;
    e->has_config = (tbl->func_config != NULL);
    e->priority = g_input_count;
    e->bridge_aui_idx = -1;

    {
        int save_flag = e->flag | (e->has_config ? 0x100 : 0);
        plugin_cache_save(ini_path, full_path, fd, host_arch,
                          &save_flag, e->name, e->filefilter, e->information);
    }

    // 0x004253e0: unload after metadata probe; lazy-load & func_init on first open/config
    FreeLibrary(hmod);
    g_input_count++;

    dbg_plugin("Discovered .aui input plugin: '%s' (%s)", e->name, full_path);
}

static void scan_aui_dir(const char *dir, const char *ini_path)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE hfind;

    if (!dir || !dir[0] || g_input_count >= INPUT_PLUGIN_MAX)
    {
        return;
    }

    _snprintf(pattern, sizeof(pattern), "%s\\*.aui", dir);
    pattern[sizeof(pattern) - 1] = '\0';

    hfind = FindFirstFileA(pattern, &fd);
    if (hfind == INVALID_HANDLE_VALUE)
    {
        return;
    }

    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            char full[MAX_PATH];
            _snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
            full[sizeof(full) - 1] = '\0';
            scan_single_aui(full, &fd, ini_path);
        }
    } while (g_input_count < INPUT_PLUGIN_MAX && FindNextFileA(hfind, &fd));

    FindClose(hfind);
}

static void register_default_builtins_only(void)
{
    static const char s_flt_avi[] = "AviFile (*.avi)\0*.avi\0";
    static const char s_flt_wav[] = "WaveFile (*.wav)\0*.wav\0";
    static const char s_flt_bmp[] = "BmpFile (*.bmp)\0*.bmp\0";
    static const char s_flt_img[] = "JPEG File (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0PNG File (*.png)\0*.png\0";
    static const char s_flt_vfw[] = "AviFile (*.avi)\0*.avi\0AllFile (*.*)\0*.*\0";

    add_builtin_reader("AVI/AVI2 File Reader", s_flt_avi, (int)sizeof(s_flt_avi),
                       INPUT_PLUGIN_FLAG_BUILTIN | INPUT_PLUGIN_FLAG_VIDEO | INPUT_PLUGIN_FLAG_AUDIO,
                       BUILTIN_INPUT_AVI, FALSE);
    add_builtin_reader("Wave File Reader", s_flt_wav, (int)sizeof(s_flt_wav),
                       INPUT_PLUGIN_FLAG_BUILTIN | INPUT_PLUGIN_FLAG_VIDEO | INPUT_PLUGIN_FLAG_AUDIO,
                       BUILTIN_INPUT_WAV, TRUE);
    add_builtin_reader("BMP File Reader", s_flt_bmp, (int)sizeof(s_flt_bmp),
                       INPUT_PLUGIN_FLAG_BUILTIN | INPUT_PLUGIN_FLAG_VIDEO,
                       BUILTIN_INPUT_BMP, FALSE);
    add_builtin_reader("JPEG/PNG File Reader", s_flt_img, (int)sizeof(s_flt_img),
                       INPUT_PLUGIN_FLAG_BUILTIN | INPUT_PLUGIN_FLAG_VIDEO,
                       BUILTIN_INPUT_JPG_PNG, FALSE);
    add_builtin_reader("AVI File Reader ( Video For Windows )", s_flt_vfw, (int)sizeof(s_flt_vfw),
                       INPUT_PLUGIN_FLAG_BUILTIN | INPUT_PLUGIN_FLAG_VIDEO | INPUT_PLUGIN_FLAG_AUDIO,
                       BUILTIN_INPUT_VFW, FALSE);
    add_builtin_reader("AVIs File Reader", s_flt_avi, (int)sizeof(s_flt_avi),
                       INPUT_PLUGIN_FLAG_HIDE_LIST | INPUT_PLUGIN_FLAG_BUILTIN |
                           INPUT_PLUGIN_FLAG_VIDEO | INPUT_PLUGIN_FLAG_AUDIO,
                       BUILTIN_INPUT_AVIS, FALSE);
}

static void save_priorities_to_ini(void)
{
    char ini[MAX_PATH];
    int i;
    get_ini_path(ini, sizeof(ini));
    for (i = 0; i < g_input_count; i++)
    {
        char sec[160];
        if (g_inputs[i].flag & INPUT_PLUGIN_FLAG_HIDE_LIST)
        {
            continue;
        }
        sanitize_ini_section(g_inputs[i].name, sec, sizeof(sec));
        if (sec[0])
        {
            ini_write_int(sec, "priority", g_inputs[i].priority, ini);
        }
    }
}

// 0x004253e0: register 6 built-in readers, scan external .aui plugins, and load priorities
void input_plugin_init(const char *plugin_dir, HWND main_hwnd)
{
    char ini[MAX_PATH];
    char exe_dir[MAX_PATH];
    int i, next_def = 0;

    input_plugin_shutdown();

    get_ini_path(ini, sizeof(ini));
    g_wav_opt_w = GetPrivateProfileIntA("Wave File Reader", "w", 0, ini);
    g_wav_opt_h = GetPrivateProfileIntA("Wave File Reader", "h", 0, ini);
    if (g_wav_opt_w < 0)
    {
        g_wav_opt_w = 0;
    }
    if (g_wav_opt_h < 0)
    {
        g_wav_opt_h = 0;
    }

    register_default_builtins_only();

    exe_dir[0] = '\0';
    if (GetModuleFileNameA(NULL, exe_dir, MAX_PATH) > 0)
    {
        char *slash = strrchr(exe_dir, '\\');
        if (slash)
        {
            *slash = '\0';
        }
        else
        {
            exe_dir[0] = '\0';
        }
    }

    if (exe_dir[0])
    {
        scan_aui_dir(exe_dir, ini);
    }
    if (plugin_dir && plugin_dir[0] && (!exe_dir[0] || lstrcmpiA(exe_dir, plugin_dir) != 0))
    {
        scan_aui_dir(plugin_dir, ini);
    }

#if REAVIUTL_X64
    if (plugin_bridge_scan_input_color(plugin_dir, main_hwnd))
    {
        int n = plugin_bridge_get_aui_count();
        for (i = 0; i < n && g_input_count < INPUT_PLUGIN_MAX; i++)
        {
            BridgeAuiInfo bi;
            if (plugin_bridge_get_aui_info(i, &bi) && !has_aui_path(bi.path))
            {
                InputPluginEntry *e = &g_inputs[g_input_count];
                memset(e, 0, sizeof(*e));
                lstrcpynA(e->name, bi.name, sizeof(e->name));
                if (bi.filefilter_len > 0 && bi.filefilter_len <= (int)sizeof(e->filefilter))
                {
                    memcpy(e->filefilter, bi.filefilter, (size_t)bi.filefilter_len);
                    e->filefilter_len = bi.filefilter_len;
                }
                else
                {
                    copy_double_nul(e->filefilter, (int)sizeof(e->filefilter),
                                    bi.filefilter, &e->filefilter_len);
                }
                lstrcpynA(e->information, bi.information[0] ? bi.information : bi.name,
                          sizeof(e->information));
                lstrcpynA(e->path, bi.path, sizeof(e->path));
                e->flag = bi.flag & ~(INPUT_PLUGIN_FLAG_BUILTIN | INPUT_PLUGIN_FLAG_HIDE_LIST);
                e->builtin_kind = BUILTIN_INPUT_NONE;
                e->has_config = (bi.has_config != 0);
                e->priority = g_input_count;
                e->bridged_x86 = TRUE;
                e->bridge_aui_idx = i;
                g_input_count++;
                dbg_plugin("Mirrored 32-bit .aui via bridge: '%s' (%s)", e->name, e->path);
            }
        }
    }
#else
    (void)main_hwnd;
#endif

    // load saved priorities from aviutl.ini
    for (i = 0; i < g_input_count; i++)
    {
        char sec[160];
        if (g_inputs[i].flag & INPUT_PLUGIN_FLAG_HIDE_LIST)
        {
            g_inputs[i].priority = 9999;
            continue;
        }
        sanitize_ini_section(g_inputs[i].name, sec, sizeof(sec));
        g_inputs[i].priority = (int)GetPrivateProfileIntA(sec, "priority", next_def, ini);
        next_def++;
    }

    // normalize priorities into contiguous 0..visible_count-1
    {
        int order[INPUT_PLUGIN_MAX];
        int vis_n = input_plugin_get_priority_order(order, INPUT_PLUGIN_MAX, FALSE);
        for (i = 0; i < vis_n; i++)
        {
            g_inputs[order[i]].priority = i;
#if REAVIUTL_X64
            if (g_inputs[order[i]].bridged_x86 && g_inputs[order[i]].bridge_aui_idx >= 0)
            {
                plugin_bridge_aui_set_priority(g_inputs[order[i]].bridge_aui_idx, i);
            }
#endif
        }
    }

    g_inputs_initialized = TRUE;
}

// 0x004260e0: save priorities and unload initialized .aui plugins
void input_plugin_shutdown(void)
{
    int i;
    if (g_inputs_initialized && g_input_count > 0)
    {
        save_priorities_to_ini();
    }
    for (i = 0; i < g_input_count; i++)
    {
        InputPluginEntry *e = &g_inputs[i];
        if (e->hmod)
        {
            if (e->inited && e->table)
            {
                seh_aui_exit(e->table);
            }
            FreeLibrary(e->hmod);
        }
        memset(e, 0, sizeof(*e));
    }
    g_input_count = 0;
    g_inputs_initialized = FALSE;
}

static void ensure_fallback_builtins(void)
{
    if (g_input_count == 0)
    {
        register_default_builtins_only();
    }
}

int input_plugin_get_count(void)
{
    ensure_fallback_builtins();
    return g_input_count;
}

BOOL input_plugin_get_summary(int idx, InputPluginSummary *out_sum)
{
    const InputPluginEntry *e;
    ensure_fallback_builtins();
    if (!out_sum || idx < 0 || idx >= g_input_count)
    {
        return FALSE;
    }
    e = &g_inputs[idx];
    memset(out_sum, 0, sizeof(*out_sum));
    lstrcpynA(out_sum->name, e->name, sizeof(out_sum->name));
    if (e->filefilter_len > 0 && e->filefilter_len <= (int)sizeof(out_sum->filefilter))
    {
        memcpy(out_sum->filefilter, e->filefilter, (size_t)e->filefilter_len);
    }
    out_sum->filefilter_len = e->filefilter_len;
    lstrcpynA(out_sum->information, e->information, sizeof(out_sum->information));
    lstrcpynA(out_sum->path, e->path, sizeof(out_sum->path));
    out_sum->flag = e->flag;
    out_sum->builtin_kind = e->builtin_kind;
    out_sum->has_config = e->has_config;
    out_sum->priority = e->priority;
    out_sum->bridged_x86 = e->bridged_x86;
    out_sum->bridge_aui_idx = e->bridge_aui_idx;
    return TRUE;
}

int input_plugin_get_ext_count(void)
{
    int i, n = 0;
    ensure_fallback_builtins();
    for (i = 0; i < g_input_count; i++)
    {
        if (!(g_inputs[i].flag & INPUT_PLUGIN_FLAG_BUILTIN))
        {
            n++;
        }
    }
    return n;
}

int input_plugin_ext_to_index(int ext_idx)
{
    int i, cur = 0;
    ensure_fallback_builtins();
    if (ext_idx < 0)
    {
        return -1;
    }
    for (i = 0; i < g_input_count; i++)
    {
        if (!(g_inputs[i].flag & INPUT_PLUGIN_FLAG_BUILTIN))
        {
            if (cur == ext_idx)
            {
                return i;
            }
            cur++;
        }
    }
    return -1;
}

BOOL input_plugin_get_ext_summary(int ext_idx, InputPluginSummary *out_sum)
{
    int idx = input_plugin_ext_to_index(ext_idx);
    if (idx < 0)
    {
        return FALSE;
    }
    return input_plugin_get_summary(idx, out_sum);
}

int input_plugin_get_priority_order(int *out_indices, int max_count, BOOL include_hidden)
{
    int i, j, n = 0;
    ensure_fallback_builtins();
    if (!out_indices || max_count <= 0)
    {
        return 0;
    }

    for (i = 0; i < g_input_count && n < max_count; i++)
    {
        if (!include_hidden && (g_inputs[i].flag & INPUT_PLUGIN_FLAG_HIDE_LIST))
        {
            continue;
        }
        out_indices[n++] = i;
    }

    // stable insertion sort by ascending priority
    for (i = 1; i < n; i++)
    {
        int key = out_indices[i];
        int kp = g_inputs[key].priority;
        j = i - 1;
        while (j >= 0 && g_inputs[out_indices[j]].priority > kp)
        {
            out_indices[j + 1] = out_indices[j];
            j--;
        }
        out_indices[j + 1] = key;
    }
    return n;
}

void input_plugin_apply_priority_order(const int *ordered_indices, int count, BOOL save_ini)
{
    int i;
    if (!ordered_indices || count <= 0)
    {
        return;
    }
    for (i = 0; i < count; i++)
    {
        int idx = ordered_indices[i];
        if (idx >= 0 && idx < g_input_count && !(g_inputs[idx].flag & INPUT_PLUGIN_FLAG_HIDE_LIST))
        {
            g_inputs[idx].priority = i;
#if REAVIUTL_X64
            if (g_inputs[idx].bridged_x86 && g_inputs[idx].bridge_aui_idx >= 0)
            {
                plugin_bridge_aui_set_priority(g_inputs[idx].bridge_aui_idx, i);
            }
#endif
        }
    }
    if (save_ini)
    {
        save_priorities_to_ini();
    }
}

void input_plugin_set_single_priority(int idx, int priority)
{
    if (idx >= 0 && idx < g_input_count)
    {
        g_inputs[idx].priority = priority;
    }
}

void input_plugin_set_wav_config_handler(InputWavConfigDialogFn fn)
{
    g_wav_cfg_fn = fn;
}

void input_plugin_get_wav_video_size(int *out_w, int *out_h)
{
    if (out_w)
    {
        *out_w = g_wav_opt_w;
    }
    if (out_h)
    {
        *out_h = g_wav_opt_h;
    }
}

void input_plugin_set_wav_video_size(int w, int h, BOOL save_ini)
{
    if (w < 0)
    {
        w = 0;
    }
    if (h < 0)
    {
        h = 0;
    }
    g_wav_opt_w = w;
    g_wav_opt_h = h;
    if (save_ini)
    {
        char ini[MAX_PATH];
        get_ini_path(ini, sizeof(ini));
        ini_write_int("Wave File Reader", "w", g_wav_opt_w, ini);
        ini_write_int("Wave File Reader", "h", g_wav_opt_h, ini);
    }
}

// 0x00427230: open plugin configuration dialog
BOOL input_plugin_config(int idx, HWND owner)
{
    InputPluginEntry *e;
    ensure_fallback_builtins();
    if (idx < 0 || idx >= g_input_count)
    {
        return FALSE;
    }
    e = &g_inputs[idx];
    if (!e->has_config)
    {
        return FALSE;
    }

    if (e->builtin_kind == BUILTIN_INPUT_WAV)
    {
        int w = g_wav_opt_w, h = g_wav_opt_h;
        if (g_wav_cfg_fn && g_wav_cfg_fn(owner, &w, &h))
        {
            input_plugin_set_wav_video_size(w, h, TRUE);
            return TRUE;
        }
        return FALSE;
    }

#if REAVIUTL_X64
    if (e->bridged_x86 && e->bridge_aui_idx >= 0)
    {
        return plugin_bridge_aui_config(e->bridge_aui_idx, owner);
    }
#endif

    if (!ensure_aui_loaded(e))
    {
        return FALSE;
    }
    return seh_aui_config(e->table, owner, (HINSTANCE)e->hmod);
}

static BOOL ext_list_contains(char exts[][32], int ext_count, const char *tok)
{
    int i;
    for (i = 0; i < ext_count; i++)
    {
        if (_stricmp(exts[i], tok) == 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void collect_filter_tokens(const char *flt, int flt_len, char exts[][32], int *inout_count, int max_exts)
{
    int pos = 0;
    if (!flt || flt_len <= 0)
    {
        return;
    }
    while (pos < flt_len && flt[pos] != '\0')
    {
        // skip display description
        int desc_len = (int)strlen(flt + pos);
        pos += desc_len + 1;
        if (pos >= flt_len || flt[pos] == '\0')
        {
            break;
        }

        // parse semicolon-delimited pattern string
        {
            const char *pat = flt + pos;
            int pat_len = (int)strlen(pat);
            const char *p = pat;
            while (*p)
            {
                char tok[32];
                int k = 0;
                while (*p == ' ' || *p == ';')
                {
                    p++;
                }
                if (!*p)
                {
                    break;
                }
                while (*p && *p != ';' && k < (int)sizeof(tok) - 1)
                {
                    tok[k++] = *p++;
                }
                tok[k] = '\0';
                while (*p && *p != ';')
                {
                    p++;
                }
                if (tok[0] && _stricmp(tok, "*.*") != 0 && *inout_count < max_exts)
                {
                    if (!ext_list_contains(exts, *inout_count, tok))
                    {
                        lstrcpynA(exts[*inout_count], tok, 32);
                        (*inout_count)++;
                    }
                }
            }
            pos += pat_len + 1;
        }
    }
}

// 0x00426fe0: build double-NUL file filter string from registered input plugins
LPSTR input_plugin_get_file_filter(int type)
{
    static char s_vid_buf[4096];
    static char s_aud_buf[4096];
    char exts[128][32];
    char joined[1536];
    int ext_count = 0;
    int order[INPUT_PLUGIN_MAX];
    int n, i, req_flag;
    char *out_buf;
    const char *label;

    ensure_fallback_builtins();

    req_flag = (type == 1) ? INPUT_PLUGIN_FLAG_AUDIO : INPUT_PLUGIN_FLAG_VIDEO;
    out_buf = (type == 1) ? s_aud_buf : s_vid_buf;
    label = (type == 1) ? "AudioFile" : "VideoFile";

    n = input_plugin_get_priority_order(order, INPUT_PLUGIN_MAX, FALSE);
    for (i = 0; i < n; i++)
    {
        const InputPluginEntry *e = &g_inputs[order[i]];
        if (e->flag & req_flag)
        {
            collect_filter_tokens(e->filefilter, e->filefilter_len, exts, &ext_count, 128);
        }
    }

    // include .mp3 in audio filter since built-in ACM/MP3 indexer supports it natively
    if (type == 1 && ext_count < 128 && !ext_list_contains(exts, ext_count, "*.mp3"))
    {
        lstrcpynA(exts[ext_count++], "*.mp3", 32);
    }

    joined[0] = '\0';
    for (i = 0; i < ext_count; i++)
    {
        size_t cur = strlen(joined);
        size_t tlen = strlen(exts[i]);
        if (cur + tlen + 2 >= sizeof(joined))
        {
            break;
        }
        if (cur > 0)
        {
            joined[cur++] = ';';
            joined[cur] = '\0';
        }
        memcpy(joined + cur, exts[i], tlen + 1);
    }
    if (!joined[0])
    {
        lstrcpynA(joined, "*.*", sizeof(joined));
    }

    memset(out_buf, 0, 4096);
    {
        int p = 0;
        p += _snprintf(out_buf + p, 1800, "%s (%s)", label, joined);
        p++;
        p += _snprintf(out_buf + p, 1800, "%s", joined);
        p++;
        memcpy(out_buf + p, "AllFile (*.*)\0*.*\0\0", 19);
    }
    return out_buf;
}

InputPluginInstance *input_plugin_open_native(int idx, const char *filepath, INPUT_INFO *out_info)
{
    InputPluginEntry *e;
    INPUT_HANDLE ih;
    INPUT_INFO info;
    InputPluginInstance *inst;

    ensure_fallback_builtins();
    if (idx < 0 || idx >= g_input_count || !filepath || !filepath[0] || !out_info)
    {
        return NULL;
    }
    e = &g_inputs[idx];
    if (e->builtin_kind != BUILTIN_INPUT_NONE || e->bridged_x86)
    {
        return NULL;
    }
    if (!ensure_aui_loaded(e))
    {
        return NULL;
    }

    ih = seh_aui_open(e->table, filepath);
    if (!ih)
    {
        return NULL;
    }

    memset(&info, 0, sizeof(info));
    if (!seh_aui_info_get(e->table, ih, &info))
    {
        seh_aui_close(e->table, ih);
        return NULL;
    }

    inst = (InputPluginInstance *)calloc(1, sizeof(InputPluginInstance));
    if (!inst)
    {
        seh_aui_close(e->table, ih);
        return NULL;
    }
    inst->plugin_idx = idx;
    inst->table = e->table;
    inst->ih = ih;
    *out_info = info;
    return inst;
}

void input_plugin_close_native(InputPluginInstance *inst)
{
    if (!inst)
    {
        return;
    }
    if (inst->table && inst->ih)
    {
        seh_aui_close(inst->table, inst->ih);
    }
    free(inst);
}

int input_plugin_read_video_native(InputPluginInstance *inst, int frame, void *buf)
{
    if (!inst)
    {
        return 0;
    }
    return seh_aui_read_video(inst->table, inst->ih, frame, buf);
}

int input_plugin_read_audio_native(InputPluginInstance *inst, int start, int length, void *buf)
{
    if (!inst)
    {
        return 0;
    }
    return seh_aui_read_audio(inst->table, inst->ih, start, length, buf);
}

BOOL input_plugin_is_keyframe_native(InputPluginInstance *inst, int frame)
{
    if (!inst)
    {
        return TRUE;
    }
    return seh_aui_is_keyframe(inst->table, inst->ih, frame);
}

