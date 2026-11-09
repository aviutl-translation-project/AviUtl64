#include <windows.h>
#include <commctrl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plugin_manager.h"
#include "plugin_loader.h"
#include "plugin_bridge.h"
#include "builtin_filters.h"
#include "profile_cfg.h"
#include "aup_project.h"
#include "editp.h"
#include "exfunc.h"
#include "yc48_convert.h"
#include "i18n.h"
#include "darken_theme.h"
#include "aviutl_dlg.h"
#include "winsnap.h"
#include "debug_console.h"

#define MAX_PLUGINS 192
#define MAX_PLUGIN_MENU_ITEMS 512
#define MAX_CONFIG_CMD_ITEMS 256
#define MAX_TRACK_CTRLS 64
#define MAX_CHECK_CTRLS 48

// internal child control IDs inside standard SDK filter windows (0x0042c930 / 0x0042f870)
#define WM_REAVIUTL_SYNC_FILTER_CONTROLS (WM_APP + 0x41)
#define IDC_FLT_EDIT_BASE 13100
#define IDC_FLT_ACTIVE 45000
#define IDC_FLT_CHECK_BASE 45100
#define IDC_FLT_STEP_DEC_BASE 45200
#define IDC_FLT_STEP_INC_BASE 45300

#define FILTER_CONTAINER_CLASS "AviUtlFilterClass"
#define HOST_DUMMY_OWNER_CLASS "AviUtlHostDummyOwner"

#if !REAVIUTL_X64
#pragma pack(push, 4)
#endif

// undocumented AviUtl 1.10 internal FILTER tail (+0x8c..+0x4c4 on x86, 0x0042c930 / aviutl_exedit_sdk FilterPlugin)
typedef struct
{
    int menu_index;               // +0x8c (puVar4[0x23] = cmd_slot)
    HWND hwnd_parent;             // +0x90 (puVar4[0x24] = owner_hwnd)
    HWND hwnd_active_btn;         // +0x94 (puVar4[0x25] = active checkbox)
    HWND track_trackbar_hwnd[32]; // +0x98
    HWND track_edit_hwnd[32];     // +0x118
    HWND track_btn_dec_hwnd[32];  // +0x198
    HWND track_btn_inc_hwnd[32];  // +0x218
    HWND track_static_hwnd[32];   // +0x298
    HWND check_hwnd[32];          // +0x318
    int track_inline[32];         // +0x398 (puVar4 + 0xe6)
    int check_inline[32];         // +0x418 (puVar4 + 0x106)
    HMENU hmenu_main;             // +0x498 (puVar4[0x126])
    int reserved_49c;             // +0x49c
    HINSTANCE hinst_parent;       // +0x4a0 (puVar4[0x128] = host HINSTANCE)
    void *cache_edit;             // +0x4a4
    void *cache_temp;             // +0x4a8
    void *cache_ptr;              // +0x4ac
    int cache_w, cache_h;         // +0x4b0, +0x4b4
    int cache_d;                  // +0x4b8
    int cache_flag;               // +0x4bc
    DWORD mode_yuy2;              // +0x4c0 (puVar4[0x130])
} FilterInternalTail;

typedef struct
{
    char path[MAX_PATH];
    char name[256];
    HMODULE module;
    FILTER_DLL *src_dll;
    FILTER runtime;                  // persistent exedit caches &e->runtime in g_fp_main
    FilterInternalTail runtime_tail; // contiguous internal tail past +0x8c for C++20 SDK plugins
    HWND hwnd;
    int index;
    int cmd_slot;
    int cmd_id;
    int priority;
    int default_flag;
    int default_priority;
    BOOL is_builtin;
    BOOL is_yuy2_filter;
    BOOL stage1_initialized;
    BOOL stage2_initialized;
    BOOL faulted;
    BOOL teardown;

    // standard SDK trackbar/checkbox/button controls (for non-custom filters)
    HWND hwnd_active_chk;
    HWND hwnd_track_bar[MAX_TRACK_CTRLS];
    HWND hwnd_track_val[MAX_TRACK_CTRLS];
    HWND hwnd_track_dec[MAX_TRACK_CTRLS];
    HWND hwnd_track_inc[MAX_TRACK_CTRLS];
    HWND hwnd_check_btn[MAX_CHECK_CTRLS];
    BOOL updating_track_ui;
} PluginEntry;

#if !REAVIUTL_X64
#pragma pack(pop)
typedef char reaviutl_abi_flt_tail_parent[((offsetof(PluginEntry, runtime_tail.hwnd_parent) - offsetof(PluginEntry, runtime)) == 0x90) ? 1 : -1];
typedef char reaviutl_abi_flt_tail_hinst[((offsetof(PluginEntry, runtime_tail.hinst_parent) - offsetof(PluginEntry, runtime)) == 0x4a0) ? 1 : -1];
typedef char reaviutl_abi_flt_tail_yuy2[((offsetof(PluginEntry, runtime_tail.mode_yuy2) - offsetof(PluginEntry, runtime)) == 0x4c0) ? 1 : -1];
#endif

typedef struct
{
    int filter_index;
    int check_idx;
} ConfigCmdEntry;

static PluginEntry g_plugins[MAX_PLUGINS];
static int g_plugin_count = 0;
static BOOL g_builtins_enabled = FALSE;
static int g_next_seq_cmd_slot = 21;

static ConfigCmdEntry g_config_cmds[MAX_CONFIG_CMD_ITEMS];
static int g_config_cmd_count = 0;

static PluginMenuItem g_menu_items[MAX_PLUGIN_MENU_ITEMS];
static int g_menu_item_count = 0;

static HWND g_main_hwnd = NULL;
static HWND g_dummy_owner_hwnd = NULL;
static BOOL g_classes_registered = FALSE;
static unsigned char *g_yuy2_edit_buf = NULL;
static unsigned char *g_yuy2_temp_buf = NULL;
static size_t g_yuy2_buf_cap = 0;

static BOOL ensure_yuy2_filter_buffers(int max_w, int max_h)
{
    size_t need;
    if (max_w <= 0 || max_h <= 0)
        return FALSE;
    need = (size_t)max_w * (size_t)max_h * 2u + 64u;
    if (need > g_yuy2_buf_cap || !g_yuy2_edit_buf || !g_yuy2_temp_buf)
    {
        unsigned char *ne = (unsigned char *)realloc(g_yuy2_edit_buf, need);
        unsigned char *nt = (unsigned char *)realloc(g_yuy2_temp_buf, need);
        if (!ne || !nt)
        {
            if (ne)
                g_yuy2_edit_buf = ne;
            if (nt)
                g_yuy2_temp_buf = nt;
            return FALSE;
        }
        g_yuy2_edit_buf = ne;
        g_yuy2_temp_buf = nt;
        g_yuy2_buf_cap = need;
    }
    return TRUE;
}

// shift-jis (CP932) to UTF-16 helper so Japanese plugin labels render on all locales
static void sjis_to_wide(const char *src, wchar_t *dst, int dst_cch)
{
    if (!dst || dst_cch <= 0)
        return;
    dst[0] = L'\0';
    if (!src || !src[0])
        return;
    if (MultiByteToWideChar(932, 0, src, -1, dst, dst_cch) > 0)
        return;
    MultiByteToWideChar(CP_ACP, 0, src, -1, dst, dst_cch);
}

static void post_host_redraw(WPARAM kind)
{
    if (g_main_hwnd && IsWindow(g_main_hwnd))
    {
        PostMessageA(g_main_hwnd, WM_REAVIUTL_PLUGIN_REDRAW, kind, 0);
    }
}

static BOOL safe_call_func_init(PluginEntry *e)
{
    BOOL ok = FALSE;
    editp_set_yuy2_sysinfo_override(e->is_yuy2_filter ? 1 : (e->is_builtin ? 0 : 2));
    __try
    {
        ok = e->runtime.func_init(&e->runtime);
    }
    __except (debug_console_seh_filter(GetExceptionInformation(), e->name, "Stage 1 func_init"))
    {
        e->faulted = TRUE;
        ok = FALSE;
    }
    editp_set_yuy2_sysinfo_override(0);
    return ok;
}

static BOOL safe_call_wndproc(PluginEntry *e, HWND hwnd, UINT msg,
                              WPARAM wp, LPARAM lp, void *editp, const char *stage)
{
    BOOL res = FALSE;
    if (!e || e->faulted || !e->runtime.func_WndProc)
        return FALSE;
    editp_set_yuy2_sysinfo_override(e->is_yuy2_filter ? 1 : (e->is_builtin ? 0 : 2));
    __try
    {
        res = e->runtime.func_WndProc(hwnd, msg, wp, lp, editp, &e->runtime);
    }
    __except (debug_console_seh_filter(GetExceptionInformation(), e->name, stage))
    {
        e->faulted = TRUE;
        e->runtime.flag &= ~FILTER_FLAG_ACTIVE;
        res = FALSE;
    }
    editp_set_yuy2_sysinfo_override(0);
    return res;
}

static BOOL safe_call_func_proc(PluginEntry *e, FILTER_PROC_INFO *fpip)
{
    BOOL res = FALSE;
    if (!e || e->faulted || !e->runtime.func_proc)
        return FALSE;
    editp_set_yuy2_sysinfo_override(e->is_yuy2_filter ? 1 : (e->is_builtin ? 0 : 2));
    __try
    {
        res = e->runtime.func_proc(&e->runtime, fpip);
    }
    __except (debug_console_seh_filter(GetExceptionInformation(), e->name, "func_proc"))
    {
        e->faulted = TRUE;
        e->runtime.flag &= ~FILTER_FLAG_ACTIVE;
        res = FALSE;
    }
    editp_set_yuy2_sysinfo_override(0);
    return res;
}

static void safe_call_func_exit(PluginEntry *e)
{
    if (!e || e->faulted || !e->runtime.func_exit)
        return;
    __try
    {
        e->runtime.func_exit(&e->runtime);
    }
    __except (debug_console_seh_filter(GetExceptionInformation(), e->name, "func_exit"))
    {
        e->faulted = TRUE;
    }
}

static void safe_call_save_start(PluginEntry *e, int s, int end_f, void *editp)
{
    if (!e || e->faulted || !e->runtime.func_save_start)
        return;
    __try
    {
        e->runtime.func_save_start(&e->runtime, s, end_f, editp);
    }
    __except (debug_console_seh_filter(GetExceptionInformation(), e->name, "func_save_start"))
    {
        e->faulted = TRUE;
    }
}

static void safe_call_save_end(PluginEntry *e, void *editp)
{
    if (!e || e->faulted || !e->runtime.func_save_end)
        return;
    __try
    {
        e->runtime.func_save_end(&e->runtime, editp);
    }
    __except (debug_console_seh_filter(GetExceptionInformation(), e->name, "func_save_end"))
    {
        e->faulted = TRUE;
    }
}

static FILTER *cb_filter_lookup(int filter_id)
{
    if (filter_id < 0 || filter_id >= g_plugin_count)
        return NULL;
    return &g_plugins[filter_id].runtime;
}

static int find_entry_index_by_fp(const FILTER *fp)
{
    int i;
    if (!fp)
        return -1;
    for (i = 0; i < g_plugin_count; i++)
    {
        if (&g_plugins[i].runtime == fp || (const FILTER *)g_plugins[i].src_dll == fp)
            return i;
        if (fp->name && g_plugins[i].runtime.name &&
            lstrcmpA(fp->name, g_plugins[i].runtime.name) == 0)
            return i;
    }
    return -1;
}

static BOOL cb_add_menu_item(FILTER *fp, LPSTR name, HWND hwnd,
                             int id, int def_key, int flag)
{
    PluginMenuItem *it;
    if (!name || !name[0] || g_menu_item_count >= MAX_PLUGIN_MENU_ITEMS)
        return FALSE;

    it = &g_menu_items[g_menu_item_count++];
    it->filter_index = find_entry_index_by_fp(fp);
    lstrcpynA(it->name, name, sizeof(it->name));
    it->hwnd = hwnd;
    it->cmd_id = id;
    it->def_key = def_key;
    it->flag = flag;
    return TRUE;
}

static void cb_host_state_changed(EDITP *ep)
{
    if (ep && plugin_bridge_is_active())
    {
        plugin_bridge_sync_to_bridge(ep);
    }
    post_host_redraw(1);
}

static BOOL cb_host_edit_open(EDITP *ep, const char *file, int flag)
{
    const char *dot;
    if (!ep || !file || !file[0])
        return FALSE;

    dot = strrchr(file, '.');
    if ((flag & EDIT_OPEN_FLAG_PROJECT) != 0 ||
        ((flag & (EDIT_OPEN_FLAG_VIDEO | EDIT_OPEN_FLAG_AUDIO | EDIT_OPEN_FLAG_ADD)) == 0 &&
         dot && lstrcmpiA(dot, ".aup") == 0))
    {
        return plugin_manager_load_aup(file, ep);
    }
    if ((flag & EDIT_OPEN_FLAG_AUDIO) != 0 && editp_is_editing(ep))
    {
        if (!editp_open_audio(ep, file))
            return FALSE;
        if (plugin_bridge_is_active())
            plugin_bridge_open_audio(file, NULL);
        plugin_manager_notify_update(ep);
        post_host_redraw(2);
        return TRUE;
    }
    if ((flag & EDIT_OPEN_FLAG_ADD) != 0 && editp_is_editing(ep))
    {
        if (!editp_append_video(ep, file))
            return FALSE;
        exfunc_invalidate_all_caches();
        if (plugin_bridge_is_active())
            plugin_bridge_invalidate_caches();
        plugin_manager_notify_update(ep);
        post_host_redraw(2);
        return TRUE;
    }
    if (editp_is_editing(ep))
    {
        plugin_manager_notify_file_close(ep);
    }
    if (!editp_open_video(ep, file))
        return FALSE;
    // broadcast WM_FILTER_FILE_OPEN before returning so exedit's 0x1003ac30 immediately initializes timeline length & canvas state
    plugin_manager_notify_file_open(ep);
    post_host_redraw(2);
    return TRUE;
}

static BOOL cb_host_edit_close(EDITP *ep)
{
    if (!ep)
        return FALSE;
    plugin_manager_notify_file_close(ep);
    editp_close_video(ep);
    post_host_redraw(2);
    return TRUE;
}

static void get_plugin_ini_path(char *out_path, size_t out_sz)
{
    char mod_path[MAX_PATH];
    char *slash;
    if (GetModuleFileNameA(NULL, mod_path, MAX_PATH) > 0)
    {
        slash = strrchr(mod_path, '\\');
        if (slash)
            *(slash + 1) = '\0';
        else
            mod_path[0] = '\0';
        _snprintf(out_path, out_sz, "%saviutl.ini", mod_path);
        out_path[out_sz - 1] = '\0';
    }
    else
    {
        lstrcpynA(out_path, ".\\aviutl.ini", (int)out_sz);
    }
}

static void sanitize_ini_section(const char *raw_name, char *out_sec, size_t out_sz)
{
    size_t i = 0, o = 0;
    if (!out_sec || out_sz == 0)
        return;
    out_sec[0] = '\0';
    if (!raw_name)
        return;
    while (raw_name[i] && o + 1 < out_sz)
    {
        if (raw_name[i] != '[' && raw_name[i] != ']')
        {
            out_sec[o++] = raw_name[i];
        }
        i++;
    }
    out_sec[o] = '\0';
}

static void sync_dynamic_config_check_arrays(PluginEntry *e)
{
    int old_n, new_n, i, any_checked = 0;
    int *new_chk;

    if (!e || !e->src_dll)
        return;
    e->runtime.check_name = e->src_dll->check_name;
    e->runtime.check_default = e->src_dll->check_default;
    new_n = e->src_dll->check_n;
    old_n = e->runtime.check_n;
    if (new_n == old_n)
        return;
    if (new_n <= 0)
    {
        free(e->runtime.check);
        e->runtime.check = NULL;
        e->runtime.check_n = 0;
        e->src_dll->check = NULL;
        return;
    }
    new_chk = (int *)calloc((size_t)new_n, sizeof(int));
    if (!new_chk)
        return;
    for (i = 0; i < new_n; i++)
    {
        if (i < old_n && e->runtime.check)
            new_chk[i] = e->runtime.check[i];
        else if (e->src_dll->check_default && e->src_dll->check_default[i] > 0)
            new_chk[i] = e->src_dll->check_default[i];
        if (new_chk[i] > 0)
            any_checked = 1;
    }
    if ((e->runtime.flag & FILTER_FLAG_CONFIG_RADIO) && !any_checked)
    {
        for (i = 0; i < new_n; i++)
        {
            if (!e->src_dll->check_default || e->src_dll->check_default[i] >= 0)
            {
                new_chk[i] = 1;
                break;
            }
        }
    }
    free(e->runtime.check);
    e->runtime.check = new_chk;
    e->runtime.check_n = new_n;
    e->src_dll->check = new_chk;
}

static int read_source_audio_with_delay(EDITP *ep, int frame_num, short *buf, BOOL apply_delay)
{
    int delay = (apply_delay && g_builtins_enabled) ? builtin_filters_get_audio_delay_frames() : 0;
    int src_frame = frame_num - delay;
    int total_frames, samples, ch;

    if (!ep || !editp_is_editing(ep))
        return 0;
    if (delay == 0)
        return editp_read_audio_frame(ep, frame_num, buf);

    samples = editp_read_audio_frame(ep, frame_num, NULL);
    if (!buf || samples <= 0)
        return samples;

    total_frames = editp_get_frame_n(ep);
    ch = editp_get_audio_ch(ep);
    if (src_frame < 0 || src_frame >= total_frames)
    {
        memset(buf, 0, (size_t)samples * (size_t)(ch > 0 ? ch : 2) * sizeof(short));
        return samples;
    }
    return editp_read_audio_frame(ep, src_frame, buf);
}

static void invalidate_downstream_from_entry(int plugin_index)
{
    int order[MAX_PLUGINS + BRIDGE_MAX_FILTERS];
    const FILTER *downstream[MAX_PLUGINS];
    int cnt, i, pos = -1, dcnt = 0;
    BOOL touch_bridge = (plugin_index >= g_plugin_count);

    cnt = plugin_manager_get_execution_order(FALSE, order, MAX_PLUGINS + BRIDGE_MAX_FILTERS);
    for (i = 0; i < cnt; i++)
    {
        if (order[i] == plugin_index)
        {
            pos = i;
            break;
        }
    }
    if (pos >= 0)
    {
        for (i = pos + 1; i < cnt; i++)
        {
            int idx = order[i];
            if (idx >= 0 && idx < g_plugin_count)
            {
                downstream[dcnt++] = &g_plugins[idx].runtime;
            }
            else if (idx >= g_plugin_count)
            {
                touch_bridge = TRUE;
            }
        }
        if (dcnt > 0)
        {
            exfunc_invalidate_filtering_caches_from(downstream, dcnt);
        }
    }
    if (touch_bridge && plugin_bridge_is_active())
    {
        plugin_bridge_invalidate_caches();
    }
}

