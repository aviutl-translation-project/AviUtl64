#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "preview.h"
#include "i18n.h"
#include "editp.h"
#include "exfunc.h"
#include "yc48_convert.h"
#include "plugin_manager.h"
#include "plugin_bridge.h"
#include "video_export.h"
#include "debug_console.h"

// shift-jis bytes for "拡張編集"
#define EXEDIT_SJIS_NAME "\x8a\x67\x92\xa3\x95\xd2\x8f\x57"

// main window class & style from aviutl 1.10 (WS_CLIPCHILDREN protects child trackbar/buttons)
#define AVIUTL_CLASS_STYLE (CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS | CS_BYTEALIGNCLIENT)
#define AVIUTL_WIN_STYLE (WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN)

#define IDC_LANG_EN 40001
#define IDC_LANG_JA 40002
#define IDC_NEW_EXEDIT_720P 40010
#define IDC_NEW_EXEDIT_1080 40011
#define IDC_TOGGLE_CONSOLE 40020

#define PLAY_TIMER_ID 1
#define AUDIO_PLAY_BUF_COUNT 4
#define AUDIO_PLAY_BUF_BYTES 16384

static const char *CLASS_NAME = "AviUtl";
static const char *IDLE_TITLE = "AviUtl";

HMENU reaviutl_build_menu(int lang);

static EDITP *g_editp = NULL;
static unsigned char *g_preview_bgr = NULL;
static size_t g_preview_bgr_cap = 0;
static short *g_audio_buf = NULL;
static int g_audio_buf_cap = 0;
static BOOL g_playing = FALSE;
static BOOL g_dragging_canvas = FALSE;
static BOOL g_peeking_unfiltered = FALSE;
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

// shift-jis (CP932) to UTF-16 helper
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

// populate aviutl's dynamic 5395 (Plugin Output), 8005 (Profile), and 10998 (Filter Windows) menu slots
static void populate_aviutl_dynamic_slots(HMENU bar, BOOL is_ja)
{
    HMENU prof_sub = GetSubMenu(bar, 4); // プロファイル / Profile
    HMENU view_sub = GetSubMenu(bar, 5); // 表示 / View

    video_export_populate_menu(bar, is_ja);

    if (prof_sub)
    {
        ModifyMenuW(prof_sub, 8005, MF_BYCOMMAND | MF_STRING | MF_CHECKED, 8005,
                    is_ja ? L"\x6a19\x6e96\x30d7\x30ed\x30d5\x30a1\x30a4\x30eb" : L"Standard");
    }

    if (view_sub)
    {
        int pos = -1, cnt = GetMenuItemCount(view_sub), i;
        for (i = 0; i < cnt; i++)
        {
            if (GetMenuItemID(view_sub, i) == 10998)
            {
                pos = i;
                break;
            }
        }
        if (pos >= 0)
        {
            int fcount = plugin_manager_get_count();
            DeleteMenu(view_sub, pos, MF_BYPOSITION);
            for (i = 0; i < fcount; i++)
            {
                int flags = plugin_manager_get_flags(i);
                const char *nm = plugin_manager_get_name(i);
                HWND fhwnd = plugin_manager_get_window(i);
                wchar_t wnm[256], witem[300];
                UINT ufl = MF_BYPOSITION | MF_STRING;

                if (flags & FILTER_FLAG_NO_CONFIG)
                    continue;
                sjis_to_wide_str(nm ? nm : "Filter", wnm, 256);
                if (is_ja)
                    wsprintfW(witem, L"%s\x306e\x8868\x793a", wnm);
                else
                    wsprintfW(witem, L"Show %s", wnm);

                if ((flags & FILTER_FLAG_WINDOW_DISP) || (fhwnd && IsWindowVisible(fhwnd)))
                {
                    ufl |= MF_CHECKED;
                }
                InsertMenuW(view_sub, pos++, ufl, (UINT_PTR)(PLUGIN_CMD_WINDOW_BASE + i), witem);
            }
            InsertMenuW(view_sub, pos++, MF_BYPOSITION | MF_STRING | (debug_console_is_visible() ? MF_CHECKED : 0),
                        IDC_TOGGLE_CONSOLE,
                        is_ja ? L"\x30c7\x30d0\x30c3\x30b0\x30b3\x30f3\x30bd\x30fc\x30eb\x306e\x8868\x793a"
                              : L"Show Debug Console");
            InsertMenuW(view_sub, pos, MF_BYPOSITION | MF_STRING, 111,
                        is_ja ? L"\x30a6\x30a3\x30f3\x30c9\x30a6\x306e\x4f4d\x7f6e\x3092\x521d\x671f\x5316"
                              : L"Reset Window Positions");
        }
    }
}

