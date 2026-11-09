#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <vfw.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "preview.h"
#include "i18n.h"
#include "aviutl_dlg.h"
#include "keycfg.h"
#include "winsnap.h"
#include "editp.h"
#include "exfunc.h"
#include "yc48_convert.h"
#include "plugin_manager.h"
#include "profile_cfg.h"
#include "plugin_bridge.h"
#include "builtin_filters.h"
#include "color_plugin.h"
#include "input_plugin.h"
#include "video_export.h"
#include "avi_tools.h"
#include "debug_console.h"

// shift-jis bytes for "拡張編集"
#define EXEDIT_SJIS_NAME "\x8a\x67\x92\xa3\x95\xd2\x8f\x57"

// main window class & style from aviutl 1.10 (WS_CLIPCHILDREN protects child trackbar/buttons)
#define AVIUTL_CLASS_STYLE (CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS | CS_BYTEALIGNCLIENT)
#define AVIUTL_WIN_STYLE (WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN)

#define IDC_NEW_EXEDIT_720P 40010
#define IDC_NEW_EXEDIT_1080 40011
#define IDC_TOGGLE_CONSOLE 40020
#define MRU_MAX_FILES 8

#define PLAY_TIMER_ID 1
#define AUDIO_PLAY_BUF_COUNT 4
#define AUDIO_PLAY_BUF_BYTES 16384

static const char *CLASS_NAME = "AviUtl";
static const char *IDLE_TITLE = "AviUtl";
static int g_priority_cmd = 1039;

static EDITP *g_editp = NULL;
static unsigned char *g_preview_bgr = NULL;
static size_t g_preview_bgr_cap = 0;
static short *g_audio_buf = NULL;
static int g_audio_buf_cap = 0;
static BOOL g_playing = FALSE;
static BOOL g_dragging_canvas = FALSE;
static BOOL g_peeking_unfiltered = FALSE;
static BOOL g_smoke_exit = FALSE;
static BOOL g_cli_no_confirm = FALSE;
static int g_last_auto_w = 0;
static int g_last_auto_h = 0;

// real-time waveOut audio playback ring buffer (0x00450820)
static HWAVEOUT g_waveout = NULL;
static WAVEHDR g_wave_hdrs[AUDIO_PLAY_BUF_COUNT];
static unsigned char *g_wave_mem = NULL;
static short *g_wave_frame_pcm = NULL;
static int g_wave_frame_cap = 0;
static int g_wave_frame_off = 0;
static int g_wave_frame_rem = 0;
static int g_play_start_frame = 0;
static int g_play_end_frame = 0;
static int g_play_audio_cursor = 0;
static int g_play_audio_rate = 44100;
static int g_play_audio_ch = 2;
static DWORD g_play_start_tick = 0;

// shift-jis (CP932) <-> UTF-16 helpers
static void sjis_to_wide_str(const char *src, wchar_t *dst, int dst_cch)
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

static void wide_to_sjis_str(const wchar_t *src, char *dst, int dst_cap)
{
    if (!dst || dst_cap <= 0)
        return;
    dst[0] = '\0';
    if (!src || !src[0])
        return;
    if (WideCharToMultiByte(932, 0, src, -1, dst, dst_cap, NULL, NULL) > 0)
        return;
    WideCharToMultiByte(CP_ACP, 0, src, -1, dst, dst_cap, NULL, NULL);
}

static void get_aviutl_ini_path(char *out_path, size_t out_sz)
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

static void get_resume_aup_path(char *out_path, size_t out_sz)
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
        _snprintf(out_path, out_sz, "%saviutl.aup", mod_path);
        out_path[out_sz - 1] = '\0';
    }
    else
    {
        lstrcpynA(out_path, ".\\aviutl.aup", (int)out_sz);
    }
}

static const char *gui_path_ext_dbcs(const char *path)
{
    const char *dot = NULL;
    const unsigned char *p = (const unsigned char *)path;
    if (!path)
        return "";
    while (*p)
    {
        if (IsDBCSLeadByteEx(932, *p) && p[1])
        {
            p += 2;
            continue;
        }
        if (*p == '\\' || *p == '/')
            dot = NULL;
        else if (*p == '.')
            dot = (const char *)p;
        p++;
    }
    return dot ? dot : "";
}

static BOOL is_pure_audio_ext(const char *ext)
{
    if (!ext || !ext[0])
        return FALSE;
    return lstrcmpiA(ext, ".wav") == 0 ||
           lstrcmpiA(ext, ".mp3") == 0 ||
           lstrcmpiA(ext, ".mp2") == 0 ||
           lstrcmpiA(ext, ".mpa") == 0 ||
           lstrcmpiA(ext, ".aac") == 0 ||
           lstrcmpiA(ext, ".m4a") == 0 ||
           lstrcmpiA(ext, ".flac") == 0 ||
           lstrcmpiA(ext, ".ogg") == 0 ||
           lstrcmpiA(ext, ".opus") == 0 ||
           lstrcmpiA(ext, ".wma") == 0 ||
           lstrcmpiA(ext, ".ac3") == 0;
}

static void ini_write_int(const char *section, const char *key, int val, const char *ini_path)
{
    char buf[32];
    wsprintfA(buf, "%d", val);
    WritePrivateProfileStringA(section, key, buf, ini_path);
}

static void apply_priority_command(int cmd_id, BOOL save_ini)
{
    DWORD cls = NORMAL_PRIORITY_CLASS;
    switch (cmd_id)
    {
    case 1037:
        cls = IDLE_PRIORITY_CLASS;
        break;
    case 1038:
        cls = BELOW_NORMAL_PRIORITY_CLASS;
        break;
    case 1039:
        cls = NORMAL_PRIORITY_CLASS;
        break;
    case 1040:
        cls = ABOVE_NORMAL_PRIORITY_CLASS;
        break;
    case 1041:
        cls = HIGH_PRIORITY_CLASS;
        break;
    case 1042:
        cls = REALTIME_PRIORITY_CLASS;
        break;
    default:
        cmd_id = 1039;
        cls = NORMAL_PRIORITY_CLASS;
        break;
    }
    g_priority_cmd = cmd_id;
    SetPriorityClass(GetCurrentProcess(), cls);
    if (save_ini)
    {
        char ini[MAX_PATH];
        get_aviutl_ini_path(ini, sizeof(ini));
        ini_write_int("system", "priority", cmd_id - 1037, ini);
    }
}

static void mru_record_file(const char *path)
{
    char ini[MAX_PATH];
    char abs_path[MAX_PATH];
    char entries[MRU_MAX_FILES][MAX_PATH];
    int count = 0, i;
    const char *ext;

    if (!path || !path[0])
        return;
    ext = strrchr(path, '.');
    if (ext && lstrcmpiA(ext, ".exedit") == 0)
        return;

    if (GetFullPathNameA(path, MAX_PATH, abs_path, NULL) == 0)
    {
        lstrcpynA(abs_path, path, MAX_PATH);
    }
    get_aviutl_ini_path(ini, sizeof(ini));

    lstrcpynA(entries[count++], abs_path, MAX_PATH);
    for (i = 0; i < MRU_MAX_FILES && count < MRU_MAX_FILES; i++)
    {
        char key[16], cur[MAX_PATH];
        wsprintfA(key, "file%d", i);
        GetPrivateProfileStringA("recent", key, "", cur, MAX_PATH, ini);
        if (cur[0] && lstrcmpiA(cur, abs_path) != 0)
        {
            lstrcpynA(entries[count++], cur, MAX_PATH);
        }
    }
    for (i = 0; i < MRU_MAX_FILES; i++)
    {
        char key[16];
        wsprintfA(key, "file%d", i);
        WritePrivateProfileStringA("recent", key, (i < count) ? entries[i] : "", ini);
    }
}

static void mru_clear_files(void)
{
    char ini[MAX_PATH];
    int i;
    get_aviutl_ini_path(ini, sizeof(ini));
    for (i = 0; i < MRU_MAX_FILES; i++)
    {
        char key[16];
        wsprintfA(key, "file%d", i);
        WritePrivateProfileStringA("recent", key, "", ini);
    }
}

// DBCS-aware path basename (0x0042c3d0)
static const char *aviutl_basename(const char *path)
{
    const char *base;
    const unsigned char *p;
    if (!path)
        return "";
    base = path;
    p = (const unsigned char *)path;
    while (*p)
    {
        if (IsDBCSLeadByte(*p))
        {
            if (p[1])
                p += 2;
            else
                break;
        }
        else
        {
            if (*p == '\\' || *p == '/' || *p == ':')
            {
                base = (const char *)(p + 1);
            }
            p++;
        }
    }
    return base;
}

// aviutl 1.10 window title formatting (0x0041c640)
static void update_window_title(HWND hwnd)
{
    char title[1024];
    wchar_t wtitle[1024];

    if (g_editp && editp_is_editing(g_editp) && editp_get_frame_n(g_editp) > 0)
    {
        int w = 0, h = 0, mw = 0, mh = 0;
        int rate = 30, scale = 1;
        int cur = editp_get_frame(g_editp);
        int fn = editp_get_frame_n(g_editp);
        const AviUtlSystemConfig *scfg = preview_get_sys_config();
        uint32_t pflags = preview_get_flags();
        int sf = scfg ? scfg->startframe : 1;
        const char *base = aviutl_basename(editp_get_edit_path(g_editp));
        BYTE fst = 0;
        int pos;

        editp_get_dimensions(g_editp, &w, &h, &mw, &mh);
        editp_get_fps(g_editp, &rate, &scale);

        // note two spaces before '[' in 0x0041c640: "%s (%d,%d) [%d/%d]"
        pos = wsprintfA(title, "%s (%d,%d)  [%d/%d]",
                        base[0] ? base : "Untitled",
                        w, h,
                        cur + sf,
                        (fn - 1) + sf);

        if ((pflags & PREVIEW_FLAG_AVIKEYFRAME) && editp_is_source_keyframe(g_editp, cur))
        {
            title[pos++] = '*';
            title[pos] = '\0';
        }
        if ((pflags & PREVIEW_FLAG_RECOMPRESSFRAME) && !editp_is_source_keyframe(g_editp, cur))
        {
            title[pos++] = '!';
            title[pos] = '\0';
        }
        if ((pflags & PREVIEW_FLAG_TIME) && rate > 0 && scale > 0)
        {
            int cs = (int)(((double)scale * (double)cur * 100.0) / (double)rate);
            if (cs < 0)
                cs = 0;
            pos += wsprintfA(title + pos, " [%02d:%02d:%02d.%02d]",
                             cs / 360000, (cs / 6000) % 60, (cs / 100) % 60, cs % 100);
        }
        if (editp_get_frame_status(g_editp, cur, &fst) && (fst & 0x0f))
        {
            pos += wsprintfA(title + pos, "  %s%s%s%s",
                             (fst & EDIT_FRAME_EDIT_FLAG_KEYFRAME) ? "[key]" : "",
                             (fst & EDIT_FRAME_EDIT_FLAG_MARKFRAME) ? "[mark]" : "",
                             (fst & EDIT_FRAME_EDIT_FLAG_DELFRAME) ? "[del]" : "",
                             (fst & EDIT_FRAME_EDIT_FLAG_NULLFRAME) ? "[copy]" : "");
        }

        plugin_manager_modify_title(g_editp, cur, title, (int)sizeof(title));
    }
    else
    {
        lstrcpynA(title, IDLE_TITLE, sizeof(title));
    }

    sjis_to_wide_str(title, wtitle, 1024);
    SetWindowTextW(hwnd, wtitle);
}