static void apply_single_native_video_entry(PluginEntry *e, EDITP *ep, int frame_num)
{
    FILTER_PROC_INFO fpip;

    if (!e || !ep || !e->stage1_initialized || e->faulted || !e->runtime.func_proc)
        return;

    ZeroMemory(&fpip, sizeof(fpip));
    fpip.flag = 0;
    fpip.ycp_edit = editp_get_ycp_edit(ep);
    fpip.ycp_temp = editp_get_ycp_temp(ep);
    editp_get_dimensions(ep, &fpip.w, &fpip.h, &fpip.max_w, &fpip.max_h);
    fpip.frame = (frame_num >= 0) ? frame_num : editp_get_frame(ep);
    fpip.frame_n = editp_get_frame_n(ep);
    editp_get_org_dimensions(ep, &fpip.org_w, &fpip.org_h);
    fpip.audiop = NULL;
    fpip.audio_n = 0;
    fpip.audio_ch = editp_get_audio_ch(ep);
    fpip.pixelp = NULL;
    fpip.editp = ep;
    if (e->is_yuy2_filter)
    {
        int prev_w = fpip.w, prev_h = fpip.h;
        int yuy2_stride = fpip.max_w * 2;
        if (!ensure_yuy2_filter_buffers(fpip.max_w, fpip.max_h))
            return;
        if (!yc48_to_yuy2(g_yuy2_edit_buf, yuy2_stride,
                          editp_get_ycp_edit(ep), fpip.max_w, prev_w, prev_h))
            return;
        memset(g_yuy2_temp_buf, 0, (size_t)yuy2_stride * (size_t)fpip.max_h);
        fpip.ycp_edit = (PIXEL_YC *)g_yuy2_edit_buf;
        fpip.ycp_temp = (PIXEL_YC *)g_yuy2_temp_buf;
        fpip.yc_size = 2;
        fpip.line_size = yuy2_stride;
        e->runtime_tail.mode_yuy2 = 1u;

        safe_call_func_proc(e, &fpip);

        if (!e->faulted && fpip.ycp_edit)
        {
            int out_w = (fpip.w > 0 && fpip.w <= fpip.max_w) ? fpip.w : prev_w;
            int out_h = (fpip.h > 0 && fpip.h <= fpip.max_h) ? fpip.h : prev_h;
            const unsigned char *res_yuy2 =
                (fpip.ycp_edit == (PIXEL_YC *)g_yuy2_temp_buf)
                    ? g_yuy2_temp_buf
                    : (const unsigned char *)fpip.ycp_edit;
            yuy2_to_yc48(editp_get_ycp_edit(ep), fpip.max_w,
                         res_yuy2, yuy2_stride, out_w, out_h, FALSE);
            editp_set_frame_dimensions(ep, out_w, out_h);
        }
        return;
    }

    fpip.yc_size = (int)sizeof(PIXEL_YC);
    fpip.line_size = fpip.max_w * (int)sizeof(PIXEL_YC);

    safe_call_func_proc(e, &fpip);

    // plugins such as exedit may swap fpip.ycp_edit and fpip.ycp_temp
    if (fpip.ycp_edit == editp_get_ycp_temp(ep))
    {
        editp_swap_ycp(ep, fpip.ycp_edit, fpip.ycp_temp);
    }
    if (fpip.w > 0 && fpip.h > 0 &&
        fpip.w <= fpip.max_w && fpip.h <= fpip.max_h)
    {
        editp_set_frame_dimensions(ep, fpip.w, fpip.h);
    }
}

static BOOL render_pipeline_range(EDITP *ep, int frame_num, const FILTER *stop_before_fp)
{
    int order[MAX_PLUGINS + BRIDGE_MAX_FILTERS];
    int k, cnt;
    BOOL host_loaded = FALSE;
    BYTE cur_inter;

    if (!ep || !editp_is_editing(ep))
        return FALSE;

    profile_cfg_apply_for_frame(ep, frame_num);
    cur_inter = editp_get_frame_inter(ep, frame_num);
    cnt = plugin_manager_get_execution_order(FALSE, order, MAX_PLUGINS + BRIDGE_MAX_FILTERS);
    for (k = 0; k < cnt; k++)
    {
        int idx = order[k];
        if (idx >= 0 && idx < g_plugin_count)
        {
            PluginEntry *e = &g_plugins[idx];
            FILTER *fp = &e->runtime;
            if (stop_before_fp && (fp == stop_before_fp ||
                                   (const FILTER *)e->src_dll == stop_before_fp))
            {
                break;
            }
            if (!e->stage1_initialized || e->faulted || !fp->func_proc)
                continue;
            if (!(fp->flag & (FILTER_FLAG_ACTIVE | FILTER_FLAG_ALWAYS_ACTIVE)))
                continue;
            if (fp->flag & FILTER_FLAG_AUDIO_FILTER)
                continue;

            if (!host_loaded)
            {
                editp_load_frame(ep, frame_num);
                host_loaded = TRUE;
            }
            builtin_filters_set_frame_interlace_override((int)cur_inter);
            apply_single_native_video_entry(e, ep, frame_num);
        }
        else if (plugin_bridge_is_active())
        {
            int bidx = idx - g_plugin_count;
            int b_run[BRIDGE_MAX_FILTERS];
            int run_n = 0;
            int fl = plugin_bridge_get_filter_flags(bidx);

            if (!(fl & (FILTER_FLAG_ACTIVE | FILTER_FLAG_ALWAYS_ACTIVE)) ||
                (fl & FILTER_FLAG_AUDIO_FILTER))
            {
                continue;
            }

            b_run[run_n++] = bidx;
            while (k + 1 < cnt && order[k + 1] >= g_plugin_count && run_n < BRIDGE_MAX_FILTERS)
            {
                int nb = order[k + 1] - g_plugin_count;
                int nfl = plugin_bridge_get_filter_flags(nb);
                k++;
                if ((nfl & (FILTER_FLAG_ACTIVE | FILTER_FLAG_ALWAYS_ACTIVE)) &&
                    !(nfl & FILTER_FLAG_AUDIO_FILTER))
                {
                    b_run[run_n++] = nb;
                }
            }
            if (!host_loaded && editp_has_virtual_edits(ep))
            {
                editp_load_frame(ep, frame_num);
                host_loaded = TRUE;
            }
            if (!plugin_bridge_render_frame_subset(ep, frame_num, host_loaded, b_run, run_n))
            {
                if (!host_loaded)
                {
                    editp_load_frame(ep, frame_num);
                    host_loaded = TRUE;
                }
            }
            else
            {
                host_loaded = TRUE;
            }
        }
    }

    builtin_filters_set_frame_interlace_override(0);
    if (!host_loaded)
    {
        editp_load_frame(ep, frame_num);
    }
    return TRUE;
}

static BOOL cb_host_render_up_to(FILTER *stop_before_fp, EDITP *ep,
                                 int frame_num, PIXEL_YC *out_ycp)
{
    int saved_frame;
    int saved_w = 0, saved_h = 0, max_w = 0, max_h = 0;
    int w = 0, h = 0;
    PIXEL_YC *saved_edit, *saved_temp;
    PIXEL_YC *tmp_edit = NULL, *tmp_temp = NULL;

    if (!ep || !out_ycp || !editp_is_editing(ep))
        return FALSE;

    saved_frame = editp_get_frame(ep);
    editp_get_dimensions(ep, &saved_w, &saved_h, &max_w, &max_h);
    saved_edit = editp_get_ycp_edit(ep);
    saved_temp = editp_get_ycp_temp(ep);

    // isolate temporary VRAM buffers so nested get_ycp_filtering* calls inside func_proc never clobber outer fpip->ycp_edit
    if (max_w > 0 && max_h > 0)
    {
        tmp_edit = (PIXEL_YC *)calloc((size_t)max_w * (size_t)max_h, sizeof(PIXEL_YC));
        tmp_temp = (PIXEL_YC *)calloc((size_t)max_w * (size_t)max_h, sizeof(PIXEL_YC));
    }
    if (tmp_edit && tmp_temp)
    {
        editp_swap_ycp(ep, tmp_edit, tmp_temp);
    }

    {
        int prev_cfg_slot = profile_cfg_get_applied_slot();
        render_pipeline_range(ep, frame_num, stop_before_fp);
        profile_cfg_apply_slot(prev_cfg_slot);
    }

    editp_get_dimensions(ep, &w, &h, &max_w, &max_h);
    memcpy(out_ycp, editp_get_ycp_edit(ep),
           (size_t)max_w * (size_t)max_h * sizeof(PIXEL_YC));

    if (tmp_edit && tmp_temp)
    {
        editp_swap_ycp(ep, saved_edit, saved_temp);
    }
    if (saved_w > 0 && saved_h > 0)
    {
        editp_set_frame_dimensions(ep, saved_w, saved_h);
    }
    free(tmp_edit);
    free(tmp_temp);

    if (saved_frame != frame_num)
    {
        editp_set_frame(ep, saved_frame);
    }
    return TRUE;
}

static int cb_host_render_audio_up_to(FILTER *stop_before_fp, EDITP *ep,
                                      int frame_num, short *out_pcm)
{
    int samples, prev_cfg_slot, res;
    if (!ep || !editp_is_editing(ep))
        return 0;
    samples = read_source_audio_with_delay(ep, frame_num, out_pcm, TRUE);
    if (!out_pcm || samples <= 0)
        return samples;
    prev_cfg_slot = profile_cfg_get_applied_slot();
    res = plugin_manager_apply_audio_filters(ep, frame_num, out_pcm, samples, stop_before_fp);
    profile_cfg_apply_slot(prev_cfg_slot);
    return res;
}