static void apply_menu(HWND hwnd)
{
    BOOL is_ja = (i18n_get_lang() == LANG_JA);
    HMENU bar = reaviutl_build_menu((int)i18n_get_lang());
    HMENU file_sub;

    populate_aviutl_dynamic_slots(bar, is_ja);

    file_sub = GetSubMenu(bar, 0);
    if (file_sub)
    {
        if (is_ja)
        {
            InsertMenuW(file_sub, 0, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
            InsertMenuW(file_sub, 0, MF_BYPOSITION | MF_STRING, IDC_NEW_EXEDIT_1080,
                        L"\x65b0\x898f\x62e1\x5f35\x7de8\x96c6\x30d7\x30ed\x30b8\x30a7\x30af\x30c8 (1920x1080 30fps)");
            InsertMenuW(file_sub, 0, MF_BYPOSITION | MF_STRING, IDC_NEW_EXEDIT_720P,
                        L"\x65b0\x898f\x62e1\x5f35\x7de8\x96c6\x30d7\x30ed\x30b8\x30a7\x30af\x30c8 (1280x720 30fps)");
        }
        else
        {
            InsertMenuW(file_sub, 0, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
            InsertMenuW(file_sub, 0, MF_BYPOSITION | MF_STRING, IDC_NEW_EXEDIT_1080,
                        L"New ExEdit Project (1920x1080 30fps)");
            InsertMenuW(file_sub, 0, MF_BYPOSITION | MF_STRING, IDC_NEW_EXEDIT_720P,
                        L"New ExEdit Project (1280x720 30fps)");
        }
    }

    // insert Plugins menu before Other/Help
    {
        HMENU filters = plugin_manager_build_menu();
        const wchar_t *filters_label = is_ja ? L"\x30d7\x30e9\x30b0\x30a4\x30f3" : L"Plugins";
        int insert_pos = GetMenuItemCount(bar) - 1;
        if (insert_pos < 0)
            insert_pos = 0;
        InsertMenuW(bar, insert_pos, MF_BYPOSITION | MF_POPUP,
                    (UINT_PTR)filters, filters_label);
    }

    // append Language submenu
    {
        HMENU lang = CreatePopupMenu();
        AppendMenuW(lang, MF_STRING, IDC_LANG_EN, TW(STR_MENU_LANG_EN));
        AppendMenuW(lang, MF_STRING, IDC_LANG_JA, TW(STR_MENU_LANG_JA));
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)lang, TW(STR_MENU_LANGUAGE));
    }

    {
        HMENU old = GetMenu(hwnd);
        SetMenu(hwnd, bar);
        if (old)
            DestroyMenu(old);
    }
    preview_sync_menu_checks(hwnd);
    DrawMenuBar(hwnd);
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
    // shut down plugins BEFORE DestroyWindow(hwnd) so owned plugin windows remain valid during WM_FILTER_EXIT when exedit calls GetWindowRect() to save its own layout
    video_export_shutdown();
    plugin_manager_shutdown();
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
        dbg_info("  auos             List discovered .auo output plugins");
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

static BOOL open_media_or_exo(HWND hwnd, const char *path)
{
    const char *ext;
    if (!g_editp || !path || !path[0])
        return FALSE;
    stop_playback(hwnd);

    ext = strrchr(path, '.');
    if (ext && lstrcmpiA(ext, ".aup") == 0)
    {
        int ex_idx = plugin_manager_find_by_name(EXEDIT_SJIS_NAME);
        if (ex_idx >= 0)
        {
            plugin_manager_show_window(ex_idx, TRUE);
        }
        if (!plugin_manager_load_aup(path, g_editp))
        {
            char msg[MAX_PATH + 128];
            wsprintfA(msg, "Failed to open AviUtl project (.aup):\n%s", path);
            MessageBoxA(hwnd, msg, "AviUtl", MB_OK | MB_ICONWARNING);
            return FALSE;
        }
        g_last_auto_w = 0;
        g_last_auto_h = 0;
        refresh_current_frame(hwnd);
        return TRUE;
    }

    if (ext && lstrcmpiA(ext, ".exo") == 0)
    {
        int ex_idx = plugin_manager_find_by_name(EXEDIT_SJIS_NAME);
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

    if (!editp_open_video(g_editp, path))
    {
        char msg[MAX_PATH + 128];
        wsprintfA(msg, "Failed to open media/project:\n%s", path);
        MessageBoxA(hwnd, msg, "AviUtl", MB_OK | MB_ICONWARNING);
        return FALSE;
    }

    g_last_auto_w = 0;
    g_last_auto_h = 0;
    plugin_manager_notify_file_open(g_editp);
    refresh_current_frame(hwnd);
    return TRUE;
}

void reaviutl_file_open(HWND owner)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = {0};
    static const char filter[] =
        "Supported Media (*.aup;*.avi;*.mp3;*.wav;*.png;*.jpg;*.bmp;*.exo)\0"
        "*.aup;*.avi;*.mp3;*.wav;*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.exo\0"
        "AviUtl Project (*.aup)\0*.aup\0"
        "ExEdit Object (*.exo)\0*.exo\0"
        "Video (*.avi)\0*.avi\0"
        "Audio (*.mp3;*.wav)\0*.mp3;*.wav\0"
        "Image (*.bmp;*.jpg;*.png;*.gif)\0*.bmp;*.jpg;*.jpeg;*.png;*.gif\0"
        "All Files (*.*)\0*.*\0";

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameA(&ofn))
    {
        open_media_or_exo(owner, file);
    }
}