static void refresh_current_frame(HWND hwnd)
{
    int w = 0, h = 0, max_w = 0, max_h = 0, stride, cur_f;
    BOOL nofilter;
    size_t need;
    const short *wave_pcm = NULL;
    int wave_shorts = 0;

    if (!g_editp || !editp_is_editing(g_editp) || editp_get_frame_n(g_editp) <= 0)
    {
        g_last_auto_w = 0;
        g_last_auto_h = 0;
        preview_set_frame_ex(g_editp, 0, 0, NULL, NULL, 0);
        update_window_title(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }

    cur_f = editp_get_frame(g_editp);
    nofilter = preview_is_effectively_nofilter(g_editp);

    if (nofilter)
    {
        editp_load_frame(g_editp, cur_f);
    }
    else if (!plugin_manager_render_video_frame(g_editp, cur_f))
    {
        editp_load_frame(g_editp, cur_f);
    }

    editp_get_dimensions(g_editp, &w, &h, &max_w, &max_h);
    if (w <= 0 || h <= 0)
    {
        preview_set_frame_ex(g_editp, 0, 0, NULL, NULL, 0);
        update_window_title(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }

    // auto-resize window if project/filter dimensions changed (e.g. exedit New Project)
    if (w != g_last_auto_w || h != g_last_auto_h)
    {
        g_last_auto_w = w;
        g_last_auto_h = h;
        preview_auto_resize(hwnd, g_editp, FALSE);
    }

    stride = (w * 3 + 3) & ~3;
    need = (size_t)stride * (size_t)h;
    if (need > g_preview_bgr_cap || !g_preview_bgr)
    {
        unsigned char *nb = (unsigned char *)realloc(g_preview_bgr, need);
        if (!nb)
            return;
        g_preview_bgr = nb;
        g_preview_bgr_cap = need;
    }

    yc2rgb_dib24(g_preview_bgr, stride, editp_get_ycp_edit(g_editp),
                 max_w, w, h, TRUE);
    editp_set_display_size(g_editp, w, h);

    // decode/mix current frame PCM16 audio when waveform display is enabled
    if ((preview_get_flags() & PREVIEW_FLAG_AUDIO) && editp_has_audio(g_editp))
    {
        int ch = editp_get_audio_ch(g_editp);
        int req = editp_read_audio_frame(g_editp, cur_f, NULL);
        if (ch < 1)
            ch = 2;
        if (req > 0)
        {
            int need_shorts = (req + 64) * ch;
            if (need_shorts > g_audio_buf_cap || !g_audio_buf)
            {
                short *ab = (short *)realloc(g_audio_buf, (size_t)need_shorts * sizeof(short));
                if (ab)
                {
                    g_audio_buf = ab;
                    g_audio_buf_cap = need_shorts;
                }
            }
            if (g_audio_buf && need_shorts <= g_audio_buf_cap)
            {
                int got = plugin_manager_render_audio_frame(g_editp, cur_f, g_audio_buf, nofilter);
                if (got > 0)
                {
                    wave_pcm = g_audio_buf;
                    wave_shorts = got * ch;
                }
            }
        }
    }

    preview_set_frame_ex(g_editp, w, h, g_preview_bgr, wave_pcm, wave_shorts);
    update_window_title(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void cb_export_preview_update(HWND hwnd)
{
    int w = 0, h = 0, mw = 0, mh = 0, stride;
    size_t need;
    if (!hwnd || !IsWindow(hwnd) || !g_editp || !editp_is_editing(g_editp))
        return;
    if (!(preview_get_flags() & PREVIEW_FLAG_PREVIEW))
        return;

    editp_get_dimensions(g_editp, &w, &h, &mw, &mh);
    if (w <= 0 || h <= 0)
        return;
    stride = (w * 3 + 3) & ~3;
    need = (size_t)stride * (size_t)h;
    if (need > g_preview_bgr_cap || !g_preview_bgr)
    {
        unsigned char *nb = (unsigned char *)realloc(g_preview_bgr, need);
        if (!nb)
            return;
        g_preview_bgr = nb;
        g_preview_bgr_cap = need;
    }
    yc2rgb_dib24(g_preview_bgr, stride, editp_get_ycp_edit(g_editp), mw, w, h, TRUE);
    preview_set_frame_ex(g_editp, w, h, g_preview_bgr, NULL, 0);
    InvalidateRect(hwnd, NULL, FALSE);
    UpdateWindow(hwnd);
}

static void cb_export_progress(HWND hwnd, int now, int total)
{
    char title[512];
    wchar_t wtitle[512];
    int pct10 = 0;

    if (!hwnd || !IsWindow(hwnd) || total <= 0)
        return;
    pct10 = (int)(((long long)now * 1000LL) / (long long)total);
    if (pct10 < 0)
        pct10 = 0;
    if (pct10 > 1000)
        pct10 = 1000;

    if (g_editp && editp_is_editing(g_editp))
    {
        int w = 0, h = 0, mw = 0, mh = 0;
        const char *base = aviutl_basename(editp_get_output_path(g_editp));
        if (!base[0])
            base = aviutl_basename(editp_get_edit_path(g_editp));
        editp_get_dimensions(g_editp, &w, &h, &mw, &mh);
        wsprintfA(title, "[%d.%d%%] %s (%d,%d)  [%d/%d]",
                  pct10 / 10, pct10 % 10,
                  base[0] ? base : "AviUtl", w, h, now, total);
    }
    else
    {
        wsprintfA(title, "[%d.%d%%] AviUtl  [%d/%d]",
                  pct10 / 10, pct10 % 10, now, total);
    }
    sjis_to_wide_str(title, wtitle, 512);
    SetWindowTextW(hwnd, wtitle);
}

static void cleanup_menu_separators(HMENU sub)
{
    int i;
    BOOL prev_sep = TRUE;

    if (!sub)
        return;
    for (i = 0; i < GetMenuItemCount(sub);)
    {
        MENUITEMINFOW mii;
        ZeroMemory(&mii, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_FTYPE;
        if (GetMenuItemInfoW(sub, (UINT)i, TRUE, &mii) && (mii.fType & MFT_SEPARATOR))
        {
            if (prev_sep || i == GetMenuItemCount(sub) - 1)
            {
                DeleteMenu(sub, (UINT)i, MF_BYPOSITION);
                continue;
            }
            prev_sep = TRUE;
        }
        else
        {
            prev_sep = FALSE;
        }
        i++;
    }
    while (GetMenuItemCount(sub) > 0)
    {
        int last = GetMenuItemCount(sub) - 1;
        MENUITEMINFOW mii;
        ZeroMemory(&mii, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_FTYPE;
        if (GetMenuItemInfoW(sub, (UINT)last, TRUE, &mii) && (mii.fType & MFT_SEPARATOR))
        {
            DeleteMenu(sub, (UINT)last, MF_BYPOSITION);
        }
        else
        {
            break;
        }
    }
}

// 0x0042c930 & 0x00423330: populate stock "AVIUTL" menu bar
static void apply_menu(HWND hwnd)
{
    HMENU bar = i18n_load_menu("AVIUTL");
    HMENU file_sub, flt_sub, set_sub, edit_sub, prof_sub, view_sub;
    const wchar_t *w_none = i18n_get_wstr(12);
    const wchar_t *w_default = i18n_get_wstr(10);
    const wchar_t *w_cfg_suffix = i18n_get_wstr(107);
    const wchar_t *w_disp_suffix = i18n_get_wstr(108);
    int i, fcount, mcount;

    if (!bar)
        return;

    file_sub = GetSubMenu(bar, 0);
    flt_sub = GetSubMenu(bar, 1);
    set_sub = GetSubMenu(bar, 2);
    edit_sub = GetSubMenu(bar, 3);
    prof_sub = GetSubMenu(bar, 4);
    view_sub = GetSubMenu(bar, 5);

    // [ファイル] -> [プラグイン出力] (5395)
    video_export_populate_menu(bar, w_none[0] ? w_none : L"\x306a\x3057");

    // [ファイル] -> [最近使ったファイル] (5596..5603)
    {
        char ini[MAX_PATH];
        int added_mru = 0;
        get_aviutl_ini_path(ini, sizeof(ini));
        for (i = 0; i < MRU_MAX_FILES; i++)
        {
            char key[16], mpath[MAX_PATH];
            wchar_t wmpath[MAX_PATH], wlabel[MAX_PATH + 16];
            wsprintfA(key, "file%d", i);
            GetPrivateProfileStringA("recent", key, "", mpath, MAX_PATH, ini);
            if (!mpath[0])
                continue;
            sjis_to_wide_str(mpath, wmpath, MAX_PATH);
            wsprintfW(wlabel, L"&%d %s", added_mru + 1, wmpath);
            InsertMenuW(bar, 5596, MF_BYCOMMAND | MF_STRING,
                        (UINT_PTR)(5596 + added_mru), wlabel);
            added_mru++;
        }
        if (added_mru > 0)
        {
            DeleteMenu(bar, 5596, MF_BYCOMMAND);
        }
        else
        {
            ModifyMenuW(bar, 5596, MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                        5596, w_none[0] ? w_none : L"\x306a\x3057");
        }
    }

    // [ファイル] -> [環境設定] -> [優先度] (1037..1042) & [入力プラグインの設定] (5595)
    CheckMenuRadioItem(bar, 1037, 1042, (UINT)g_priority_cmd, MF_BYCOMMAND);
    {
        int icount = input_plugin_get_count();
        int added_inp = 0;
        for (i = 0; i < icount; i++)
        {
            InputPluginSummary isum;
            wchar_t wnm[256];
            if (!input_plugin_get_summary(i, &isum) || !isum.has_config)
                continue;
            sjis_to_wide_str(isum.name[0] ? isum.name : "Input", wnm, 256);
            InsertMenuW(bar, 5595, MF_BYCOMMAND | MF_STRING,
                        (UINT_PTR)(INPUT_PLUGIN_CMD_CFG_BASE + i), wnm);
            added_inp++;
        }
        if (added_inp > 0)
        {
            DeleteMenu(bar, 5595, MF_BYCOMMAND);
        }
        else
        {
            ModifyMenuW(bar, 5595, MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                        5595, w_none[0] ? w_none : L"\x306a\x3057");
        }
    }

    // [フィルタ], [設定], [表示], [インポート], [エクスポート] (0x0042c930)
    plugin_manager_clear_config_cmd_map();
    builtin_filters_reload_dynamic_lists();
    fcount = plugin_manager_get_count();
    {
        HMENU cur_cfg_popup = NULL;
        HMENU interlace_cfg_popup = NULL;

        for (i = 0; i < fcount; i++)
        {
            int flags = plugin_manager_get_flags(i);
            int slot = plugin_manager_get_cmd_slot(i);
            const char *nm = plugin_manager_get_name(i);
            HWND fhwnd = plugin_manager_get_window(i);
            wchar_t wnm[256], witem[300];
            BOOL is_audio = (flags & FILTER_FLAG_AUDIO_FILTER) != 0;
            BOOL is_disp = (flags & FILTER_FLAG_DISP_FILTER) != 0;
            BOOL is_interlace = (flags & FILTER_FLAG_INTERLACE_FILTER) != 0;
            BOOL is_cfg_menu = (flags & (FILTER_FLAG_CONFIG_POPUP | FILTER_FLAG_CONFIG_CHECK |
                                         FILTER_FLAG_CONFIG_RADIO | BUILTIN_FLAG_POPUP_APPEND)) != 0;

            sjis_to_wide_str((nm && nm[0]) ? nm : "Filter", wnm, 256);

            if (slot >= 0 &&
                !(flags & FILTER_FLAG_ALWAYS_ACTIVE) &&
                !is_disp && !is_interlace && !is_cfg_menu)
            {
                UINT ufl = MF_BYCOMMAND | MF_STRING |
                           ((flags & FILTER_FLAG_ACTIVE) ? MF_CHECKED : MF_UNCHECKED);
                InsertMenuW(bar, is_audio ? 10999u : 10000u, ufl,
                            (UINT_PTR)(PLUGIN_CMD_TOGGLE_BASE + slot), wnm);
            }

            if (!(flags & FILTER_FLAG_NO_CONFIG))
            {
                BOOL vis = ((flags & FILTER_FLAG_WINDOW_DISP) != 0) ||
                           (fhwnd && IsWindowVisible(fhwnd));
                UINT ufl = MF_STRING | (vis ? MF_CHECKED : MF_UNCHECKED);

                if (is_disp)
                {
                    if (slot >= 0)
                    {
                        wsprintfW(witem, L"%s%s", wnm,
                                  (flags & BUILTIN_FLAG_NO_CFG_SUFFIX) ? L"" : w_disp_suffix);
                        InsertMenuW(bar, 10998u, MF_BYCOMMAND | ufl,
                                    (UINT_PTR)(PLUGIN_CMD_WINDOW_BASE + slot), witem);
                    }
                }
                else if (is_cfg_menu)
                {
                    int c, ccount = plugin_manager_get_config_check_count(i);
                    BOOL use_popup = (flags & (FILTER_FLAG_CONFIG_POPUP | BUILTIN_FLAG_POPUP_APPEND)) != 0;

                    if ((flags & FILTER_FLAG_CONFIG_POPUP) && !(flags & BUILTIN_FLAG_POPUP_APPEND))
                    {
                        cur_cfg_popup = CreatePopupMenu();
                        wsprintfW(witem, L"%s%s", wnm,
                                  (flags & BUILTIN_FLAG_NO_CFG_SUFFIX) ? L"" : w_cfg_suffix);
                        InsertMenuW(bar, 11998u, MF_BYCOMMAND | MF_POPUP,
                                    (UINT_PTR)cur_cfg_popup, witem);
                        if (i == 17 && plugin_manager_is_builtin(i))
                        {
                            interlace_cfg_popup = cur_cfg_popup;
                        }
                    }

                    for (c = 0; c < ccount; c++)
                    {
                        const char *cname = plugin_manager_get_config_check_name(i, c);
                        int cdef = plugin_manager_get_config_check_default(i, c);
                        int cval = plugin_manager_get_config_check_value(i, c);
                        wchar_t wcname[256];

                        sjis_to_wide_str(cname ? cname : "", wcname, 256);
                        if (cdef < 0)
                        {
                            if (use_popup && cur_cfg_popup)
                            {
                                AppendMenuW(cur_cfg_popup, MF_STRING | MF_GRAYED | MF_DISABLED,
                                            0, wcname);
                            }
                            else
                            {
                                InsertMenuW(bar, 11998u,
                                            MF_BYCOMMAND | MF_STRING | MF_GRAYED | MF_DISABLED,
                                            0, wcname);
                            }
                        }
                        else
                        {
                            int cmd_id = plugin_manager_register_config_cmd(i, c);
                            UINT cfl = MF_STRING | (cval ? MF_CHECKED : MF_UNCHECKED);
                            if (cmd_id > 0)
                            {
                                if (use_popup && cur_cfg_popup)
                                {
                                    AppendMenuW(cur_cfg_popup, cfl, (UINT_PTR)cmd_id, wcname);
                                }
                                else
                                {
                                    InsertMenuW(bar, 11998u, MF_BYCOMMAND | cfl,
                                                (UINT_PTR)cmd_id, wcname);
                                }
                            }
                        }
                    }

                    if ((flags & BUILTIN_FLAG_POPUP_SEP) && use_popup && cur_cfg_popup)
                    {
                        AppendMenuW(cur_cfg_popup, MF_SEPARATOR, 0, NULL);
                    }
                }
                else if (is_interlace)
                {
                    if (slot >= 0)
                    {
                        wsprintfW(witem, L"%s%s", wnm, w_cfg_suffix);
                        if (interlace_cfg_popup)
                        {
                            AppendMenuW(interlace_cfg_popup, ufl,
                                        (UINT_PTR)(PLUGIN_CMD_WINDOW_BASE + slot), witem);
                        }
                        else
                        {
                            InsertMenuW(bar, 11000u, MF_BYCOMMAND | ufl,
                                        (UINT_PTR)(PLUGIN_CMD_WINDOW_BASE + slot), witem);
                        }
                    }
                }
                else if (slot >= 0)
                {
                    wsprintfW(witem, L"%s%s", wnm,
                              (flags & BUILTIN_FLAG_NO_CFG_SUFFIX) ? L"" : w_cfg_suffix);
                    InsertMenuW(bar, is_audio ? 11999u : 11000u, MF_BYCOMMAND | ufl,
                                (UINT_PTR)(PLUGIN_CMD_WINDOW_BASE + slot), witem);
                }
            }

            if ((flags & FILTER_FLAG_IMPORT) && slot >= 0)
            {
                InsertMenuW(bar, 14000u, MF_BYCOMMAND | MF_STRING,
                            (UINT_PTR)(PLUGIN_CMD_IMPORT_BASE + slot), wnm);
            }
            if ((flags & FILTER_FLAG_EXPORT) && slot >= 0)
            {
                InsertMenuW(bar, 15000u, MF_BYCOMMAND | MF_STRING,
                            (UINT_PTR)(PLUGIN_CMD_EXPORT_BASE + slot), wnm);
            }
        }
    }

    DeleteMenu(bar, 10000, MF_BYCOMMAND);
    DeleteMenu(bar, 10999, MF_BYCOMMAND);
    DeleteMenu(bar, 11000, MF_BYCOMMAND);
    DeleteMenu(bar, 11999, MF_BYCOMMAND);
    DeleteMenu(bar, 11998, MF_BYCOMMAND);
    DeleteMenu(bar, 10998, MF_BYCOMMAND);
    DeleteMenu(bar, 14000, MF_BYCOMMAND);
    DeleteMenu(bar, 15000, MF_BYCOMMAND);

    cleanup_menu_separators(file_sub);
    cleanup_menu_separators(flt_sub);
    cleanup_menu_separators(set_sub);
    cleanup_menu_separators(view_sub);

    // [編集] -> plugin add_menu_item submenus (0x00423330)
    mcount = plugin_manager_get_menu_item_count();
    if (edit_sub && mcount > 0)
    {
        BOOL sep_added = FALSE;
        for (i = 0; i < fcount; i++)
        {
            HMENU fsub = NULL;
            int m;
            for (m = 0; m < mcount; m++)
            {
                PluginMenuItem it;
                wchar_t wlabel[256];
                if (!plugin_manager_get_menu_item(m, &it) || it.filter_index != i)
                    continue;
                if (!fsub)
                {
                    wchar_t wfn[256];
                    const char *fn = plugin_manager_get_name(i);
                    sjis_to_wide_str((fn && fn[0]) ? fn : "Plugin", wfn, 256);
                    fsub = CreatePopupMenu();
                    if (!sep_added)
                    {
                        AppendMenuW(edit_sub, MF_SEPARATOR, 0, NULL);
                        sep_added = TRUE;
                    }
                    AppendMenuW(edit_sub, MF_POPUP, (UINT_PTR)fsub, wfn);
                }
                sjis_to_wide_str(it.name, wlabel, 256);
                AppendMenuW(fsub, MF_STRING, (UINT_PTR)(PLUGIN_CMD_MENU_ITEM_BASE + m), wlabel);
            }
        }
    }

    // [編集] -> [現在のフレームのプロファイル] (5207..5215)
    for (i = 1; i <= 9; i++)
    {
        if (profile_cfg_has_project_slot(i))
        {
            const char *snm = profile_cfg_get_slot_name(i);
            wchar_t wsnm[260], wlbl[300];
            sjis_to_wide_str((snm && snm[0]) ? snm : "Profile", wsnm, 260);
            wsprintfW(wlbl, L"%s (%d)", wsnm, i);
            ModifyMenuW(bar, (UINT)(5206 + i), MF_BYCOMMAND | MF_STRING,
                        (UINT_PTR)(5206 + i), wlbl);
        }
    }

    // [プロファイル] -> 8006+ (0x0041465d)
    if (prof_sub)
    {
        int pcount = profile_cfg_get_count();
        int pactive = profile_cfg_get_active();
        for (i = 0; i < pcount; i++)
        {
            const char *pnm = profile_cfg_get_name(i);
            wchar_t wpnm[260];
            UINT pfl = MF_BYCOMMAND | MF_STRING |
                       ((i == pactive) ? MF_CHECKED : MF_UNCHECKED);
            if (i == 0 && (!pnm || !pnm[0]))
            {
                lstrcpynW(wpnm, w_default[0] ? w_default : L"\x30c7\x30d5\x30a9\x30eb\x30c8", 260);
            }
            else
            {
                sjis_to_wide_str((pnm && pnm[0]) ? pnm : "Default", wpnm, 260);
            }
            InsertMenuW(prof_sub, 8005, pfl,
                        (UINT_PTR)(PROFILE_CMD_SELECT_BASE + i), wpnm);
        }
        DeleteMenu(prof_sub, 8005, MF_BYCOMMAND);
        EnableMenuItem(prof_sub, PROFILE_CMD_DELETE,
                       MF_BYCOMMAND | ((pactive > 0) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
    }

    {
        HMENU old = GetMenu(hwnd);
        SetMenu(hwnd, bar);
        if (old)
            DestroyMenu(old);
    }
    preview_sync_menu_checks(hwnd);
    DrawMenuBar(hwnd);
    builtin_filters_notify_menu_rebuilt(hwnd);
}

static BOOL fill_and_submit_wave_hdr(WAVEHDR *wh)
{
    int filled = 0;

    if (!g_playing || !g_waveout || !wh || !g_editp || !editp_is_editing(g_editp))
    {
        return FALSE;
    }

    while (filled < AUDIO_PLAY_BUF_BYTES)
    {
        if (g_wave_frame_rem <= 0)
        {
            if (g_play_audio_cursor > g_play_end_frame)
            {
                memset(wh->lpData + filled, 0, (size_t)(AUDIO_PLAY_BUF_BYTES - filled));
                filled = AUDIO_PLAY_BUF_BYTES;
                break;
            }
            else
            {
                int f = g_play_audio_cursor++;
                BOOL noflt = preview_is_effectively_nofilter(g_editp);
                int need_smp = editp_read_audio_frame(g_editp, f, NULL);
                int ch = g_play_audio_ch > 0 ? g_play_audio_ch : 2;
                int need_sh = (need_smp > 0 ? need_smp : 2048) * ch + 128;

                if (need_sh > g_wave_frame_cap || !g_wave_frame_pcm)
                {
                    short *nb = (short *)realloc(g_wave_frame_pcm,
                                                 (size_t)need_sh * sizeof(short));
                    if (!nb)
                    {
                        memset(wh->lpData + filled, 0, (size_t)(AUDIO_PLAY_BUF_BYTES - filled));
                        filled = AUDIO_PLAY_BUF_BYTES;
                        break;
                    }
                    g_wave_frame_pcm = nb;
                    g_wave_frame_cap = need_sh;
                }

                if (need_smp > 0)
                {
                    int got = plugin_manager_render_audio_frame(
                        g_editp, f, g_wave_frame_pcm, noflt);
                    if (got <= 0)
                    {
                        memset(g_wave_frame_pcm, 0,
                               (size_t)need_smp * (size_t)ch * sizeof(short));
                        got = need_smp;
                    }
                    g_wave_frame_off = 0;
                    g_wave_frame_rem = got * ch * (int)sizeof(short);
                }
                else
                {
                    int fallback_bytes = 1024 * ch * (int)sizeof(short);
                    memset(g_wave_frame_pcm, 0, (size_t)fallback_bytes);
                    g_wave_frame_off = 0;
                    g_wave_frame_rem = fallback_bytes;
                }
            }
        }

        if (g_wave_frame_rem > 0)
        {
            int chunk = AUDIO_PLAY_BUF_BYTES - filled;
            if (chunk > g_wave_frame_rem)
                chunk = g_wave_frame_rem;
            memcpy(wh->lpData + filled,
                   ((const unsigned char *)g_wave_frame_pcm) + g_wave_frame_off,
                   (size_t)chunk);
            filled += chunk;
            g_wave_frame_off += chunk;
            g_wave_frame_rem -= chunk;
        }
    }

    wh->dwBufferLength = AUDIO_PLAY_BUF_BYTES;
    return waveOutWrite(g_waveout, wh, sizeof(WAVEHDR)) == MMSYSERR_NOERROR;
}

// 0x004507c0, stop & tear down waveOut + playback timer
static void stop_playback(HWND hwnd)
{
    BOOL was_playing = g_playing;
    g_playing = FALSE;

    builtin_filters_stop_playback();

    if (was_playing)
    {
        KillTimer(hwnd, PLAY_TIMER_ID);
    }
    if (g_waveout)
    {
        HWAVEOUT hwo = g_waveout;
        int i;
        g_waveout = NULL;
        waveOutReset(hwo);
        for (i = 0; i < AUDIO_PLAY_BUF_COUNT; i++)
        {
            if (g_wave_hdrs[i].dwFlags & WHDR_PREPARED)
            {
                waveOutUnprepareHeader(hwo, &g_wave_hdrs[i], sizeof(WAVEHDR));
            }
            ZeroMemory(&g_wave_hdrs[i], sizeof(WAVEHDR));
        }
        waveOutClose(hwo);
    }
    g_wave_frame_off = 0;
    g_wave_frame_rem = 0;

    if (was_playing)
    {
        preview_set_playing_state(FALSE);
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

static void shutdown_main_window(HWND hwnd)
{
    dbg_info("Shutting down main window & plugins...");
    stop_playback(hwnd);
    video_export_request_abort();
    debug_console_shutdown();
    // persist [window] geometry & view flags while main window is still live (0x004614a0)
    preview_save_ini(hwnd);
    // 0x004041d0: [system] editresume=1 auto-saves <exe_dir>\aviutl.aup on exit when tempconfig==0
    if (!g_smoke_exit && preview_get_sys_config()->editresume != 0 && g_editp)
    {
        char ini[MAX_PATH];
        get_aviutl_ini_path(ini, sizeof(ini));
        if (GetPrivateProfileIntA("system", "tempconfig", 0, ini) == 0)
        {
            char resume_aup[MAX_PATH];
            get_resume_aup_path(resume_aup, sizeof(resume_aup));
            if (editp_is_editing(g_editp))
            {
                char prev_proj[MAX_PATH];
                lstrcpynA(prev_proj, editp_get_project_path(g_editp), sizeof(prev_proj));
                plugin_manager_save_aup(resume_aup, g_editp);
                editp_set_project_path(g_editp, prev_proj);
            }
            else
            {
                DeleteFileA(resume_aup);
            }
        }
    }
    if (g_editp && editp_is_editing(g_editp))
    {
        plugin_manager_notify_file_close(g_editp);
        editp_close_video(g_editp);
    }
    if (!g_smoke_exit)
    {
        profile_cfg_save_active();
    }
    profile_cfg_shutdown();
    // shut down plugins BEFORE DestroyWindow(hwnd) so owned plugin windows remain valid during WM_FILTER_EXIT when exedit calls GetWindowRect() to save its own layout
    video_export_shutdown();
    plugin_manager_shutdown();
    input_plugin_shutdown();
    color_plugin_shutdown();
    DestroyWindow(hwnd);
}

static void handle_debug_console_command(HWND hwnd, const char *raw_cmd)
{
    if (!raw_cmd || !raw_cmd[0])
        return;

    if (lstrcmpiA(raw_cmd, "help") == 0 || lstrcmpA(raw_cmd, "?") == 0)
    {
        dbg_info("commands:");
        dbg_info("  help             Show this command reference");
        dbg_info("  status           Dump EDITP handle, ABI fields, timeline & media state");
        dbg_info("  plugins          Dump loaded .auf modules, HMODULE base, FILTER*, HWND & flags");
        dbg_info("  auis             List registered input plugins (.aui & built-ins) by priority");
        dbg_info("  auos             List discovered .auo output plugins");
        dbg_info("  aucs             List registered color conversion plugins (.auc & built-ins)");
        dbg_info("  menus            List registered plugin add_menu_item shortcuts");
        dbg_info("  cmd <flt> <id>   Send WM_FILTER_COMMAND <id> to filter index <flt>");
        dbg_info("  show <flt>       Show filter window <flt>");
        dbg_info("  hide <flt>       Hide filter window <flt>");
        dbg_info("  trace on|off     Enable/disable live EXFUNC callback tracing");
        dbg_info("  cls              Clear debug console screen");
        return;
    }

    if (lstrcmpiA(raw_cmd, "cls") == 0 || lstrcmpiA(raw_cmd, "clear") == 0)
    {
        debug_console_clear();
        return;
    }

    if (lstrcmpiA(raw_cmd, "plugins") == 0)
    {
        plugin_manager_debug_dump();
        return;
    }

    if (lstrcmpiA(raw_cmd, "auis") == 0)
    {
        int order[INPUT_PLUGIN_MAX];
        int i, n = input_plugin_get_priority_order(order, INPUT_PLUGIN_MAX, TRUE);
        dbg_plugin("input plugins (.aui + built-ins): %d", n);
        for (i = 0; i < n; i++)
        {
            InputPluginSummary sum;
            if (input_plugin_get_summary(order[i], &sum))
            {
                dbg_plugin("  [#%d idx=%d prio=%d] '%s' %s%s(cfg=%d flag=0x%X) info='%s'",
                           i, order[i], sum.priority, sum.name,
                           (sum.flag & INPUT_PLUGIN_FLAG_BUILTIN) ? "[built-in] " : "",
                           sum.bridged_x86 ? "[x86 Bridge] " : "",
                           sum.has_config ? 1 : 0, (unsigned)sum.flag, sum.information);
            }
        }
        return;
    }

    if (lstrcmpiA(raw_cmd, "aucs") == 0)
    {
        int i, n = color_plugin_get_count();
        dbg_plugin("color plugins (.auc + built-ins): %d", n);
        for (i = 0; i < n; i++)
        {
            ColorPluginSummary sum;
            if (color_plugin_get_summary(i, &sum))
            {
                dbg_plugin("  [%d] '%s' %s%s(flag=0x%X) info='%s'",
                           i, sum.name,
                           (sum.flag & COLOR_PLUGIN_FLAG_BUILTIN) ? "[built-in] " : "",
                           sum.bridged_x86 ? "[x86 Bridge] " : "",
                           (unsigned)sum.flag, sum.information);
            }
        }
        return;
    }

    if (lstrcmpiA(raw_cmd, "auos") == 0)
    {
        int i, n = video_export_get_auo_count();
        dbg_plugin("output plugins (.auo): %d", n);
        for (i = 0; i < n; i++)
        {
            AuoPluginSummary sum;
            if (video_export_get_auo_summary(i, &sum))
            {
                dbg_plugin("  [%d] cmd=%d '%s' %s(cfg=%d) info='%s'",
                           i, AUO_CMD_BASE + i, sum.name,
                           sum.bridged_x86 ? "[x86 Bridge] " : "",
                           sum.has_config ? 1 : 0, sum.information);
            }
        }
        return;
    }

    if (lstrcmpiA(raw_cmd, "menus") == 0)
    {
        int i, n = plugin_manager_get_menu_item_count();
        dbg_plugin("plugin menu items: %d", n);
        for (i = 0; i < n; i++)
        {
            PluginMenuItem it;
            if (plugin_manager_get_menu_item(i, &it))
            {
                dbg_plugin("  [%2d] filter=%d cmd_id=%d key=0x%02X flag=0x%X '%s'",
                           i, it.filter_index, it.cmd_id,
                           (unsigned)it.def_key, (unsigned)it.flag, it.name);
            }
        }
        return;
    }

    if (lstrcmpiA(raw_cmd, "status") == 0)
    {
        if (!g_editp)
        {
            dbg_info("EDITP is NULL");
        }
        else
        {
            int w = 0, h = 0, mw = 0, mh = 0, rate = 0, scale = 0, s = 0, e = 0;
            editp_get_dimensions(g_editp, &w, &h, &mw, &mh);
            editp_get_fps(g_editp, &rate, &scale);
            editp_get_select_frame(g_editp, &s, &e);
            dbg_info("Host HWND=0x%p EDITP=0x%p EXFUNC=0x%p bridged=%d",
                     (void *)hwnd, (void *)g_editp, (void *)exfunc_get_table(),
                     plugin_manager_is_bridged() ? 1 : 0);
            dbg_info("  editing=%d size=%dx%d (VRAM %dx%d) frame=%d/%d sel=[%d..%d] fps=%d/%d audio=%dHz/%dch",
                     editp_is_editing(g_editp), w, h, mw, mh,
                     editp_get_frame(g_editp), editp_get_frame_n(g_editp),
                     s, e, rate, scale,
                     editp_get_audio_rate(g_editp), editp_get_audio_ch(g_editp));
            dbg_info("  edit_path    = '%s'", editp_get_edit_path(g_editp));
            dbg_info("  project_path = '%s'", editp_get_project_path(g_editp));
            dbg_info("  output_path  = '%s'", editp_get_output_path(g_editp));
        }
        if (plugin_manager_is_bridged())
        {
            plugin_bridge_debug_cmd("status");
        }
        return;
    }

    if (lstrcmpiA(raw_cmd, "trace on") == 0)
    {
        debug_console_set_exfunc_trace(TRUE);
        dbg_info("EXFUNC callback tracing ENABLED");
        if (plugin_manager_is_bridged())
            plugin_bridge_debug_cmd("trace on");
        return;
    }
    if (lstrcmpiA(raw_cmd, "trace off") == 0)
    {
        debug_console_set_exfunc_trace(FALSE);
        dbg_info("EXFUNC callback tracing DISABLED");
        if (plugin_manager_is_bridged())
            plugin_bridge_debug_cmd("trace off");
        return;
    }

    if (_strnicmp(raw_cmd, "show ", 5) == 0)
    {
        int idx = atoi(raw_cmd + 5);
        plugin_manager_show_window(idx, TRUE);
        apply_menu(hwnd);
        dbg_plugin("Showed filter[%d] window", idx);
        return;
    }
    if (_strnicmp(raw_cmd, "hide ", 5) == 0)
    {
        int idx = atoi(raw_cmd + 5);
        plugin_manager_show_window(idx, FALSE);
        apply_menu(hwnd);
        dbg_plugin("Hid filter[%d] window", idx);
        return;
    }
    if (_strnicmp(raw_cmd, "cmd ", 4) == 0)
    {
        int flt = -1, cid = -1;
        if (sscanf(raw_cmd + 4, "%d %d", &flt, &cid) == 2)
        {
            BOOL ok = plugin_manager_send_filter_command(flt, cid);
            refresh_current_frame(hwnd);
            dbg_plugin("WM_FILTER_COMMAND(filter=%d, id=%d) -> %s", flt, cid, ok ? "OK" : "FALSE");
            return;
        }
    }

    dbg_warn("Unknown debug command '%s' (type 'help' for commands)", raw_cmd);
}

// 0x00450820, start/stop A/V-synced waveOut playback
static void toggle_playback(HWND hwnd)
{
    int fn, cur;

    if (!g_editp || !editp_is_editing(g_editp))
        return;
    if (g_playing)
    {
        stop_playback(hwnd);
        return;
    }

    fn = editp_get_frame_n(g_editp);
    if (fn <= 0)
        return;

    cur = editp_get_frame(g_editp);
    if (cur >= fn - 1)
    {
        cur = 0;
        editp_set_frame(g_editp, 0);
        plugin_manager_notify_update(g_editp);
        refresh_current_frame(hwnd);
    }

    g_play_start_frame = cur;
    g_play_end_frame = fn - 1;
    g_play_audio_cursor = cur;
    g_wave_frame_off = 0;
    g_wave_frame_rem = 0;
    g_play_start_tick = GetTickCount();
    g_playing = TRUE;
    preview_set_playing_state(TRUE);

    if (editp_has_audio(g_editp))
    {
        WAVEFORMATEX wfx;
        g_play_audio_rate = editp_get_audio_rate(g_editp);
        g_play_audio_ch = editp_get_audio_ch(g_editp);
        if (g_play_audio_rate <= 0)
            g_play_audio_rate = 44100;
        if (g_play_audio_ch <= 0)
            g_play_audio_ch = 2;

        if (!g_wave_mem)
        {
            g_wave_mem = (unsigned char *)calloc(AUDIO_PLAY_BUF_COUNT, AUDIO_PLAY_BUF_BYTES);
        }
        ZeroMemory(&wfx, sizeof(wfx));
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = (WORD)g_play_audio_ch;
        wfx.nSamplesPerSec = (DWORD)g_play_audio_rate;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = (WORD)(wfx.nChannels * 2);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * (DWORD)wfx.nBlockAlign;

        if (g_wave_mem &&
            waveOutOpen(&g_waveout, WAVE_MAPPER, &wfx,
                        (DWORD_PTR)hwnd, 0, CALLBACK_WINDOW) == MMSYSERR_NOERROR &&
            g_waveout)
        {
            int i;
            BOOL ok = TRUE;
            waveOutPause(g_waveout);
            for (i = 0; i < AUDIO_PLAY_BUF_COUNT; i++)
            {
                ZeroMemory(&g_wave_hdrs[i], sizeof(WAVEHDR));
                g_wave_hdrs[i].lpData = (LPSTR)(g_wave_mem + i * AUDIO_PLAY_BUF_BYTES);
                g_wave_hdrs[i].dwBufferLength = AUDIO_PLAY_BUF_BYTES;
                if (waveOutPrepareHeader(g_waveout, &g_wave_hdrs[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
                {
                    ok = FALSE;
                    break;
                }
            }
            if (ok)
            {
                for (i = 0; i < AUDIO_PLAY_BUF_COUNT; i++)
                {
                    fill_and_submit_wave_hdr(&g_wave_hdrs[i]);
                }
                g_play_start_tick = GetTickCount();
                waveOutRestart(g_waveout);
            }
            else
            {
                HWAVEOUT hwo = g_waveout;
                g_waveout = NULL;
                for (i = 0; i < AUDIO_PLAY_BUF_COUNT; i++)
                {
                    if (g_wave_hdrs[i].dwFlags & WHDR_PREPARED)
                    {
                        waveOutUnprepareHeader(hwo, &g_wave_hdrs[i], sizeof(WAVEHDR));
                    }
                    ZeroMemory(&g_wave_hdrs[i], sizeof(WAVEHDR));
                }
                waveOutClose(hwo);
            }
        }
        else
        {
            g_waveout = NULL;
        }
    }

    SetTimer(hwnd, PLAY_TIMER_ID, 15, NULL);
    InvalidateRect(hwnd, NULL, FALSE);
}

static BOOL confirm_discard_current_edit(HWND hwnd)
{
    const AviUtlSystemConfig *scfg;
    if (g_smoke_exit || g_cli_no_confirm || !g_editp ||
        !editp_is_editing(g_editp) || editp_is_saving(g_editp))
        return TRUE;
    scfg = preview_get_sys_config();
    if (!scfg || scfg->closedialog == 0)
        return TRUE;
    return aviutl_msgbox_w(hwnd, i18n_get_wstr(1046), L"AviUtl",
                           MB_OKCANCEL | MB_ICONQUESTION) == IDOK;
}

typedef struct LoadOptionHookCtx
{
    BOOL is_append;
    EditpLoadOptions opts;
} LoadOptionHookCtx;

static void parse_fps_spec(const char *s, int *out_rate, int *out_scale)
{
    double v;
    if (!s || !s[0] || !out_rate || !out_scale)
        return;
    v = atof(s);
    if (v <= 0.0)
        return;
    if (fabs(v - 29.97) < 0.02)
    {
        *out_rate = 30000;
        *out_scale = 1001;
    }
    else if (fabs(v - 23.976) < 0.02)
    {
        *out_rate = 24000;
        *out_scale = 1001;
    }
    else if (fabs(v - 59.94) < 0.02)
    {
        *out_rate = 60000;
        *out_scale = 1001;
    }
    else if (fabs(v - 119.88) < 0.02)
    {
        *out_rate = 120000;
        *out_scale = 1001;
    }
    else
    {
        int r1000 = (int)(v * 1000.0 + 0.5);
        if (r1000 > 0)
        {
            if ((r1000 % 1000) == 0)
            {
                *out_rate = r1000 / 1000;
                *out_scale = 1;
            }
            else
            {
                *out_rate = r1000;
                *out_scale = 1000;
            }
        }
    }
}

static UINT_PTR CALLBACK LoadOptionHookProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    if (msg == WM_INITDIALOG)
    {
        OPENFILENAMEA *ofn = (OPENFILENAMEA *)lp;
        LoadOptionHookCtx *ctx = ofn ? (LoadOptionHookCtx *)ofn->lCustData : NULL;
        HWND hCombo = GetDlgItem(hdlg, 32500);
        HWND hIntoSel = GetDlgItem(hdlg, 32203);
        char ini[MAX_PATH], fpslist[260];

        SetWindowLongPtrA(hdlg, GWLP_USERDATA, (LONG_PTR)ctx);
        if (hIntoSel)
        {
            EnableWindow(hIntoSel, (ctx && ctx->is_append) ? TRUE : FALSE);
        }
        if (hCombo)
        {
            const wchar_t *no_conv = i18n_get_wstr(33);
            char *p;
            SendMessageW(hCombo, CB_ADDSTRING, 0,
                         (LPARAM)((no_conv && no_conv[0]) ? no_conv : L"No fps change"));

            get_aviutl_ini_path(ini, sizeof(ini));
            GetPrivateProfileStringA("system", "fpslist",
                                     "29.97,23.976,24,15,10", fpslist, sizeof(fpslist), ini);
            p = fpslist;
            while (*p)
            {
                char item[64];
                wchar_t witem[96];
                int k = 0;
                while (*p == ' ' || *p == ',')
                    p++;
                while (*p && *p != ',' && k + 1 < (int)sizeof(item))
                    item[k++] = *p++;
                item[k] = '\0';
                if (k > 0)
                {
                    wsprintfW(witem, L"%Sfps", item);
                    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)witem);
                }
            }
            SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
        }
        return TRUE;
    }
    if (msg == WM_NOTIFY && lp)
    {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->code == CDN_FILEOK)
        {
            LoadOptionHookCtx *ctx = (LoadOptionHookCtx *)GetWindowLongPtrA(hdlg, GWLP_USERDATA);
            if (ctx)
            {
                HWND hCombo = GetDlgItem(hdlg, 32500);
                ctx->opts.seq_avi = (IsDlgButtonChecked(hdlg, 32201) == BST_CHECKED);
                ctx->opts.fps_adjust = (IsDlgButtonChecked(hdlg, 32202) == BST_CHECKED);
                ctx->opts.load_60fps = (IsDlgButtonChecked(hdlg, 32200) == BST_CHECKED);
                ctx->opts.into_selection = ctx->is_append &&
                                           (IsDlgButtonChecked(hdlg, 32203) == BST_CHECKED);
                ctx->opts.fps_rate = 0;
                ctx->opts.fps_scale = 0;
                if (hCombo)
                {
                    int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                    char txt[96] = {0};
                    if (sel > 0)
                    {
                        SendMessageA(hCombo, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)txt);
                        parse_fps_spec(txt, &ctx->opts.fps_rate, &ctx->opts.fps_scale);
                    }
                    else
                    {
                        GetWindowTextA(hCombo, txt, sizeof(txt));
                        if (txt[0] >= '0' && txt[0] <= '9')
                            parse_fps_spec(txt, &ctx->opts.fps_rate, &ctx->opts.fps_scale);
                    }
                }
            }
        }
    }
    return 0;
}

static int find_exedit_filter_index(void)
{
    int idx = plugin_manager_find_by_name(EXEDIT_SJIS_NAME);
    int i, cnt;
    if (idx >= 0)
        return idx;
    idx = plugin_manager_find_by_name("Advanced Editing");
    if (idx >= 0)
        return idx;
    cnt = plugin_manager_get_count();
    for (i = 0; i < cnt; i++)
    {
        const char *p = plugin_manager_get_path(i);
        if (p && p[0])
        {
            const char *base = strrchr(p, '\\');
            base = base ? (base + 1) : p;
            if (lstrcmpiA(base, "exedit.auf") == 0)
                return i;
        }
    }
    return -1;
}

static BOOL open_media_or_exo_with_opts(HWND hwnd, const char *path, BOOL force_project_aup,
                                        const EditpLoadOptions *in_opts)
{
    const char *ext;
    BOOL treat_as_aup;
    EditpLoadOptions opts;
    char ini[MAX_PATH];

    if (!g_editp || !path || !path[0])
        return FALSE;
    stop_playback(hwnd);

    ext = strrchr(path, '.');
    if (ext && lstrcmpiA(ext, ".exo") == 0)
    {
        int ex_idx = find_exedit_filter_index();
        if (ex_idx >= 0)
        {
            plugin_manager_show_window(ex_idx, TRUE);
            if (plugin_manager_import_exo(ex_idx, path, g_editp))
            {
                g_last_auto_w = 0;
                g_last_auto_h = 0;
                refresh_current_frame(hwnd);
                return TRUE;
            }
        }
    }

    if (!confirm_discard_current_edit(hwnd))
    {
        return FALSE;
    }

    treat_as_aup = (ext && lstrcmpiA(ext, ".aup") == 0) &&
                   (force_project_aup || preview_get_sys_config()->openprojectaup != 0);
    if (treat_as_aup)
    {
        int ex_idx = find_exedit_filter_index();
        if (ex_idx >= 0)
        {
            plugin_manager_show_window(ex_idx, TRUE);
        }
        if (!plugin_manager_load_aup(path, g_editp))
        {
            aviutl_msgbox_w(hwnd, i18n_get_wstr(1000), L"AviUtl", MB_OK | MB_ICONWARNING);
            return FALSE;
        }
        g_last_auto_w = 0;
        g_last_auto_h = 0;
        mru_record_file(path);
        apply_menu(hwnd);
        refresh_current_frame(hwnd);
        return TRUE;
    }

    ZeroMemory(&opts, sizeof(opts));
    if (in_opts)
        opts = *in_opts;
    get_aviutl_ini_path(ini, sizeof(ini));
    if (!opts.fps_adjust && GetPrivateProfileIntA("system", "loadfpsadjust", 0, ini) != 0)
    {
        opts.fps_adjust = TRUE;
    }

    if (editp_is_editing(g_editp))
    {
        plugin_manager_notify_file_close(g_editp);
    }
    if (!editp_open_video_ex(g_editp, path, &opts))
    {
        aviutl_msgbox_w(hwnd, i18n_get_wstr(1038), L"AviUtl", MB_OK | MB_ICONWARNING);
        return FALSE;
    }
    if (opts.fps_rate <= 0 && GetPrivateProfileIntA("system", "load30fps", 0, ini) != 0)
    {
        int r = 0, sc = 0;
        editp_get_fps(g_editp, &r, &sc);
        if (r > 0 && sc > 0)
        {
            double fps = (double)r / (double)sc;
            if (fps > 29.90 && fps < 30.00 && !(r == 30000 && sc == 1001))
                editp_set_fps(g_editp, 30000, 1001);
        }
    }

    g_last_auto_w = 0;
    g_last_auto_h = 0;
    mru_record_file(path);
    apply_menu(hwnd);
    plugin_manager_notify_file_open(g_editp);
    refresh_current_frame(hwnd);
    return TRUE;
}

static BOOL open_media_or_exo_ex(HWND hwnd, const char *path, BOOL force_project_aup)
{
    return open_media_or_exo_with_opts(hwnd, path, force_project_aup, NULL);
}

static BOOL open_media_or_exo(HWND hwnd, const char *path)
{
    return open_media_or_exo_ex(hwnd, path, FALSE);
}

static void build_video_open_filter(char *flt_buf, int flt_cap, BOOL include_project_exo)
{
    const char *base_flt = input_plugin_get_file_filter(0);
    int p = 0;

    memset(flt_buf, 0, (size_t)flt_cap);
    if (base_flt && base_flt[0])
    {
        int l1 = (int)strlen(base_flt);
        int l2 = (int)strlen(base_flt + l1 + 1);
        if (l1 > 0 && l2 > 0 && l1 + l2 + 128 < flt_cap)
        {
            if (include_project_exo)
            {
                p += _snprintf(flt_buf + p, 1900, "%s;*.aup;*.exo)", base_flt);
                if (p > 0 && p >= 13 && flt_buf[p - 12] == ')')
                {
                    memmove(flt_buf + p - 12, flt_buf + p - 11, 12);
                    p--;
                }
                p++;
                p += _snprintf(flt_buf + p, 1900, "%s;*.aup;*.exo", base_flt + l1 + 1);
                p++;
            }
            else
            {
                memcpy(flt_buf, base_flt, (size_t)(l1 + 1 + l2 + 1));
                p = l1 + 1 + l2 + 1;
            }
        }
    }
    if (p == 0)
    {
        memcpy(flt_buf, "VideoFile (*.*)\0*.*\0", 20);
        p = 20;
    }
    if (include_project_exo)
    {
        memcpy(flt_buf + p,
               "AviUtl Project (*.aup)\0*.aup\0"
               "ExEdit Object (*.exo)\0*.exo\0"
               "AllFile (*.*)\0*.*\0\0",
               76);
    }
    else
    {
        memcpy(flt_buf + p, "AllFile (*.*)\0*.*\0\0", 19);
    }
}

static void reaviutl_file_open_with_initial(HWND owner, const char *initial_path)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = {0};
    char flt_buf[4096];
    LoadOptionHookCtx hook_ctx;
    const DLGTEMPLATE *tmpl = i18n_load_dialog_template("LOAD_OPTION");

    if (initial_path && initial_path[0])
    {
        lstrcpynA(file, initial_path, sizeof(file));
    }

    build_video_open_filter(flt_buf, sizeof(flt_buf), TRUE);
    ZeroMemory(&hook_ctx, sizeof(hook_ctx));
    hook_ctx.is_append = FALSE;

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = flt_buf;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (tmpl)
    {
        ofn.hInstance = (HINSTANCE)tmpl;
        ofn.lpfnHook = LoadOptionHookProc;
        ofn.lCustData = (LPARAM)&hook_ctx;
        ofn.Flags |= OFN_EXPLORER | OFN_ENABLESIZING | OFN_ENABLEHOOK | OFN_ENABLETEMPLATEHANDLE;
    }

    if (GetOpenFileNameA(&ofn))
    {
        open_media_or_exo_with_opts(owner, file, FALSE, &hook_ctx.opts);
    }
}

static BOOL append_video_direct(HWND owner, const char *path)
{
    EditpLoadOptions opts;
    char ini[MAX_PATH];

    if (!g_editp || !path || !path[0])
        return FALSE;
    if (!editp_is_editing(g_editp))
        return open_media_or_exo_ex(owner, path, FALSE);

    stop_playback(owner);
    ZeroMemory(&opts, sizeof(opts));
    get_aviutl_ini_path(ini, sizeof(ini));
    if (GetPrivateProfileIntA("system", "loadfpsadjust", 0, ini) != 0)
    {
        opts.fps_adjust = TRUE;
    }

    if (!editp_append_video_ex(g_editp, path, &opts))
    {
        aviutl_msgbox_w(owner, i18n_get_wstr(1038), L"AviUtl", MB_OK | MB_ICONWARNING);
        return FALSE;
    }
    exfunc_invalidate_all_caches();
    if (plugin_manager_is_bridged())
        plugin_bridge_invalidate_caches();
    plugin_manager_notify_update(g_editp);
    refresh_current_frame(owner);
    return TRUE;
}

static void reaviutl_file_append_with_initial(HWND owner, const char *initial_path)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = {0};
    char flt_buf[4096];
    char title_buf[128];
    LoadOptionHookCtx hook_ctx;
    const DLGTEMPLATE *tmpl;

    if (!g_editp || !editp_is_editing(g_editp))
    {
        reaviutl_file_open_with_initial(owner, initial_path);
        return;
    }

    if (initial_path && initial_path[0])
    {
        lstrcpynA(file, initial_path, sizeof(file));
    }

    stop_playback(owner);
    build_video_open_filter(flt_buf, sizeof(flt_buf), FALSE);
    ZeroMemory(&hook_ctx, sizeof(hook_ctx));
    hook_ctx.is_append = TRUE;
    lstrcpynA(title_buf, i18n_get_str(139), sizeof(title_buf));
    tmpl = i18n_load_dialog_template("LOAD_OPTION");

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = flt_buf;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrTitle = title_buf[0] ? title_buf : NULL;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (tmpl)
    {
        ofn.hInstance = (HINSTANCE)tmpl;
        ofn.lpfnHook = LoadOptionHookProc;
        ofn.lCustData = (LPARAM)&hook_ctx;
        ofn.Flags |= OFN_EXPLORER | OFN_ENABLESIZING | OFN_ENABLEHOOK | OFN_ENABLETEMPLATEHANDLE;
    }

    if (!GetOpenFileNameA(&ofn))
        return;

    if (!editp_append_video_ex(g_editp, file, &hook_ctx.opts))
    {
        aviutl_msgbox_w(owner, i18n_get_wstr(1038), L"AviUtl", MB_OK | MB_ICONWARNING);
        return;
    }
    exfunc_invalidate_all_caches();
    if (plugin_manager_is_bridged())
        plugin_bridge_invalidate_caches();
    plugin_manager_notify_update(g_editp);
    refresh_current_frame(owner);
}

static void reaviutl_file_append(HWND owner)
{
    reaviutl_file_append_with_initial(owner, NULL);
}

void reaviutl_file_open(HWND owner)
{
    reaviutl_file_open_with_initial(owner, NULL);
}

static BOOL load_audio_into_current_edit(HWND owner, const char *file)
{
    if (!g_editp || !file || !file[0])
        return FALSE;

    stop_playback(owner);
    if (!editp_is_editing(g_editp))
    {
        return open_media_or_exo_ex(owner, file, FALSE);
    }
    if (!editp_open_audio(g_editp, file))
    {
        char msg[MAX_PATH + 128];
        wsprintfA(msg, "Failed to load audio file:\n%s", file);
        MessageBoxA(owner, msg, "AviUtl", MB_OK | MB_ICONWARNING);
        return FALSE;
    }
    if (plugin_manager_is_bridged())
    {
        plugin_bridge_open_audio(file, g_editp);
    }
    plugin_manager_notify_update(g_editp);
    refresh_current_frame(owner);
    return TRUE;
}

static void reaviutl_audio_open_with_initial(HWND owner, const char *initial_path)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = {0};
    const char *filter = input_plugin_get_file_filter(1);

    if (!g_editp)
        return;
    if (initial_path && initial_path[0])
    {
        lstrcpynA(file, initial_path, sizeof(file));
    }

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameA(&ofn))
    {
        return;
    }
    load_audio_into_current_edit(owner, file);
}

static void reaviutl_audio_open(HWND owner)
{
    reaviutl_audio_open_with_initial(owner, NULL);
}

static BOOL reaviutl_wav_export(HWND owner)
{
    BOOL ok;
    if (!g_editp || !editp_is_editing(g_editp))
        return FALSE;
    stop_playback(owner);
    ok = video_export_wav_dialog(owner, g_editp);
    refresh_current_frame(owner);
    return ok;
}

static void reaviutl_project_open(HWND owner)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = {0};
    static const char filter[] =
        "AviUtl Project (*.aup)\0*.aup\0"
        "All Files (*.*)\0*.*\0";

    if (g_editp && editp_get_project_path(g_editp)[0])
    {
        lstrcpynA(file, editp_get_project_path(g_editp), sizeof(file));
    }

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrDefExt = "aup";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameA(&ofn))
    {
        open_media_or_exo_ex(owner, file, TRUE);
    }
}

static BOOL reaviutl_project_save(HWND owner, BOOL force_save_as)
{
    char file[MAX_PATH] = {0};

    if (!g_editp || !editp_is_editing(g_editp))
    {
        return FALSE;
    }

    if (!force_save_as && editp_get_project_path(g_editp)[0])
    {
        lstrcpynA(file, editp_get_project_path(g_editp), sizeof(file));
    }
    else
    {
        OPENFILENAMEA ofn;
        static const char filter[] =
            "AviUtl Project (*.aup)\0*.aup\0"
            "All Files (*.*)\0*.*\0";

        if (editp_get_project_path(g_editp)[0])
        {
            lstrcpynA(file, editp_get_project_path(g_editp), sizeof(file));
        }
        else
        {
            const char *ed = editp_get_edit_path(g_editp);
            char *dot;
            lstrcpynA(file, aviutl_basename(ed), sizeof(file));
            dot = strrchr(file, '.');
            if (dot)
                *dot = '\0';
            if (!file[0])
                lstrcpynA(file, "project", sizeof(file));
            if (strlen(file) + 5 < sizeof(file))
            {
                lstrcatA(file, ".aup");
            }
        }

        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = filter;
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = file;
        ofn.nMaxFile = sizeof(file);
        ofn.lpstrDefExt = "aup";
        ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST |
                    ((preview_get_sys_config()->overwritecheck == 0) ? OFN_OVERWRITEPROMPT : 0);

        if (!GetSaveFileNameA(&ofn))
        {
            return FALSE;
        }
    }

    if (!plugin_manager_save_aup(file, g_editp))
    {
        char msg[MAX_PATH + 128];
        wsprintfA(msg, "Failed to save AviUtl project (.aup):\n%s", file);
        MessageBoxA(owner, msg, "AviUtl", MB_OK | MB_ICONWARNING);
        return FALSE;
    }
    update_window_title(owner);
    return TRUE;
}

static void seek_to_frame_ex(HWND hwnd, int target_frame, BOOL allow_shift_select)
{
    BOOL shift_held = FALSE;
    BOOL shift_cfg = TRUE;
    const AviUtlSystemConfig *scfg;

    if (!g_editp || !editp_is_editing(g_editp))
        return;
    scfg = preview_get_sys_config();
    if (scfg)
        shift_cfg = (scfg->shiftselect != 0);
    if (allow_shift_select)
    {
        shift_held = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    }

    if (editp_seek_frame_ex(g_editp, target_frame, shift_held, shift_cfg))
    {
        plugin_manager_notify_update(g_editp);
        refresh_current_frame(hwnd);
    }
}

static void copy_current_frame_to_clipboard(HWND hwnd, BOOL raw_source)
{
    int w = 0, h = 0, mw = 0, mh = 0, stride;
    size_t img_sz;
    HGLOBAL hMem;
    BITMAPINFOHEADER *bih;

    if (!g_editp || !editp_is_editing(g_editp))
        return;
    if (raw_source)
    {
        editp_load_frame(g_editp, editp_get_frame(g_editp));
    }
    else if (!plugin_manager_render_video_frame(g_editp, editp_get_frame(g_editp)))
    {
        editp_load_frame(g_editp, editp_get_frame(g_editp));
    }
    editp_get_dimensions(g_editp, &w, &h, &mw, &mh);
    if (w <= 0 || h <= 0)
        return;

    stride = (w * 3 + 3) & ~3;
    img_sz = (size_t)stride * (size_t)h;
    if (!OpenClipboard(hwnd))
        return;
    EmptyClipboard();
    hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + img_sz);
    if (hMem)
    {
        bih = (BITMAPINFOHEADER *)GlobalLock(hMem);
        if (bih)
        {
            ZeroMemory(bih, sizeof(*bih));
            bih->biSize = sizeof(*bih);
            bih->biWidth = w;
            bih->biHeight = h;
            bih->biPlanes = 1;
            bih->biBitCount = 24;
            bih->biCompression = BI_RGB;
            bih->biSizeImage = (DWORD)img_sz;
            yc2rgb_dib24((unsigned char *)bih + sizeof(*bih), stride,
                         editp_get_ycp_edit(g_editp), mw, w, h, TRUE);
            GlobalUnlock(hMem);
            SetClipboardData(CF_DIB, hMem);
        }
        else
        {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
    if (raw_source)
    {
        refresh_current_frame(hwnd);
    }
}

static void copy_frame_number_to_clipboard(HWND hwnd)
{
    char buf[32];
    HGLOBAL hMem;
    const AviUtlSystemConfig *scfg;
    int sf;

    if (!g_editp || !editp_is_editing(g_editp))
        return;
    scfg = preview_get_sys_config();
    sf = scfg ? scfg->startframe : 1;
    wsprintfA(buf, "%d", editp_get_frame(g_editp) + sf);

    if (!OpenClipboard(hwnd))
        return;
    EmptyClipboard();
    hMem = GlobalAlloc(GMEM_MOVEABLE, (size_t)lstrlenA(buf) + 1u);
    if (hMem)
    {
        char *dst = (char *)GlobalLock(hMem);
        if (dst)
        {
            lstrcpyA(dst, buf);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        else
        {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

static void copy_current_frame_yc48(HWND hwnd)
{
    int org_w = 0, org_h = 0, w = 0, h = 0, mw = 0, mh = 0, cur;

    if (!g_editp || !editp_is_editing(g_editp))
        return;
    cur = editp_get_frame(g_editp);
    editp_load_frame(g_editp, cur);
    editp_get_dimensions(g_editp, &w, &h, &mw, &mh);
    editp_get_org_dimensions(g_editp, &org_w, &org_h);
    if (org_w <= 0)
        org_w = w;
    if (org_h <= 0)
        org_h = h;
    if (org_w > 0 && org_h > 0 && mw >= org_w)
    {
        editp_copy_frame_image(g_editp, editp_get_ycp_edit(g_editp), mw, org_w, org_h);
    }
    refresh_current_frame(hwnd);
}

static void toggle_frame_edit_flag(HWND hwnd, BYTE mask)
{
    BYTE st = 0;
    int cur;
    if (!g_editp || !editp_is_editing(g_editp))
        return;
    cur = editp_get_frame(g_editp);
    editp_get_frame_status(g_editp, cur, &st);
    st ^= mask;
    editp_set_frame_status(g_editp, cur, st);
    plugin_manager_notify_update(g_editp);
    refresh_current_frame(hwnd);
}

static void seek_matching_frame(HWND hwnd, int dir, BOOL match_mark)
{
    int cur, fn, f;
    if (!g_editp || !editp_is_editing(g_editp))
        return;
    cur = editp_get_frame(g_editp);
    fn = editp_get_frame_n(g_editp);
    for (f = cur + dir; f >= 0 && f < fn; f += dir)
    {
        if (match_mark)
        {
            BYTE st = 0;
            if (editp_get_frame_status(g_editp, f, &st) && (st & EDIT_FRAME_EDIT_FLAG_MARKFRAME))
            {
                seek_to_frame_ex(hwnd, f, TRUE);
                return;
            }
        }
        else if (editp_is_source_keyframe(g_editp, f))
        {
            seek_to_frame_ex(hwnd, f, TRUE);
            return;
        }
    }
}

static void seek_skipping_deleted_frames(HWND hwnd, int dir)
{
    int cur, fn, f;
    if (!g_editp || !editp_is_editing(g_editp))
        return;
    cur = editp_get_frame(g_editp);
    fn = editp_get_frame_n(g_editp);
    for (f = cur + dir; f >= 0 && f < fn; f += dir)
    {
        BYTE st = 0;
        editp_get_frame_status(g_editp, f, &st);
        if (!(st & EDIT_FRAME_EDIT_FLAG_DELFRAME))
        {
            seek_to_frame_ex(hwnd, f, TRUE);
            return;
        }
    }
    seek_to_frame_ex(hwnd, (dir > 0) ? (fn - 1) : 0, TRUE);
}

static void seek_frame_discontinuity(HWND hwnd, int dir)
{
    int cur, fn, f;
    if (!g_editp || !editp_is_editing(g_editp))
        return;
    cur = editp_get_frame(g_editp);
    fn = editp_get_frame_n(g_editp);
    for (f = cur + dir; f > 0 && f < fn; f += dir)
    {
        if (editp_is_frame_discontinuity(g_editp, f))
        {
            seek_to_frame_ex(hwnd, f, TRUE);
            return;
        }
    }
    seek_to_frame_ex(hwnd, (dir > 0) ? (fn - 1) : 0, TRUE);
}

static void seek_profile_change_frame(HWND hwnd, int dir)
{
    int cur, fn, f;
    if (!g_editp || !editp_is_editing(g_editp))
        return;
    cur = editp_get_frame(g_editp);
    fn = editp_get_frame_n(g_editp);
    for (f = cur + dir; f > 0 && f < fn; f += dir)
    {
        if (editp_get_frame_config(g_editp, f) != editp_get_frame_config(g_editp, f - 1))
        {
            seek_to_frame_ex(hwnd, f, TRUE);
            return;
        }
    }
    seek_to_frame_ex(hwnd, (dir > 0) ? (fn - 1) : 0, TRUE);
}

static void run_inter_dialog(HWND hwnd)
{
    AviUtlDlgState st;
    const wchar_t *items[7];
    wchar_t wbuf[6][96];
    static const UINT str_ids[6] = {23, 24, 17, 18, 19, 21};
    int i, s = 0, e = 0, cur;

    if (!g_editp || !editp_is_editing(g_editp))
        return;

    for (i = 0; i < 6; i++)
    {
        lstrcpynW(wbuf[i], i18n_get_wstr(str_ids[i]), 96);
        items[i] = wbuf[i];
    }
    items[6] = NULL;

    cur = editp_get_frame(g_editp);
    aviutl_dlg_init(&st);
    st.combo_items[0] = items;
    st.combo_sel[0] = (int)editp_get_frame_inter(g_editp, cur);
    if (st.combo_sel[0] < 0 || st.combo_sel[0] > 5)
        st.combo_sel[0] = 0;

    if (aviutl_dlg_run(hwnd, "INTER", &st) != IDOK)
        return;
    if (st.combo_sel[0] < 0 || st.combo_sel[0] > 5)
        return;

    editp_get_select_frame(g_editp, &s, &e);
    if (s <= e)
    {
        editp_push_undo(g_editp);
        for (i = s; i <= e; i++)
        {
            editp_set_frame_inter(g_editp, i, (BYTE)st.combo_sel[0]);
        }
        exfunc_invalidate_all_caches();
        if (plugin_manager_is_bridged())
            plugin_bridge_invalidate_caches();
        plugin_manager_notify_update(g_editp);
        refresh_current_frame(hwnd);
    }
}

static void import_keyframe_list(HWND hwnd)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = {0};
    static const char filter[] = "Text File (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    FILE *fp;
    char line[256];
    int sf, fn;

    if (!g_editp || !editp_is_editing(g_editp))
        return;

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrDefExt = "txt";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn))
        return;

    fp = fopen(file, "r");
    if (!fp)
    {
        aviutl_msgbox_w(hwnd, i18n_get_wstr(1000), L"AviUtl", MB_OK | MB_ICONWARNING);
        return;
    }

    editp_push_undo(g_editp);
    sf = preview_get_sys_config() ? preview_get_sys_config()->startframe : 1;
    fn = editp_get_frame_n(g_editp);
    while (fgets(line, sizeof(line), fp))
    {
        char *p = line;
        char *endp = NULL;
        long v;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p < '0' || *p > '9')
            continue;
        v = strtol(p, &endp, 10) - sf;
        if (endp != p && v >= 0 && v < fn)
        {
            BYTE st = 0;
            editp_get_frame_status(g_editp, (int)v, &st);
            editp_set_frame_status(g_editp, (int)v, (BYTE)(st | EDIT_FRAME_EDIT_FLAG_KEYFRAME));
        }
    }
    fclose(fp);
    plugin_manager_notify_update(g_editp);
    refresh_current_frame(hwnd);
}

static void export_keyframe_list(HWND hwnd)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = {0};
    static const char filter[] = "Text File (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    FILE *fp;
    int sf, fn, f;

    if (!g_editp || !editp_is_editing(g_editp))
        return;

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrDefExt = "txt";
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                ((preview_get_sys_config()->overwritecheck == 0) ? OFN_OVERWRITEPROMPT : 0);
    if (!GetSaveFileNameA(&ofn))
        return;

    fp = fopen(file, "w");
    if (!fp)
    {
        aviutl_msgbox_w(hwnd, i18n_get_wstr(1002), L"AviUtl", MB_OK | MB_ICONWARNING);
        return;
    }

    sf = preview_get_sys_config() ? preview_get_sys_config()->startframe : 1;
    fn = editp_get_frame_n(g_editp);
    for (f = 0; f < fn; f++)
    {
        BYTE st = 0;
        editp_get_frame_status(g_editp, f, &st);
        if ((st & EDIT_FRAME_EDIT_FLAG_KEYFRAME) || editp_is_source_keyframe(g_editp, f))
        {
            fprintf(fp, "%d\n", f + sf);
        }
    }
    fclose(fp);
}

static void reset_window_positions(HWND hwnd)
{
    RECT wa;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0))
    {
        SetRect(&wa, 0, 0, 1280, 720);
    }
    SetWindowPos(hwnd, NULL, wa.left + 32, wa.top + 32, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    plugin_manager_reset_window_positions(hwnd);
}

// 0x00404b60: Browse button handler inside SYSTEM dialog (108)
static BOOL on_system_dlg_cmd(HWND hdlg, int cmd_id, AviUtlDlgState *st, void *user)
{
    (void)user;
    if (cmd_id == 32301) // 0x7e2d: browse folder for defaultsavefile
    {
        BROWSEINFOW bi;
        LPITEMIDLIST pidl;
        wchar_t dir[MAX_PATH];

        dir[0] = L'\0';
        ZeroMemory(&bi, sizeof(bi));
        bi.hwndOwner = hdlg;
        bi.ulFlags = BIF_RETURNONLYFSDIRS;
        pidl = SHBrowseForFolderW(&bi);
        if (pidl)
        {
            if (SHGetPathFromIDListW(pidl, dir) && dir[0])
            {
                int len = lstrlenW(dir);
                if (len > 0 && dir[len - 1] != L'\\' && len + 2 < MAX_PATH)
                {
                    dir[len] = L'\\';
                    dir[len + 1] = L'\0';
                }
                lstrcpynW(st->strs[1], dir, 260);
                SetDlgItemTextW(hdlg, 32101, st->strs[1]);
            }
            CoTaskMemFree(pidl);
        }
    }
    else if (cmd_id == 32302) // 0x7e2e: browse WAV for finishsound
    {
        OPENFILENAMEW ofn;
        wchar_t file[MAX_PATH];
        static const wchar_t filter[] = L"Wave File (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";

        lstrcpynW(file, st->strs[2], MAX_PATH);
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hdlg;
        ofn.lpstrFilter = filter;
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn))
        {
            lstrcpynW(st->strs[2], file, 260);
            SetDlgItemTextW(hdlg, 32102, st->strs[2]);
        }
    }
    return TRUE;
}