// 0x00431390 & 0x00431560: synchronize standard host controls from e->runtime
static void sync_filter_entry_controls(PluginEntry *e)
{
    int t, c;
    if (!e || e->teardown)
        return;

    e->updating_track_ui = TRUE;

    if (e->hwnd_active_chk && IsWindow(e->hwnd_active_chk))
    {
        SendMessageA(e->hwnd_active_chk, BM_SETCHECK,
                     (e->runtime.flag & FILTER_FLAG_ACTIVE) ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    if (e->runtime.track)
    {
        for (t = 0; t < e->runtime.track_n && t < MAX_TRACK_CTRLS; t++)
        {
            char buf[32];
            int val = e->runtime.track[t];
            if (e->hwnd_track_bar[t] && IsWindow(e->hwnd_track_bar[t]))
            {
                SendMessageA(e->hwnd_track_bar[t], TBM_SETPOS, TRUE, val);
            }
            if (e->hwnd_track_val[t] && IsWindow(e->hwnd_track_val[t]))
            {
                wsprintfA(buf, "%d", val);
                SetWindowTextA(e->hwnd_track_val[t], buf);
            }
        }
    }

    if (e->runtime.check)
    {
        for (c = 0; c < e->runtime.check_n && c < MAX_CHECK_CTRLS; c++)
        {
            int cdef = e->runtime.check_default ? e->runtime.check_default[c] : 0;
            if (cdef >= 0 && e->hwnd_check_btn[c] && IsWindow(e->hwnd_check_btn[c]))
            {
                SendMessageA(e->hwnd_check_btn[c], BM_SETCHECK,
                             e->runtime.check[c] ? BST_CHECKED : BST_UNCHECKED, 0);
            }
        }
    }

    e->updating_track_ui = FALSE;
}

static void apply_track_value_change(PluginEntry *e, HWND hwnd, int t, int new_val, BOOL sync_edit)
{
    int s_val = e->runtime.track_s ? e->runtime.track_s[t] : 0;
    int e_val = e->runtime.track_e ? e->runtime.track_e[t] : 256;
    char buf[32];

    if (new_val < s_val)
        new_val = s_val;
    if (new_val > e_val)
        new_val = e_val;
    if (e->runtime.track[t] == new_val)
        return;

    e->runtime.track[t] = new_val;
    e->updating_track_ui = TRUE;
    if (e->hwnd_track_bar[t])
        SendMessageA(e->hwnd_track_bar[t], TBM_SETPOS, TRUE, new_val);
    if (sync_edit && e->hwnd_track_val[t])
    {
        wsprintfA(buf, "%d", new_val);
        SetWindowTextA(e->hwnd_track_val[t], buf);
    }
    e->updating_track_ui = FALSE;

    if (e->runtime.func_update)
        e->runtime.func_update(&e->runtime, FILTER_UPDATE_STATUS_TRACK + t);
    if (e->runtime.func_WndProc)
    {
        safe_call_wndproc(e, hwnd, WM_FILTER_CHANGE_PARAM, 0, 0,
                          exfunc_get_editp(), "WM_FILTER_CHANGE_PARAM");
    }
    invalidate_downstream_from_entry(e->index);
    post_host_redraw(1);
}

// 0x0042f870 lines 188..218: reset filter parameters & ex_data to defaults (CONTEXT2 -> 13001)
static void reset_filter_entry_defaults(PluginEntry *e, HWND hwnd)
{
    int t, c;
    if (!e)
        return;

    if (e->runtime.track)
    {
        for (t = 0; t < e->runtime.track_n && t < MAX_TRACK_CTRLS; t++)
        {
            e->runtime.track[t] = e->runtime.track_default ? e->runtime.track_default[t] : 0;
        }
    }

    if (e->runtime.check)
    {
        int first_radio_checked = -1;
        for (c = 0; c < e->runtime.check_n && c < MAX_CHECK_CTRLS; c++)
        {
            int cdef = e->runtime.check_default ? e->runtime.check_default[c] : 0;
            if (cdef >= 0)
            {
                e->runtime.check[c] = cdef;
                if ((e->runtime.flag & FILTER_FLAG_RADIO_BUTTON) && cdef > 0 && first_radio_checked < 0)
                {
                    first_radio_checked = c;
                }
            }
        }
        if ((e->runtime.flag & FILTER_FLAG_RADIO_BUTTON) && e->runtime.check_n > 0)
        {
            if (first_radio_checked < 0)
                first_radio_checked = 0;
            for (c = 0; c < e->runtime.check_n && c < MAX_CHECK_CTRLS; c++)
            {
                int cdef = e->runtime.check_default ? e->runtime.check_default[c] : 0;
                if (cdef >= 0)
                    e->runtime.check[c] = (c == first_radio_checked) ? 1 : 0;
            }
        }
    }

    if ((e->runtime.flag & FILTER_FLAG_EX_DATA) &&
        e->runtime.ex_data_ptr && e->runtime.ex_data_def && e->runtime.ex_data_size > 0)
    {
        memcpy(e->runtime.ex_data_ptr, e->runtime.ex_data_def, (size_t)e->runtime.ex_data_size);
    }

    sync_filter_entry_controls(e);
    if (e->runtime.func_update)
        e->runtime.func_update(&e->runtime, FILTER_UPDATE_STATUS_ALL);
    if (e->runtime.func_WndProc)
    {
        safe_call_wndproc(e, hwnd, WM_FILTER_CHANGE_PARAM, 0, 0,
                          exfunc_get_editp(), "WM_FILTER_CHANGE_PARAM");
    }
    invalidate_downstream_from_entry(e->index);
    InvalidateRect(hwnd, NULL, FALSE);
    post_host_redraw(1);
}

// 0x0042f870, filter container wndproc
static LRESULT CALLBACK AviUtlFilterWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PluginEntry *e;

    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;
        e = cs ? (PluginEntry *)cs->lpCreateParams : NULL;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)e);
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    e = (PluginEntry *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    if (msg == WM_SETTEXT && lp != 0)
    {
        // convert cp932 SetWindowTextA titles to utf-16 for non-japanese locales
        wchar_t wtitle[512];
        sjis_to_wide((const char *)lp, wtitle, 512);
        return DefWindowProcW(hwnd, WM_SETTEXT, wp, (LPARAM)wtitle);
    }

    if (msg == WM_REAVIUTL_SYNC_FILTER_CONTROLS && e)
    {
        sync_filter_entry_controls(e);
        return 0;
    }

    if (msg == WM_GETMINMAXINFO)
    {
        LRESULT lr = DefWindowProcA(hwnd, msg, wp, lp);
        if (lp && (GetWindowLongA(hwnd, GWL_STYLE) & WS_THICKFRAME))
        {
            MINMAXINFO *mmi = (MINMAXINFO *)lp;
            if (mmi->ptMinTrackSize.x < 128)
                mmi->ptMinTrackSize.x = 128;
            if (mmi->ptMinTrackSize.y < 96)
                mmi->ptMinTrackSize.y = 96;
        }
        return lr;
    }

    if (msg == WM_SIZE && e && e->hwnd_active_chk && IsWindow(e->hwnd_active_chk))
    {
        RECT crc;
        GetClientRect(hwnd, &crc);
        SetWindowPos(e->hwnd_active_chk, NULL, crc.right - 15, 2, 12, 11,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // 0x00405450 lines 72..98: paint track value EDIT boxes on COLOR_BTNFACE
    if (msg == WM_CTLCOLOREDIT && e && lp != 0)
    {
        int t;
        for (t = 0; t < e->runtime.track_n && t < MAX_TRACK_CTRLS; t++)
        {
            if (e->hwnd_track_val[t] == (HWND)lp)
            {
                HDC hdc = (HDC)wp;
                SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
                return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
            }
        }
    }

    if (msg == WM_CLOSE && e && !e->teardown)
    {
        // aviutl hides filter windows on [X] instead of destroying them
        plugin_manager_show_window(e->index, FALSE);
        return 0;
    }

    // standard SDK trackbar/checkbox/edit/context2 handling when filter uses host controls
    if (e && e->stage1_initialized && !e->teardown)
    {
        if (msg == WM_HSCROLL && lp != 0 && e->runtime.track)
        {
            int t;
            HWND hBar = (HWND)lp;
            for (t = 0; t < e->runtime.track_n && t < MAX_TRACK_CTRLS; t++)
            {
                if (e->hwnd_track_bar[t] == hBar)
                {
                    int val = (int)SendMessageA(hBar, TBM_GETPOS, 0, 0);
                    apply_track_value_change(e, hwnd, t, val, TRUE);
                    return 0;
                }
            }
        }
        else if (msg == WM_COMMAND)
        {
            int id = LOWORD(wp);
            int code = HIWORD(wp);

            if (id == IDC_FLT_ACTIVE && e->hwnd_active_chk)
            {
                BOOL want = (SendMessageA(e->hwnd_active_chk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                plugin_manager_set_active(e->index, want);
                return 0;
            }
            if (id == 13000)
            {
                plugin_manager_toggle_active(e->index);
                return 0;
            }
            if (id == 13001)
            {
                reset_filter_entry_defaults(e, hwnd);
                return 0;
            }
            if (id >= IDC_FLT_STEP_DEC_BASE &&
                id < IDC_FLT_STEP_DEC_BASE + e->runtime.track_n &&
                id - IDC_FLT_STEP_DEC_BASE < MAX_TRACK_CTRLS && e->runtime.track)
            {
                int t = id - IDC_FLT_STEP_DEC_BASE;
                apply_track_value_change(e, hwnd, t, e->runtime.track[t] - 1, TRUE);
                return 0;
            }
            if (id >= IDC_FLT_STEP_INC_BASE &&
                id < IDC_FLT_STEP_INC_BASE + e->runtime.track_n &&
                id - IDC_FLT_STEP_INC_BASE < MAX_TRACK_CTRLS && e->runtime.track)
            {
                int t = id - IDC_FLT_STEP_INC_BASE;
                apply_track_value_change(e, hwnd, t, e->runtime.track[t] + 1, TRUE);
                return 0;
            }
            if (id >= IDC_FLT_EDIT_BASE &&
                id < IDC_FLT_EDIT_BASE + e->runtime.track_n &&
                id - IDC_FLT_EDIT_BASE < MAX_TRACK_CTRLS && e->runtime.track)
            {
                int t = id - IDC_FLT_EDIT_BASE;
                if (code == EN_SETFOCUS && e->hwnd_track_val[t])
                {
                    PostMessageA(e->hwnd_track_val[t], EM_SETSEL, 0, -1);
                    return 0;
                }
                if (code == EN_UPDATE && !e->updating_track_ui &&
                    e->hwnd_track_val[t] && GetFocus() == e->hwnd_track_val[t])
                {
                    char txt[32];
                    int parsed;
                    txt[0] = '\0';
                    GetWindowTextA(e->hwnd_track_val[t], txt, sizeof(txt));
                    if (txt[0] == '\0')
                        parsed = e->runtime.track_default ? e->runtime.track_default[t] : 0;
                    else
                        parsed = atoi(txt);
                    apply_track_value_change(e, hwnd, t, parsed, FALSE);
                    return 0;
                }
                if (code == EN_KILLFOCUS && !e->updating_track_ui && e->hwnd_track_val[t])
                {
                    char txt[32];
                    e->updating_track_ui = TRUE;
                    wsprintfA(txt, "%d", e->runtime.track[t]);
                    SetWindowTextA(e->hwnd_track_val[t], txt);
                    e->updating_track_ui = FALSE;
                    return 0;
                }
            }
            if (id >= IDC_FLT_CHECK_BASE &&
                id < IDC_FLT_CHECK_BASE + e->runtime.check_n &&
                id - IDC_FLT_CHECK_BASE < MAX_CHECK_CTRLS)
            {
                int c = id - IDC_FLT_CHECK_BASE;
                int cdef = e->runtime.check_default ? e->runtime.check_default[c] : 0;
                if (cdef >= 0 && e->runtime.check && e->hwnd_check_btn[c])
                {
                    if (e->runtime.flag & FILTER_FLAG_RADIO_BUTTON)
                    {
                        int k;
                        for (k = 0; k < e->runtime.check_n && k < MAX_CHECK_CTRLS; k++)
                        {
                            int kdef = e->runtime.check_default ? e->runtime.check_default[k] : 0;
                            if (kdef >= 0)
                            {
                                e->runtime.check[k] = (k == c) ? 1 : 0;
                                if (e->hwnd_check_btn[k])
                                {
                                    SendMessageA(e->hwnd_check_btn[k], BM_SETCHECK,
                                                 (k == c) ? BST_CHECKED : BST_UNCHECKED, 0);
                                }
                            }
                        }
                    }
                    else
                    {
                        e->runtime.check[c] =
                            (SendMessageA(e->hwnd_check_btn[c], BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
                    }
                    if (e->runtime.func_update)
                    {
                        e->runtime.func_update(&e->runtime, FILTER_UPDATE_STATUS_CHECK + c);
                    }
                    if (e->runtime.func_WndProc)
                    {
                        safe_call_wndproc(e, hwnd, WM_FILTER_CHANGE_PARAM, 0, 0,
                                          exfunc_get_editp(), "WM_FILTER_CHANGE_PARAM");
                    }
                    invalidate_downstream_from_entry(e->index);
                    post_host_redraw(1);
                    return 0;
                }
            }
        }
    }

    if (e && e->stage1_initialized && !e->teardown && e->runtime.func_WndProc)
    {
        BOOL need_redraw = safe_call_wndproc(
            e, hwnd, msg, wp, lp, exfunc_get_editp(), "AviUtlFilterWndProc");
        if (need_redraw)
        {
            invalidate_downstream_from_entry(e->index);
            post_host_redraw(1);
        }
        if (msg == WM_CONTEXTMENU && need_redraw)
        {
            return 0;
        }
        // custom WM_FILTER_* messages (0x0464) do not go to DefWindowProc
        if (msg >= WM_FILTER_UPDATE && msg <= 0x0490)
        {
            return (LRESULT)need_redraw;
        }
    }

    // 0x0042f870 lines 68..110: client-area right-click CONTEXT2 menu (ON/OFF, Reset defaults)
    if (msg == WM_CONTEXTMENU && e && !e->teardown)
    {
        UINT flg = (UINT)e->runtime.flag;
        if (!(flg & FILTER_FLAG_DISP_FILTER) &&
            (!(flg & FILTER_FLAG_ALWAYS_ACTIVE) || e->runtime.track_n > 0 ||
             e->runtime.check_n > 0 || (flg & FILTER_FLAG_EX_DATA)))
        {
            int sx = (short)LOWORD(lp);
            int sy = (short)HIWORD(lp);
            POINT pt;
            RECT crc;
            GetClientRect(hwnd, &crc);
            if (sx == -1 && sy == -1)
            {
                pt.x = 8;
                pt.y = 8;
                ClientToScreen(hwnd, &pt);
                sx = pt.x;
                sy = pt.y;
                pt.x = 8;
                pt.y = 8;
            }
            else
            {
                pt.x = sx;
                pt.y = sy;
                ScreenToClient(hwnd, &pt);
            }
            if (PtInRect(&crc, pt))
            {
                HMENU hMenu = i18n_load_menu("CONTEXT2");
                if (hMenu)
                {
                    HMENU hSub = GetSubMenu(hMenu, 0);
                    if (hSub)
                    {
                        EDITP *ep = exfunc_get_editp();
                        BOOL editing = (ep && editp_is_editing(ep));
                        if (!editing || (flg & FILTER_FLAG_ALWAYS_ACTIVE))
                        {
                            EnableMenuItem(hSub, 13000, MF_BYCOMMAND | MF_GRAYED | MF_DISABLED);
                        }
                        if (!editing)
                        {
                            EnableMenuItem(hSub, 13001, MF_BYCOMMAND | MF_GRAYED | MF_DISABLED);
                        }
                        TrackPopupMenu(hSub, TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                                       sx, sy, 0, hwnd, NULL);
                    }
                    DestroyMenu(hMenu);
                    return 0;
                }
            }
        }
    }

    // magnetic window snapping (0x00405450 lines 1289..1318)
    if (msg == WM_ENTERSIZEMOVE || msg == WM_EXITSIZEMOVE || msg == WM_MOVING)
    {
        if (winsnap_handle_message(hwnd, g_main_hwnd, msg, wp, lp))
        {
            return (msg == WM_MOVING) ? TRUE : 0;
        }
    }

    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void ensure_window_classes(void)
{
    WNDCLASSA wc;
    HINSTANCE hInst;
    if (g_classes_registered)
        return;

    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = 0xFFFF;
        InitCommonControlsEx(&icc);
    }
    hInst = GetModuleHandleA(NULL);

    ZeroMemory(&wc, sizeof(wc));
    wc.style = CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = AviUtlFilterWndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = FILTER_CONTAINER_CLASS;
    RegisterClassA(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = hInst;
    wc.lpszClassName = HOST_DUMMY_OWNER_CLASS;
    RegisterClassA(&wc);

    g_classes_registered = TRUE;
}

static HWND ensure_owner_hwnd(HWND main_hwnd)
{
    if (main_hwnd && IsWindow(main_hwnd))
    {
        return main_hwnd;
    }
    if (!g_dummy_owner_hwnd || !IsWindow(g_dummy_owner_hwnd))
    {
        g_dummy_owner_hwnd = CreateWindowExA(
            0, HOST_DUMMY_OWNER_CLASS, "AviUtl", WS_OVERLAPPEDWINDOW,
            0, 0, 320, 240, NULL, NULL, GetModuleHandleA(NULL), NULL);
    }
    return g_dummy_owner_hwnd;
}

static HWND create_filter_window(PluginEntry *e, HWND owner_hwnd)
{
    const FILTER_DLL *f = e->src_dll;
    DWORD style;
    DWORD ex_style = WS_EX_TOOLWINDOW;
    int w, h, raw_x, raw_y, t, c, y_pos;
    RECT rc;
    HWND hwnd;
    HMENU sysm;
    HINSTANCE hInst = GetModuleHandleA(NULL);
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HICON hIconLeft = darken_theme_recolor_icon(
        (HICON)LoadImageA(hInst, "ICON_LEFT", IMAGE_ICON, 16, 16, 0), "ICON_LEFT");
    HICON hIconRight = darken_theme_recolor_icon(
        (HICON)LoadImageA(hInst, "ICON_RIGHT", IMAGE_ICON, 16, 16, 0), "ICON_RIGHT");

    if (!f)
        return NULL;
    // match aviutl 0042c930: skip window when NO_CONFIG or popup/check/radio menu item
    if (f->flag & (FILTER_FLAG_NO_CONFIG | FILTER_FLAG_CONFIG_POPUP |
                   FILTER_FLAG_CONFIG_CHECK | FILTER_FLAG_CONFIG_RADIO))
    {
        return NULL;
    }

    style = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    if (f->flag & FILTER_FLAG_WINDOW_THICKFRAME)
        style |= WS_THICKFRAME;
    if (f->flag & FILTER_FLAG_WINDOW_HSCROLL)
        style |= WS_HSCROLL;
    if (f->flag & FILTER_FLAG_WINDOW_VSCROLL)
        style |= WS_VSCROLL;

    w = 314;
    h = ((f->flag & FILTER_FLAG_ALWAYS_ACTIVE) ? 0 : 4) + 14 +
        f->check_n * 20 + f->track_n * 24;

    raw_x = (f->flag & FILTER_FLAG_WINDOW_SIZE) ? f->x : FILTER_WINDOW_SIZE_CLIENT;
    raw_y = (f->flag & FILTER_FLAG_WINDOW_SIZE) ? f->y : FILTER_WINDOW_SIZE_CLIENT;

    if (f->flag & FILTER_FLAG_WINDOW_SIZE)
    {
        int vx = raw_x & 0x0fffffff;
        int vy = raw_y & 0x0fffffff;
        w = (raw_x & 0x20000000) ? (w + vx) : vx;
        h = (raw_y & 0x20000000) ? (h + vy) : vy;
    }
    if (w < 160)
        w = 560; // default initial width before plugin's WM_FILTER_INIT SetWindowPos
    if (h < 24 && f->track_n == 0 && f->check_n == 0)
        h = 200;

    SetRect(&rc, 0, 0, w, h);
    AdjustWindowRectEx(&rc, style, FALSE, ex_style);
    if (raw_x & FILTER_WINDOW_SIZE_CLIENT)
        w = rc.right - rc.left;
    if (raw_y & FILTER_WINDOW_SIZE_CLIENT)
        h = rc.bottom - rc.top;

    // create owned top-level tool window (hWndParent = owner_hwnd without WS_CHILD sets GW_OWNER == owner_hwnd, required by exedit's func_init)
    hwnd = CreateWindowExA(
        ex_style,
        FILTER_CONTAINER_CLASS,
        f->name ? f->name : "Filter",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        owner_hwnd, NULL, hInst, e);
    if (!hwnd)
        return NULL;

    // trigger WM_SETTEXT handler so CP932 caption renders as UTF-16 on all locales
    if (f->name && f->name[0])
    {
        SetWindowTextA(hwnd, f->name);
    }

    sysm = GetSystemMenu(hwnd, FALSE);
    if (sysm)
    {
        DeleteMenu(sysm, SC_MAXIMIZE, MF_BYCOMMAND);
        DeleteMenu(sysm, SC_MINIMIZE, MF_BYCOMMAND);
    }

    // 0x0042c930 lines 515..524: top-right active checkbox (12x11 at right-15, 2)
    y_pos = ((f->flag & FILTER_FLAG_ALWAYS_ACTIVE) ? 0 : 4) + 8;
    if ((f->track_n > 0 || f->check_n > 0) && !(f->flag & FILTER_FLAG_ALWAYS_ACTIVE))
    {
        RECT crc;
        GetClientRect(hwnd, &crc);
        e->hwnd_active_chk = CreateWindowExA(
            0, "BUTTON", "", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            crc.right - 15, 2, 12, 11, hwnd,
            (HMENU)(UINT_PTR)IDC_FLT_ACTIVE, hInst, NULL);
        if (e->hwnd_active_chk && (f->flag & FILTER_FLAG_ACTIVE))
        {
            SendMessageA(e->hwnd_active_chk, BM_SETCHECK, BST_CHECKED, 0);
        }
    }

    // 0x0042c930 lines 526..589: trackbars + step <- / -> buttons + editable numeric EDIT
    e->updating_track_ui = TRUE;
    for (t = 0; t < f->track_n && t < MAX_TRACK_CTRLS; t++)
    {
        const char *tname = (f->track_name && f->track_name[t]) ? f->track_name[t] : "";
        wchar_t wtname[128];
        int s_val = f->track_s ? f->track_s[t] : 0;
        int e_val = f->track_e ? f->track_e[t] : 256;
        int d_val = f->track_default ? f->track_default[t] : 0;
        char vbuf[32];
        HWND hLbl;

        sjis_to_wide(tname, wtname, 128);
        hLbl = CreateWindowExW(0, L"STATIC", wtname, WS_CHILD | WS_VISIBLE,
                               6, y_pos + 2, 50, 16, hwnd, NULL, hInst, NULL);
        if (hLbl)
            SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, 0);
        if (t < 32)
            e->runtime_tail.track_static_hwnd[t] = hLbl;

        e->hwnd_track_bar[t] = CreateWindowExA(
            0, TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
            56, y_pos - 2, 190, 24, hwnd, NULL, hInst, NULL);
        if (e->hwnd_track_bar[t])
        {
            int page_sz = (e_val - s_val) / 2;
            if (page_sz < 1)
                page_sz = 1;
            SendMessageA(e->hwnd_track_bar[t], TBM_SETRANGEMIN, TRUE, s_val);
            SendMessageA(e->hwnd_track_bar[t], TBM_SETRANGEMAX, TRUE, e_val);
            SendMessageA(e->hwnd_track_bar[t], TBM_SETPAGESIZE, 0, page_sz);
            SendMessageA(e->hwnd_track_bar[t], TBM_SETTICFREQ, (WPARAM)page_sz, 0);
            SendMessageA(e->hwnd_track_bar[t], TBM_SETPOS, TRUE, d_val);
        }

        e->hwnd_track_dec[t] = CreateWindowExA(
            0, "BUTTON", "<",
            WS_CHILD | WS_VISIBLE | (hIconLeft ? BS_ICON : BS_PUSHBUTTON),
            246, y_pos + 2, 16, 16, hwnd,
            (HMENU)(UINT_PTR)(IDC_FLT_STEP_DEC_BASE + t), hInst, NULL);
        if (e->hwnd_track_dec[t])
        {
            if (hIconLeft)
                SendMessageA(e->hwnd_track_dec[t], BM_SETIMAGE, IMAGE_ICON, (LPARAM)hIconLeft);
            else
                SendMessageA(e->hwnd_track_dec[t], WM_SETFONT, (WPARAM)hFont, 0);
        }

        e->hwnd_track_inc[t] = CreateWindowExA(
            0, "BUTTON", ">",
            WS_CHILD | WS_VISIBLE | (hIconRight ? BS_ICON : BS_PUSHBUTTON),
            262, y_pos + 2, 16, 16, hwnd,
            (HMENU)(UINT_PTR)(IDC_FLT_STEP_INC_BASE + t), hInst, NULL);
        if (e->hwnd_track_inc[t])
        {
            if (hIconRight)
                SendMessageA(e->hwnd_track_inc[t], BM_SETIMAGE, IMAGE_ICON, (LPARAM)hIconRight);
            else
                SendMessageA(e->hwnd_track_inc[t], WM_SETFONT, (WPARAM)hFont, 0);
        }

        wsprintfA(vbuf, "%d", d_val);
        e->hwnd_track_val[t] = CreateWindowExA(
            0, "EDIT", vbuf, WS_CHILD | WS_VISIBLE,
            282, y_pos + 2, 30, 16, hwnd,
            (HMENU)(UINT_PTR)(IDC_FLT_EDIT_BASE + t), hInst, NULL);
        if (e->hwnd_track_val[t])
            SendMessageA(e->hwnd_track_val[t], WM_SETFONT, (WPARAM)hFont, 0);

        if (t < 32)
        {
            e->runtime_tail.track_trackbar_hwnd[t] = e->hwnd_track_bar[t];
            e->runtime_tail.track_edit_hwnd[t] = e->hwnd_track_val[t];
            e->runtime_tail.track_btn_dec_hwnd[t] = e->hwnd_track_dec[t];
            e->runtime_tail.track_btn_inc_hwnd[t] = e->hwnd_track_inc[t];
        }

        y_pos += 24;
    }
    e->updating_track_ui = FALSE;

    // 0x0042c930 lines 591..614: checkboxes, radio buttons, and MID_FILTER_BUTTON (12004 + c) pushbuttons
    y_pos += 2;
    for (c = 0; c < f->check_n && c < MAX_CHECK_CTRLS; c++)
    {
        const char *cname = (f->check_name && f->check_name[c]) ? f->check_name[c] : "";
        wchar_t wcname[256];
        int cdef = f->check_default ? f->check_default[c] : 0;

        sjis_to_wide(cname, wcname, 256);
        if (cdef < 0)
        {
            e->hwnd_check_btn[c] = CreateWindowExW(
                0, L"BUTTON", wcname,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_CENTER | BS_VCENTER,
                10, y_pos, 294, 18, hwnd,
                (HMENU)(UINT_PTR)(MID_FILTER_BUTTON + c), hInst, NULL);
        }
        else
        {
            DWORD bstyle = WS_CHILD | WS_VISIBLE |
                           ((f->flag & FILTER_FLAG_RADIO_BUTTON) ? BS_RADIOBUTTON : BS_AUTOCHECKBOX);
            e->hwnd_check_btn[c] = CreateWindowExW(
                0, L"BUTTON", wcname, bstyle,
                8, y_pos, 290, 16, hwnd,
                (HMENU)(UINT_PTR)(IDC_FLT_CHECK_BASE + c), hInst, NULL);
            if (e->hwnd_check_btn[c] && cdef > 0)
            {
                SendMessageW(e->hwnd_check_btn[c], BM_SETCHECK, BST_CHECKED, 0);
            }
        }
        if (e->hwnd_check_btn[c])
        {
            SendMessageW(e->hwnd_check_btn[c], WM_SETFONT, (WPARAM)hFont, 0);
        }
        if (c < 32)
        {
            e->runtime_tail.check_hwnd[c] = e->hwnd_check_btn[c];
        }
        y_pos += 20;
    }

    return hwnd;
}

static void bind_host_exfunc_callbacks(HWND owner_hwnd, EDITP *editp)
{
    exfunc_set_filter_lookup(cb_filter_lookup);
    exfunc_set_add_menu_item_handler(cb_add_menu_item);
    exfunc_set_host_callbacks(cb_host_state_changed, cb_host_edit_open,
                              cb_host_edit_close, cb_host_render_up_to);
    exfunc_set_audio_render_callback(cb_host_render_audio_up_to);
    if (editp)
    {
        exfunc_set_editp(editp);
        editp_set_main_hwnd(editp, owner_hwnd);
    }
}

static void notify_stage3_change_window(void)
{
    int i;
    // stage 3 (0x0042c930 lines 800..806): send WM_FILTER_CHANGE_WINDOW (0x046e) to every initialized filter window
    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        if (!e->stage2_initialized || e->faulted)
            continue;
        if (e->runtime.func_WndProc && e->runtime.hwnd)
        {
            safe_call_wndproc(e, e->runtime.hwnd, WM_FILTER_CHANGE_WINDOW, 0, 0,
                              exfunc_get_editp(), "Stage 3 WM_FILTER_CHANGE_WINDOW");
        }
    }
}

static void initialize_registered_plugins(HWND owner_hwnd, EDITP *editp, BOOL defer_stage3)
{
    int i;

    editp_set_registered_filter_count(g_plugin_count);
    bind_host_exfunc_callbacks(owner_hwnd, editp);

    // create owned tool windows, restore saved aviutl.ini geometry/visibility, & populate runtime FILTER structs for ALL plugins before calling func_init (0x0042c930)
    {
        char ini[MAX_PATH];
        int next_vid_prio = 1;
        int next_aud_prio = 1;
        int vx0 = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy0 = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        HINSTANCE host_hinst = GetModuleHandleA(NULL);
        HMENU host_menu = (owner_hwnd && IsWindow(owner_hwnd)) ? GetMenu(owner_hwnd) : NULL;
        if (vw <= 0)
            vw = GetSystemMetrics(SM_CXSCREEN);
        if (vh <= 0)
            vh = GetSystemMetrics(SM_CYSCREEN);
        get_plugin_ini_path(ini, sizeof(ini));

        for (i = 0; i < g_plugin_count; i++)
        {
            PluginEntry *e = &g_plugins[i];
            char sec[256];
            int fl, def_prio;

            e->hwnd = create_filter_window(e, owner_hwnd);
            plugin_setup_runtime_filter(&e->runtime, e->src_dll, e->module, e->path, e->hwnd);

            e->runtime_tail.menu_index = e->cmd_slot;
            e->runtime_tail.hwnd_parent = owner_hwnd;
            e->runtime_tail.hwnd_active_btn = e->hwnd_active_chk;
            e->runtime_tail.hmenu_main = host_menu;
            e->runtime_tail.hinst_parent = host_hinst;
            e->runtime_tail.mode_yuy2 = e->is_yuy2_filter ? 1u : 0u;

            fl = e->runtime.flag;
            e->default_flag = fl;
            sanitize_ini_section(e->name, sec, sizeof(sec));

            if (fl & FILTER_FLAG_INTERLACE_FILTER)
            {
                def_prio = -1;
                e->priority = -1;
            }
            else
            {
                if (fl & FILTER_FLAG_PRIORITY_HIGHEST)
                {
                    def_prio = 0;
                }
                else if (fl & FILTER_FLAG_PRIORITY_LOWEST)
                {
                    def_prio = 127;
                }
                else if (fl & FILTER_FLAG_AUDIO_FILTER)
                {
                    def_prio = (next_aud_prio < 126) ? next_aud_prio++ : 126;
                }
                else
                {
                    def_prio = (next_vid_prio < 126) ? next_vid_prio++ : 126;
                }
                if (sec[0])
                {
                    e->priority = (int)GetPrivateProfileIntA(sec, "priority", def_prio, ini);
                }
                else
                {
                    e->priority = def_prio;
                }
            }
            e->default_priority = def_prio;

            // 0x0042c930 lines 308..668: restore saved x, y, w, h, and disp before func_init / WM_FILTER_INIT
            if (sec[0] && e->hwnd && IsWindow(e->hwnd))
            {
                int wx = (int)GetPrivateProfileIntA(sec, "x", -32000, ini);
                int wy = (int)GetPrivateProfileIntA(sec, "y", -32000, ini);
                int ww = 0, wh = 0, disp;
                UINT swp = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE;

                if (GetWindowLongA(e->hwnd, GWL_STYLE) & WS_THICKFRAME)
                {
                    ww = (int)GetPrivateProfileIntA(sec, "w", 0, ini);
                    wh = (int)GetPrivateProfileIntA(sec, "h", 0, ini);
                    if (ww >= 128 && wh >= 96)
                        swp &= ~SWP_NOSIZE;
                }
                if (wx != -32000 && wy != -32000)
                {
                    if (wx < vx0 - 64)
                        wx = vx0;
                    if (wy < vy0 - 16)
                        wy = vy0;
                    if (wx > vx0 + vw - 48)
                        wx = vx0 + vw - 128;
                    if (wy > vy0 + vh - 48)
                        wy = vy0 + vh - 96;
                    SetWindowPos(e->hwnd, NULL, wx, wy, ww, wh, swp);
                }
                else if (swp == (SWP_NOZORDER | SWP_NOACTIVATE))
                {
                    SetWindowPos(e->hwnd, NULL, 0, 0, ww, wh, swp | SWP_NOMOVE);
                }

                disp = (int)GetPrivateProfileIntA(sec, "disp", 0, ini);
                if (disp != 0)
                {
                    e->runtime.flag |= FILTER_FLAG_WINDOW_DISP;
                    if (e->src_dll)
                        e->src_dll->flag = e->runtime.flag;
                    ShowWindow(e->hwnd, SW_SHOWNA);
                }
                else
                {
                    e->runtime.flag &= ~FILTER_FLAG_WINDOW_DISP;
                    if (e->src_dll)
                        e->src_dll->flag = e->runtime.flag;
                }
            }

            dbg_plugin("Filter[%d] '%s' | slot=%d HMODULE=0x%p FILTER=0x%p HWND=0x%p flag=0x%08X prio=%d",
                       i, e->name, e->cmd_slot, (void *)e->module, (void *)&e->runtime,
                       (void *)e->hwnd, (unsigned)e->runtime.flag, e->priority);
        }
    }

    // stage 1: run func_init(fp) on each filter in order
    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        DWORD t0 = GetTickCount();
        e->stage1_initialized = TRUE;
        if (e->runtime.func_init)
        {
            if (!safe_call_func_init(e))
            {
                dbg_warn("Filter[%d] '%s' Stage 1 func_init() %s (%lu ms) -> disabled",
                         i, e->name, e->faulted ? "FAULTED" : "returned FALSE",
                         (unsigned long)(GetTickCount() - t0));
                e->stage1_initialized = FALSE;
                e->runtime.flag &= ~(FILTER_FLAG_ACTIVE | FILTER_FLAG_WINDOW_DISP);
                if (e->src_dll)
                    e->src_dll->flag = e->runtime.flag;
                if (e->hwnd && IsWindow(e->hwnd))
                {
                    ShowWindow(e->hwnd, SW_HIDE);
                }
                continue;
            }
            dbg_plugin("Filter[%d] '%s' Stage 1 func_init() OK (%lu ms)",
                       i, e->name, (unsigned long)(GetTickCount() - t0));
        }
    }

    // stage 2: send WM_FILTER_INIT (0x0467) to each initialized filter
    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        int menus_before = g_menu_item_count;
        DWORD t0 = GetTickCount();
        if (!e->stage1_initialized || e->faulted)
            continue;
        if (e->runtime.func_WndProc)
        {
            safe_call_wndproc(e, e->runtime.hwnd, WM_FILTER_INIT, 0, 0,
                              exfunc_get_editp(), "Stage 2 WM_FILTER_INIT");
            if (e->faulted)
            {
                dbg_warn("Filter[%d] '%s' Stage 2 WM_FILTER_INIT FAULTED -> disabled",
                         i, e->name);
                e->stage1_initialized = FALSE;
                e->runtime.flag &= ~(FILTER_FLAG_ACTIVE | FILTER_FLAG_WINDOW_DISP);
                if (e->src_dll)
                    e->src_dll->flag = e->runtime.flag;
                if (e->hwnd && IsWindow(e->hwnd))
                    ShowWindow(e->hwnd, SW_HIDE);
                continue;
            }
            dbg_plugin("Filter[%d] '%s' Stage 2 WM_FILTER_INIT OK (%lu ms, +%d menu items)",
                       i, e->name, (unsigned long)(GetTickCount() - t0),
                       g_menu_item_count - menus_before);
        }
        e->stage2_initialized = TRUE;
    }

    if (!defer_stage3)
    {
        notify_stage3_change_window();
    }

    // if a project/video is already open in editp, notify newly initialized filters
    if (editp && editp_is_editing(editp))
    {
        for (i = 0; i < g_plugin_count; i++)
        {
            PluginEntry *e = &g_plugins[i];
            if (e->stage2_initialized && e->runtime.func_WndProc)
            {
                e->runtime.func_WndProc(e->hwnd, WM_FILTER_FILE_OPEN, 0, 0,
                                        editp, &e->runtime);
            }
        }
    }
}

static void scan_auf_directory(const char *dir, BOOL *saw_32bit_plugin_on_x64)
{
    WIN32_FIND_DATAA fd;
    HANDLE hFind;
    char search_path[MAX_PATH];

    if (!dir || !dir[0])
        return;
    wsprintfA(search_path, "%s\\*.auf", dir);
    hFind = FindFirstFileA(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        return;
    }

    do
    {
        char full_path[MAX_PATH];
        PluginReport rep;
        int rc, i, k;
        BOOL duplicate = FALSE;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (g_plugin_count >= MAX_PLUGINS)
            break;

        // skip if a .auf with the same filename was already loaded from <exe_dir>
        for (k = 0; k < g_plugin_count; k++)
        {
            const char *existing = strrchr(g_plugins[k].path, '\\');
            existing = existing ? (existing + 1) : g_plugins[k].path;
            if (lstrcmpiA(existing, fd.cFileName) == 0)
            {
                duplicate = TRUE;
                break;
            }
        }
        if (duplicate)
            continue;

        wsprintfA(full_path, "%s\\%s", dir, fd.cFileName);
        rc = plugin_inspect_auf(full_path, &rep);
        if (rc < 0)
        {
#if REAVIUTL_X64
            if (rep.last_error == ERROR_BAD_EXE_FORMAT && saw_32bit_plugin_on_x64)
            {
                *saw_32bit_plugin_on_x64 = TRUE;
                dbg_plugin("Detected 32-bit plugin '%s' in 64-bit host -> will bridge via reaviutl_bridge32.exe",
                           fd.cFileName);
            }
            else
            {
                dbg_warn("Failed to load plugin '%s': %s", full_path, rep.err);
            }
#else
            (void)saw_32bit_plugin_on_x64;
            if (rep.last_error == ERROR_BAD_EXE_FORMAT)
            {
                dbg_plugin("Skipping 64-bit plugin '%s' in 32-bit process", fd.cFileName);
            }
            else
            {
                dbg_warn("Failed to load plugin '%s': %s", full_path, rep.err);
            }
#endif
            continue;
        }
        dbg_plugin("Loaded '%s' @ HMODULE=0x%p (%d filter%s)",
                   fd.cFileName, rep.module, rep.count, (rep.count == 1) ? "" : "s");

        for (i = 0; i < rep.count && g_plugin_count < MAX_PLUGINS; i++)
        {
            PluginEntry *e = &g_plugins[g_plugin_count];
            ZeroMemory(e, sizeof(*e));
            e->index = g_plugin_count;
            lstrcpynA(e->path, full_path, MAX_PATH);
            lstrcpynA(e->name,
                      rep.filters[i]->name ? rep.filters[i]->name : fd.cFileName,
                      sizeof(e->name));
            e->module = (HMODULE)rep.module;
            e->src_dll = rep.filters[i];
            e->is_builtin = FALSE;
            e->is_yuy2_filter = rep.is_yuy2[i];
            e->cmd_slot = g_next_seq_cmd_slot++;
            e->cmd_id = PLUGIN_CMD_WINDOW_BASE + e->cmd_slot;
            g_plugin_count++;
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
}

// 0x0042c530 / 0x0042e920: scan one level of subdirectories under plugins\ for *.auf
static void scan_auf_subdirectories(const char *plugin_dir, BOOL *saw_32bit_plugin_on_x64)
{
    WIN32_FIND_DATAA fd;
    HANDLE hFind;
    char pattern[MAX_PATH];

    if (!plugin_dir || !plugin_dir[0])
        return;
    _snprintf(pattern, sizeof(pattern), "%s\\*", plugin_dir);
    pattern[sizeof(pattern) - 1] = '\0';

    hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        char subdir[MAX_PATH];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == '.')
            continue;
        if (g_plugin_count >= MAX_PLUGINS)
            break;
        _snprintf(subdir, sizeof(subdir), "%s\\%s", plugin_dir, fd.cFileName);
        subdir[sizeof(subdir) - 1] = '\0';
        scan_auf_directory(subdir, saw_32bit_plugin_on_x64);
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
}

void plugin_manager_set_builtins_enabled(BOOL enabled)
{
    g_builtins_enabled = enabled ? TRUE : FALSE;
}

BOOL plugin_manager_init_ex(const char *plugin_dir, HWND main_hwnd, EDITP *editp)
{
    char parent_dir[MAX_PATH] = {0};
    BOOL has_parent_dir = FALSE;
    HWND owner_hwnd;
    BOOL saw_32bit_plugin_on_x64 = FALSE;

    if (g_plugin_count > 0 || plugin_bridge_get_filter_count() > 0)
    {
        plugin_manager_shutdown();
    }
    ensure_window_classes();

    g_main_hwnd = main_hwnd;
    owner_hwnd = ensure_owner_hwnd(main_hwnd);
    g_next_seq_cmd_slot = 21;

    if (!plugin_dir || !plugin_dir[0])
        return FALSE;

    if (g_builtins_enabled)
    {
        int b, bcnt;
        builtin_filters_prepare_tables(owner_hwnd);
        bcnt = builtin_filters_get_count();
        for (b = 0; b < bcnt && g_plugin_count < MAX_PLUGINS; b++)
        {
            FILTER_DLL *bf = builtin_filters_get_dll(b);
            PluginEntry *e;
            int fixed;
            if (!bf)
                continue;
            e = &g_plugins[g_plugin_count];
            ZeroMemory(e, sizeof(*e));
            e->index = g_plugin_count;
            e->path[0] = '\0';
            lstrcpynA(e->name, bf->name ? bf->name : "Builtin", sizeof(e->name));
            e->module = NULL;
            e->src_dll = bf;
            e->is_builtin = TRUE;
            fixed = builtin_filters_get_fixed_cmd_slot(b);
            e->cmd_slot = (fixed >= 0) ? fixed : g_next_seq_cmd_slot++;
            e->cmd_id = PLUGIN_CMD_WINDOW_BASE + e->cmd_slot;
            g_plugin_count++;
        }
    }

    // match aviutl 1.10 0x0042c930: scan both <exe_dir>\*.auf and <exe_dir>\plugins\*.auf
    {
        const char *slash = strrchr(plugin_dir, '\\');
        if (slash && lstrcmpiA(slash + 1, "plugins") == 0 && (slash - plugin_dir) > 0)
        {
            int plen = (int)(slash - plugin_dir);
            if (plen < MAX_PATH)
            {
                memcpy(parent_dir, plugin_dir, (size_t)plen);
                parent_dir[plen] = '\0';
                has_parent_dir = TRUE;
            }
        }
    }

    if (has_parent_dir)
    {
        scan_auf_directory(parent_dir, &saw_32bit_plugin_on_x64);
    }
    scan_auf_subdirectories(plugin_dir, &saw_32bit_plugin_on_x64);
    scan_auf_directory(plugin_dir, &saw_32bit_plugin_on_x64);

    if (g_plugin_count > 0)
    {
        initialize_registered_plugins(owner_hwnd, editp, saw_32bit_plugin_on_x64);
    }

#if REAVIUTL_X64
    // on 64-bit host, delegate 32-bit .auf plugins to reaviutl_bridge32.exe
    if (saw_32bit_plugin_on_x64)
    {
        bind_host_exfunc_callbacks(owner_hwnd, editp);
        if (!plugin_bridge_load_dir(plugin_dir, owner_hwnd, editp) && has_parent_dir)
        {
            plugin_bridge_load_dir(parent_dir, owner_hwnd, editp);
        }
    }
#else
    (void)saw_32bit_plugin_on_x64;
#endif

    editp_set_registered_filter_count(
        g_plugin_count + (plugin_bridge_is_active() ? plugin_bridge_get_filter_count() : 0));

    if (saw_32bit_plugin_on_x64 && g_plugin_count > 0)
    {
        notify_stage3_change_window();
    }

    return (g_plugin_count > 0 || plugin_bridge_is_active());
}

BOOL plugin_manager_init(const char *plugin_dir)
{
    return plugin_manager_init_ex(plugin_dir, g_main_hwnd, exfunc_get_editp());
}

BOOL plugin_manager_load_single_auf(const char *auf_path, HWND main_hwnd, EDITP *editp)
{
    PluginReport rep;
    HWND owner_hwnd;
    int rc, i;

    if (g_plugin_count > 0 || plugin_bridge_get_filter_count() > 0)
    {
        plugin_manager_shutdown();
    }
    ensure_window_classes();

    g_main_hwnd = main_hwnd;
    owner_hwnd = ensure_owner_hwnd(main_hwnd);

    if (!auf_path || !auf_path[0])
        return FALSE;

    rc = plugin_inspect_auf(auf_path, &rep);
    if (rc < 0)
    {
#if REAVIUTL_X64
        if (rep.last_error == ERROR_BAD_EXE_FORMAT)
        {
            dbg_plugin("32-bit plugin '%s' in 64-bit host -> bridging via reaviutl_bridge32.exe",
                       auf_path);
            bind_host_exfunc_callbacks(owner_hwnd, editp);
            if (plugin_bridge_load_file(auf_path, owner_hwnd, editp))
            {
                editp_set_registered_filter_count(plugin_bridge_get_filter_count());
                return TRUE;
            }
        }
#endif
        dbg_warn("Failed to load plugin '%s': %s", auf_path, rep.err);
        return FALSE;
    }
    dbg_plugin("Loaded '%s' @ HMODULE=0x%p (%d filter%s)",
               auf_path, rep.module, rep.count, (rep.count == 1) ? "" : "s");

    for (i = 0; i < rep.count && g_plugin_count < MAX_PLUGINS; i++)
    {
        PluginEntry *e = &g_plugins[g_plugin_count];
        ZeroMemory(e, sizeof(*e));
        e->index = g_plugin_count;
        lstrcpynA(e->path, auf_path, MAX_PATH);
        lstrcpynA(e->name,
                  rep.filters[i]->name ? rep.filters[i]->name : auf_path,
                  sizeof(e->name));
        e->module = (HMODULE)rep.module;
        e->src_dll = rep.filters[i];
        e->is_builtin = FALSE;
        e->is_yuy2_filter = rep.is_yuy2[i];
        e->cmd_slot = g_next_seq_cmd_slot++;
        e->cmd_id = PLUGIN_CMD_WINDOW_BASE + e->cmd_slot;
        g_plugin_count++;
    }

    if (g_plugin_count > 0)
    {
        initialize_registered_plugins(owner_hwnd, editp, FALSE);
        return TRUE;
    }
    return FALSE;
}

void plugin_manager_shutdown(void)
{
    int i, j;
    HMODULE unloaded[MAX_PLUGINS];
    int unloaded_count = 0;

    if (g_builtins_enabled)
    {
        builtin_filters_stop_playback();
    }

    if (plugin_bridge_is_active())
    {
        plugin_bridge_shutdown();
    }

    // save filter window positions & visibility before tearing down HWNDs
    if (g_plugin_count > 0)
    {
        char ini[MAX_PATH];
        get_plugin_ini_path(ini, sizeof(ini));
        for (i = 0; i < g_plugin_count; i++)
        {
            PluginEntry *e = &g_plugins[i];
            char sec[256], vbuf[32];
            RECT wrc;
            if (!e->stage1_initialized || !e->hwnd || !IsWindow(e->hwnd))
                continue;
            sanitize_ini_section(e->name, sec, sizeof(sec));
            if (!sec[0])
                continue;
            if (!IsIconic(e->hwnd) && GetWindowRect(e->hwnd, &wrc))
            {
                wsprintfA(vbuf, "%d", (int)wrc.left);
                WritePrivateProfileStringA(sec, "x", vbuf, ini);
                wsprintfA(vbuf, "%d", (int)wrc.top);
                WritePrivateProfileStringA(sec, "y", vbuf, ini);
                if (GetWindowLongA(e->hwnd, GWL_STYLE) & WS_THICKFRAME)
                {
                    wsprintfA(vbuf, "%d", (int)(wrc.right - wrc.left));
                    WritePrivateProfileStringA(sec, "w", vbuf, ini);
                    wsprintfA(vbuf, "%d", (int)(wrc.bottom - wrc.top));
                    WritePrivateProfileStringA(sec, "h", vbuf, ini);
                }
            }
            WritePrivateProfileStringA(
                sec, "disp",
                (e->runtime.flag & FILTER_FLAG_WINDOW_DISP) ? "1" : "0", ini);
        }
    }

    // stage 2 teardown, send WM_FILTER_EXIT (0x0468) while HWNDs & EDITP live
    for (i = g_plugin_count - 1; i >= 0; i--)
    {
        PluginEntry *e = &g_plugins[i];
        if (e->stage2_initialized && !e->faulted && e->runtime.func_WndProc)
        {
            safe_call_wndproc(e, e->runtime.hwnd, WM_FILTER_EXIT, 0, 0,
                              exfunc_get_editp(), "WM_FILTER_EXIT");
        }
        e->stage2_initialized = FALSE;
    }

    // destroy host-owned filter container windows
    for (i = g_plugin_count - 1; i >= 0; i--)
    {
        PluginEntry *e = &g_plugins[i];
        e->teardown = TRUE;
        if (e->hwnd && IsWindow(e->hwnd))
        {
            DestroyWindow(e->hwnd);
        }
        e->hwnd = NULL;
    }

    // stage 1 teardown, call func_exit(fp)
    for (i = g_plugin_count - 1; i >= 0; i--)
    {
        PluginEntry *e = &g_plugins[i];
        if (e->stage1_initialized && !e->faulted && e->runtime.func_exit)
        {
            safe_call_func_exit(e);
        }
        e->stage1_initialized = FALSE;
        plugin_free_runtime_filter(&e->runtime);
    }

    // free each unique HMODULE once (multi-filter .auf shares one HMODULE)
    for (i = 0; i < g_plugin_count; i++)
    {
        HMODULE mod = g_plugins[i].module;
        BOOL seen = FALSE;
        if (!mod)
            continue;
        for (j = 0; j < unloaded_count; j++)
        {
            if (unloaded[j] == mod)
            {
                seen = TRUE;
                break;
            }
        }
        if (!seen)
        {
            unloaded[unloaded_count++] = mod;
            FreeLibrary(mod);
        }
        g_plugins[i].module = NULL;
    }

    g_plugin_count = 0;
    g_menu_item_count = 0;
    g_config_cmd_count = 0;
    g_next_seq_cmd_slot = 21;
    free(g_yuy2_edit_buf);
    free(g_yuy2_temp_buf);
    g_yuy2_edit_buf = NULL;
    g_yuy2_temp_buf = NULL;
    g_yuy2_buf_cap = 0;
    aup_clear_preserved_chunks();
    editp_set_registered_filter_count(0);

    if (g_dummy_owner_hwnd && IsWindow(g_dummy_owner_hwnd))
    {
        DestroyWindow(g_dummy_owner_hwnd);
    }
    g_dummy_owner_hwnd = NULL;
}

BOOL plugin_manager_is_bridged(void)
{
    return plugin_bridge_is_active();
}

int plugin_manager_get_count(void)
{
    return g_plugin_count + (plugin_bridge_is_active() ? plugin_bridge_get_filter_count() : 0);
}

int plugin_manager_get_native_count(void)
{
    return g_plugin_count;
}

static BOOL is_exedit_main_name(const char *s)
{
    if (!s || !s[0])
        return FALSE;
    return lstrcmpA(s, "\x8a\x67\x92\xa3\x95\xd2\x8f\x57") == 0 ||
           _stricmp(s, "Advanced Editing") == 0 ||
           _stricmp(s, "Adv.Edit") == 0;
}

static BOOL is_exedit_audio_name(const char *s)
{
    if (!s || !s[0])
        return FALSE;
    return lstrcmpA(s, "\x8a\x67\x92\xa3\x95\xd2\x8f\x57(\x89\xb9\x90\xba)") == 0 ||
           _stricmp(s, "Advanced Editing (Audio)") == 0 ||
           _stricmp(s, "Advanced Editing(Audio)") == 0;
}

BOOL plugin_manager_filter_names_match(const char *a, const char *b)
{
    if (!a || !b)
        return FALSE;
    if (lstrcmpA(a, b) == 0)
        return TRUE;
    if (is_exedit_main_name(a) && is_exedit_main_name(b))
        return TRUE;
    if (is_exedit_audio_name(a) && is_exedit_audio_name(b))
        return TRUE;
    return FALSE;
}

BOOL plugin_manager_is_builtin(int index)
{
    if (index < 0 || index >= g_plugin_count)
        return FALSE;
    return g_plugins[index].is_builtin;
}

int plugin_manager_get_cmd_slot(int index)
{
    if (index < 0)
        return -1;
    if (index < g_plugin_count)
        return g_plugins[index].cmd_slot;
    if (plugin_bridge_is_active() && index < plugin_manager_get_count())
        return g_next_seq_cmd_slot + (index - g_plugin_count);
    return -1;
}

int plugin_manager_find_by_cmd_slot(int cmd_slot)
{
    int i, total;
    if (cmd_slot < 0)
        return -1;
    for (i = 0; i < g_plugin_count; i++)
    {
        if (g_plugins[i].cmd_slot == cmd_slot)
            return i;
    }
    if (plugin_bridge_is_active())
    {
        total = plugin_manager_get_count();
        for (i = g_plugin_count; i < total; i++)
        {
            if (plugin_manager_get_cmd_slot(i) == cmd_slot)
                return i;
        }
    }
    return -1;
}

const char *plugin_manager_get_name(int index)
{
    if (index < 0)
        return NULL;
    if (index < g_plugin_count)
        return g_plugins[index].name;
    if (plugin_bridge_is_active())
        return plugin_bridge_get_filter_name(index - g_plugin_count);
    return NULL;
}

const char *plugin_manager_get_information(int index)
{
    if (index < 0)
        return NULL;
    if (index < g_plugin_count)
    {
        if (g_plugins[index].runtime.information && g_plugins[index].runtime.information[0])
        {
            return g_plugins[index].runtime.information;
        }
        return g_plugins[index].name;
    }
    if (plugin_bridge_is_active())
        return plugin_bridge_get_filter_info(index - g_plugin_count);
    return NULL;
}

const char *plugin_manager_get_path(int index)
{
    if (index < 0)
        return NULL;
    if (index < g_plugin_count)
        return g_plugins[index].path;
    if (plugin_bridge_is_active())
        return plugin_bridge_get_filter_path(index - g_plugin_count);
    return NULL;
}

int plugin_manager_get_flags(int index)
{
    if (index < 0)
        return 0;
    if (index < g_plugin_count)
        return g_plugins[index].runtime.flag;
    if (plugin_bridge_is_active())
        return plugin_bridge_get_filter_flags(index - g_plugin_count);
    return 0;
}

BOOL plugin_manager_has_func_proc(int index)
{
    if (index < 0)
        return FALSE;
    if (index < g_plugin_count)
    {
        return g_plugins[index].stage1_initialized && !g_plugins[index].faulted &&
               g_plugins[index].runtime.func_proc != NULL;
    }
    if (plugin_bridge_is_active())
    {
        return plugin_bridge_get_filter_has_proc(index - g_plugin_count);
    }
    return FALSE;
}

int plugin_manager_get_priority(int index)
{
    if (index < 0)
        return 64;
    if (index < g_plugin_count)
        return g_plugins[index].priority;
    if (plugin_bridge_is_active())
        return plugin_bridge_get_filter_priority(index - g_plugin_count);
    return 64;
}

BOOL plugin_manager_set_priority(int index, int priority, BOOL save_ini)
{
    if (index < 0)
        return FALSE;
    if (index < g_plugin_count)
    {
        PluginEntry *e = &g_plugins[index];
        e->priority = priority;
        if (save_ini && e->name[0])
        {
            char ini[MAX_PATH], sec[256], val[32];
            get_plugin_ini_path(ini, sizeof(ini));
            sanitize_ini_section(e->name, sec, sizeof(sec));
            if (sec[0])
            {
                wsprintfA(val, "%d", priority);
                WritePrivateProfileStringA(sec, "priority", val, ini);
            }
        }
        exfunc_invalidate_all_caches();
        return TRUE;
    }
    if (plugin_bridge_is_active())
    {
        BOOL ok = plugin_bridge_set_filter_priority(index - g_plugin_count, priority, save_ini);
        exfunc_invalidate_all_caches();
        return ok;
    }
    return FALSE;
}

static int filter_priority_tier(int flags)
{
    if (flags & FILTER_FLAG_INTERLACE_FILTER)
        return -1;
    if (flags & FILTER_FLAG_PRIORITY_HIGHEST)
        return 0;
    if (flags & FILTER_FLAG_PRIORITY_LOWEST)
        return 2;
    return 1;
}

// 0x00430770: sorted filter execution order across native & bridged filters
int plugin_manager_get_execution_order(BOOL audio_mode, int *out_indices, int max_count)
{
    int cand[MAX_PLUGINS + BRIDGE_MAX_FILTERS];
    int prios[MAX_PLUGINS + BRIDGE_MAX_FILTERS];
    int tiers[MAX_PLUGINS + BRIDGE_MAX_FILTERS];
    int n = 0, i, j, total = plugin_manager_get_count();

    if (!out_indices || max_count <= 0)
        return 0;

    for (i = 0; i < total && n < (int)(sizeof(cand) / sizeof(cand[0])); i++)
    {
        int fl = plugin_manager_get_flags(i);
        BOOL is_aud = (fl & FILTER_FLAG_AUDIO_FILTER) != 0;
        if (is_aud != (audio_mode != 0))
            continue;
        if (fl & FILTER_FLAG_DISP_FILTER)
            continue;
        if (!plugin_manager_has_func_proc(i))
            continue;
        cand[n] = i;
        prios[n] = plugin_manager_get_priority(i);
        tiers[n] = filter_priority_tier(fl);
        n++;
    }

    // stable insertion sort by (priority, tier, registration order)
    for (i = 1; i < n; i++)
    {
        int ci = cand[i], pi = prios[i], ti = tiers[i];
        j = i - 1;
        while (j >= 0 && (prios[j] > pi || (prios[j] == pi && tiers[j] > ti)))
        {
            cand[j + 1] = cand[j];
            prios[j + 1] = prios[j];
            tiers[j + 1] = tiers[j];
            j--;
        }
        cand[j + 1] = ci;
        prios[j + 1] = pi;
        tiers[j + 1] = ti;
    }

    if (n > max_count)
        n = max_count;
    for (i = 0; i < n; i++)
    {
        out_indices[i] = cand[i];
    }
    return n;
}

// 0x00431210: Video (5139) / Audio (5181) Filter Execution Order ("PRIORITYLIST") dialog
BOOL plugin_manager_run_priority_dialog(HWND owner, BOOL audio_mode)
{
    enum
    {
        MAX_PRIO_DLG = MAX_PLUGINS + BRIDGE_MAX_FILTERS
    };
    wchar_t names[MAX_PRIO_DLG][256];
    const wchar_t *ptrs[MAX_PRIO_DLG + 1];
    int reg_idx[MAX_PRIO_DLG];
    int ord[MAX_PRIO_DLG];
    int exec_order[MAX_PRIO_DLG];
    int count = 0, exec_n, pass, i, k, rank, total;
    AviUtlDlgState st;

    total = plugin_manager_get_count();
    // populate reg_idx in 3 passes (HIGHEST, normal, LOWEST) so IDC_DLG_LST_RESET (32700) ord[k]=k matches 0x00430fe0
    for (pass = 0; pass < 3; pass++)
    {
        for (i = 0; i < total && count < MAX_PRIO_DLG; i++)
        {
            int fl = plugin_manager_get_flags(i);
            BOOL is_aud = (fl & FILTER_FLAG_AUDIO_FILTER) != 0;
            if (is_aud != (audio_mode != 0))
                continue;
            if (fl & (FILTER_FLAG_INTERLACE_FILTER | FILTER_FLAG_DISP_FILTER))
                continue;
            if (!plugin_manager_has_func_proc(i))
                continue;
            if (filter_priority_tier(fl) != pass)
                continue;

            sjis_to_wide(plugin_manager_get_name(i), names[count], 256);
            if (!names[count][0])
                lstrcpyW(names[count], L"Filter");
            ptrs[count] = names[count];
            reg_idx[count] = i;
            ord[count] = count;
            count++;
        }
    }
    ptrs[count] = NULL;
    if (count <= 0)
        return FALSE;

    exec_n = plugin_manager_get_execution_order(audio_mode, exec_order, MAX_PRIO_DLG);
    rank = 0;
    for (i = 0; i < exec_n; i++)
    {
        for (k = 0; k < count; k++)
        {
            if (reg_idx[k] == exec_order[i])
            {
                ord[k] = rank++;
                break;
            }
        }
    }

    aviutl_dlg_init(&st);
    st.list_items[0] = ptrs;
    st.list_order[0] = ord;
    st.list_count[0] = count;

    if (aviutl_dlg_run(owner, "PRIORITYLIST", &st) != IDOK)
        return FALSE;

    for (rank = 0; rank < count; rank++)
    {
        int chosen = reg_idx[rank];
        for (k = 0; k < count; k++)
        {
            if (ord[k] == rank)
            {
                chosen = reg_idx[k];
                break;
            }
        }
        // assign 1..count so any unlisted INTERLACE_FILTER (priority 0) stays ahead
        plugin_manager_set_priority(chosen, rank + 1, TRUE);
    }

    exfunc_invalidate_all_caches();
    if (plugin_bridge_is_active())
    {
        plugin_bridge_invalidate_caches();
    }
    if (g_builtins_enabled)
    {
        builtin_filters_refresh_tool_window();
    }
    return TRUE;
}

FILTER *plugin_manager_get_filter(int index)
{
    if (index < 0 || index >= g_plugin_count)
        return NULL;
    if (!g_plugins[index].stage1_initialized)
        return NULL;
    return &g_plugins[index].runtime;
}

HWND plugin_manager_get_window(int index)
{
    if (index < 0)
        return NULL;
    if (index < g_plugin_count)
        return g_plugins[index].hwnd;
    if (plugin_bridge_is_active())
        return plugin_bridge_get_filter_hwnd(index - g_plugin_count);
    return NULL;
}

int plugin_manager_find_by_name(const char *name)
{
    int i, n = plugin_manager_get_count();
    if (!name)
        return -1;
    for (i = 0; i < n; i++)
    {
        const char *nm = plugin_manager_get_name(i);
        if (nm && plugin_manager_filter_names_match(nm, name))
            return i;
    }
    return -1;
}

int plugin_manager_get_menu_item_count(void)
{
    return g_menu_item_count + (plugin_bridge_is_active() ? plugin_bridge_get_menu_item_count() : 0);
}

BOOL plugin_manager_get_menu_item(int index, PluginMenuItem *out_item)
{
    if (!out_item || index < 0)
        return FALSE;
    if (index < g_menu_item_count)
    {
        *out_item = g_menu_items[index];
        return TRUE;
    }
    if (plugin_bridge_is_active())
    {
        BridgeMenuItemInfo bi;
        if (!plugin_bridge_get_menu_item(index - g_menu_item_count, &bi))
            return FALSE;
        ZeroMemory(out_item, sizeof(*out_item));
        out_item->filter_index = bi.filter_index + g_plugin_count;
        lstrcpynA(out_item->name, bi.name, sizeof(out_item->name));
        out_item->cmd_id = bi.cmd_id;
        out_item->def_key = bi.def_key;
        out_item->flag = bi.flag;
        return TRUE;
    }
    return FALSE;
}

BOOL plugin_manager_show_window(int index, BOOL show)
{
    PluginEntry *e;
    BOOL changed;

    if (index < 0)
        return FALSE;
    if (index >= g_plugin_count)
    {
        if (plugin_bridge_is_active())
            return plugin_bridge_show_window(index - g_plugin_count, show ? 1 : 0);
        return FALSE;
    }

    e = &g_plugins[index];
    if (!e->hwnd || !IsWindow(e->hwnd))
        return FALSE;

    changed = (((e->runtime.flag & FILTER_FLAG_WINDOW_DISP) != 0) != (show != 0));
    if (show)
    {
        e->runtime.flag |= FILTER_FLAG_WINDOW_DISP;
        if (e->src_dll)
            e->src_dll->flag = e->runtime.flag;
        ShowWindow(e->hwnd, SW_SHOWNA);
    }
    else
    {
        e->runtime.flag &= ~FILTER_FLAG_WINDOW_DISP;
        if (e->src_dll)
            e->src_dll->flag = e->runtime.flag;
        ShowWindow(e->hwnd, SW_HIDE);
    }

    if (changed && e->stage2_initialized && e->runtime.func_WndProc)
    {
        e->runtime.func_WndProc(e->hwnd, WM_FILTER_CHANGE_WINDOW, 0, 0,
                                exfunc_get_editp(), &e->runtime);
    }
    post_host_redraw(3);
    return TRUE;
}

BOOL plugin_manager_toggle_window(int index)
{
    if (index < 0)
        return FALSE;
    if (index >= g_plugin_count)
    {
        if (plugin_bridge_is_active())
            return plugin_bridge_show_window(index - g_plugin_count, 2);
        return FALSE;
    }
    return plugin_manager_show_window(
        index, (g_plugins[index].runtime.flag & FILTER_FLAG_WINDOW_DISP) == 0);
}

BOOL plugin_manager_set_active(int index, BOOL active)
{
    PluginEntry *e;
    BOOL was_active;

    if (index < 0)
        return FALSE;
    if (index >= g_plugin_count)
    {
        if (plugin_bridge_is_active())
        {
            BOOL ok = plugin_bridge_set_filter_active(index - g_plugin_count, active ? 1 : 0);
            if (ok)
            {
                invalidate_downstream_from_entry(index);
                post_host_redraw(3);
            }
            return ok;
        }
        return FALSE;
    }

    e = &g_plugins[index];
    if (e->faulted || (e->runtime.flag & FILTER_FLAG_ALWAYS_ACTIVE))
        return FALSE;

    was_active = (e->runtime.flag & FILTER_FLAG_ACTIVE) != 0;
    if (active)
        e->runtime.flag |= FILTER_FLAG_ACTIVE;
    else
        e->runtime.flag &= ~FILTER_FLAG_ACTIVE;

    if (e->src_dll)
        e->src_dll->flag = e->runtime.flag;
    if (e->hwnd_active_chk && IsWindow(e->hwnd_active_chk))
    {
        SendMessageA(e->hwnd_active_chk, BM_SETCHECK,
                     active ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (was_active != (active != 0))
    {
        if (e->stage1_initialized && e->runtime.func_update)
        {
            e->runtime.func_update(&e->runtime, FILTER_UPDATE_STATUS_ALL);
        }
        if (e->stage2_initialized && e->runtime.func_WndProc)
        {
            safe_call_wndproc(e, e->hwnd, WM_FILTER_CHANGE_ACTIVE, 0, 0,
                              exfunc_get_editp(), "WM_FILTER_CHANGE_ACTIVE");
        }
        invalidate_downstream_from_entry(index);
    }
    post_host_redraw(3);
    post_host_redraw(1);
    if (g_builtins_enabled)
    {
        builtin_filters_refresh_tool_window();
    }
    return TRUE;
}

BOOL plugin_manager_toggle_active(int index)
{
    if (index < 0)
        return FALSE;
    if (index >= g_plugin_count)
    {
        if (plugin_bridge_is_active())
        {
            BOOL ok = plugin_bridge_set_filter_active(index - g_plugin_count, 2);
            if (ok)
            {
                invalidate_downstream_from_entry(index);
                post_host_redraw(3);
                post_host_redraw(1);
                if (g_builtins_enabled)
                    builtin_filters_refresh_tool_window();
            }
            return ok;
        }
        return FALSE;
    }
    return plugin_manager_set_active(
        index, (g_plugins[index].runtime.flag & FILTER_FLAG_ACTIVE) == 0);
}

void plugin_manager_disable_all_filters(void)
{
    int i;
    for (i = 0; i < g_plugin_count; i++)
    {
        if (!(g_plugins[i].runtime.flag & FILTER_FLAG_ALWAYS_ACTIVE) &&
            (g_plugins[i].runtime.flag & FILTER_FLAG_ACTIVE))
        {
            plugin_manager_set_active(i, FALSE);
        }
    }
    if (plugin_bridge_is_active())
    {
        plugin_bridge_set_filter_active(0, 3);
        exfunc_invalidate_all_caches();
        plugin_bridge_invalidate_caches();
        post_host_redraw(3);
        post_host_redraw(1);
        if (g_builtins_enabled)
            builtin_filters_refresh_tool_window();
    }
}

static uint32_t cfg_rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void cfg_wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

// 0x0042f5f0: serialize "AviUtl FilterConfigFile version 0.1" binary state blob
BOOL plugin_manager_serialize_cfg_blob(uint8_t **out_buf, size_t *out_size)
{
    size_t total_sz = PROFILE_CFG_HEADER_SIZE;
    uint8_t *buf;
    size_t pos;
    int i, t, c;

    if (!out_buf || !out_size)
        return FALSE;
    *out_buf = NULL;
    *out_size = 0;

    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        FILTER *fp = &e->runtime;
        uint32_t name_bytes, num_fields, fsz2, fsz3, fsz4;
        BOOL has_ex;

        if (!e->stage1_initialized || !fp->name || !fp->name[0])
            continue;
        sync_dynamic_config_check_arrays(e);

        name_bytes = (uint32_t)strlen(fp->name) + 1u;
        has_ex = ((fp->flag & FILTER_FLAG_EX_DATA) != 0 &&
                  fp->ex_data_size > 0 && fp->ex_data_ptr != NULL);
        num_fields = has_ex ? 5u : 4u;
        fsz2 = (fp->track_n > 0 && fp->track != NULL) ? ((uint32_t)fp->track_n * 4u) : 0u;
        fsz3 = (fp->check_n > 0 && fp->check != NULL) ? ((uint32_t)fp->check_n * 4u) : 0u;
        fsz4 = has_ex ? (uint32_t)fp->ex_data_size : 0u;

        total_sz += (size_t)(4u + name_bytes + 4u + num_fields * 4u +
                             4u + 4u + fsz2 + fsz3 + fsz4);
    }

    buf = (uint8_t *)calloc(1, total_sz);
    if (!buf)
        return FALSE;

    memcpy(buf, PROFILE_CFG_MAGIC, strlen(PROFILE_CFG_MAGIC));
    pos = PROFILE_CFG_HEADER_SIZE;

    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        FILTER *fp = &e->runtime;
        uint32_t name_bytes, num_fields, fsz[5], rec_total;
        BOOL has_ex;

        if (!e->stage1_initialized || !fp->name || !fp->name[0])
            continue;

        name_bytes = (uint32_t)strlen(fp->name) + 1u;
        has_ex = ((fp->flag & FILTER_FLAG_EX_DATA) != 0 &&
                  fp->ex_data_size > 0 && fp->ex_data_ptr != NULL);
        num_fields = has_ex ? 5u : 4u;
        fsz[0] = 4u;
        fsz[1] = 4u;
        fsz[2] = (fp->track_n > 0 && fp->track != NULL) ? ((uint32_t)fp->track_n * 4u) : 0u;
        fsz[3] = (fp->check_n > 0 && fp->check != NULL) ? ((uint32_t)fp->check_n * 4u) : 0u;
        fsz[4] = has_ex ? (uint32_t)fp->ex_data_size : 0u;

        rec_total = 4u + name_bytes + 4u + num_fields * 4u +
                    fsz[0] + fsz[1] + fsz[2] + fsz[3] + (has_ex ? fsz[4] : 0u);

        cfg_wr_u32(buf + pos, rec_total);
        pos += 4u;
        memcpy(buf + pos, fp->name, (size_t)name_bytes);
        pos += (size_t)name_bytes;
        cfg_wr_u32(buf + pos, num_fields);
        pos += 4u;
        for (t = 0; t < (int)num_fields; t++)
        {
            cfg_wr_u32(buf + pos, fsz[t]);
            pos += 4u;
        }

        cfg_wr_u32(buf + pos, (uint32_t)fp->flag);
        pos += 4u;
        cfg_wr_u32(buf + pos, (uint32_t)e->priority);
        pos += 4u;

        if (fsz[2] > 0)
        {
            for (t = 0; t < fp->track_n; t++)
            {
                cfg_wr_u32(buf + pos, (uint32_t)fp->track[t]);
                pos += 4u;
            }
        }
        if (fsz[3] > 0)
        {
            for (c = 0; c < fp->check_n; c++)
            {
                cfg_wr_u32(buf + pos, (uint32_t)fp->check[c]);
                pos += 4u;
            }
        }
        if (has_ex && fsz[4] > 0)
        {
            memcpy(buf + pos, fp->ex_data_ptr, (size_t)fsz[4]);
            pos += (size_t)fsz[4];
        }
    }

    *out_buf = buf;
    *out_size = pos;
    return TRUE;
}

static PluginEntry *find_plugin_entry_for_cfg_name(const char *rec_name)
{
    int i;
    const char *gt;

    if (!rec_name || !rec_name[0])
        return NULL;

    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        if (!e->stage1_initialized)
            continue;
        if ((e->runtime.name && plugin_manager_filter_names_match(e->runtime.name, rec_name)) ||
            plugin_manager_filter_names_match(e->name, rec_name))
        {
            return e;
        }
    }

    if (rec_name[0] == '<')
    {
        gt = strchr(rec_name, '>');
        if (gt && gt[1])
        {
            const char *sub = gt + 1;
            for (i = 0; i < g_plugin_count; i++)
            {
                PluginEntry *e = &g_plugins[i];
                if (!e->stage1_initialized)
                    continue;
                if ((e->runtime.name && plugin_manager_filter_names_match(e->runtime.name, sub)) ||
                    plugin_manager_filter_names_match(e->name, sub))
                {
                    return e;
                }
            }
        }
    }
    return NULL;
}

BOOL plugin_manager_apply_cfg_blob(const uint8_t *buf, size_t size, BOOL update_ui)
{
    size_t pos;

    if (!buf || size < PROFILE_CFG_HEADER_SIZE)
        return FALSE;
    if (memcmp(buf, PROFILE_CFG_MAGIC, strlen(PROFILE_CFG_MAGIC)) != 0)
        return FALSE;

    pos = PROFILE_CFG_HEADER_SIZE;
    while (pos + 12u <= size)
    {
        uint32_t rec_total = cfg_rd_u32(buf + pos);
        const uint8_t *rec_start = buf + pos;
        const uint8_t *rec_end;
        const uint8_t *nul_p;
        const char *rec_name;
        const uint8_t *p;
        uint32_t num_fields;
        uint32_t fsz[16];
        const uint8_t *fptr[16];
        uint32_t f;
        PluginEntry *e;

        if (rec_total < 12u || pos + (size_t)rec_total > size)
            break;
        rec_end = rec_start + rec_total;
        pos += (size_t)rec_total;

        nul_p = (const uint8_t *)memchr(rec_start + 4u, 0, (size_t)(rec_end - (rec_start + 4u)));
        if (!nul_p)
            continue;
        rec_name = (const char *)(rec_start + 4u);
        p = nul_p + 1;
        if (p + 4u > rec_end)
            continue;

        num_fields = cfg_rd_u32(p);
        p += 4u;
        if (num_fields == 0 || num_fields > 16u || p + (size_t)num_fields * 4u > rec_end)
            continue;

        for (f = 0; f < num_fields; f++)
        {
            fsz[f] = cfg_rd_u32(p);
            p += 4u;
        }
        for (f = 0; f < num_fields; f++)
        {
            if ((size_t)fsz[f] > (size_t)(rec_end - p))
                break;
            fptr[f] = p;
            p += (size_t)fsz[f];
        }
        if (f < num_fields)
            continue;

        e = find_plugin_entry_for_cfg_name(rec_name);
        if (!e || e->faulted)
            continue;

        sync_dynamic_config_check_arrays(e);

        if (num_fields >= 1u && fsz[0] >= 4u)
        {
            int32_t saved_flag = (int32_t)cfg_rd_u32(fptr[0]);
            if (!(e->runtime.flag & (FILTER_FLAG_ALWAYS_ACTIVE | FILTER_FLAG_DISP_FILTER)))
            {
                if (saved_flag & FILTER_FLAG_ACTIVE)
                    e->runtime.flag |= FILTER_FLAG_ACTIVE;
                else
                    e->runtime.flag &= ~FILTER_FLAG_ACTIVE;
                if (e->src_dll)
                    e->src_dll->flag = e->runtime.flag;
            }
        }

        if (num_fields >= 2u && fsz[1] >= 4u)
        {
            int32_t saved_prio = (int32_t)cfg_rd_u32(fptr[1]);
            if (!(e->runtime.flag & FILTER_FLAG_INTERLACE_FILTER))
            {
                e->priority = saved_prio;
            }
        }

        if (num_fields >= 3u && e->runtime.track && e->runtime.track_n > 0)
        {
            int cnt = (int)(fsz[2] / 4u);
            int t;
            if (cnt > e->runtime.track_n)
                cnt = e->runtime.track_n;
            for (t = 0; t < cnt; t++)
            {
                int val = (int)(int32_t)cfg_rd_u32(fptr[2] + (size_t)t * 4u);
                if (e->runtime.track_s && val < e->runtime.track_s[t])
                    val = e->runtime.track_s[t];
                if (e->runtime.track_e && val > e->runtime.track_e[t])
                    val = e->runtime.track_e[t];
                e->runtime.track[t] = val;
            }
        }

        if (num_fields >= 4u && e->runtime.check && e->runtime.check_n > 0)
        {
            int cnt = (int)(fsz[3] / 4u);
            int c;
            if (cnt > e->runtime.check_n)
                cnt = e->runtime.check_n;
            for (c = 0; c < cnt; c++)
            {
                int cdef = e->runtime.check_default ? e->runtime.check_default[c] : 0;
                if (cdef >= 0)
                {
                    e->runtime.check[c] = (int)(int32_t)cfg_rd_u32(fptr[3] + (size_t)c * 4u);
                }
            }
        }

        if (num_fields >= 5u && fsz[4] > 0 &&
            (e->runtime.flag & FILTER_FLAG_EX_DATA) &&
            e->runtime.ex_data_ptr && e->runtime.ex_data_size > 0)
        {
            size_t cp = (size_t)fsz[4];
            if (cp > (size_t)e->runtime.ex_data_size)
                cp = (size_t)e->runtime.ex_data_size;
            memcpy(e->runtime.ex_data_ptr, fptr[4], cp);
        }

        if (e->runtime.func_update)
        {
            e->runtime.func_update(&e->runtime, FILTER_UPDATE_STATUS_ALL);
            sync_dynamic_config_check_arrays(e);
        }

        if (update_ui)
        {
            sync_filter_entry_controls(e);
            if (e->stage2_initialized && e->runtime.func_WndProc)
            {
                safe_call_wndproc(e, e->hwnd, WM_FILTER_CHANGE_ACTIVE, 0, 0,
                                  exfunc_get_editp(), "WM_FILTER_CHANGE_ACTIVE");
                safe_call_wndproc(e, e->hwnd, WM_FILTER_CHANGE_PARAM, 0, 0,
                                  exfunc_get_editp(), "WM_FILTER_CHANGE_PARAM");
            }
            if (e->hwnd && IsWindow(e->hwnd))
            {
                InvalidateRect(e->hwnd, NULL, FALSE);
            }
        }
    }

    exfunc_invalidate_all_caches();
    if (plugin_bridge_is_active())
    {
        plugin_bridge_invalidate_caches();
    }
    if (update_ui)
    {
        if (g_builtins_enabled)
            builtin_filters_refresh_tool_window();
        post_host_redraw(3);
        post_host_redraw(1);
    }
    return TRUE;
}

BOOL plugin_manager_save_cfg_file(const char *cfg_path)
{
    uint8_t *blob = NULL;
    size_t sz = 0;
    HANDLE hFile;
    DWORD written = 0;
    BOOL ok;

    if (!cfg_path || !cfg_path[0])
        return FALSE;
    if (!plugin_manager_serialize_cfg_blob(&blob, &sz) || !blob || sz == 0)
        return FALSE;

    hFile = CreateFileA(cfg_path, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        free(blob);
        return FALSE;
    }
    ok = WriteFile(hFile, blob, (DWORD)sz, &written, NULL) && (size_t)written == sz;
    CloseHandle(hFile);
    free(blob);
    if (!ok)
        DeleteFileA(cfg_path);
    return ok;
}

BOOL plugin_manager_load_cfg_file(const char *cfg_path, BOOL update_ui)
{
    HANDLE hFile;
    DWORD sz_lo, sz_hi = 0, got = 0;
    uint8_t *buf;
    BOOL ok;

    if (!cfg_path || !cfg_path[0])
        return FALSE;
    hFile = CreateFileA(cfg_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;

    sz_lo = GetFileSize(hFile, &sz_hi);
    if (sz_lo == INVALID_FILE_SIZE || sz_hi != 0 || sz_lo < PROFILE_CFG_HEADER_SIZE ||
        sz_lo > (64u * 1024u * 1024u))
    {
        CloseHandle(hFile);
        return FALSE;
    }

    buf = (uint8_t *)malloc((size_t)sz_lo);
    if (!buf)
    {
        CloseHandle(hFile);
        return FALSE;
    }
    if (!ReadFile(hFile, buf, sz_lo, &got, NULL) || got != sz_lo)
    {
        free(buf);
        CloseHandle(hFile);
        return FALSE;
    }
    CloseHandle(hFile);

    ok = plugin_manager_apply_cfg_blob(buf, (size_t)sz_lo, update_ui);
    free(buf);
    return ok;
}

void plugin_manager_reset_all_filters_to_defaults(void)
{
    int i, t, c;
    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        if (!e->stage1_initialized || e->faulted)
            continue;

        sync_dynamic_config_check_arrays(e);

        if (!(e->runtime.flag & (FILTER_FLAG_ALWAYS_ACTIVE | FILTER_FLAG_DISP_FILTER)))
        {
            if (e->default_flag & FILTER_FLAG_ACTIVE)
                e->runtime.flag |= FILTER_FLAG_ACTIVE;
            else
                e->runtime.flag &= ~FILTER_FLAG_ACTIVE;
            if (e->src_dll)
                e->src_dll->flag = e->runtime.flag;
        }

        if (!(e->runtime.flag & FILTER_FLAG_INTERLACE_FILTER))
        {
            e->priority = e->default_priority;
        }

        if (e->runtime.track)
        {
            for (t = 0; t < e->runtime.track_n; t++)
            {
                e->runtime.track[t] = e->runtime.track_default ? e->runtime.track_default[t] : 0;
            }
        }

        if (e->runtime.check)
        {
            int first_radio_checked = -1;
            for (c = 0; c < e->runtime.check_n; c++)
            {
                int cdef = e->runtime.check_default ? e->runtime.check_default[c] : 0;
                if (cdef >= 0)
                {
                    e->runtime.check[c] = cdef;
                    if ((e->runtime.flag & (FILTER_FLAG_RADIO_BUTTON | FILTER_FLAG_CONFIG_RADIO)) &&
                        cdef > 0 && first_radio_checked < 0)
                    {
                        first_radio_checked = c;
                    }
                }
            }
            if ((e->runtime.flag & (FILTER_FLAG_RADIO_BUTTON | FILTER_FLAG_CONFIG_RADIO)) &&
                e->runtime.check_n > 0)
            {
                if (first_radio_checked < 0)
                    first_radio_checked = 0;
                for (c = 0; c < e->runtime.check_n; c++)
                {
                    int cdef = e->runtime.check_default ? e->runtime.check_default[c] : 0;
                    if (cdef >= 0)
                        e->runtime.check[c] = (c == first_radio_checked) ? 1 : 0;
                }
            }
        }

        if ((e->runtime.flag & FILTER_FLAG_EX_DATA) &&
            e->runtime.ex_data_ptr && e->runtime.ex_data_def && e->runtime.ex_data_size > 0)
        {
            memcpy(e->runtime.ex_data_ptr, e->runtime.ex_data_def, (size_t)e->runtime.ex_data_size);
        }

        sync_filter_entry_controls(e);
        if (e->runtime.func_update)
        {
            e->runtime.func_update(&e->runtime, FILTER_UPDATE_STATUS_ALL);
            sync_dynamic_config_check_arrays(e);
        }
        if (e->stage2_initialized && e->runtime.func_WndProc)
        {
            safe_call_wndproc(e, e->hwnd, WM_FILTER_CHANGE_ACTIVE, 0, 0,
                              exfunc_get_editp(), "WM_FILTER_CHANGE_ACTIVE");
            safe_call_wndproc(e, e->hwnd, WM_FILTER_CHANGE_PARAM, 0, 0,
                              exfunc_get_editp(), "WM_FILTER_CHANGE_PARAM");
        }
        if (e->hwnd && IsWindow(e->hwnd))
        {
            InvalidateRect(e->hwnd, NULL, FALSE);
        }
    }

    if (plugin_bridge_is_active())
    {
        plugin_bridge_set_filter_active(0, 3);
        plugin_bridge_invalidate_caches();
    }
    exfunc_invalidate_all_caches();
    if (g_builtins_enabled)
    {
        builtin_filters_refresh_tool_window();
    }
    post_host_redraw(3);
    post_host_redraw(1);
}

int plugin_manager_get_config_check_count(int index)
{
    if (index < 0 || index >= g_plugin_count)
        return 0;
    sync_dynamic_config_check_arrays(&g_plugins[index]);
    return g_plugins[index].runtime.check_n;
}

const char *plugin_manager_get_config_check_name(int index, int check_idx)
{
    PluginEntry *e;
    if (index < 0 || index >= g_plugin_count)
        return NULL;
    e = &g_plugins[index];
    sync_dynamic_config_check_arrays(e);
    if (check_idx < 0 || check_idx >= e->runtime.check_n || !e->runtime.check_name)
        return NULL;
    return e->runtime.check_name[check_idx];
}

int plugin_manager_get_config_check_default(int index, int check_idx)
{
    PluginEntry *e;
    if (index < 0 || index >= g_plugin_count)
        return 0;
    e = &g_plugins[index];
    sync_dynamic_config_check_arrays(e);
    if (check_idx < 0 || check_idx >= e->runtime.check_n || !e->runtime.check_default)
        return 0;
    return e->runtime.check_default[check_idx];
}

int plugin_manager_get_config_check_value(int index, int check_idx)
{
    PluginEntry *e;
    if (index < 0 || index >= g_plugin_count)
        return 0;
    e = &g_plugins[index];
    sync_dynamic_config_check_arrays(e);
    if (check_idx < 0 || check_idx >= e->runtime.check_n || !e->runtime.check)
        return 0;
    return e->runtime.check[check_idx];
}

BOOL plugin_manager_select_config_check(int index, int check_idx)
{
    PluginEntry *e;
    int k;

    if (index < 0 || index >= g_plugin_count)
        return FALSE;
    e = &g_plugins[index];
    sync_dynamic_config_check_arrays(e);
    if (!e->runtime.check || check_idx < 0 || check_idx >= e->runtime.check_n)
        return FALSE;
    if (e->runtime.check_default && e->runtime.check_default[check_idx] < 0)
        return FALSE;

    if (e->runtime.flag & (FILTER_FLAG_CONFIG_RADIO | FILTER_FLAG_RADIO_BUTTON))
    {
        for (k = 0; k < e->runtime.check_n; k++)
        {
            int kdef = e->runtime.check_default ? e->runtime.check_default[k] : 0;
            if (kdef >= 0)
                e->runtime.check[k] = (k == check_idx) ? 1 : 0;
        }
    }
    else
    {
        e->runtime.check[check_idx] = e->runtime.check[check_idx] ? 0 : 1;
    }

    sync_filter_entry_controls(e);
    if (e->stage1_initialized && e->runtime.func_update)
    {
        e->runtime.func_update(&e->runtime, FILTER_UPDATE_STATUS_CHECK + check_idx);
        sync_dynamic_config_check_arrays(e);
    }
    if (e->stage2_initialized && e->runtime.func_WndProc)
    {
        safe_call_wndproc(e, e->hwnd, WM_FILTER_CHANGE_PARAM, 0, 0,
                          exfunc_get_editp(), "WM_FILTER_CHANGE_PARAM");
    }
    invalidate_downstream_from_entry(index);
    exfunc_invalidate_all_caches();
    if (plugin_bridge_is_active())
    {
        plugin_bridge_invalidate_caches();
    }
    post_host_redraw(3);
    post_host_redraw(1);
    return TRUE;
}

void plugin_manager_clear_config_cmd_map(void)
{
    g_config_cmd_count = 0;
}

int plugin_manager_register_config_cmd(int filter_index, int check_idx)
{
    int id;
    if (g_config_cmd_count >= MAX_CONFIG_CMD_ITEMS)
        return 0;
    id = PLUGIN_CMD_CONFIG_ITEM_BASE + g_config_cmd_count;
    g_config_cmds[g_config_cmd_count].filter_index = filter_index;
    g_config_cmds[g_config_cmd_count].check_idx = check_idx;
    g_config_cmd_count++;
    return id;
}

void plugin_manager_reset_window_positions(HWND main_hwnd)
{
    RECT wa, mrc;
    int base_x = 64, base_y = 64;
    int step = 0, i;

    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0))
    {
        SetRect(&wa, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }
    if (main_hwnd && IsWindow(main_hwnd) && GetWindowRect(main_hwnd, &mrc))
    {
        base_x = mrc.right + 8;
        base_y = mrc.top;
        if (base_x + 240 > wa.right)
        {
            base_x = wa.left + 32;
            base_y = wa.top + 32;
        }
    }

    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        int x, y;
        if (!e->hwnd || !IsWindow(e->hwnd))
            continue;
        x = base_x + (step % 8) * 24;
        y = base_y + (step % 8) * 24;
        if (x + 160 > wa.right)
            x = wa.left + 24;
        if (y + 120 > wa.bottom)
            y = wa.top + 24;
        SetWindowPos(e->hwnd, NULL, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        step++;
    }
}

BOOL plugin_manager_exec_import_export(int index, BOOL is_export)
{
    PluginEntry *e;
    UINT msg = is_export ? WM_FILTER_EXPORT : WM_FILTER_IMPORT;

    if (index < 0)
        return FALSE;
    if (index >= g_plugin_count)
    {
        if (plugin_bridge_is_active())
        {
            BOOL ok = plugin_bridge_filter_io(index - g_plugin_count, is_export, exfunc_get_editp());
            if (ok)
            {
                if (!is_export)
                    invalidate_downstream_from_entry(index);
                post_host_redraw(1);
            }
            return ok;
        }
        return FALSE;
    }
    e = &g_plugins[index];
    if (!e->stage2_initialized || e->faulted || !e->runtime.func_WndProc)
        return FALSE;
    if (safe_call_wndproc(e, e->hwnd, msg, 0, 0, exfunc_get_editp(),
                          is_export ? "WM_FILTER_EXPORT" : "WM_FILTER_IMPORT"))
    {
        if (!is_export)
            invalidate_downstream_from_entry(index);
        post_host_redraw(1);
    }
    return TRUE;
}

HMENU plugin_manager_build_menu(void)
{
    HMENU menu = CreatePopupMenu();
    int i, count = plugin_manager_get_count();
    int mcount = plugin_manager_get_menu_item_count();

    if (count == 0)
    {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"(no plugins loaded)");
        return menu;
    }

    for (i = 0; i < count; i++)
    {
        const char *name = plugin_manager_get_name(i);
        int flags = plugin_manager_get_flags(i);
        HWND hwnd = plugin_manager_get_window(i);
        wchar_t wlabel[300];
        UINT uFlags = MF_STRING;

        if (flags & FILTER_FLAG_NO_CONFIG)
            continue;
        sjis_to_wide(name ? name : "(unnamed)", wlabel, 256);
        if (i >= g_plugin_count && plugin_bridge_is_active())
        {
            lstrcatW(wlabel, L" [x86 Bridge]");
        }
        if ((flags & FILTER_FLAG_WINDOW_DISP) || (hwnd && IsWindowVisible(hwnd)))
        {
            uFlags |= MF_CHECKED;
        }
        {
            int slot = plugin_manager_get_cmd_slot(i);
            if (slot >= 0)
            {
                AppendMenuW(menu, uFlags, (UINT_PTR)(PLUGIN_CMD_WINDOW_BASE + slot), wlabel);
            }
        }
    }

    if (mcount > 0)
    {
        HMENU sub = CreatePopupMenu();
        for (i = 0; i < mcount; i++)
        {
            PluginMenuItem it;
            wchar_t wname[256];
            if (!plugin_manager_get_menu_item(i, &it))
                continue;
            sjis_to_wide(it.name, wname, 256);
            AppendMenuW(sub, MF_STRING, (UINT_PTR)(PLUGIN_CMD_MENU_ITEM_BASE + i), wname);
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"Plugin Commands / \x62e1\x5f35\x7de8\x96c6");
    }

    return menu;
}

BOOL plugin_manager_exec_menu_item_index(int menu_index)
{
    PluginMenuItem *it;
    PluginEntry *e;

    if (menu_index < 0)
        return FALSE;
    if (menu_index >= g_menu_item_count)
    {
        if (plugin_bridge_is_active())
        {
            BOOL ok = plugin_bridge_exec_menu_item(menu_index - g_menu_item_count);
            if (ok)
                post_host_redraw(1);
            return ok;
        }
        return FALSE;
    }

    it = &g_menu_items[menu_index];
    if (it->filter_index >= 0 && it->filter_index < g_plugin_count)
    {
        e = &g_plugins[it->filter_index];
        if (e->stage2_initialized && e->runtime.func_WndProc)
        {
            if (e->runtime.func_WndProc(it->hwnd ? it->hwnd : e->hwnd,
                                        WM_FILTER_COMMAND, (WPARAM)it->cmd_id, 0,
                                        exfunc_get_editp(), &e->runtime))
            {
                post_host_redraw(1);
            }
            return TRUE;
        }
    }
    if (it->hwnd && IsWindow(it->hwnd))
    {
        SendMessageA(it->hwnd, WM_FILTER_COMMAND, (WPARAM)it->cmd_id, 0);
        return TRUE;
    }
    return FALSE;
}

BOOL plugin_manager_handle_command(HWND hwnd, int cmd_id)
{
    int idx;
    (void)hwnd;

    if (cmd_id >= PLUGIN_CMD_TOGGLE_BASE && cmd_id < PLUGIN_CMD_TOGGLE_BASE + 900)
    {
        idx = plugin_manager_find_by_cmd_slot(cmd_id - PLUGIN_CMD_TOGGLE_BASE);
        return (idx >= 0) ? plugin_manager_toggle_active(idx) : FALSE;
    }
    if (cmd_id >= PLUGIN_CMD_WINDOW_BASE && cmd_id < 11998)
    {
        idx = plugin_manager_find_by_cmd_slot(cmd_id - PLUGIN_CMD_WINDOW_BASE);
        return (idx >= 0) ? plugin_manager_toggle_window(idx) : FALSE;
    }
    if (cmd_id >= PLUGIN_CMD_CONFIG_ITEM_BASE &&
        cmd_id < PLUGIN_CMD_CONFIG_ITEM_BASE + g_config_cmd_count)
    {
        int ci = cmd_id - PLUGIN_CMD_CONFIG_ITEM_BASE;
        return plugin_manager_select_config_check(
            g_config_cmds[ci].filter_index, g_config_cmds[ci].check_idx);
    }
    if (cmd_id >= PLUGIN_CMD_IMPORT_BASE && cmd_id < PLUGIN_CMD_IMPORT_BASE + 900)
    {
        idx = plugin_manager_find_by_cmd_slot(cmd_id - PLUGIN_CMD_IMPORT_BASE);
        return (idx >= 0) ? plugin_manager_exec_import_export(idx, FALSE) : FALSE;
    }
    if (cmd_id >= PLUGIN_CMD_EXPORT_BASE && cmd_id < PLUGIN_CMD_EXPORT_BASE + 900)
    {
        idx = plugin_manager_find_by_cmd_slot(cmd_id - PLUGIN_CMD_EXPORT_BASE);
        return (idx >= 0) ? plugin_manager_exec_import_export(idx, TRUE) : FALSE;
    }
    if (cmd_id >= PLUGIN_CMD_MENU_ITEM_BASE &&
        cmd_id < PLUGIN_CMD_MENU_ITEM_BASE + plugin_manager_get_menu_item_count())
    {
        return plugin_manager_exec_menu_item_index(cmd_id - PLUGIN_CMD_MENU_ITEM_BASE);
    }
    return FALSE;
}

BOOL plugin_manager_handle_key(HWND hwnd, UINT vk, BOOL is_down, LPARAM key_lparam)
{
    int i;
    BOOL handled = FALSE;
    (void)hwnd;

    // forward WM_FILTER_MAIN_KEY_DOWN / UP to native MAIN_MESSAGE filters (0x00430490)
    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        UINT fmsg = is_down ? WM_FILTER_MAIN_KEY_DOWN : WM_FILTER_MAIN_KEY_UP;
        if (!e->stage2_initialized || !e->runtime.func_WndProc)
            continue;
        if (!(e->runtime.flag & FILTER_FLAG_MAIN_MESSAGE))
            continue;
        if (!(e->runtime.flag & FILTER_FLAG_WINDOW_DISP))
            continue;
        if (e->runtime.func_WndProc(e->hwnd, fmsg, (WPARAM)vk, key_lparam,
                                    exfunc_get_editp(), &e->runtime))
        {
            handled = TRUE;
        }
    }

    if (plugin_bridge_is_active())
    {
        if (plugin_bridge_forward_key(vk, is_down, key_lparam))
        {
            handled = TRUE;
        }
    }

    if (handled)
        post_host_redraw(1);
    return handled;
}