static void reaviutl_audio_open(HWND owner)
{
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = {0};
    static const char filter[] =
        "Audio Files (*.mp3;*.wav;*.avi)\0*.mp3;*.wav;*.avi\0"
        "All Files (*.*)\0*.*\0";

    if (!g_editp)
        return;

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

    stop_playback(owner);
    if (!editp_is_editing(g_editp))
    {
        open_media_or_exo(owner, file);
        return;
    }
    if (!editp_open_audio(g_editp, file))
    {
        char msg[MAX_PATH + 128];
        wsprintfA(msg, "Failed to load audio file:\n%s", file);
        MessageBoxA(owner, msg, "AviUtl", MB_OK | MB_ICONWARNING);
        return;
    }
    if (plugin_manager_is_bridged())
    {
        plugin_bridge_open_audio(file, g_editp);
    }
    plugin_manager_notify_update(g_editp);
    refresh_current_frame(owner);
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
        open_media_or_exo(owner, file);
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
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;

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

static void reset_window_positions(HWND hwnd)
{
    RECT wa;
    int i, fcount = plugin_manager_get_count();
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0))
    {
        SetRect(&wa, 0, 0, 1280, 720);
    }
    SetWindowPos(hwnd, NULL, wa.left + 32, wa.top + 32, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    for (i = 0; i < fcount; i++)
    {
        HWND fh = plugin_manager_get_window(i);
        if (fh && IsWindow(fh))
        {
            SetWindowPos(fh, NULL, wa.left + 48 + (i % 6) * 28, wa.top + 360 + (i % 6) * 24,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

// show aviutl right-click "CONTEXT" popup menu (0x00412300)
static void show_preview_context_menu(HWND hwnd, int screen_x, int screen_y)
{
    const char *res_name = (i18n_get_lang() == LANG_EN) ? "CONTEXT_EN" : "CONTEXT";
    HMENU hMenu = LoadMenuA(GetModuleHandleA(NULL), res_name);
    HMENU hSub;

    if (!hMenu)
        hMenu = LoadMenuA(GetModuleHandleA(NULL), "CONTEXT");
    if (!hMenu)
        return;

    hSub = GetSubMenu(hMenu, 0);
    if (hSub)
    {
        BOOL active_edit = (g_editp && editp_is_editing(g_editp) && !editp_is_saving(g_editp));
        CheckMenuRadioItem(hSub, 9200, 9206, 9200 + preview_get_zoom(), MF_BYCOMMAND);

        if (!active_edit)
        {
            static const UINT gray_ids[] = {
                1020, 5117, 1026, 5123, 1070, 5101, 5143, 5112, 5130, 5147};
            int k;
            for (k = 0; k < (int)(sizeof(gray_ids) / sizeof(gray_ids[0])); k++)
            {
                EnableMenuItem(hSub, gray_ids[k], MF_BYCOMMAND | MF_GRAYED | MF_DISABLED);
            }
        }
        else
        {
            BYTE st = 0;
            if (editp_get_frame_status(g_editp, editp_get_frame(g_editp), &st) &&
                (st & EDIT_FRAME_EDIT_FLAG_MARKFRAME))
            {
                CheckMenuItem(hSub, 5130, MF_BYCOMMAND | MF_CHECKED);
            }
        }

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

    case WM_MOVE:
        preview_on_size(hwnd, g_editp);
        plugin_manager_forward_main_misc(WM_FILTER_MAIN_MOVESIZE, 0, 0);
        return 0;

    case WM_SIZE:
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
        char path[MAX_PATH] = {0};
        HDROP hDrop = (HDROP)wp;
        const AviUtlSystemConfig *scfg = preview_get_sys_config();
        if (scfg && scfg->dragdropactive)
        {
            SetForegroundWindow(hwnd);
        }
        if (DragQueryFileA(hDrop, 0, path, MAX_PATH) > 0)
        {
            open_media_or_exo(hwnd, path);
        }
        DragFinish(hDrop);
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
        if (wp == VK_ESCAPE)
        {
            SendMessageA(hwnd, WM_COMMAND, 1009, 0);
            return 0;
        }
        if (ctrl && wp == 'O')
        {
            SendMessageA(hwnd, WM_COMMAND, 5097, 0);
            return 0;
        }
        if (ctrl && wp == 'S')
        {
            SendMessageA(hwnd, WM_COMMAND, shift ? 1024 : 1003, 0);
            return 0;
        }
        if (ctrl && wp == 'N')
        {
            open_media_or_exo(hwnd, "1280x720_30fps_44100Hz.exedit");
            return 0;
        }
        if (ctrl && wp == 'C')
        {
            SendMessageA(hwnd, WM_COMMAND, 1020, 0);
            return 0;
        }
        if (ctrl && wp == 'A')
        {
            SendMessageA(hwnd, WM_COMMAND, 5112, 0);
            return 0;
        }
        if (wp == VK_SPACE)
        {
            toggle_playback(hwnd);
            return 0;
        }
        if (wp == VK_ADD)
        {
            SendMessageA(hwnd, WM_COMMAND, 9107, 0);
            return 0;
        }
        if (wp == VK_SUBTRACT)
        {
            SendMessageA(hwnd, WM_COMMAND, 9108, 0);
            return 0;
        }

        if (plugin_manager_handle_key(hwnd, (UINT)wp, TRUE, lp))
        {
            return 0;
        }
        if (g_editp && editp_is_editing(g_editp))
        {
            if (wp == VK_LEFT)
            {
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) - 1, TRUE);
                return 0;
            }
            if (wp == VK_RIGHT)
            {
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) + 1, TRUE);
                return 0;
            }
            if (wp == VK_UP)
            {
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) - 1, TRUE);
                return 0;
            }
            if (wp == VK_DOWN)
            {
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) + 1, TRUE);
                return 0;
            }
            if (wp == VK_HOME)
            {
                seek_to_frame_ex(hwnd, 0, TRUE);
                return 0;
            }
            if (wp == VK_END)
            {
                seek_to_frame_ex(hwnd, editp_get_frame_n(g_editp) - 1, TRUE);
                return 0;
            }
            if (wp == VK_NEXT)
            {
                SendMessageA(hwnd, WM_COMMAND, 5187, 0);
                return 0;
            }
            if (wp == VK_PRIOR)
            {
                SendMessageA(hwnd, WM_COMMAND, 5188, 0);
                return 0;
            }
            if (wp == VK_OEM_4)
            {
                SendMessageA(hwnd, WM_COMMAND, 9004, 0);
                return 0;
            }
            if (wp == VK_OEM_6)
            {
                SendMessageA(hwnd, WM_COMMAND, 9003, 0);
                return 0;
            }
        }
        return 0;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP:
        plugin_manager_handle_key(hwnd, (UINT)wp, FALSE, lp);
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

    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        BOOL need_rerender = FALSE;

        if (plugin_manager_handle_command(hwnd, id))
        {
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

        switch (id)
        {
        case IDC_TOGGLE_CONSOLE:
            debug_console_toggle_visible();
            apply_menu(hwnd);
            return 0;
        case IDC_NEW_EXEDIT_720P:
            open_media_or_exo(hwnd, "1280x720_30fps_44100Hz.exedit");
            return 0;
        case IDC_NEW_EXEDIT_1080:
            open_media_or_exo(hwnd, "1920x1080_30fps_48000Hz.exedit");
            return 0;
        case 5097: // file -> Open (Ctrl+O)
            reaviutl_file_open(hwnd);
            return 0;
        case 5168: // file -> Load Audio (音声読み込み)
            reaviutl_audio_open(hwnd);
            return 0;
        case 1003: // file -> AVI Output (Ctrl+S)
            stop_playback(hwnd);
            video_export_avi_dialog(hwnd, g_editp);
            refresh_current_frame(hwnd);
            return 0;
        case 1062: // file -> WAV Output (WAV出力)
            reaviutl_wav_export(hwnd);
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
            video_export_auo_config_dialog(hwnd, -1);
            return 0;
        case 105: // other -> Output Plugin Information
            video_export_show_auo_info(hwnd, i18n_get_lang() == LANG_JA);
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
            stop_playback(hwnd);
            if (g_editp && editp_is_editing(g_editp))
            {
                plugin_manager_notify_file_close(g_editp);
                editp_close_video(g_editp);
                refresh_current_frame(hwnd);
            }
            return 0;
        case 1002: // file -> Exit
            shutdown_main_window(hwnd);
            return 0;

        case 9001: // '<' Prev frame
        case 5141:
            if (g_editp)
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) - 1, TRUE);
            return 0;
        case 9002: // '>' Next frame
        case 5142:
            if (g_editp)
                seek_to_frame_ex(hwnd, editp_get_frame(g_editp) + 1, TRUE);
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
        case 1099: // play / Stop
            toggle_playback(hwnd);
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

        case 1020: // copy filtered output image (Ctrl+C)
        case 1070:
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
            MessageBoxA(hwnd,
                        "AviUtl version 1.10 (reaviutl)\r\nbuild 11003",
                        "Version Information", MB_OK | MB_ICONINFORMATION);
            return 0;

        case IDC_LANG_EN:
            i18n_set_lang(LANG_EN);
            apply_menu(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case IDC_LANG_JA:
            i18n_set_lang(LANG_JA);
            apply_menu(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        default:
            return 0;
        }
    }

    case WM_CLOSE:
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

    (void)hPrev;

    // init console before loading plugins so early stdout/lua output is visible
    debug_console_init(FALSE);
    dbg_info("reaviutl %s starting (PID=%lu, AviUtl 1.10 Build 11003 ABI, VRAM=%dx%d)",
             REAVIUTL_X64 ? "64-bit (x64)" : "32-bit (x86)",
             (unsigned long)GetCurrentProcessId(),
             REAVIUTL_VRAM_W, REAVIUTL_VRAM_H);

    if (!register_classes(hInst))
    {
        MessageBoxA(NULL, "Failed to register window class.", "AviUtl", MB_ICONERROR);
        return 1;
    }

    preview_load_ini(&win_x, &win_y, &win_w, &win_h, &win_max);

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
    plugin_manager_init_ex(plugin_dir, hwnd, g_editp);
    video_export_set_ui_callbacks(cb_export_preview_update, cb_export_progress);
    video_export_scan_auo(plugin_dir, hwnd, g_editp);

    apply_menu(hwnd);
    preview_on_size(hwnd, g_editp);

    if (preview_get_flags() & PREVIEW_FLAG_ALWAYSTOP)
    {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    ShowWindow(hwnd, win_max ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(hwnd);

    // automatically show the exedit timeline window if exedit.auf is installed
    ex_idx = plugin_manager_find_by_name(EXEDIT_SJIS_NAME);
    if (ex_idx >= 0)
    {
        plugin_manager_show_window(ex_idx, TRUE);
    }

    if (!cmdline || strstr(cmdline, "--smoke-exit") == NULL)
    {
        debug_console_start_input(hwnd);
        dbg_info("ready (type 'help' for commands)");
    }

    // open file passed on command line if any (supports --smoke-exit for automated GUI tests)
    if (cmdline && cmdline[0])
    {
        if (strstr(cmdline, "--smoke-exit") != NULL)
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
            char arg[MAX_PATH];
            const char *s = cmdline;
            while (*s == ' ')
                s++;
            if (*s == '"')
            {
                int k = 0;
                s++;
                while (*s && *s != '"' && k < MAX_PATH - 1)
                    arg[k++] = *s++;
                arg[k] = '\0';
            }
            else
            {
                lstrcpynA(arg, s, MAX_PATH);
            }
            if (arg[0])
                open_media_or_exo(hwnd, arg);
        }
    }

    while (GetMessageA(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    debug_console_shutdown();
    video_export_shutdown();
    plugin_manager_shutdown();
    exfunc_shutdown();
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
    return (int)msg.wParam;
}