// 0x00405450 case 0x6c (108): System Settings ("SYSTEM") dialog
static void run_system_settings_dialog(HWND owner)
{
    AviUtlDlgState st;
    char ini[MAX_PATH];
    char sbuf[260];

    get_aviutl_ini_path(ini, sizeof(ini));
    aviutl_dlg_init(&st);
    st.on_cmd = on_system_dlg_cmd;

    st.ints[0] = GetPrivateProfileIntA("system", "width", REAVIUTL_DEFAULT_VRAM_W, ini);
    st.ints[1] = GetPrivateProfileIntA("system", "height", REAVIUTL_DEFAULT_VRAM_H, ini);
    st.ints[2] = GetPrivateProfileIntA("system", "frame", REAVIUTL_DEFAULT_MAX_FRAME, ini);
    st.ints[3] = GetPrivateProfileIntA("system", "sharecache", 256, ini);
    st.ints[5] = GetPrivateProfileIntA("system", "moveA", 5, ini);
    st.ints[6] = GetPrivateProfileIntA("system", "moveB", 30, ini);
    st.ints[7] = GetPrivateProfileIntA("system", "moveC", 899, ini);
    st.ints[8] = GetPrivateProfileIntA("system", "moveD", 8991, ini);
    st.ints[9] = GetPrivateProfileIntA("system", "saveunitsize", 64, ini);
    st.ints[10] = GetPrivateProfileIntA("system", "multithread", 0, ini);

    GetPrivateProfileStringA("system", "defaultsavefile", "%p", sbuf, sizeof(sbuf), ini);
    aviutl_dlg_set_str_sjis(&st, 1, sbuf);
    GetPrivateProfileStringA("system", "finishsound", "", sbuf, sizeof(sbuf), ini);
    aviutl_dlg_set_str_sjis(&st, 2, sbuf);
    GetPrivateProfileStringA("system", "resizelist",
                             "640x480,352x480,352x240,320x240", sbuf, sizeof(sbuf), ini);
    aviutl_dlg_set_str_sjis(&st, 3, sbuf);
    GetPrivateProfileStringA("system", "fpslist",
                             "29.97,23.976,24,15,10", sbuf, sizeof(sbuf), ini);
    aviutl_dlg_set_str_sjis(&st, 4, sbuf);

    st.checks[1] = GetPrivateProfileIntA("system", "compprofile", 1, ini) ? 1 : 0;
    st.checks[2] = GetPrivateProfileIntA("system", "plugincache", 0, ini) ? 1 : 0;
    st.checks[3] = GetPrivateProfileIntA("system", "startframe", 1, ini) ? 1 : 0;
    st.checks[4] = GetPrivateProfileIntA("system", "shiftselect", 1, ini) ? 1 : 0;
    st.checks[5] = GetPrivateProfileIntA("system", "yuy2mode", 0, ini) ? 1 : 0;
    st.checks[6] = GetPrivateProfileIntA("system", "sse", 1, ini) ? 1 : 0;
    st.checks[7] = GetPrivateProfileIntA("system", "sse2", 1, ini) ? 1 : 0;
    st.checks[8] = GetPrivateProfileIntA("system", "vfplugin", 1, ini) ? 1 : 0;
    st.checks[9] = GetPrivateProfileIntA("system", "editresume", 0, ini) ? 1 : 0;
    st.checks[10] = GetPrivateProfileIntA("system", "fpsnoconvert", 0, ini) ? 1 : 0;
    st.checks[11] = GetPrivateProfileIntA("system", "tempconfig", 0, ini) ? 1 : 0;
    st.checks[12] = GetPrivateProfileIntA("system", "load30fps", 0, ini) ? 1 : 0;
    st.checks[13] = GetPrivateProfileIntA("system", "loadfpsadjust", 0, ini) ? 1 : 0;
    st.checks[14] = GetPrivateProfileIntA("system", "yuy2limit", 1, ini) ? 1 : 0;
    st.checks[15] = GetPrivateProfileIntA("system", "overwritecheck", 0, ini) ? 1 : 0;
    st.checks[17] = GetPrivateProfileIntA("system", "threadcpuseparate", 0, ini) ? 1 : 0;
    st.checks[18] = GetPrivateProfileIntA("system", "dragdropdialog", 0, ini) ? 1 : 0;
    st.checks[19] = GetPrivateProfileIntA("system", "openprojectaup", 1, ini) ? 1 : 0;
    st.checks[20] = GetPrivateProfileIntA("system", "closedialog", 0, ini) ? 1 : 0;
    st.checks[21] = GetPrivateProfileIntA("system", "projectonfig", 0, ini) ? 1 : 0;
    st.checks[22] = GetPrivateProfileIntA("system", "windowsnap", 1, ini) ? 1 : 0;
    st.checks[23] = GetPrivateProfileIntA("system", "movieplaymain", 0, ini) ? 1 : 0;
    st.checks[24] = GetPrivateProfileIntA("system", "dragdropactive", 1, ini) ? 1 : 0;
    st.checks[25] = GetPrivateProfileIntA("system", "trackbarclick", 1, ini) ? 1 : 0;

    if (aviutl_dlg_run(owner, "SYSTEM", &st) != IDOK)
        return;

    if (st.ints[10] < 0)
        st.ints[10] = 0;
    if (st.ints[10] > 64)
        st.ints[10] = 64;

    ini_write_int("system", "width", st.ints[0], ini);
    ini_write_int("system", "height", st.ints[1], ini);
    ini_write_int("system", "frame", st.ints[2], ini);
    ini_write_int("system", "sharecache", st.ints[3], ini);
    ini_write_int("system", "moveA", st.ints[5], ini);
    ini_write_int("system", "moveB", st.ints[6], ini);
    ini_write_int("system", "moveC", st.ints[7], ini);
    ini_write_int("system", "moveD", st.ints[8], ini);
    ini_write_int("system", "saveunitsize", st.ints[9], ini);
    ini_write_int("system", "multithread", st.ints[10], ini);

    ini_write_int("system", "compprofile", st.checks[1] > 0 ? 1 : 0, ini);
    ini_write_int("system", "plugincache", st.checks[2] > 0 ? 1 : 0, ini);
    ini_write_int("system", "startframe", st.checks[3] > 0 ? 1 : 0, ini);
    ini_write_int("system", "shiftselect", st.checks[4] > 0 ? 1 : 0, ini);
    ini_write_int("system", "yuy2mode", st.checks[5] > 0 ? 1 : 0, ini);
    ini_write_int("system", "sse", st.checks[6] > 0 ? 1 : 0, ini);
    ini_write_int("system", "sse2", st.checks[7] > 0 ? 1 : 0, ini);
    ini_write_int("system", "vfplugin", st.checks[8] > 0 ? 1 : 0, ini);
    ini_write_int("system", "editresume", st.checks[9] > 0 ? 1 : 0, ini);
    ini_write_int("system", "fpsnoconvert", st.checks[10] > 0 ? 1 : 0, ini);
    ini_write_int("system", "tempconfig", st.checks[11] > 0 ? 1 : 0, ini);
    ini_write_int("system", "load30fps", st.checks[12] > 0 ? 1 : 0, ini);
    ini_write_int("system", "loadfpsadjust", st.checks[13] > 0 ? 1 : 0, ini);
    ini_write_int("system", "yuy2limit", st.checks[14] > 0 ? 1 : 0, ini);
    ini_write_int("system", "overwritecheck", st.checks[15] > 0 ? 1 : 0, ini);
    ini_write_int("system", "threadcpuseparate", st.checks[17] > 0 ? 1 : 0, ini);
    ini_write_int("system", "dragdropdialog", st.checks[18] > 0 ? 1 : 0, ini);
    ini_write_int("system", "openprojectaup", st.checks[19] > 0 ? 1 : 0, ini);
    ini_write_int("system", "closedialog", st.checks[20] > 0 ? 1 : 0, ini);
    ini_write_int("system", "projectonfig", st.checks[21] > 0 ? 1 : 0, ini);
    ini_write_int("system", "windowsnap", st.checks[22] > 0 ? 1 : 0, ini);
    ini_write_int("system", "movieplaymain", st.checks[23] > 0 ? 1 : 0, ini);
    ini_write_int("system", "dragdropactive", st.checks[24] > 0 ? 1 : 0, ini);
    ini_write_int("system", "trackbarclick", st.checks[25] > 0 ? 1 : 0, ini);

    aviutl_dlg_get_str_sjis(&st, 1, sbuf, sizeof(sbuf));
    WritePrivateProfileStringA("system", "defaultsavefile", sbuf, ini);
    aviutl_dlg_get_str_sjis(&st, 2, sbuf, sizeof(sbuf));
    WritePrivateProfileStringA("system", "finishsound", sbuf, ini);
    aviutl_dlg_get_str_sjis(&st, 3, sbuf, sizeof(sbuf));
    WritePrivateProfileStringA("system", "resizelist", sbuf, ini);
    aviutl_dlg_get_str_sjis(&st, 4, sbuf, sizeof(sbuf));
    WritePrivateProfileStringA("system", "fpslist", sbuf, ini);

    preview_reload_sys_config(owner);
    exfunc_reload_system_settings();
    if (plugin_manager_is_bridged())
    {
        plugin_bridge_invalidate_caches();
    }
    refresh_current_frame(owner);
}