BOOL plugin_manager_forward_main_mouse(UINT filter_mouse_msg, WPARAM mk_flags,
                                       int img_x, int img_y)
{
    int i;
    BOOL need_redraw = FALSE;
    LPARAM lp = MAKELPARAM((short)img_x, (short)img_y);

    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        if (!e->stage2_initialized || !e->runtime.func_WndProc)
            continue;
        if (!(e->runtime.flag & FILTER_FLAG_MAIN_MESSAGE))
            continue;
        if (!(e->runtime.flag & FILTER_FLAG_WINDOW_DISP))
            continue;
        if (e->runtime.func_WndProc(e->hwnd, filter_mouse_msg, mk_flags, lp,
                                    exfunc_get_editp(), &e->runtime))
        {
            need_redraw = TRUE;
        }
    }

    if (plugin_bridge_is_active())
    {
        if (plugin_bridge_forward_mouse(filter_mouse_msg, mk_flags, img_x, img_y))
        {
            need_redraw = TRUE;
        }
    }

    if (need_redraw)
        post_host_redraw(1);
    return need_redraw;
}

BOOL plugin_manager_forward_main_misc(UINT filter_msg, WPARAM wp, LPARAM lp)
{
    int i;
    BOOL need_redraw = FALSE;

    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        if (!e->stage2_initialized || !e->runtime.func_WndProc)
            continue;
        if (!(e->runtime.flag & FILTER_FLAG_MAIN_MESSAGE))
            continue;
        if (!(e->runtime.flag & FILTER_FLAG_WINDOW_DISP))
            continue;
        if (e->runtime.func_WndProc(e->hwnd, filter_msg, wp, lp,
                                    exfunc_get_editp(), &e->runtime))
        {
            // match aviutl 0x004305a0: WM_FILTER_MAIN_CONTEXTMENU stops on first 1
            if (filter_msg == WM_FILTER_MAIN_CONTEXTMENU)
            {
                post_host_redraw(1);
                return TRUE;
            }
            need_redraw = TRUE;
        }
    }
    if (need_redraw)
        post_host_redraw(1);
    return need_redraw;
}

