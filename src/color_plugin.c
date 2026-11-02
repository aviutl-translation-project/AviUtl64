#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "color_plugin.h"
#include "yc48_convert.h"
#include "exfunc.h"
#include "plugin_bridge.h"
#include "debug_console.h"

typedef COLOR_PLUGIN_TABLE *(__stdcall *GetColorPluginTableFn)(void);

typedef struct ColorPluginEntry
{
    HMODULE hmod;
    COLOR_PLUGIN_TABLE *table;
    char name[128];
    char information[256];
    char path[MAX_PATH];
    int flag;
    int builtin_kind; // 0 = external, 1 = bt601, 2 = bt709, 3 = auto
    BOOL inited;
    BOOL is_yuy2_table;
    BOOL bridged_x86;
} ColorPluginEntry;

static ColorPluginEntry g_color_plugins[COLOR_PLUGIN_MAX];
static int g_color_count = 0;
static BOOL g_use_sse = TRUE;
static BOOL g_use_sse2 = TRUE;
static BOOL g_yuy2_mode = FALSE;
static int g_selected_input = 2;  // 0=BT.601, 1=BT.709, 2=Auto, >=3=external .auc
static int g_selected_output = 2; // 0=BT.601, 1=BT.709, 2=Auto, >=3=external .auc

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

static BOOL seh_color_init(COLOR_PLUGIN_TABLE *tbl)
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
        dbg_error(".auc func_init crashed in '%s'", tbl->name ? tbl->name : "?");
        return FALSE;
    }
#else
    return tbl->func_init();
#endif
}

static void seh_color_exit(COLOR_PLUGIN_TABLE *tbl)
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
        dbg_error(".auc func_exit crashed");
    }
#else
    tbl->func_exit();
#endif
}

static BOOL seh_color_pixel2yc(COLOR_PLUGIN_TABLE *tbl, COLOR_PROC_INFO *cpi)
{
    if (!tbl || !tbl->func_pixel2yc)
    {
        return FALSE;
    }
#ifdef _MSC_VER
    __try
    {
        return tbl->func_pixel2yc(cpi);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return FALSE;
    }
#else
    return tbl->func_pixel2yc(cpi);
#endif
}

static BOOL seh_color_yc2pixel(COLOR_PLUGIN_TABLE *tbl, COLOR_PROC_INFO *cpi)
{
    if (!tbl || !tbl->func_yc2pixel)
    {
        return FALSE;
    }
#ifdef _MSC_VER
    __try
    {
        return tbl->func_yc2pixel(cpi);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return FALSE;
    }
#else
    return tbl->func_yc2pixel(cpi);
#endif
}

static BOOL has_auc_path(const char *path)
{
    int i;
    for (i = 0; i < g_color_count; i++)
    {
        if (g_color_plugins[i].path[0] && lstrcmpiA(g_color_plugins[i].path, path) == 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void add_builtin_color(const char *name, int builtin_kind)
{
    ColorPluginEntry *e;
    if (g_color_count >= COLOR_PLUGIN_MAX)
    {
        return;
    }
    e = &g_color_plugins[g_color_count++];
    memset(e, 0, sizeof(*e));
    lstrcpynA(e->name, name, sizeof(e->name));
    lstrcpynA(e->information, name, sizeof(e->information));
    e->flag = COLOR_PLUGIN_FLAG_BUILTIN;
    e->builtin_kind = builtin_kind;
    e->inited = TRUE;
}

static void load_single_auc(const char *full_path)
{
    HMODULE hmod;
    GetColorPluginTableFn fn = NULL;
    COLOR_PLUGIN_TABLE *tbl = NULL;
    ColorPluginEntry *e;
    BOOL used_yuy2 = FALSE;

    if (!full_path || !full_path[0] || g_color_count >= COLOR_PLUGIN_MAX || has_auc_path(full_path))
    {
        return;
    }

    hmod = LoadLibraryA(full_path);
    if (!hmod)
    {
        return;
    }

    if (g_yuy2_mode)
    {
        fn = (GetColorPluginTableFn)GetProcAddress(hmod, "GetColorPluginTableYUY2");
        if (!fn)
        {
            fn = (GetColorPluginTableFn)GetProcAddress(hmod, "_GetColorPluginTableYUY2@0");
        }
        if (fn)
            used_yuy2 = TRUE;
    }
    if (!fn)
    {
        fn = (GetColorPluginTableFn)GetProcAddress(hmod, "GetColorPluginTable");
        if (!fn)
        {
            fn = (GetColorPluginTableFn)GetProcAddress(hmod, "_GetColorPluginTable@0");
        }
    }
    if (!fn && !g_yuy2_mode)
    {
        fn = (GetColorPluginTableFn)GetProcAddress(hmod, "GetColorPluginTableYUY2");
        if (!fn)
        {
            fn = (GetColorPluginTableFn)GetProcAddress(hmod, "_GetColorPluginTableYUY2@0");
        }
        if (fn)
            used_yuy2 = TRUE;
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
        FreeLibrary(hmod);
        return;
    }

    if (!seh_color_init(tbl))
    {
        FreeLibrary(hmod);
        return;
    }

    e = &g_color_plugins[g_color_count++];
    memset(e, 0, sizeof(*e));
    e->hmod = hmod;
    e->table = tbl;
    lstrcpynA(e->name, tbl->name ? tbl->name : "ColorPlugin", sizeof(e->name));
    lstrcpynA(e->information,
              (tbl->information && tbl->information[0]) ? tbl->information : e->name,
              sizeof(e->information));
    lstrcpynA(e->path, full_path, sizeof(e->path));
    e->flag = tbl->flag & ~COLOR_PLUGIN_FLAG_BUILTIN;
    e->builtin_kind = 0;
    e->inited = TRUE;
    e->is_yuy2_table = used_yuy2;

    dbg_plugin("Loaded .auc color plugin: '%s' (%s)", e->name, full_path);
}

static void scan_auc_dir(const char *dir)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE hfind;

    if (!dir || !dir[0] || g_color_count >= COLOR_PLUGIN_MAX)
    {
        return;
    }

    _snprintf(pattern, sizeof(pattern), "%s\\*.auc", dir);
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
            load_single_auc(full);
        }
    } while (g_color_count < COLOR_PLUGIN_MAX && FindNextFileA(hfind, &fd));

    FindClose(hfind);
}