static BOOL on_exout_dlg_cmd(HWND hdlg, int cmd_id, AviUtlDlgState *st, void *user)
{
    (void)user;
    if (cmd_id == AVIUTL_DLG_CMD_INIT || cmd_id == 32500)
    {
        AuoPluginSummary sum;
        BOOL en = FALSE;
        if (st->combo_sel[0] >= 0 && video_export_get_auo_summary(st->combo_sel[0], &sum))
        {
            en = sum.has_config;
        }
        EnableWindow(GetDlgItem(hdlg, 32300), en);
    }
    else if (cmd_id == 32300) // "設定" button
    {
        if (st->combo_sel[0] >= 0)
        {
            video_export_auo_config_dialog(hdlg, st->combo_sel[0]);
        }
    }
    return TRUE;
}

// 5399: Plugin Output Settings ("EXOUT") dialog
static void run_exout_config_dialog(HWND owner)
{
    int count = video_export_get_auo_count();
    wchar_t names[AUO_MAX_PLUGINS][256];
    const wchar_t *ptrs[AUO_MAX_PLUGINS + 1];
    AviUtlDlgState st;
    int i, n = 0;

    if (count <= 0)
    {
        aviutl_msgbox_w(owner, i18n_get_wstr(1026), L"AviUtl", MB_OK | MB_ICONWARNING);
        return;
    }

    for (i = 0; i < count && n < AUO_MAX_PLUGINS; i++)
    {
        AuoPluginSummary sum;
        if (!video_export_get_auo_summary(i, &sum))
            continue;
        sjis_to_wide_str(sum.name, names[n], 256);
        ptrs[n] = names[n];
        n++;
    }
    ptrs[n] = NULL;
    if (n <= 0)
        return;

    aviutl_dlg_init(&st);
    st.combo_items[0] = ptrs;
    st.combo_sel[0] = video_export_get_last_auo_index();
    st.on_cmd = on_exout_dlg_cmd;

    if (aviutl_dlg_run(owner, "EXOUT", &st) == IDOK && st.combo_sel[0] >= 0)
    {
        video_export_set_last_auo_index(st.combo_sel[0]);
    }
}