void plugin_manager_modify_title(EDITP *ep, int frame, char *title, int max_title)
{
    int i;
    if (!title || max_title <= 0)
        return;

    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        FILTER *fp = &e->runtime;
        if (!e->stage1_initialized)
            continue;
        if (!(fp->flag & (FILTER_FLAG_ACTIVE | FILTER_FLAG_ALWAYS_ACTIVE)))
            continue;
        if (!fp->func_modify_title)
            continue;
        fp->func_modify_title(fp, ep ? ep : exfunc_get_editp(), frame, title, max_title);
    }
}

int plugin_manager_apply_audio_filter(int filter_id, EDITP *ep, int frame_num,
                                      short *buf, int samples)
{
    PluginEntry *e;
    FILTER *fp;
    FILTER_PROC_INFO fpip;

    if (!ep || !buf || samples <= 0 || filter_id < 0 || filter_id >= g_plugin_count)
        return samples;

    e = &g_plugins[filter_id];
    fp = &e->runtime;
    if (!e->stage1_initialized || e->faulted || !fp->func_proc)
        return samples;
    if (!(fp->flag & (FILTER_FLAG_ACTIVE | FILTER_FLAG_ALWAYS_ACTIVE)))
        return samples;
    if (!(fp->flag & FILTER_FLAG_AUDIO_FILTER))
        return samples;

    ZeroMemory(&fpip, sizeof(fpip));
    fpip.ycp_edit = editp_get_ycp_edit(ep);
    fpip.ycp_temp = editp_get_ycp_temp(ep);
    editp_get_dimensions(ep, &fpip.w, &fpip.h, &fpip.max_w, &fpip.max_h);
    fpip.frame = frame_num;
    fpip.frame_n = editp_get_frame_n(ep);
    fpip.org_w = fpip.w;
    fpip.org_h = fpip.h;
    fpip.audiop = buf;
    fpip.audio_n = samples;
    fpip.audio_ch = editp_get_audio_ch(ep);
    fpip.editp = ep;
    fpip.yc_size = e->is_yuy2_filter ? 2 : (int)sizeof(PIXEL_YC);
    fpip.line_size = fpip.max_w * fpip.yc_size;

    safe_call_func_proc(e, &fpip);
    if (fpip.audio_n > 0)
    {
        samples = fpip.audio_n;
    }
    return samples;
}