// 0x00424a70: initialize built-in color converters and scan .auc plugins
void color_plugin_init(const char *plugin_dir, HWND main_hwnd)
{
    char ini[MAX_PATH];
    char exe_dir[MAX_PATH];

    color_plugin_shutdown();

    get_ini_path(ini, sizeof(ini));
    g_yuy2_mode = GetPrivateProfileIntA("system", "yuy2mode", 0, ini) != 0;
    g_use_sse = GetPrivateProfileIntA("system", "sse", 1, ini) != 0;
    g_use_sse2 = GetPrivateProfileIntA("system", "sse2", 1, ini) != 0;

    // 3 stock built-in color entries: BT.601, BT.709, and 自動 ("\x8e\xa9\x93\xae")
    add_builtin_color("BT.601", 1);
    add_builtin_color("BT.709", 2);
    add_builtin_color("\x8e\xa9\x93\xae", 3);

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
        scan_auc_dir(exe_dir);
    }
    if (plugin_dir && plugin_dir[0] && (!exe_dir[0] || lstrcmpiA(exe_dir, plugin_dir) != 0))
    {
        scan_auc_dir(plugin_dir);
    }

#if REAVIUTL_X64
    if (plugin_bridge_scan_input_color(plugin_dir, main_hwnd))
    {
        int i, n = plugin_bridge_get_auc_count();
        for (i = 0; i < n && g_color_count < COLOR_PLUGIN_MAX; i++)
        {
            BridgeAucInfo bi;
            if (plugin_bridge_get_auc_info(i, &bi) && !has_auc_path(bi.path))
            {
                ColorPluginEntry *e = &g_color_plugins[g_color_count++];
                memset(e, 0, sizeof(*e));
                lstrcpynA(e->name, bi.name, sizeof(e->name));
                lstrcpynA(e->information, bi.information[0] ? bi.information : bi.name,
                          sizeof(e->information));
                lstrcpynA(e->path, bi.path, sizeof(e->path));
                e->flag = bi.flag & ~COLOR_PLUGIN_FLAG_BUILTIN;
                e->bridged_x86 = TRUE;
                dbg_plugin("Mirrored 32-bit .auc via bridge: '%s' (%s)", e->name, e->path);
            }
        }
    }
#else
    (void)main_hwnd;
#endif
}

// 0x00425040: unload external .auc plugins
void color_plugin_shutdown(void)
{
    int i;
    for (i = 0; i < g_color_count; i++)
    {
        ColorPluginEntry *e = &g_color_plugins[i];
        if (e->hmod)
        {
            if (e->inited && e->table)
            {
                seh_color_exit(e->table);
            }
            FreeLibrary(e->hmod);
        }
        memset(e, 0, sizeof(*e));
    }
    g_color_count = 0;
}

int color_plugin_get_count(void)
{
    return g_color_count;
}