// 5131: Jump to Frame ("MOVE") dialog (Ctrl+G)
static void run_move_dialog(HWND owner)
{
    AviUtlDlgState st;
    const AviUtlSystemConfig *scfg;
    int sf;

    if (!g_editp || !editp_is_editing(g_editp))
        return;
    scfg = preview_get_sys_config();
    sf = scfg ? scfg->startframe : 1;

    aviutl_dlg_init(&st);
    st.ints[0] = editp_get_frame(g_editp) + sf;
    if (aviutl_dlg_run(owner, "MOVE", &st) == IDOK)
    {
        seek_to_frame_ex(owner, st.ints[0] - sf, FALSE);
    }
}

// 5115: Specify Selection Range ("SELECT") dialog
static void run_select_dialog(HWND owner)
{
    AviUtlDlgState st;
    const AviUtlSystemConfig *scfg;
    int sf, s = 0, e = 0, fn;

    if (!g_editp || !editp_is_editing(g_editp))
        return;
    fn = editp_get_frame_n(g_editp);
    if (fn <= 0)
        return;
    scfg = preview_get_sys_config();
    sf = scfg ? scfg->startframe : 1;

    if (!editp_get_select_frame(g_editp, &s, &e) || s < 0 || e < s)
    {
        s = 0;
        e = fn - 1;
    }

    aviutl_dlg_init(&st);
    st.ints[0] = s + sf;
    st.ints[1] = e + sf;
    if (aviutl_dlg_run(owner, "SELECT", &st) == IDOK)
    {
        int ns = st.ints[0] - sf;
        int ne = st.ints[1] - sf;
        if (ns < 0)
            ns = 0;
        if (ne >= fn)
            ne = fn - 1;
        if (ns <= ne)
        {
            editp_set_select_frame(g_editp, ns, ne);
            plugin_manager_notify_update(g_editp);
            refresh_current_frame(owner);
        }
    }
}

// 5398: Change Playback Speed Info ("FILE_INFO") dialog
static void run_playback_speed_dialog(HWND owner)
{
    AviUtlDlgState st;
    int rate = 30, scale = 1, arate = 0, ach = 2;
    double fps;
    char buf[64];

    if (!g_editp || !editp_is_editing(g_editp))
        return;

    editp_get_fps(g_editp, &rate, &scale);
    if (rate <= 0)
        rate = 30;
    if (scale <= 0)
        scale = 1;
    fps = (double)rate / (double)scale;
    arate = editp_get_audio_rate(g_editp);
    ach = editp_get_audio_ch(g_editp);

    aviutl_dlg_init(&st);
    _snprintf(buf, sizeof(buf), "%.4f", fps);
    buf[sizeof(buf) - 1] = '\0';
    // trim trailing zeros after decimal
    {
        char *dot = strchr(buf, '.');
        if (dot)
        {
            char *end = buf + strlen(buf) - 1;
            while (end > dot && *end == '0')
                *end-- = '\0';
            if (end == dot)
                *end = '\0';
        }
    }
    aviutl_dlg_set_str_sjis(&st, 0, buf);
    {
        char disp_buf[64];
        _snprintf(disp_buf, sizeof(disp_buf), "(%s fps)", buf);
        disp_buf[sizeof(disp_buf) - 1] = '\0';
        aviutl_dlg_set_str_sjis(&st, 2, disp_buf);
    }

    wsprintfA(buf, "%d", arate);
    aviutl_dlg_set_str_sjis(&st, 1, buf);
    wsprintfA(buf, "(%d Hz)", arate);
    aviutl_dlg_set_str_sjis(&st, 3, buf);

    if (aviutl_dlg_run(owner, "FILE_INFO", &st) == IDOK)
    {
        double nfps;
        int narate;
        aviutl_dlg_get_str_sjis(&st, 0, buf, sizeof(buf));
        nfps = atof(buf);
        if (nfps > 0.001 && nfps <= 10000.0)
        {
            // preserve exact NTSC rationals when close to 29.97 / 59.94 / 23.976
            if (nfps > 29.969 && nfps < 29.971)
                editp_set_fps(g_editp, 30000, 1001);
            else if (nfps > 59.939 && nfps < 59.941)
                editp_set_fps(g_editp, 60000, 1001);
            else if (nfps > 23.975 && nfps < 23.977)
                editp_set_fps(g_editp, 24000, 1001);
            else
                editp_set_fps(g_editp, (int)(nfps * 10000.0 + 0.5), 10000);
        }
        aviutl_dlg_get_str_sjis(&st, 1, buf, sizeof(buf));
        narate = atoi(buf);
        if (narate > 0 && editp_has_audio(g_editp))
        {
            editp_set_audio_format(g_editp, narate, ach > 0 ? ach : 2, TRUE);
        }
        plugin_manager_notify_update(g_editp);
        refresh_current_frame(owner);
    }
}

// 101: Filter Plugin Information (0x00405450 case 0x65)
static void show_filter_plugin_info(HWND owner)
{
    wchar_t buf[8192];
    int i, fcount = plugin_manager_get_count();
    int added = 0;

    buf[0] = L'\0';
    for (i = 0; i < fcount; i++)
    {
        const char *inf = plugin_manager_get_information(i);
        const char *nm = plugin_manager_get_name(i);
        const char *pth = plugin_manager_get_path(i);
        const char *base = aviutl_basename(pth);
        wchar_t winf[256], wbase[256], line[600];

        if (plugin_manager_is_builtin(i))
            continue;

        sjis_to_wide_str((inf && inf[0]) ? inf : (nm ? nm : ""), winf, 256);
        sjis_to_wide_str(base, wbase, 256);
        if (wbase[0])
            wsprintfW(line, L"%s ( %s )\n", winf, wbase);
        else
            wsprintfW(line, L"%s\n", winf);
        if (lstrlenW(buf) + lstrlenW(line) + 2 < 8190)
        {
            lstrcatW(buf, line);
            added++;
        }
    }
    if (added == 0)
    {
        lstrcpynW(buf, i18n_get_wstr(1020), 256);
    }
    aviutl_msgbox_w(owner, buf, i18n_get_wstr(1029), MB_OK | MB_ICONINFORMATION);
}

// "WAV_OPTION" dialog callback
static BOOL run_wav_option_dialog(HWND owner, int *inout_w, int *inout_h)
{
    AviUtlDlgState st;
    if (!inout_w || !inout_h)
        return FALSE;
    aviutl_dlg_init(&st);
    st.ints[0] = *inout_w;
    st.ints[1] = *inout_h;
    if (aviutl_dlg_run(owner, "WAV_OPTION", &st) != IDOK)
        return FALSE;
    *inout_w = (st.ints[0] > 0) ? st.ints[0] : 0;
    *inout_h = (st.ints[1] > 0) ? st.ints[1] : 0;
    return TRUE;
}

typedef struct InputListDlgCtx
{
    int reg_idx[INPUT_PLUGIN_MAX];
    int ord[INPUT_PLUGIN_MAX];
    int count;
} InputListDlgCtx;

static int input_list_selected_plugin(HWND hdlg, const InputListDlgCtx *ctx)
{
    int sel, k;
    if (!ctx || ctx->count <= 0)
        return -1;
    sel = (int)SendDlgItemMessageW(hdlg, 32600, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= ctx->count)
        return -1;
    for (k = 0; k < ctx->count; k++)
    {
        if (ctx->ord[k] == sel)
            return ctx->reg_idx[k];
    }
    return -1;
}

static BOOL on_input_list_dlg_cmd(HWND hdlg, int cmd_id, AviUtlDlgState *st, void *user)
{
    InputListDlgCtx *ctx = (InputListDlgCtx *)user;
    (void)st;
    if (!ctx)
        return TRUE;

    if (cmd_id == 32708) // "設定" button
    {
        int pidx = input_list_selected_plugin(hdlg, ctx);
        if (pidx >= 0)
        {
            input_plugin_config(pidx, hdlg);
        }
        return TRUE;
    }

    {
        InputPluginSummary sum;
        int pidx = input_list_selected_plugin(hdlg, ctx);
        BOOL en = (pidx >= 0 && input_plugin_get_summary(pidx, &sum) && sum.has_config);
        EnableWindow(GetDlgItem(hdlg, 32708), en);
    }
    return TRUE;
}

// 1086: Input Plugin Priority ("INPUTLIST") dialog (0x00405a60)
static void run_input_priority_dialog(HWND owner)
{
    InputListDlgCtx ctx;
    wchar_t names[INPUT_PLUGIN_MAX][256];
    const wchar_t *ptrs[INPUT_PLUGIN_MAX + 1];
    int prio_order[INPUT_PLUGIN_MAX];
    int prio_n;
    AviUtlDlgState st;
    int i, k, total;

    ZeroMemory(&ctx, sizeof(ctx));
    total = input_plugin_get_count();
    for (i = 0; i < total && ctx.count < INPUT_PLUGIN_MAX; i++)
    {
        InputPluginSummary sum;
        if (!input_plugin_get_summary(i, &sum))
            continue;
        if (sum.flag & INPUT_PLUGIN_FLAG_HIDE_LIST)
            continue;
        sjis_to_wide_str(sum.name[0] ? sum.name : "Input", names[ctx.count], 256);
        ptrs[ctx.count] = names[ctx.count];
        ctx.reg_idx[ctx.count] = i;
        ctx.ord[ctx.count] = ctx.count;
        ctx.count++;
    }
    ptrs[ctx.count] = NULL;
    if (ctx.count <= 0)
        return;

    prio_n = input_plugin_get_priority_order(prio_order, INPUT_PLUGIN_MAX, FALSE);
    for (i = 0; i < prio_n; i++)
    {
        for (k = 0; k < ctx.count; k++)
        {
            if (ctx.reg_idx[k] == prio_order[i])
            {
                ctx.ord[k] = i;
                break;
            }
        }
    }

    aviutl_dlg_init(&st);
    st.list_items[0] = ptrs;
    st.list_order[0] = ctx.ord;
    st.list_count[0] = ctx.count;
    st.on_cmd = on_input_list_dlg_cmd;
    st.user = &ctx;

    if (aviutl_dlg_run(owner, "INPUTLIST", &st) == IDOK)
    {
        int new_order[INPUT_PLUGIN_MAX];
        for (i = 0; i < ctx.count; i++)
        {
            new_order[i] = ctx.reg_idx[i];
            for (k = 0; k < ctx.count; k++)
            {
                if (ctx.ord[k] == i)
                {
                    new_order[i] = ctx.reg_idx[k];
                    break;
                }
            }
        }
        input_plugin_apply_priority_order(new_order, ctx.count, TRUE);
    }
}

// 103: Input Plugin Information (0x00405450 case 0x67)
static void show_input_plugin_info(HWND owner)
{
    wchar_t buf[8192];
    int i, count = input_plugin_get_ext_count();
    int added = 0;

    buf[0] = L'\0';
    for (i = 0; i < count; i++)
    {
        InputPluginSummary sum;
        const char *base;
        wchar_t winf[256], wbase[256], line[600];
        if (!input_plugin_get_ext_summary(i, &sum))
            continue;
        base = aviutl_basename(sum.path);
        sjis_to_wide_str(sum.information[0] ? sum.information : sum.name, winf, 256);
        sjis_to_wide_str(base, wbase, 256);
        if (wbase[0])
            wsprintfW(line, L"%s ( %s )\n", winf, wbase);
        else
            wsprintfW(line, L"%s\n", winf);
        if (lstrlenW(buf) + lstrlenW(line) + 2 < 8190)
        {
            lstrcatW(buf, line);
            added++;
        }
    }
    if (added == 0)
    {
        lstrcpynW(buf, i18n_get_wstr(1020), 256);
    }
    aviutl_msgbox_w(owner, buf, i18n_get_wstr(1030), MB_OK | MB_ICONINFORMATION);
}