int plugin_manager_apply_audio_filters(EDITP *ep, int frame_num, short *buf,
                                       int samples, const FILTER *stop_before_fp)
{
    int order[MAX_PLUGINS + BRIDGE_MAX_FILTERS];
    int k, cnt;

    if (!ep || !buf || samples <= 0 || !editp_is_editing(ep))
    {
        return samples;
    }

    profile_cfg_apply_for_frame(ep, frame_num);
    cnt = plugin_manager_get_execution_order(TRUE, order, MAX_PLUGINS + BRIDGE_MAX_FILTERS);
    for (k = 0; k < cnt; k++)
    {
        int idx = order[k];
        if (idx >= 0 && idx < g_plugin_count)
        {
            PluginEntry *e = &g_plugins[idx];
            FILTER *fp = &e->runtime;
            if (stop_before_fp && (fp == stop_before_fp ||
                                   (const FILTER *)e->src_dll == stop_before_fp))
            {
                break;
            }
            samples = plugin_manager_apply_audio_filter(idx, ep, frame_num, buf, samples);
        }
        else if (plugin_bridge_is_active())
        {
            int bidx = idx - g_plugin_count;
            int b_run[BRIDGE_MAX_FILTERS];
            int run_n = 0;
            int fl = plugin_bridge_get_filter_flags(bidx);

            if (!(fl & (FILTER_FLAG_ACTIVE | FILTER_FLAG_ALWAYS_ACTIVE)) ||
                !(fl & FILTER_FLAG_AUDIO_FILTER))
            {
                continue;
            }
            b_run[run_n++] = bidx;
            while (k + 1 < cnt && order[k + 1] >= g_plugin_count && run_n < BRIDGE_MAX_FILTERS)
            {
                int nb = order[k + 1] - g_plugin_count;
                int nfl = plugin_bridge_get_filter_flags(nb);
                k++;
                if ((nfl & (FILTER_FLAG_ACTIVE | FILTER_FLAG_ALWAYS_ACTIVE)) &&
                    (nfl & FILTER_FLAG_AUDIO_FILTER))
                {
                    b_run[run_n++] = nb;
                }
            }
            samples = plugin_bridge_render_audio_subset(
                ep, frame_num, buf, samples, editp_get_audio_ch(ep), b_run, run_n);
        }
    }
    return samples;
}