BOOL color_plugin_get_summary(int idx, ColorPluginSummary *out_sum)
{
    const ColorPluginEntry *e;
    if (!out_sum || idx < 0 || idx >= g_color_count)
    {
        return FALSE;
    }
    e = &g_color_plugins[idx];
    memset(out_sum, 0, sizeof(*out_sum));
    lstrcpynA(out_sum->name, e->name, sizeof(out_sum->name));
    lstrcpynA(out_sum->information, e->information, sizeof(out_sum->information));
    lstrcpynA(out_sum->path, e->path, sizeof(out_sum->path));
    out_sum->flag = e->flag;
    out_sum->bridged_x86 = e->bridged_x86;
    return TRUE;
}

int color_plugin_get_ext_count(void)
{
    int i, n = 0;
    for (i = 0; i < g_color_count; i++)
    {
        if (!(g_color_plugins[i].flag & COLOR_PLUGIN_FLAG_BUILTIN))
        {
            n++;
        }
    }
    return n;
}

BOOL color_plugin_get_ext_summary(int ext_idx, ColorPluginSummary *out_sum)
{
    int i, cur = 0;
    if (!out_sum || ext_idx < 0)
    {
        return FALSE;
    }
    for (i = 0; i < g_color_count; i++)
    {
        if (!(g_color_plugins[i].flag & COLOR_PLUGIN_FLAG_BUILTIN))
        {
            if (cur == ext_idx)
            {
                return color_plugin_get_summary(i, out_sum);
            }
            cur++;
        }
    }
    return FALSE;
}

void color_plugin_set_selected_input(int idx)
{
    if (idx >= 0 && idx < g_color_count)
        g_selected_input = idx;
}

int color_plugin_get_selected_input(void)
{
    return (g_selected_input >= 0 && g_selected_input < g_color_count) ? g_selected_input : 2;
}

void color_plugin_set_selected_output(int idx)
{
    if (idx >= 0 && idx < g_color_count)
        g_selected_output = idx;
}

int color_plugin_get_selected_output(void)
{
    return (g_selected_output >= 0 && g_selected_output < g_color_count) ? g_selected_output : 2;
}

static void fill_color_proc_info(COLOR_PROC_INFO *cpi, PIXEL_YC *ycp, int yc_stride,
                                 void *pixelp, DWORD norm_fmt, int w, int h, BOOL invert_height)
{
    EXFUNC *exf = exfunc_get_table();
    memset(cpi, 0, sizeof(*cpi));
    if (invert_height)
    {
        cpi->flag |= COLOR_PROC_INFO_FLAG_INVERT_HEIGHT;
    }
    if (g_use_sse)
    {
        cpi->flag |= COLOR_PROC_INFO_FLAG_USE_SSE;
    }
    if (g_use_sse2)
    {
        cpi->flag |= COLOR_PROC_INFO_FLAG_USE_SSE2;
    }
    cpi->ycp = ycp;
    cpi->pixelp = pixelp;
    cpi->format = norm_fmt;
    cpi->w = w;
    cpi->h = h;
    cpi->line_size = (yc_stride > 0 ? yc_stride : w) * (int)sizeof(PIXEL_YC);
    cpi->yc_size = (int)sizeof(PIXEL_YC);
    cpi->exec_multi_thread_func = exf ? exf->exec_multi_thread_func : NULL;
}

static BOOL invoke_auc_pixel2yc(ColorPluginEntry *e, COLOR_PROC_INFO *cpi,
                                PIXEL_YC *ycp, int yc_stride, int w, int h)
{
    if (!e || e->builtin_kind != 0 || e->bridged_x86 || !e->table)
        return FALSE;
    if (!e->is_yuy2_table)
        return seh_color_pixel2yc(e->table, cpi);
    {
        int yuy2_stride = ((w + 1) / 2) * 4;
        unsigned char *yuy2_buf = (unsigned char *)calloc((size_t)yuy2_stride, (size_t)h);
        COLOR_PROC_INFO ycpi = *cpi;
        BOOL ok;
        if (!yuy2_buf)
            return FALSE;
        ycpi.ycp = (PIXEL_YC *)yuy2_buf;
        ycpi.yc_size = 2;
        ycpi.line_size = yuy2_stride;
        ok = seh_color_pixel2yc(e->table, &ycpi);
        if (ok)
        {
            yuy2_to_yc48(ycp, yc_stride, yuy2_buf, yuy2_stride, w, h, FALSE);
        }
        free(yuy2_buf);
        return ok;
    }
}