// 110: Color Conversion Plugin Information (0x00405450 case 0x6e)
static void show_color_plugin_info(HWND owner)
{
    wchar_t buf[8192];
    int i, count = color_plugin_get_ext_count();
    int added = 0;

    buf[0] = L'\0';
    for (i = 0; i < count; i++)
    {
        ColorPluginSummary sum;
        const char *base;
        wchar_t winf[256], wbase[256], line[600];
        if (!color_plugin_get_ext_summary(i, &sum))
            continue;
        base = aviutl_basename(sum.path);
        sjis_to_wide_str(sum.information[0] ? sum.information : sum.name, winf, 256);
        sjis_to_wide_str(base, wbase, 256);
        if (wbase[0])
            wsprintfW(line, L"%s ( %s )\n", winf, wbase);
        else
            wsprintfW(line, L"%s\n", winf);
        if (lstrlenW(buf) + lstrlenW(line) + 2 < 8190)
        {
            lstrcatW(buf, line);
            added++;
        }
    }
    if (added == 0)
    {
        lstrcpynW(buf, i18n_get_wstr(1020), 256);
    }
    aviutl_msgbox_w(owner, buf, i18n_get_wstr(1044), MB_OK | MB_ICONINFORMATION);
}

// 105: Output Plugin Information (0x00405450 case 0x69)
static void show_output_plugin_info(HWND owner)
{
    wchar_t buf[8192];
    int i, count = video_export_get_auo_count();
    int added = 0;

    buf[0] = L'\0';
    for (i = 0; i < count; i++)
    {
        AuoPluginSummary sum;
        const char *base;
        wchar_t winf[256], wbase[256], line[600];
        if (!video_export_get_auo_summary(i, &sum))
            continue;
        base = aviutl_basename(sum.path);
        sjis_to_wide_str(sum.information[0] ? sum.information : sum.name, winf, 256);
        sjis_to_wide_str(base, wbase, 256);
        if (wbase[0])
            wsprintfW(line, L"%s ( %s )\n", winf, wbase);
        else
            wsprintfW(line, L"%s\n", winf);
        if (lstrlenW(buf) + lstrlenW(line) + 2 < 8190)
        {
            lstrcatW(buf, line);
            added++;
        }
    }
    if (added == 0)
    {
        lstrcpynW(buf, i18n_get_wstr(1020), 256);
    }
    aviutl_msgbox_w(owner, buf, i18n_get_wstr(1031), MB_OK | MB_ICONINFORMATION);
}

// 1006: Current File Information
static void show_current_file_info(HWND owner)
{
    wchar_t buf[2048];
    int w = 0, h = 0, mw = 0, mh = 0, rate = 30, scale = 1, fn = 0;
    wchar_t wpath[MAX_PATH], wfps[64];
    int cs;

    if (!g_editp || !editp_is_editing(g_editp) || editp_get_frame_n(g_editp) <= 0)
    {
        aviutl_msgbox_w(owner, i18n_get_wstr(1021), i18n_get_wstr(1028), MB_OK | MB_ICONINFORMATION);
        return;
    }

    editp_get_dimensions(g_editp, &w, &h, &mw, &mh);
    editp_get_fps(g_editp, &rate, &scale);
    fn = editp_get_frame_n(g_editp);
    if (rate <= 0)
        rate = 30;
    if (scale <= 0)
        scale = 1;
    cs = (int)(((double)scale * (double)fn * 100.0) / (double)rate);
    {
        char fbuf[64];
        _snprintf(fbuf, sizeof(fbuf), "%.3f", (double)rate / (double)scale);
        fbuf[sizeof(fbuf) - 1] = '\0';
        sjis_to_wide_str(fbuf, wfps, 64);
    }
    sjis_to_wide_str(editp_get_edit_path(g_editp), wpath, MAX_PATH);

    if (editp_has_audio(g_editp))
    {
        _snwprintf(buf, 2048,
                   L"%s\n\n"
                   L"%s : %dx%d\n"
                   L"%s : %02d:%02d:%02d.%02d\n"
                   L"%s : %s fps\n"
                   L"%s : %d\n"
                   L"%s : %d Hz\n"
                   L"%s : %d ch",
                   wpath,
                   i18n_get_wstr(34), w, h,
                   i18n_get_wstr(35), cs / 360000, (cs / 6000) % 60, (cs / 100) % 60, cs % 100,
                   i18n_get_wstr(36), wfps,
                   i18n_get_wstr(37), fn,
                   i18n_get_wstr(39), editp_get_audio_rate(g_editp),
                   i18n_get_wstr(40), editp_get_audio_ch(g_editp));
    }
    else
    {
        _snwprintf(buf, 2048,
                   L"%s\n\n"
                   L"%s : %dx%d\n"
                   L"%s : %02d:%02d:%02d.%02d\n"
                   L"%s : %s fps\n"
                   L"%s : %d",
                   wpath,
                   i18n_get_wstr(34), w, h,
                   i18n_get_wstr(35), cs / 360000, (cs / 6000) % 60, (cs / 100) % 60, cs % 100,
                   i18n_get_wstr(36), wfps,
                   i18n_get_wstr(37), fn);
    }
    buf[2047] = L'\0';
    aviutl_msgbox_w(owner, buf, i18n_get_wstr(1028), MB_OK | MB_ICONINFORMATION);
}