int plugin_manager_render_audio_frame(EDITP *ep, int frame_num, short *buf, BOOL nofilter)
{
    int samples, out_smp;

    if (!ep || !editp_is_editing(ep))
        return 0;
    if (!nofilter)
        profile_cfg_apply_for_frame(ep, frame_num);
    samples = read_source_audio_with_delay(ep, frame_num, buf, !nofilter);
    if (!buf || samples <= 0 || nofilter)
    {
        if (!nofilter && !editp_is_saving(ep))
            profile_cfg_restore_active();
        return samples;
    }
    out_smp = plugin_manager_apply_audio_filters(ep, frame_num, buf, samples, NULL);
    if (!editp_is_saving(ep))
        profile_cfg_restore_active();
    return out_smp;
}

BOOL plugin_manager_send_filter_command(int filter_index, int filter_cmd_id)
{
    PluginEntry *e;
    if (filter_index < 0)
        return FALSE;
    if (filter_index >= g_plugin_count)
    {
        if (plugin_bridge_is_active())
        {
            BOOL ok = plugin_bridge_send_filter_command(filter_index - g_plugin_count, filter_cmd_id);
            if (ok)
                post_host_redraw(1);
            return ok;
        }
        return FALSE;
    }
    e = &g_plugins[filter_index];
    if (!e->stage2_initialized || !e->runtime.func_WndProc)
        return FALSE;
    if (e->runtime.func_WndProc(e->hwnd, WM_FILTER_COMMAND, (WPARAM)filter_cmd_id, 0,
                                exfunc_get_editp(), &e->runtime))
    {
        post_host_redraw(1);
    }
    return TRUE;
}