static BOOL invoke_auc_yc2pixel(ColorPluginEntry *e, COLOR_PROC_INFO *cpi,
                                const PIXEL_YC *ycp, int yc_stride, int w, int h)
{
    if (!e || e->builtin_kind != 0 || e->bridged_x86 || !e->table)
        return FALSE;
    if (!e->is_yuy2_table)
        return seh_color_yc2pixel(e->table, cpi);
    {
        int yuy2_stride = ((w + 1) / 2) * 4;
        unsigned char *yuy2_buf = (unsigned char *)malloc((size_t)yuy2_stride * (size_t)h);
        COLOR_PROC_INFO ycpi = *cpi;
        BOOL ok = FALSE;
        if (!yuy2_buf)
            return FALSE;
        if (yc48_to_yuy2(yuy2_buf, yuy2_stride, ycp, yc_stride, w, h))
        {
            ycpi.ycp = (PIXEL_YC *)yuy2_buf;
            ycpi.yc_size = 2;
            ycpi.line_size = yuy2_stride;
            ok = seh_color_yc2pixel(e->table, &ycpi);
        }
        free(yuy2_buf);
        return ok;
    }
}

// 0x00425100: convert raw pixel format to yc48
BOOL color_plugin_pixel2yc(PIXEL_YC *ycp, int yc_stride,
                           const void *pixelp, DWORD format,
                           int w, int h, BOOL invert_height)
{
    DWORD norm;
    COLOR_PROC_INFO cpi;
    int i, sel = color_plugin_get_selected_input();

    if (!ycp || !pixelp || w <= 0 || h <= 0)
    {
        return FALSE;
    }
    if (yc_stride <= 0)
    {
        yc_stride = w;
    }

    norm = yc48_normalize_format(format);
    fill_color_proc_info(&cpi, ycp, yc_stride, (void *)pixelp, norm, w, h, invert_height);

    // explicit BT.601 or BT.709 selection
    if ((sel == 0 || sel == 1) && norm == 0x32595559u)
    {
        int stride = ((w + 1) / 2) * 4;
        return yuy2_to_yc48(ycp, yc_stride, (const unsigned char *)pixelp, stride,
                            w, h, (sel == 1) ? TRUE : FALSE);
    }

    // explicit external .auc selection first
    if (sel >= 0 && sel < g_color_count)
    {
        if (invoke_auc_pixel2yc(&g_color_plugins[sel], &cpi, ycp, yc_stride, w, h))
            return TRUE;
    }

    // try native external .auc plugins
    for (i = 0; i < g_color_count; i++)
    {
        if (invoke_auc_pixel2yc(&g_color_plugins[i], &cpi, ycp, yc_stride, w, h))
            return TRUE;
    }

    // built-in "自動" (auto: BT.601 for h < 720, BT.709 for h >= 720) handles YUY2
    if (norm == 0x32595559u) // "YUY2"
    {
        int stride = ((w + 1) / 2) * 4;
        BOOL bt709 = (sel == 0) ? FALSE : (sel == 1 ? TRUE : ((h >= 720) ? TRUE : FALSE));
        return yuy2_to_yc48(ycp, yc_stride, (const unsigned char *)pixelp, stride,
                            w, h, bt709);
    }

    return FALSE;
}

// 0x00420e70: convert yc48 to target pixel format
BOOL color_plugin_yc2pixel(void *pixelp, const PIXEL_YC *ycp, int yc_stride,
                           DWORD format, int w, int h, BOOL invert_height)
{
    DWORD norm;
    COLOR_PROC_INFO cpi;
    int i, sel = color_plugin_get_selected_output();

    if (!pixelp || !ycp || w <= 0 || h <= 0)
    {
        return FALSE;
    }
    if (yc_stride <= 0)
    {
        yc_stride = w;
    }

    norm = yc48_normalize_format(format);
    fill_color_proc_info(&cpi, (PIXEL_YC *)ycp, yc_stride, pixelp, norm, w, h, invert_height);

    if ((sel == 0 || sel == 1) && norm == 0x32595559u)
    {
        int stride = ((w + 1) / 2) * 4;
        return yc48_to_yuy2_ex((unsigned char *)pixelp, stride, ycp, yc_stride,
                               w, h, (sel == 1) ? TRUE : FALSE);
    }

    if (sel >= 0 && sel < g_color_count)
    {
        if (invoke_auc_yc2pixel(&g_color_plugins[sel], &cpi, ycp, yc_stride, w, h))
            return TRUE;
    }

    for (i = 0; i < g_color_count; i++)
    {
        if (invoke_auc_yc2pixel(&g_color_plugins[i], &cpi, ycp, yc_stride, w, h))
            return TRUE;
    }

    if (norm == 0x32595559u) // "YUY2"
    {
        int stride = ((w + 1) / 2) * 4;
        BOOL bt709 = (sel == 0) ? FALSE : (sel == 1 ? TRUE : ((h >= 720) ? TRUE : FALSE));
        return yc48_to_yuy2_ex((unsigned char *)pixelp, stride, ycp, yc_stride,
                               w, h, bt709);
    }

    return FALSE;
}