static void sync_edit_menu_states(HMENU hMenu)
{
    BOOL active_edit = (g_editp && editp_is_editing(g_editp) && !editp_is_saving(g_editp));
    static const UINT edit_only_ids[] = {
        5157, 5100, 5168, 1023, 1024, 1003, 1062, 5167, 1054,
        1020, 5117, 1026, 5123, 1070, 5131, 5115, 5184, 5143, 5101,
        5147, 5112, 5109, 5126, 5125, 5129, 5130, 5132, 5156, 5398,
        5196, 5197, 5198, 5199, 5200, 5201,
        5206, 5207, 5208, 5209, 5210, 5211, 5212, 5213, 5214, 5215};
    int k;

    if (!hMenu)
        return;

    for (k = 0; k < (int)(sizeof(edit_only_ids) / sizeof(edit_only_ids[0])); k++)
    {
        EnableMenuItem(hMenu, edit_only_ids[k],
                       MF_BYCOMMAND | (active_edit ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
    }
    EnableMenuItem(hMenu, 1009,
                   MF_BYCOMMAND | ((g_editp && editp_is_saving(g_editp)) ? MF_ENABLED : (MF_GRAYED | MF_DISABLED)));
    EnableMenuItem(hMenu, PROFILE_CMD_DELETE,
                   MF_BYCOMMAND | ((profile_cfg_get_active() > 0 && !(g_editp && editp_is_saving(g_editp)))
                                       ? MF_ENABLED
                                       : (MF_GRAYED | MF_DISABLED)));

    if (!active_edit)
        return;

    for (k = 1; k <= 9; k++)
    {
        if (!profile_cfg_has_project_slot(k))
        {
            EnableMenuItem(hMenu, (UINT)(5206 + k), MF_BYCOMMAND | MF_GRAYED | MF_DISABLED);
        }
    }

    if (!editp_can_undo(g_editp))
        EnableMenuItem(hMenu, 5147, MF_BYCOMMAND | MF_GRAYED | MF_DISABLED);
    if (!editp_has_copied_frame_image(g_editp))
    {
        EnableMenuItem(hMenu, 5117, MF_BYCOMMAND | MF_GRAYED | MF_DISABLED);
        EnableMenuItem(hMenu, 5184, MF_BYCOMMAND | MF_GRAYED | MF_DISABLED);
    }
    if (!IsClipboardFormatAvailable(CF_DIB))
    {
        EnableMenuItem(hMenu, 5123, MF_BYCOMMAND | MF_GRAYED | MF_DISABLED);
    }

    {
        int cur = editp_get_frame(g_editp);
        BYTE st = 0;
        BYTE inter = editp_get_frame_inter(g_editp, cur);
        BYTE cfg = editp_get_frame_config(g_editp, cur);

        editp_get_frame_status(g_editp, cur, &st);
        CheckMenuItem(hMenu, 5129, MF_BYCOMMAND | ((st & EDIT_FRAME_EDIT_FLAG_KEYFRAME) ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, 5130, MF_BYCOMMAND | ((st & EDIT_FRAME_EDIT_FLAG_MARKFRAME) ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, 5132, MF_BYCOMMAND | ((st & EDIT_FRAME_EDIT_FLAG_DELFRAME) ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(hMenu, 5156, MF_BYCOMMAND | ((st & EDIT_FRAME_EDIT_FLAG_NULLFRAME) ? MF_CHECKED : MF_UNCHECKED));

        for (k = 0; k < 6; k++)
        {
            CheckMenuItem(hMenu, (UINT)(5196 + k),
                          MF_BYCOMMAND | ((inter == (BYTE)k) ? MF_CHECKED : MF_UNCHECKED));
        }
        for (k = 0; k < 10; k++)
        {
            CheckMenuItem(hMenu, (UINT)(5206 + k),
                          MF_BYCOMMAND | ((cfg == (BYTE)k) ? MF_CHECKED : MF_UNCHECKED));
        }
    }
}

// show aviutl right-click "CONTEXT" popup menu (0x00412300)
static void cb_batch_ui_refresh(HWND hwnd)
{
    g_last_auto_w = 0;
    g_last_auto_h = 0;
    apply_menu(hwnd);
    refresh_current_frame(hwnd);
}

// 0x0040fa20: AviUtl 1.10 command-line & .aus script parser
static const char *parse_cli_token(const char *s, char *out, size_t cap)
{
    size_t k = 0;
    char delim = ' ';

    if (out && cap > 0)
        out[0] = '\0';
    if (!s)
        return "";

    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s)
        return s;

    if (*s == '"')
    {
        delim = '"';
        s++;
    }

    while (*s && *s != delim && !(delim == ' ' && *s == '\t'))
    {
        if (out && k + 1 < cap)
            out[k++] = *s;
        s++;
    }
    if (out && cap > 0)
        out[k] = '\0';
    if (*s == '"')
        s++;
    return s;
}

static void execute_cli_line(HWND hwnd, const char *line, int depth, BOOL *io_quit)
{
    char in_path[MAX_PATH] = {0};
    char audio_path[MAX_PATH] = {0};
    char profile_name[MAX_PATH] = {0};
    char out_path[MAX_PATH] = {0};
    char out_mode = 'a';
    BOOL is_batch = FALSE;
    BOOL batch_start = FALSE;
    BOOL local_quit = FALSE;
    int range_start = -1;
    int range_end = -1;
    const char *p = line;

    if (!p || depth > 8)
        return;

    while (*p)
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;

        if (*p == '-' || *p == '/')
        {
            char opt = p[1];
            p += 2;
            switch (opt)
            {
            case 'a':
            case 'A':
                p = parse_cli_token(p, audio_path, sizeof(audio_path));
                break;

            case 'p':
            case 'P':
                p = parse_cli_token(p, profile_name, sizeof(profile_name));
                break;

            case 'q':
            case 'Q':
                local_quit = TRUE;
                if (io_quit)
                    *io_quit = TRUE;
                while (*p && *p != ' ' && *p != '\t')
                    p++;
                break;

            case 'r':
            case 'R':
            {
                int v = 0;
                while (*p == ' ' || *p == '\t')
                    p++;
                range_start = 0;
                while (*p >= '0' && *p <= '9')
                {
                    v = v * 10 + (*p - '0');
                    p++;
                }
                range_start = v;
                while (*p == ' ' || *p == '\t')
                    p++;
                if (*p == ',')
                {
                    int v2 = 0;
                    p++;
                    while (*p == ' ' || *p == '\t')
                        p++;
                    range_end = 0;
                    while (*p >= '0' && *p <= '9')
                    {
                        v2 = v2 * 10 + (*p - '0');
                        p++;
                    }
                    range_end = v2;
                }
                break;
            }

            case 'b':
            case 'B':
                if ((*p == 's' || *p == 'S') &&
                    (p[1] == '\0' || p[1] == ' ' || p[1] == '\t'))
                {
                    batch_start = TRUE;
                    p++;
                    break;
                }
                is_batch = TRUE;
                out_mode = 'a';
                if ((*p == 'a' || *p == 'A' || *p == 'w' || *p == 'W' || *p == 'p' || *p == 'P') &&
                    (p[1] == '\0' || p[1] == ' ' || p[1] == '\t' || p[1] == '"'))
                {
                    out_mode = (char)((*p >= 'A' && *p <= 'Z') ? (*p - 'A' + 'a') : *p);
                    p++;
                }
                p = parse_cli_token(p, out_path, sizeof(out_path));
                break;

            case 'o':
            case 'O':
                is_batch = FALSE;
                out_mode = 'a';
                if ((*p == 'a' || *p == 'A' || *p == 'w' || *p == 'W' || *p == 'p' || *p == 'P') &&
                    (p[1] == '\0' || p[1] == ' ' || p[1] == '\t' || p[1] == '"'))
                {
                    out_mode = (char)((*p >= 'A' && *p <= 'Z') ? (*p - 'A' + 'a') : *p);
                    p++;
                }
                p = parse_cli_token(p, out_path, sizeof(out_path));
                break;

            default:
                while (*p && *p != ' ' && *p != '\t')
                    p++;
                break;
            }
        }
        else
        {
            p = parse_cli_token(p, in_path, sizeof(in_path));
        }
    }

    if (local_quit || (io_quit && *io_quit))
    {
        g_cli_no_confirm = TRUE;
    }

    if (profile_name[0])
    {
        int pidx = profile_cfg_find_by_name(profile_name);
        if (pidx >= 0)
        {
            profile_cfg_select(pidx, TRUE);
            exfunc_invalidate_all_caches();
            if (plugin_manager_is_bridged())
                plugin_bridge_invalidate_caches();
            apply_menu(hwnd);
        }
    }

    if (in_path[0])
    {
        const char *ext = gui_path_ext_dbcs(in_path);
        if (lstrcmpiA(ext, ".aus") == 0)
        {
            FILE *fp = fopen(in_path, "rb");
            if (fp)
            {
                char sline[2048];
                while (fgets(sline, sizeof(sline), fp))
                {
                    char *end = sline + strlen(sline);
                    char *lead = sline;
                    while (end > sline && (end[-1] == '\r' || end[-1] == '\n'))
                        *--end = '\0';
                    if ((unsigned char)lead[0] == 0xEF &&
                        (unsigned char)lead[1] == 0xBB &&
                        (unsigned char)lead[2] == 0xBF)
                    {
                        lead += 3;
                    }
                    while (*lead == ' ' || *lead == '\t')
                        lead++;
                    if (!*lead || *lead == '#' || *lead == ';')
                        continue;
                    execute_cli_line(hwnd, lead, depth + 1, io_quit);
                }
                fclose(fp);
            }
        }
        else if (lstrcmpiA(ext, ".aup") == 0)
        {
            open_media_or_exo_ex(hwnd, in_path, TRUE);
        }
        else
        {
            open_media_or_exo_ex(hwnd, in_path, FALSE);
        }
    }

    if (audio_path[0] && g_editp)
    {
        load_audio_into_current_edit(hwnd, audio_path);
    }

    if (range_start >= 0 && g_editp && editp_is_editing(g_editp))
    {
        int sf = preview_get_sys_config() ? preview_get_sys_config()->startframe : 1;
        int fn = editp_get_frame_n(g_editp);
        int cur_s = 0, cur_e = (fn > 0) ? (fn - 1) : 0;
        int ns, ne;

        editp_get_select_frame(g_editp, &cur_s, &cur_e);
        ns = range_start - sf;
        ne = (range_end >= 0) ? (range_end - sf) : cur_e;
        if (ns < 0)
            ns = 0;
        if (fn > 0 && ns >= fn)
            ns = fn - 1;
        if (ne < 0)
            ne = 0;
        if (fn > 0 && ne >= fn)
            ne = fn - 1;
        if (ne < ns)
            ne = ns;
        editp_set_select_frame(g_editp, ns, ne);
        plugin_manager_notify_update(g_editp);
        refresh_current_frame(hwnd);
    }

    if (out_path[0] && g_editp && editp_is_editing(g_editp))
    {
        BOOL prev_no_confirm = g_cli_no_confirm;
        g_cli_no_confirm = TRUE;
        if (is_batch)
        {
            if (out_mode == 'w')
            {
                avi_tools_batch_register_ex(hwnd, g_editp, out_path, NULL,
                                            OUTPUT_INFO_FLAG_AUDIO | OUTPUT_INFO_FLAG_BATCH, NULL);
            }
            else if (out_mode == 'p')
            {
                int auo_idx = video_export_get_last_auo_index();
                const char *auo_name = video_export_get_auo_name(auo_idx);
                int fl = OUTPUT_INFO_FLAG_VIDEO |
                         (editp_has_audio(g_editp) ? OUTPUT_INFO_FLAG_AUDIO : 0) |
                         OUTPUT_INFO_FLAG_BATCH;
                avi_tools_batch_register_ex(hwnd, g_editp, out_path,
                                            auo_name ? auo_name : "", fl, NULL);
            }
            else
            {
                int fl = OUTPUT_INFO_FLAG_VIDEO |
                         (editp_has_audio(g_editp) ? OUTPUT_INFO_FLAG_AUDIO : 0) |
                         OUTPUT_INFO_FLAG_BATCH;
                avi_tools_batch_register_current(hwnd, g_editp, out_path, fl, NULL);
            }
        }
        else
        {
            int batch_bit = (local_quit || (io_quit && *io_quit) || depth > 0) ? OUTPUT_INFO_FLAG_BATCH : 0;
            if (out_mode == 'w')
            {
                video_export_wav_file(hwnd, g_editp, out_path);
            }
            else if (out_mode == 'p')
            {
                int auo_idx = video_export_get_last_auo_index();
                int fl = OUTPUT_INFO_FLAG_VIDEO |
                         (editp_has_audio(g_editp) ? OUTPUT_INFO_FLAG_AUDIO : 0) |
                         batch_bit;
                if (auo_idx >= 0)
                    video_export_auo_file(hwnd, g_editp, auo_idx, out_path, fl);
            }
            else
            {
                int fl = OUTPUT_INFO_FLAG_VIDEO |
                         (editp_has_audio(g_editp) ? OUTPUT_INFO_FLAG_AUDIO : 0) |
                         batch_bit;
                video_export_avi_file(hwnd, g_editp, out_path, fl);
            }
        }
        g_cli_no_confirm = prev_no_confirm;
    }

    if (batch_start && g_editp)
    {
        BOOL auto_close = (local_quit || (io_quit && *io_quit) || depth > 0);
        avi_tools_run_batch_dialog_ex(hwnd, g_editp, cb_batch_ui_refresh, TRUE, auto_close);
    }
}

static void show_preview_context_menu(HWND hwnd, int screen_x, int screen_y)
{
    HMENU hMenu = i18n_load_menu("CONTEXT");
    HMENU hSub;

    if (!hMenu)
        return;

    hSub = GetSubMenu(hMenu, 0);
    if (hSub)
    {
        CheckMenuRadioItem(hSub, 9200, 9206, 9200 + preview_get_zoom(), MF_BYCOMMAND);
        sync_edit_menu_states(hSub);

        if (screen_x == -1 && screen_y == -1)
        {
            POINT pt = {32, 32};
            ClientToScreen(hwnd, &pt);
            screen_x = pt.x;
            screen_y = pt.y;
        }
        TrackPopupMenu(hSub, TPM_LEFTALIGN | TPM_RIGHTBUTTON,
                       screen_x, screen_y, 0, hwnd, NULL);
    }
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK AviUtlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        preview_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ENTERSIZEMOVE:
    case WM_EXITSIZEMOVE:
        if (winsnap_handle_message(hwnd, hwnd, msg, wp, lp))
            return 0;
        break;

    case WM_MOVING:
        if (winsnap_handle_message(hwnd, hwnd, msg, wp, lp))
            return TRUE;
        break;

    case WM_MOVE:
        winsnap_handle_message(hwnd, hwnd, msg, wp, lp);
        preview_on_size(hwnd, g_editp);
        plugin_manager_forward_main_misc(WM_FILTER_MAIN_MOVESIZE, 0, 0);
        return 0;

    case WM_SIZE:
        winsnap_handle_message(hwnd, hwnd, msg, wp, lp);
        preview_on_size(hwnd, g_editp);
        plugin_manager_forward_main_misc(WM_FILTER_MAIN_MOVESIZE, 0, 0);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_HSCROLL:
    {
        HWND hTb = preview_get_trackbar_hwnd();
        if (hTb && (HWND)lp == hTb)
        {
            int code = LOWORD(wp);
            if (code == SB_ENDSCROLL)
            {
                if (GetFocus() == hTb)
                {
                    SetFocus(hwnd);
                }
            }
            else if (g_editp && editp_is_editing(g_editp))
            {
                int pos = (int)SendMessageA(hTb, TBM_GETPOS, 0, 0);
                if (g_playing)
                    stop_playback(hwnd);
                seek_to_frame_ex(hwnd, pos, TRUE);
            }
            return 0;
        }
        break;
    }

    case MM_WOM_DONE:
        if (g_playing && g_waveout && (HWAVEOUT)wp == g_waveout && lp != 0)
        {
            fill_and_submit_wave_hdr((WAVEHDR *)lp);
        }
        return 0;

    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wp;
        UINT nfiles = DragQueryFileA(hDrop, 0xFFFFFFFFu, NULL, 0);
        UINT i;
        BOOL ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        BOOL drop_quit = FALSE;
        const AviUtlSystemConfig *scfg = preview_get_sys_config();

        if (scfg && scfg->dragdropactive)
        {
            SetForegroundWindow(hwnd);
        }
        for (i = 0; i < nfiles; i++)
        {
            char path[MAX_PATH] = {0};
            const char *ext;
            if (DragQueryFileA(hDrop, i, path, MAX_PATH) == 0 || !path[0])
                continue;
            ext = gui_path_ext_dbcs(path);

            if (lstrcmpiA(ext, ".aus") == 0)
            {
                char qline[MAX_PATH + 4];
                _snprintf(qline, sizeof(qline), "\"%s\"", path);
                qline[sizeof(qline) - 1] = '\0';
                execute_cli_line(hwnd, qline, 0, &drop_quit);
                continue;
            }

            if (i == 0)
            {
                if (is_pure_audio_ext(ext) && g_editp && editp_is_editing(g_editp))
                {
                    if (scfg && scfg->dragdropdialog && nfiles == 1)
                        reaviutl_audio_open_with_initial(hwnd, path);
                    else
                        load_audio_into_current_edit(hwnd, path);
                }
                else if (ctrl_down && g_editp && editp_is_editing(g_editp) &&
                         !(lstrcmpiA(ext, ".aup") == 0 || lstrcmpiA(ext, ".exo") == 0))
                {
                    if (scfg && scfg->dragdropdialog && nfiles == 1)
                        reaviutl_file_append_with_initial(hwnd, path);
                    else
                        append_video_direct(hwnd, path);
                }
                else if (scfg && scfg->dragdropdialog && nfiles == 1 &&
                         !(lstrcmpiA(ext, ".aup") == 0 || lstrcmpiA(ext, ".exo") == 0))
                {
                    reaviutl_file_open_with_initial(hwnd, path);
                }
                else
                {
                    open_media_or_exo_ex(hwnd, path, FALSE);
                }
            }
            else
            {
                if (is_pure_audio_ext(ext) && g_editp && editp_is_editing(g_editp))
                {
                    load_audio_into_current_edit(hwnd, path);
                }
                else if (lstrcmpiA(ext, ".exo") == 0)
                {
                    open_media_or_exo_ex(hwnd, path, FALSE);
                }
                else if (g_editp && editp_is_editing(g_editp))
                {
                    append_video_direct(hwnd, path);
                }
                else
                {
                    open_media_or_exo_ex(hwnd, path, FALSE);
                }
            }
        }
        DragFinish(hDrop);
        if (drop_quit)
        {
            g_cli_no_confirm = TRUE;
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == PLAY_TIMER_ID && g_playing && g_editp && editp_is_editing(g_editp))
        {
            int rate = 30, scale = 1, target_f;
            editp_get_fps(g_editp, &rate, &scale);
            if (rate <= 0)
                rate = 30;
            if (scale <= 0)
                scale = 1;

            if (g_waveout && g_play_audio_rate > 0)
            {
                MMTIME mmt;
                int64_t played_smp = 0;
                ZeroMemory(&mmt, sizeof(mmt));
                mmt.wType = TIME_SAMPLES;
                if (waveOutGetPosition(g_waveout, &mmt, sizeof(mmt)) == MMSYSERR_NOERROR)
                {
                    if (mmt.wType == TIME_SAMPLES)
                    {
                        played_smp = (int64_t)mmt.u.sample;
                    }
                    else if (mmt.wType == TIME_BYTES && g_play_audio_ch > 0)
                    {
                        played_smp = (int64_t)mmt.u.cb / (g_play_audio_ch * 2);
                    }
                }
                target_f = g_play_start_frame +
                           (int)((played_smp * (int64_t)rate) /
                                 ((int64_t)g_play_audio_rate * (int64_t)scale));
            }
            else
            {
                DWORD elapsed_ms = GetTickCount() - g_play_start_tick;
                target_f = g_play_start_frame +
                           (int)(((int64_t)elapsed_ms * (int64_t)rate) /
                                 (1000LL * (int64_t)scale));
            }

            if (target_f > g_play_end_frame)
            {
                editp_set_frame(g_editp, g_play_end_frame);
                plugin_manager_notify_update(g_editp);
                refresh_current_frame(hwnd);
                stop_playback(hwnd);
            }
            else if (target_f != editp_get_frame(g_editp))
            {
                editp_set_frame(g_editp, target_f);
                plugin_manager_notify_update(g_editp);
                refresh_current_frame(hwnd);
            }
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        int cx = (short)LOWORD(lp);
        int cy = (short)HIWORD(lp);
        int ix = 0, iy = 0;
        SetFocus(hwnd);
        if (preview_client_to_image(hwnd, cx, cy, &ix, &iy))
        {
            g_dragging_canvas = TRUE;
            SetCapture(hwnd);
            plugin_manager_forward_main_mouse(WM_FILTER_MAIN_MOUSE_DOWN, wp, ix, iy);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK:
    {
        int cx = (short)LOWORD(lp);
        int cy = (short)HIWORD(lp);
        int ix = 0, iy = 0;
        RECT area;
        preview_layout(hwnd, &area);
        if (cy < area.bottom && g_editp && editp_is_editing(g_editp))
        {
            // aviutl 0x00412300: double-click & hold peeks unfiltered source frame
            if (!(preview_get_flags() & PREVIEW_FLAG_NOFILTER))
            {
                g_peeking_unfiltered = TRUE;
                preview_set_peek_unfiltered(TRUE);
                SetCapture(hwnd);
                refresh_current_frame(hwnd);
            }
            if (preview_client_to_image(hwnd, cx, cy, &ix, &iy))
            {
                plugin_manager_forward_main_mouse(WM_FILTER_MAIN_MOUSE_DBLCLK, wp, ix, iy);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        int cx = (short)LOWORD(lp);
        int cy = (short)HIWORD(lp);
        if (g_peeking_unfiltered)
        {
            g_peeking_unfiltered = FALSE;
            preview_set_peek_unfiltered(FALSE);
            if (GetCapture() == hwnd)
                ReleaseCapture();
            refresh_current_frame(hwnd);
        }
        if (g_dragging_canvas)
        {
            g_dragging_canvas = FALSE;
            if (GetCapture() == hwnd)
                ReleaseCapture();
        }
        if (g_editp && editp_is_editing(g_editp))
        {
            int ix = 0, iy = 0;
            preview_client_to_image(hwnd, cx, cy, &ix, &iy);
            plugin_manager_forward_main_mouse(WM_FILTER_MAIN_MOUSE_UP, wp, ix, iy);
        }
        return 0;
    }

    case WM_RBUTTONDOWN:
    {
        int cx = (short)LOWORD(lp);
        int cy = (short)HIWORD(lp);
        int ix = 0, iy = 0;
        SetFocus(hwnd);
        if (preview_client_to_image(hwnd, cx, cy, &ix, &iy))
        {
            plugin_manager_forward_main_mouse(WM_FILTER_MAIN_MOUSE_R_DOWN, wp, ix, iy);
        }
        return 0;
    }

    case WM_RBUTTONUP:
    {
        int cx = (short)LOWORD(lp);
        int cy = (short)HIWORD(lp);
        int ix = 0, iy = 0;
        if (preview_client_to_image(hwnd, cx, cy, &ix, &iy))
        {
            if (plugin_manager_forward_main_mouse(WM_FILTER_MAIN_MOUSE_R_UP, wp, ix, iy))
            {
                return 0;
            }
        }
        // let DefWindowProcA emit WM_CONTEXTMENU for the preview area
        break;
    }

    case WM_MOUSEMOVE:
    {
        int cx = (short)LOWORD(lp);
        int cy = (short)HIWORD(lp);
        if (g_editp && editp_is_editing(g_editp))
        {
            int ix = 0, iy = 0;
            if (preview_client_to_image(hwnd, cx, cy, &ix, &iy) || g_dragging_canvas)
            {
                plugin_manager_forward_main_mouse(WM_FILTER_MAIN_MOUSE_MOVE, wp, ix, iy);
            }
        }
        return 0;
    }

    case WM_CONTEXTMENU:
    {
        int sx = (short)LOWORD(lp);
        int sy = (short)HIWORD(lp);
        if ((HWND)wp != hwnd && lp != -1)
        {
            return 0;
        }
        if (sy != -1)
        {
            POINT pt = {sx, sy};
            RECT area;
            ScreenToClient(hwnd, &pt);
            preview_layout(hwnd, &area);
            if (pt.y >= area.bottom)
            {
                return 0;
            }
        }
        if (plugin_manager_forward_main_misc(WM_FILTER_MAIN_CONTEXTMENU, wp, lp))
        {
            return 0;
        }
        show_preview_context_menu(hwnd, sx, sy);
        return 0;
    }

    case WM_MOUSEWHEEL:
        if (plugin_manager_forward_main_misc(WM_FILTER_MAIN_MOUSE_WHEEL, wp, lp))
        {
            return 0;
        }
        if (g_editp && editp_is_editing(g_editp))
        {
            short delta = GET_WHEEL_DELTA_WPARAM(wp);
            seek_to_frame_ex(hwnd, editp_get_frame(g_editp) + ((delta < 0) ? 1 : -1), TRUE);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        BOOL ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        BOOL shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        BOOL alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

        if (wp == VK_ESCAPE)
        {
            SendMessageA(hwnd, WM_COMMAND, 1009, 0);
            return 0;
        }

        // 0x00412300 lines 446..461: MAIN_MESSAGE filter keydown runs first, then shortcut table
        if (msg == WM_KEYDOWN && plugin_manager_handle_key(hwnd, (UINT)wp, TRUE, lp))
        {
            return 0;
        }
        if (keycfg_translate_keydown(hwnd, msg, (UINT)wp))
        {
            return 0;
        }

        if (msg == WM_KEYDOWN)
        {
            if (ctrl && shift && !alt && wp == 'S')
            {
                SendMessageA(hwnd, WM_COMMAND, 1024, 0);
                return 0;
            }
            if (ctrl && !shift && !alt && wp == 'N')
            {
                open_media_or_exo(hwnd, "1280x720_30fps_44100Hz.exedit");
                return 0;
            }
            if (!ctrl && !shift && !alt && wp == VK_F12)
            {
                SendMessageA(hwnd, WM_COMMAND, IDC_TOGGLE_CONSOLE, 0);
                return 0;
            }
            if (!ctrl && !shift && !alt && wp == VK_SPACE)
            {
                SendMessageA(hwnd, WM_COMMAND, 1099, 0);
                return 0;
            }
            return 0;
        }
        break;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP:
        plugin_manager_handle_key(hwnd, (UINT)wp, FALSE, lp);
        if (msg == WM_SYSKEYUP)
            break;
        return 0;

    case WM_REAVIUTL_PLUGIN_REDRAW:
        if (wp == 3)
        {
            apply_menu(hwnd);
        }
        else
        {
            refresh_current_frame(hwnd);
        }
        return 0;

    case WM_REAVIUTL_BRIDGE_NOTIFY:
        if (g_editp)
        {
            plugin_manager_sync_from_bridge(g_editp);
        }
        if (wp == 3)
        {
            apply_menu(hwnd);
        }
        else
        {
            refresh_current_frame(hwnd);
        }
        return 0;

    case WM_REAVIUTL_DEBUG_CMD:
    {
        char dbg_cmd[256];
        while (debug_console_pop_command(dbg_cmd, sizeof(dbg_cmd)))
        {
            handle_debug_console_command(hwnd, dbg_cmd);
        }
        return 0;
    }

    case WM_INITMENUPOPUP:
        if (!HIWORD(lp))
        {
            sync_edit_menu_states((HMENU)wp);
        }
        break;

    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        BOOL need_rerender = FALSE;

        if (plugin_manager_handle_command(hwnd, id))
        {
            return 0;
        }
        if (id >= INPUT_PLUGIN_CMD_CFG_BASE &&
            id < INPUT_PLUGIN_CMD_CFG_BASE + input_plugin_get_count())
        {
            input_plugin_config(id - INPUT_PLUGIN_CMD_CFG_BASE, hwnd);
            return 0;
        }
        if (id >= AUO_CMD_BASE && id < AUO_CMD_BASE + video_export_get_auo_count())
        {
            stop_playback(hwnd);
            video_export_auo_dialog(hwnd, g_editp, id - AUO_CMD_BASE);
            refresh_current_frame(hwnd);
            return 0;
        }
        if (preview_on_command(hwnd, g_editp, id, &need_rerender))
        {
            if (need_rerender)
                refresh_current_frame(hwnd);
            else
                update_window_title(hwnd);
            return 0;
        }

        if (id >= 5596 && id < 5596 + MRU_MAX_FILES)
        {
            char ini[MAX_PATH], key[16], mpath[MAX_PATH];
            get_aviutl_ini_path(ini, sizeof(ini));
            wsprintfA(key, "file%d", id - 5596);
            GetPrivateProfileStringA("recent", key, "", mpath, MAX_PATH, ini);
            if (mpath[0])
            {
                open_media_or_exo(hwnd, mpath);
            }
            return 0;
        }
        if (id >= 1037 && id <= 1042)
        {
            apply_priority_command(id, TRUE);
            apply_menu(hwnd);
            return 0;
        }
        if (id >= PROFILE_CMD_SELECT_BASE &&
            id < PROFILE_CMD_SELECT_BASE + PROFILE_MAX_COUNT)
        {
            if (profile_cfg_select(id - PROFILE_CMD_SELECT_BASE, TRUE))
            {
                exfunc_invalidate_all_caches();
                if (plugin_manager_is_bridged())
                    plugin_bridge_invalidate_caches();
                apply_menu(hwnd);
                plugin_manager_notify_update(g_editp);
                refresh_current_frame(hwnd);
            }
            return 0;
        }

        switch (id)
        {
        case IDC_TOGGLE_CONSOLE:
            debug_console_toggle_visible();
            return 0;
        case IDC_NEW_EXEDIT_720P:
            open_media_or_exo(hwnd, "1280x720_30fps_44100Hz.exedit");
            return 0;
        case IDC_NEW_EXEDIT_1080:
            open_media_or_exo(hwnd, "1920x1080_30fps_48000Hz.exedit");
            return 0;
        case 1073: // file -> Clear Recent Files
            mru_clear_files();
            apply_menu(hwnd);
            return 0;
        case 5183: // filter -> Disable All Filters
            plugin_manager_disable_all_filters();
            apply_menu(hwnd);
            refresh_current_frame(hwnd);
            return 0;
        case 5097: // file -> Open (Ctrl+O)
            reaviutl_file_open(hwnd);
            return 0;
        case 5100: // file -> Append Load (追加読み込み)
            reaviutl_file_append(hwnd);
            return 0;
        case 5168: // file -> Load Audio (音声読み込み)
            reaviutl_audio_open(hwnd);
            return 0;
        case 5167: // file -> Import -> Keyframe List
            import_keyframe_list(hwnd);
            return 0;
        case 1054: // file -> Export -> Keyframe List
            export_keyframe_list(hwnd);
            return 0;
        case 1003: // file -> AVI Output (Ctrl+S)
            stop_playback(hwnd);
            video_export_avi_dialog(hwnd, g_editp);
            refresh_current_frame(hwnd);
            return 0;
        case 1062: // file -> WAV Output (WAV出力)
            reaviutl_wav_export(hwnd);
            return 0;
        case 1025: // file -> Batch Output (バッチ出力)
            stop_playback(hwnd);
            avi_tools_run_batch_dialog(hwnd, g_editp, cb_batch_ui_refresh);
            return 0;
        case 1057: // file -> AVI File Operations -> Concatenate AVI Files
            stop_playback(hwnd);
            avi_tools_run_avichain_dialog(hwnd);
            update_window_title(hwnd);
            return 0;
        case 1058: // file -> AVI File Operations -> Multiplex AVI File
            stop_playback(hwnd);
            avi_tools_run_avimix_dialog(hwnd);
            update_window_title(hwnd);
            return 0;
        case 1009: // file -> Abort Output (Esc)
            video_export_request_abort();
            return 0;
        case 5127: // settings -> compression -> Video Compression
            video_export_choose_video_codec(hwnd, g_editp);
            return 0;
        case 5128: // settings -> compression -> Audio Compression
            video_export_choose_audio_codec(hwnd, g_editp);
            return 0;
        case 5399: // settings -> compression -> Plugin Output Settings
            run_exout_config_dialog(hwnd);
            return 0;
        case 5139: // settings -> Video Filter Order
            if (plugin_manager_run_priority_dialog(hwnd, FALSE))
            {
                refresh_current_frame(hwnd);
            }
            return 0;
        case 5181: // settings -> Audio Filter Order
            if (plugin_manager_run_priority_dialog(hwnd, TRUE))
            {
                refresh_current_frame(hwnd);
            }
            return 0;
        case 5131: // edit -> Jump to Frame (Ctrl+G)
            run_move_dialog(hwnd);
            return 0;
        case 5115: // edit -> Specify Selection Range
            run_select_dialog(hwnd);
            return 0;
        case 5398: // edit -> Change Playback Speed Info
            run_playback_speed_dialog(hwnd);
            return 0;
        case 1086: // file -> settings -> Input Plugin Priority
            run_input_priority_dialog(hwnd);
            return 0;
        case 102: // file -> settings -> Shortcut Keys
            keycfg_run_dialog(hwnd);
            return 0;
        case 106: // file -> settings -> Language
            if (i18n_show_lang_dialog(hwnd))
            {
                apply_menu(hwnd);
                update_window_title(hwnd);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case 107: // other -> Language Extension Resource Info
            i18n_show_lang_info(hwnd);
            return 0;
        case 108: // file -> settings -> System Settings
            run_system_settings_dialog(hwnd);
            winsnap_set_enabled(preview_get_sys_config()->windowsnap != 0);
            apply_menu(hwnd);
            builtin_filters_refresh_tool_window();
            return 0;
        case 109: // file -> settings -> Codec Settings (コーデックの設定)
            video_export_run_codec_settings_dialog(hwnd);
            return 0;
        case 1006: // other -> File Information
            show_current_file_info(hwnd);
            return 0;
        case 101: // other -> Filter Plugin Information
            show_filter_plugin_info(hwnd);
            return 0;
        case 103: // other -> Input Plugin Information
            show_input_plugin_info(hwnd);
            return 0;
        case 104: // other -> Overlay Information
            aviutl_msgbox_w(hwnd, i18n_get_wstr(1021), i18n_get_wstr(1032),
                            MB_OK | MB_ICONINFORMATION);
            return 0;
        case 105: // other -> Output Plugin Information
            show_output_plugin_info(hwnd);
            return 0;
        case 110: // other -> Color Conversion Plugin Information
            show_color_plugin_info(hwnd);
            return 0;
        case 5118: // file -> Open Edit Project (.aup)
            reaviutl_project_open(hwnd);
            return 0;
        case 1023: // file -> Save Edit Project (.aup Save As)
            reaviutl_project_save(hwnd, TRUE);
            return 0;
        case 1024: // file -> Overwrite Edit Project (.aup Save)
            reaviutl_project_save(hwnd, FALSE);
            return 0;
        case 5157: // file -> Close
            if (!confirm_discard_current_edit(hwnd))
                return 0;
            stop_playback(hwnd);
            if (g_editp && editp_is_editing(g_editp))
            {
                plugin_manager_notify_file_close(g_editp);
                editp_close_video(g_editp);
                apply_menu(hwnd);
                refresh_current_frame(hwnd);
            }
            return 0;

        case PROFILE_CMD_CREATE: // 8001: Make a new profile
            if (profile_cfg_create_interactive(hwnd, NULL) >= 0)
            {
                apply_menu(hwnd);
                refresh_current_frame(hwnd);
            }
            return 0;
        case PROFILE_CMD_DELETE: // 8002: Delete current profile
            if (profile_cfg_delete_active(g_editp))
            {
                exfunc_invalidate_all_caches();
                if (plugin_manager_is_bridged())
                    plugin_bridge_invalidate_caches();
                apply_menu(hwnd);
                plugin_manager_notify_update(g_editp);
                refresh_current_frame(hwnd);
            }
            return 0;
        case PROFILE_CMD_RESET: // 8000: Reset current profile to defaults
            if (profile_cfg_reset_active())
            {
                exfunc_invalidate_all_caches();
                if (plugin_manager_is_bridged())
                    plugin_bridge_invalidate_caches();
                apply_menu(hwnd);
                plugin_manager_notify_update(g_editp);
                refresh_current_frame(hwnd);
            }
            return 0;
        case 1002: // file -> Exit
            if (!confirm_discard_current_edit(hwnd))
                return 0;
            shutdown_main_window(hwnd);
            return 0;

        case 9001: // '<' Prev frame
            if (g_editp)
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) - 1, TRUE);
            return 0;
        case 9002: // '>' Next frame
            if (g_editp)
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) + 1, TRUE);
            return 0;
        case 5141: // up: Prev frame skipping deleted frames
            seek_skipping_deleted_frames(hwnd, -1);
            return 0;
        case 5142: // down: Next frame skipping deleted frames
            seek_skipping_deleted_frames(hwnd, +1);
            return 0;
        case 5176: // next profile change frame
            seek_profile_change_frame(hwnd, +1);
            return 0;
        case 5177: // prev profile change frame
            seek_profile_change_frame(hwnd, -1);
            return 0;
        case 5170: // next frame discontinuity
            seek_frame_discontinuity(hwnd, +1);
            return 0;
        case 5171: // prev frame discontinuity
            seek_frame_discontinuity(hwnd, -1);
            return 0;
        case 9003: // '|->' Set selection end
        case 5111: // ']' Set selection end
            if (g_editp && editp_is_editing(g_editp))
            {
                int s = 0, e = 0, cur = editp_get_frame(g_editp);
                editp_get_select_frame(g_editp, &s, &e);
                if (s < 0 || s > cur)
                    s = 0;
                editp_set_select_frame(g_editp, s, cur);
                plugin_manager_notify_update(g_editp);
                refresh_current_frame(hwnd);
            }
            return 0;
        case 9004: // '<-|' Set selection start
        case 5110: // '[' Set selection start
            if (g_editp && editp_is_editing(g_editp))
            {
                int s = 0, e = 0, cur = editp_get_frame(g_editp);
                editp_get_select_frame(g_editp, &s, &e);
                if (e < cur)
                    e = editp_get_frame_n(g_editp) - 1;
                editp_set_select_frame(g_editp, cur, e);
                plugin_manager_notify_update(g_editp);
                refresh_current_frame(hwnd);
            }
            return 0;
        case 9005: // play button (movieplaymain)
            toggle_playback(hwnd);
            return 0;
        case 1099: // play / Stop (0x00405450: Playback Window unless movieplaymain != 0)
            if (preview_get_sys_config()->movieplaymain != 0)
            {
                toggle_playback(hwnd);
            }
            else if (!builtin_filters_toggle_playback_window_play(hwnd, g_editp))
            {
                toggle_playback(hwnd);
            }
            return 0;
        case 5396: // first frame (Home)
            seek_to_frame_ex(hwnd, 0, TRUE);
            return 0;
        case 5397: // last frame (End)
            if (g_editp)
                seek_to_frame_ex(hwnd, editp_get_frame_n(g_editp) - 1, TRUE);
            return 0;
        case 5103: // jump to selection start
            if (g_editp && editp_is_editing(g_editp))
            {
                int s = 0, e = 0;
                if (editp_get_select_frame(g_editp, &s, &e) && s >= 0)
                {
                    seek_to_frame_ex(hwnd, s, FALSE);
                }
            }
            return 0;
        case 5104: // jump to selection end
            if (g_editp && editp_is_editing(g_editp))
            {
                int s = 0, e = 0;
                if (editp_get_select_frame(g_editp, &s, &e) && e >= 0)
                {
                    seek_to_frame_ex(hwnd, e, FALSE);
                }
            }
            return 0;
        case 5112: // select All Frames (Ctrl+A)
            if (g_editp && editp_is_editing(g_editp))
            {
                editp_set_select_frame(g_editp, 0, editp_get_frame_n(g_editp) - 1);
                plugin_manager_notify_update(g_editp);
                refresh_current_frame(hwnd);
            }
            return 0;

        // step jumps moveA..moveD (PageDown / PageUp / etc.)
        case 5187:
            if (g_editp)
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) + preview_get_sys_config()->moveA, TRUE);
            return 0;
        case 5188:
            if (g_editp)
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) - preview_get_sys_config()->moveA, TRUE);
            return 0;
        case 5189:
            if (g_editp)
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) + preview_get_sys_config()->moveB, TRUE);
            return 0;
        case 5190:
            if (g_editp)
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) - preview_get_sys_config()->moveB, TRUE);
            return 0;
        case 5191:
            if (g_editp)
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) + preview_get_sys_config()->moveC, TRUE);
            return 0;
        case 5192:
            if (g_editp)
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) - preview_get_sys_config()->moveC, TRUE);
            return 0;
        case 5193:
            if (g_editp)
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) + preview_get_sys_config()->moveD, TRUE);
            return 0;
        case 5194:
            if (g_editp)
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) - preview_get_sys_config()->moveD, TRUE);
            return 0;

        case 5129:
            toggle_frame_edit_flag(hwnd, EDIT_FRAME_EDIT_FLAG_KEYFRAME);
            return 0;
        case 5130:
            toggle_frame_edit_flag(hwnd, EDIT_FRAME_EDIT_FLAG_MARKFRAME);
            return 0;
        case 5132:
            toggle_frame_edit_flag(hwnd, EDIT_FRAME_EDIT_FLAG_DELFRAME);
            return 0;
        case 5156:
            toggle_frame_edit_flag(hwnd, EDIT_FRAME_EDIT_FLAG_NULLFRAME);
            return 0;
        case 5174:
            seek_matching_frame(hwnd, +1, FALSE);
            return 0;
        case 5175:
            seek_matching_frame(hwnd, -1, FALSE);
            return 0;
        case 5178:
            seek_matching_frame(hwnd, +1, TRUE);
            return 0;
        case 5179:
            seek_matching_frame(hwnd, -1, TRUE);
            return 0;

        case 5101: // delete selection range (Del)
            if (g_editp && editp_is_editing(g_editp))
            {
                int s = 0, e = 0;
                stop_playback(hwnd);
                editp_get_select_frame(g_editp, &s, &e);
                if (editp_delete_range(g_editp, s, e))
                {
                    if (plugin_manager_is_bridged())
                        plugin_bridge_invalidate_caches();
                    plugin_manager_notify_update(g_editp);
                    refresh_current_frame(hwnd);
                }
            }
            return 0;
        case 5143: // trim/Cut to selection range
            if (g_editp && editp_is_editing(g_editp))
            {
                int s = 0, e = 0;
                stop_playback(hwnd);
                editp_get_select_frame(g_editp, &s, &e);
                if (editp_trim_to_range(g_editp, s, e))
                {
                    if (plugin_manager_is_bridged())
                        plugin_bridge_invalidate_caches();
                    plugin_manager_notify_update(g_editp);
                    refresh_current_frame(hwnd);
                }
            }
            return 0;
        case 5147: // undo (Ctrl+Z)
            if (g_editp && editp_is_editing(g_editp))
            {
                stop_playback(hwnd);
                if (editp_undo(g_editp))
                {
                    if (plugin_manager_is_bridged())
                        plugin_bridge_invalidate_caches();
                    plugin_manager_notify_update(g_editp);
                    refresh_current_frame(hwnd);
                }
            }
            return 0;
        case 5109: // make selection a new profile (0x00413ba4)
            if (g_editp && editp_is_editing(g_editp))
            {
                if (profile_cfg_bind_selection_new_profile(hwnd, g_editp) >= 1)
                {
                    exfunc_invalidate_all_caches();
                    if (plugin_manager_is_bridged())
                        plugin_bridge_invalidate_caches();
                    apply_menu(hwnd);
                    plugin_manager_notify_update(g_editp);
                    refresh_current_frame(hwnd);
                }
            }
            return 0;
        case 5126: // make selection a new compression setting (0x00413db6)
            if (g_editp && editp_is_editing(g_editp))
            {
                if (video_export_choose_video_codec(hwnd, g_editp))
                {
                    int s = 0, e = 0, fn = editp_get_frame_n(g_editp);
                    BYTE st = 0;
                    editp_get_select_frame(g_editp, &s, &e);
                    editp_push_undo(g_editp);
                    if (s >= 0 && s < fn)
                    {
                        editp_get_frame_status(g_editp, s, &st);
                        editp_set_frame_status(g_editp, s, (BYTE)(st | EDIT_FRAME_EDIT_FLAG_KEYFRAME));
                    }
                    if (e + 1 >= 0 && e + 1 < fn)
                    {
                        st = 0;
                        editp_get_frame_status(g_editp, e + 1, &st);
                        editp_set_frame_status(g_editp, e + 1, (BYTE)(st | EDIT_FRAME_EDIT_FLAG_KEYFRAME));
                    }
                    plugin_manager_notify_update(g_editp);
                    refresh_current_frame(hwnd);
                }
            }
            return 0;
        case 5206:
        case 5207:
        case 5208:
        case 5209:
        case 5210:
        case 5211:
        case 5212:
        case 5213:
        case 5214:
        case 5215: // current frame profile (0..9)
            if (g_editp && editp_is_editing(g_editp))
            {
                int slot = id - 5206;
                if (slot == 0 || profile_cfg_has_project_slot(slot))
                {
                    editp_push_undo(g_editp);
                    editp_set_frame_config(g_editp, editp_get_frame(g_editp), (BYTE)slot);
                    exfunc_invalidate_all_caches();
                    if (plugin_manager_is_bridged())
                        plugin_bridge_invalidate_caches();
                    plugin_manager_notify_update(g_editp);
                    refresh_current_frame(hwnd);
                }
            }
            return 0;
        case 5125: // specify selection deinterlace (INTER dialog)
            run_inter_dialog(hwnd);
            return 0;
        case 5196:
        case 5197:
        case 5198:
        case 5199:
        case 5200:
        case 5201: // current frame deinterlace mode (Numpad 0..5)
            if (g_editp && editp_is_editing(g_editp))
            {
                editp_push_undo(g_editp);
                editp_set_frame_inter(g_editp, editp_get_frame(g_editp), (BYTE)(id - 5196));
                exfunc_invalidate_all_caches();
                if (plugin_manager_is_bridged())
                    plugin_bridge_invalidate_caches();
                plugin_manager_notify_update(g_editp);
                refresh_current_frame(hwnd);
            }
            return 0;

        case 1020: // copy current frame YC48 image (Ctrl+C)
            copy_current_frame_yc48(hwnd);
            return 0;
        case 5117: // paste copied YC48 image into current frame (Ctrl+V)
            if (g_editp && editp_is_editing(g_editp))
            {
                int cur = editp_get_frame(g_editp);
                if (editp_paste_frame_image(g_editp, cur, cur))
                {
                    if (plugin_manager_is_bridged())
                        plugin_bridge_invalidate_caches();
                    plugin_manager_notify_update(g_editp);
                    refresh_current_frame(hwnd);
                }
            }
            return 0;
        case 5184: // paste copied YC48 image into selection range
            if (g_editp && editp_is_editing(g_editp))
            {
                int s = 0, e = 0;
                editp_get_select_frame(g_editp, &s, &e);
                if (editp_paste_frame_image(g_editp, s, e))
                {
                    if (plugin_manager_is_bridged())
                        plugin_bridge_invalidate_caches();
                    plugin_manager_notify_update(g_editp);
                    refresh_current_frame(hwnd);
                }
            }
            return 0;
        case 5123: // paste Windows CF_DIB clipboard into current frame
            if (g_editp && editp_is_editing(g_editp))
            {
                if (editp_paste_clipboard_dib(hwnd, g_editp, editp_get_frame(g_editp), TRUE))
                {
                    if (plugin_manager_is_bridged())
                        plugin_bridge_invalidate_caches();
                    plugin_manager_notify_update(g_editp);
                    refresh_current_frame(hwnd);
                }
            }
            return 0;
        case 1070: // copy filtered output image to clipboard
            copy_current_frame_to_clipboard(hwnd, FALSE);
            return 0;
        case 1026: // copy raw source frame to clipboard
            copy_current_frame_to_clipboard(hwnd, TRUE);
            return 0;
        case 1090: // copy current frame number to clipboard
            copy_frame_number_to_clipboard(hwnd);
            return 0;

        case 111:
            reset_window_positions(hwnd);
            return 0;
        case 100:
            aviutl_msgbox_w(hwnd,
                            L"AviUtl64 version 1.10\n\nBy \xff2b\xff25\xff2e\x304f\x3093 ( Kenkun )\nhttps://github.com/aviutl-translation-project/aviutl64",
                            i18n_get_wstr(1033), MB_OK | MB_ICONINFORMATION);
            return 0;
        default:
            return 0;
        }
    }

    case WM_CLOSE:
        if (!confirm_discard_current_edit(hwnd))
            return 0;
        shutdown_main_window(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static BOOL register_classes(HINSTANCE hInst)
{
    WNDCLASSA wc;
    if (!preview_register_classes(hInst))
    {
        return FALSE;
    }
    ZeroMemory(&wc, sizeof(wc));
    wc.style = AVIUTL_CLASS_STYLE;
    wc.lpfnWndProc = AviUtlWndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconA(hInst, "ICON");
    if (!wc.hIcon)
        wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = CLASS_NAME;
    return RegisterClassA(&wc) != 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdline, int show)
{
    MSG msg;
    HWND hwnd;
    char plugin_dir[MAX_PATH];
    int win_x = CW_USEDEFAULT, win_y = CW_USEDEFAULT, win_w = 640, win_h = 480;
    BOOL win_max = FALSE;
    int ex_idx;
    HRESULT ole_hr;

    (void)hPrev;
    g_smoke_exit = (cmdline && strstr(cmdline, "--smoke-exit") != NULL);

    // match aviutl.exe 0x00402e70 startup: initialize STA COM/OLE, CommonControls, and AVIFile on UI thread before loading plugins
    ole_hr = OleInitialize(NULL);
    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = 0xFFFF;
        InitCommonControlsEx(&icc);
    }
    AVIFileInit();

    // init system max resolution/frame limits and console before loading plugins
    {
        char ini[MAX_PATH];
        get_aviutl_ini_path(ini, sizeof(ini));
        editp_init_sys_limits_from_ini(ini);
    }
    debug_console_init(FALSE);
    dbg_info("AviUtl64 %s starting (PID=%lu, AviUtl 1.10 Build 11003 ABI, VRAM=%dx%d, MaxFrame=%d)",
             REAVIUTL_X64 ? "64-bit (x64)" : "32-bit (x86)",
             (unsigned long)GetCurrentProcessId(),
             REAVIUTL_VRAM_W, REAVIUTL_VRAM_H, REAVIUTL_MAX_FRAME);

    i18n_init(hInst);

    if (!register_classes(hInst))
    {
        MessageBoxA(NULL, "Failed to register window class.", "AviUtl", MB_ICONERROR);
        return 1;
    }
    {
        char ini[MAX_PATH];
        int prio;
        get_aviutl_ini_path(ini, sizeof(ini));
        prio = GetPrivateProfileIntA("system", "priority", 2, ini);
        if (prio < 0 || prio > 5)
            prio = 2;
        apply_priority_command(1037 + prio, FALSE);
    }

    preview_load_ini(&win_x, &win_y, &win_w, &win_h, &win_max);
    exfunc_reload_system_settings();
    winsnap_set_enabled(preview_get_sys_config()->windowsnap != 0);

    hwnd = CreateWindowExA(
        0, CLASS_NAME, IDLE_TITLE, AVIUTL_WIN_STYLE,
        win_x, win_y, win_w, win_h,
        NULL, NULL, hInst, NULL);
    if (!hwnd)
    {
        MessageBoxA(NULL, "Failed to create main window.", "AviUtl", MB_ICONERROR);
        return 1;
    }

    preview_init_controls(hwnd, hInst);
    DragAcceptFiles(hwnd, TRUE);

    // create editp and bind trackbar (+0x388) before loading plugins
    g_editp = editp_create_idle(hwnd);
    editp_set_trackbar_hwnd(g_editp, preview_get_trackbar_hwnd());
    exfunc_set_editp(g_editp);
    dbg_info("Main HWND=0x%p EDITP=0x%p EXFUNC=0x%p",
             (void *)hwnd, (void *)g_editp, (void *)exfunc_get_table());

    // load .auf plugins from <exe_dir>\plugins
    GetModuleFileNameA(NULL, plugin_dir, MAX_PATH);
    {
        char *p = strrchr(plugin_dir, '\\');
        if (p)
            *p = '\0';
        lstrcatA(plugin_dir, "\\plugins");
    }
    dbg_info("Scanning plugins directory: %s", plugin_dir);
    input_plugin_set_wav_config_handler(run_wav_option_dialog);
    color_plugin_init(plugin_dir, hwnd);
    input_plugin_init(plugin_dir, hwnd);
    plugin_manager_set_builtins_enabled(TRUE);
    plugin_manager_init_ex(plugin_dir, hwnd, g_editp);
    profile_cfg_init();
    video_export_set_ui_callbacks(cb_export_preview_update, cb_export_progress);
    video_export_scan_auo(plugin_dir, hwnd, g_editp);

    apply_menu(hwnd);
    keycfg_init_and_load(hwnd);
    preview_on_size(hwnd, g_editp);

    if (preview_get_flags() & PREVIEW_FLAG_ALWAYSTOP)
    {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    ShowWindow(hwnd, win_max ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(hwnd);

    // automatically show the exedit timeline window if exedit.auf is installed
    ex_idx = find_exedit_filter_index();
    if (ex_idx >= 0)
    {
        plugin_manager_show_window(ex_idx, TRUE);
    }

    if (!cmdline || strstr(cmdline, "--smoke-exit") == NULL)
    {
        debug_console_start_input(hwnd);
        dbg_info("ready (type 'help' for commands)");
    }

    // 0x00402e70 + 0x00407b50: auto-resume <exe_dir>\aviutl.aup first, then run CLI / .aus (0x0040fa20)
    {
        BOOL cli_quit = FALSE;
        const char *trimmed_cmd = cmdline ? cmdline : "";
        while (*trimmed_cmd == ' ' || *trimmed_cmd == '\t')
            trimmed_cmd++;

        if (g_smoke_exit)
        {
            open_media_or_exo(hwnd, "1280x720_30fps_44100Hz.exedit");
            if (ex_idx >= 0)
            {
                // add a Figure ("図形", command 2004) onto Layer 1 and render it
                plugin_manager_send_filter_command(ex_idx, 2004);
                refresh_current_frame(hwnd);
            }
            UpdateWindow(hwnd);
            PostMessageA(hwnd, WM_CLOSE, 0, 0);
        }
        else
        {
            if (preview_get_sys_config()->editresume != 0)
            {
                char resume_aup[MAX_PATH];
                DWORD attr;
                get_resume_aup_path(resume_aup, sizeof(resume_aup));
                attr = GetFileAttributesA(resume_aup);
                if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
                {
                    if (plugin_manager_load_aup(resume_aup, g_editp))
                    {
                        editp_set_project_path(g_editp, "");
                        plugin_manager_notify_update(g_editp);
                        g_last_auto_w = 0;
                        g_last_auto_h = 0;
                        apply_menu(hwnd);
                        refresh_current_frame(hwnd);
                    }
                }
            }
            if (trimmed_cmd[0])
            {
                execute_cli_line(hwnd, trimmed_cmd, 0, &cli_quit);
                if (cli_quit)
                {
                    g_cli_no_confirm = TRUE;
                    PostMessageA(hwnd, WM_CLOSE, 0, 0);
                }
            }
        }
    }

    while (GetMessageA(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    debug_console_shutdown();
    profile_cfg_shutdown();
    video_export_shutdown();
    plugin_manager_shutdown();
    input_plugin_shutdown();
    color_plugin_shutdown();
    exfunc_shutdown();
    i18n_shutdown();
    if (g_editp)
    {
        editp_destroy(g_editp);
        g_editp = NULL;
    }
    if (g_preview_bgr)
    {
        free(g_preview_bgr);
        g_preview_bgr = NULL;
    }
    if (g_audio_buf)
    {
        free(g_audio_buf);
        g_audio_buf = NULL;
    }
    if (g_wave_mem)
    {
        free(g_wave_mem);
        g_wave_mem = NULL;
    }
    if (g_wave_frame_pcm)
    {
        free(g_wave_frame_pcm);
        g_wave_frame_pcm = NULL;
    }
    AVIFileExit();
    if (SUCCEEDED(ole_hr))
    {
        OleUninitialize();
    }
    return (int)msg.wParam;
}