BOOL plugin_manager_import_exo(int filter_index, const char *exo_path, EDITP *ep)
{
    char abs_exo[MAX_PATH];
    PluginEntry *e;

    if (!exo_path || !exo_path[0] || filter_index < 0)
        return FALSE;
    if (GetFullPathNameA(exo_path, MAX_PATH, abs_exo, NULL) == 0)
    {
        lstrcpynA(abs_exo, exo_path, MAX_PATH);
    }

    if (filter_index >= g_plugin_count)
    {
        if (plugin_bridge_is_active())
        {
            BOOL ok = plugin_bridge_import_exo(filter_index - g_plugin_count, abs_exo, ep);
            if (ok)
                post_host_redraw(2);
            return ok;
        }
        return FALSE;
    }

    e = &g_plugins[filter_index];
    if (!e->stage2_initialized || e->faulted || !e->runtime.func_WndProc)
        return FALSE;

    // exedit's func_WndProc handles (WM_COMMAND, 0x423, (LPARAM)exo_path) directly
    safe_call_wndproc(e, e->hwnd, WM_COMMAND, 0x423, (LPARAM)abs_exo,
                      ep ? ep : exfunc_get_editp(), "import_exo");
    if (e->faulted)
        return FALSE;
    post_host_redraw(2);
    return TRUE;
}

static BOOL g_suppress_bridge_file_notify = FALSE;

BOOL plugin_manager_load_aup(const char *aup_path, EDITP *ep)
{
    char abs_aup[MAX_PATH];
    EDITP *target_ep = ep ? ep : exfunc_get_editp();
    BOOL ok;

    if (!aup_path || !aup_path[0] || !target_ep)
        return FALSE;
    if (GetFullPathNameA(aup_path, MAX_PATH, abs_aup, NULL) == 0)
    {
        lstrcpynA(abs_aup, aup_path, MAX_PATH);
    }

    if (plugin_bridge_is_active())
        g_suppress_bridge_file_notify = TRUE;
    ok = aup_load_local(abs_aup, target_ep);
    g_suppress_bridge_file_notify = FALSE;

    if (ok && plugin_bridge_is_active())
    {
        plugin_bridge_load_aup(abs_aup, target_ep);
        plugin_bridge_sync_to_bridge(target_ep);
    }
    if (ok)
    {
        exfunc_invalidate_all_caches();
        if (plugin_bridge_is_active())
            plugin_bridge_invalidate_caches();
        post_host_redraw(2);
    }
    return ok;
}

BOOL plugin_manager_save_aup(const char *aup_path, EDITP *ep)
{
    char abs_aup[MAX_PATH];
    EDITP *target_ep = ep ? ep : exfunc_get_editp();

    if (!aup_path || !aup_path[0] || !target_ep || !editp_is_editing(target_ep))
    {
        return FALSE;
    }
    if (GetFullPathNameA(aup_path, MAX_PATH, abs_aup, NULL) == 0)
    {
        lstrcpynA(abs_aup, aup_path, MAX_PATH);
    }

    if (plugin_bridge_is_active())
    {
        editp_set_project_path(target_ep, abs_aup);
        plugin_bridge_sync_to_bridge(target_ep);
        if (plugin_bridge_save_aup(abs_aup, NULL))
        {
            aup_harvest_unmatched_chunks(abs_aup);
        }
    }
    return aup_save_local(abs_aup, target_ep);
}

void plugin_manager_notify_file_open(EDITP *ep)
{
    int i;
    exfunc_invalidate_all_caches();
    if (plugin_bridge_is_active() && !g_suppress_bridge_file_notify)
    {
        if (ep && editp_get_edit_path(ep)[0])
        {
            plugin_bridge_open_video(editp_get_edit_path(ep), ep);
        }
    }
    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        if (e->stage2_initialized && e->runtime.func_WndProc)
        {
            e->runtime.func_WndProc(e->hwnd, WM_FILTER_FILE_OPEN, 0, 0,
                                    ep ? ep : exfunc_get_editp(), &e->runtime);
        }
    }
}

void plugin_manager_notify_file_close(EDITP *ep)
{
    int i;
    exfunc_invalidate_all_caches();
    aup_clear_preserved_chunks();
    profile_cfg_clear_project_slots();
    if (plugin_bridge_is_active() && !g_suppress_bridge_file_notify)
    {
        plugin_bridge_close_video(ep);
    }
    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        if (e->stage2_initialized && e->runtime.func_WndProc)
        {
            e->runtime.func_WndProc(e->hwnd, WM_FILTER_FILE_CLOSE, 0, 0,
                                    ep ? ep : exfunc_get_editp(), &e->runtime);
        }
    }
}

void plugin_manager_notify_update(EDITP *ep)
{
    int i;
    if (plugin_bridge_is_active() && !g_suppress_bridge_file_notify)
    {
        plugin_bridge_sync_to_bridge(ep);
    }
    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        if (e->stage2_initialized && e->runtime.func_WndProc)
        {
            e->runtime.func_WndProc(e->hwnd, WM_FILTER_UPDATE, 0, 0,
                                    ep ? ep : exfunc_get_editp(), &e->runtime);
        }
    }
}

// 0x00430dc0, notify filters before export starts
void plugin_manager_notify_save_start(EDITP *ep, int s, int e, int out_flags)
{
    int i;
    EDITP *target_ep = ep ? ep : exfunc_get_editp();

    if (target_ep)
    {
        editp_set_saving(target_ep, TRUE);
    }
    if (plugin_bridge_is_active())
    {
        plugin_bridge_notify_save_start(target_ep, s, e, out_flags);
    }
    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *pe = &g_plugins[i];
        FILTER *fp = &pe->runtime;
        BOOL is_aud = (fp->flag & FILTER_FLAG_AUDIO_FILTER) != 0;
        if (!pe->stage1_initialized || pe->faulted || !fp->func_save_start)
            continue;
        if (((out_flags & 1) != 0 && !is_aud) || ((out_flags & 2) != 0 && is_aud))
        {
            safe_call_save_start(pe, s, e, target_ep);
        }
    }
    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *pe = &g_plugins[i];
        if (!pe->stage2_initialized || pe->faulted || !pe->runtime.func_WndProc)
            continue;
        if (pe->hwnd && IsWindow(pe->hwnd))
        {
            SendMessageA(pe->hwnd, WM_FILTER_SAVE_START, 0, 0);
        }
        else
        {
            safe_call_wndproc(pe, pe->hwnd, WM_FILTER_SAVE_START, 0, 0,
                              target_ep, "WM_FILTER_SAVE_START");
        }
    }
}

// 0x00430ed0, notify filters after export ends
void plugin_manager_notify_save_end(EDITP *ep, int out_flags)
{
    int i;
    EDITP *target_ep = ep ? ep : exfunc_get_editp();

    if (plugin_bridge_is_active())
    {
        plugin_bridge_notify_save_end(target_ep, out_flags);
    }
    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *pe = &g_plugins[i];
        if (!pe->stage2_initialized || pe->faulted || !pe->runtime.func_WndProc)
            continue;
        if (pe->hwnd && IsWindow(pe->hwnd))
        {
            SendMessageA(pe->hwnd, WM_FILTER_SAVE_END, 0, 0);
        }
        else
        {
            safe_call_wndproc(pe, pe->hwnd, WM_FILTER_SAVE_END, 0, 0,
                              target_ep, "WM_FILTER_SAVE_END");
        }
    }
    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *pe = &g_plugins[i];
        FILTER *fp = &pe->runtime;
        BOOL is_aud = (fp->flag & FILTER_FLAG_AUDIO_FILTER) != 0;
        if (!pe->stage1_initialized || pe->faulted || !fp->func_save_end)
            continue;
        if (((out_flags & 1) != 0 && !is_aud) || ((out_flags & 2) != 0 && is_aud))
        {
            safe_call_save_end(pe, target_ep);
        }
    }
    if (target_ep)
    {
        editp_set_saving(target_ep, FALSE);
    }
    profile_cfg_restore_active();
}

BOOL plugin_manager_has_active_audio_filter(void)
{
    int i, n = plugin_manager_get_count();
    for (i = 0; i < n; i++)
    {
        int fl = plugin_manager_get_flags(i);
        if ((fl & FILTER_FLAG_AUDIO_FILTER) &&
            (fl & (FILTER_FLAG_ACTIVE | FILTER_FLAG_ALWAYS_ACTIVE)))
        {
            return TRUE;
        }
    }
    return FALSE;
}

BOOL plugin_manager_sync_from_bridge(EDITP *ep)
{
    if (plugin_bridge_is_active())
    {
        return plugin_bridge_sync_from_bridge(ep);
    }
    return FALSE;
}

BOOL plugin_manager_render_video_frame(EDITP *ep, int frame_num)
{
    int i;
    if (!ep || !editp_is_editing(ep))
        return FALSE;
    if (!render_pipeline_range(ep, frame_num, NULL))
        return FALSE;

    // run active display filters (e.g. Histogram) after the video pipeline completes
    for (i = 0; i < g_plugin_count; i++)
    {
        PluginEntry *e = &g_plugins[i];
        FILTER *fp = &e->runtime;
        if (!e->stage1_initialized || e->faulted || !fp->func_proc)
            continue;
        if (!(fp->flag & FILTER_FLAG_DISP_FILTER))
            continue;
        if (!(fp->flag & FILTER_FLAG_WINDOW_DISP))
            continue;
        apply_single_native_video_entry(e, ep, frame_num);
    }
    if (!editp_is_saving(ep))
        profile_cfg_restore_active();
    return TRUE;
}

BOOL plugin_manager_apply_filter(int filter_id, void *editp_ptr)
{
    EDITP *ep = (EDITP *)editp_ptr;
    PluginEntry *e;

    if (!ep || filter_id < 0 || filter_id >= g_plugin_count)
        return FALSE;
    e = &g_plugins[filter_id];
    if (!e->stage1_initialized || e->faulted || !e->runtime.func_proc)
        return FALSE;
    if (e->runtime.flag & FILTER_FLAG_AUDIO_FILTER)
        return FALSE;

    apply_single_native_video_entry(e, ep, editp_get_frame(ep));
    return TRUE;
}

void plugin_manager_debug_dump(void)
{
    int i;
    dbg_plugin("loaded native filters: %d (native menu items: %d)", g_plugin_count, g_menu_item_count);
    for (i = 0; i < g_plugin_count; i++)
    {
        const PluginEntry *e = &g_plugins[i];
        const FILTER *fp = &e->runtime;
        dbg_plugin("  [%d] '%s' | HMODULE=0x%p FILTER=0x%p DLL_TBL=0x%p HWND=0x%p",
                   i, e->name, (void *)e->module, (void *)fp,
                   (void *)e->src_dll, (void *)e->hwnd);
        dbg_plugin("      flag=0x%08X (%s%s%s%s%s%s) tracks=%d checks=%d ex_data=%dB path='%s'",
                   (unsigned)fp->flag,
                   e->faulted ? "FAULTED " : "",
                   (fp->flag & FILTER_FLAG_ACTIVE) ? "ACTIVE " : "",
                   (fp->flag & FILTER_FLAG_ALWAYS_ACTIVE) ? "ALWAYS_ACTIVE " : "",
                   (fp->flag & FILTER_FLAG_WINDOW_DISP) ? "WIN_DISP " : "",
                   (fp->flag & FILTER_FLAG_AUDIO_FILTER) ? "AUDIO " : "",
                   (fp->flag & FILTER_FLAG_MAIN_MESSAGE) ? "MAIN_MSG " : "",
                   fp->track_n, fp->check_n, fp->ex_data_size, e->path);
    }

    if (plugin_bridge_is_active())
    {
        dbg_plugin("bridged 32-bit filters: %d", plugin_bridge_get_filter_count());
        plugin_bridge_debug_cmd("plugins");
    }
}
