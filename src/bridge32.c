#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plugin_bridge.h"
#include "plugin_manager.h"
#include "video_export.h"
#include "color_plugin.h"
#include "input_plugin.h"
#include "vfw_input.h"
#include "exfunc.h"
#include "editp.h"
#include "i18n.h"
#include "debug_console.h"

#if REAVIUTL_X64
#error "bridge32.c must be compiled as 32-bit (x86)"
#endif

static BridgeSharedMem *g_shm = NULL;
static HANDLE g_shm_handle = NULL;
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static HANDLE g_read_ev = NULL;
static HANDLE g_write_ev = NULL;
static HWND g_bridge_hwnd = NULL;
static HWND g_host_main_hwnd = NULL;
static EDITP *g_editp = NULL;
static VFWInput *g_bridge_aui_handles[BRIDGE_MAX_OPEN_AUIS];
static BOOL g_in_rpc = FALSE;
static BOOL g_running = TRUE;

static void sync_shm_from_local(void)
{
    int i, fcount, mcount;
    int s = 0, e = 0;

    if (!g_shm)
        return;

    if (g_editp)
    {
        int rate = 30, scale = 1;
        g_shm->is_editing = editp_is_editing(g_editp) ? 1 : 0;
        editp_get_dimensions(g_editp, &g_shm->w, &g_shm->h,
                             &g_shm->max_w, &g_shm->max_h);
        g_shm->frame = editp_get_frame(g_editp);
        g_shm->frame_n = editp_get_frame_n(g_editp);
        editp_get_fps(g_editp, &rate, &scale);
        g_shm->fps_rate = rate;
        g_shm->fps_scale = scale;
        g_shm->audio_rate = editp_get_audio_rate(g_editp);
        g_shm->audio_ch = editp_get_audio_ch(g_editp);
        g_shm->has_audio = editp_has_audio(g_editp) ? 1 : 0;
        editp_get_select_frame(g_editp, &s, &e);
        g_shm->select_s = s;
        g_shm->select_e = e;
        lstrcpynA(g_shm->edit_path, editp_get_edit_path(g_editp),
                  sizeof(g_shm->edit_path));
        lstrcpynA(g_shm->project_path, editp_get_project_path(g_editp),
                  sizeof(g_shm->project_path));
        lstrcpynA(g_shm->output_path, editp_get_output_path(g_editp),
                  sizeof(g_shm->output_path));
        if (g_shm->frame_n > 0)
        {
            int lim = (g_shm->frame_n < REAVIUTL_MAX_FRAME) ? g_shm->frame_n : REAVIUTL_MAX_FRAME;
            const BYTE *tbl = editp_get_frame_status_table(g_editp, 0);
            if (tbl)
            {
                memcpy(g_shm->frame_status, tbl, (size_t)lim);
            }
        }
    }

    fcount = plugin_manager_get_count();
    if (fcount > BRIDGE_MAX_FILTERS)
        fcount = BRIDGE_MAX_FILTERS;
    g_shm->filter_count = fcount;
    for (i = 0; i < fcount; i++)
    {
        const char *nm = plugin_manager_get_name(i);
        const char *inf = plugin_manager_get_information(i);
        const char *pth = plugin_manager_get_path(i);
        HWND hw = plugin_manager_get_window(i);
        int fl = plugin_manager_get_flags(i);
        lstrcpynA(g_shm->filters[i].name, nm ? nm : "", sizeof(g_shm->filters[i].name));
        lstrcpynA(g_shm->filters[i].information, inf ? inf : "",
                  sizeof(g_shm->filters[i].information));
        lstrcpynA(g_shm->filters[i].path, pth ? pth : "", sizeof(g_shm->filters[i].path));
        g_shm->filters[i].flag = fl;
        g_shm->filters[i].has_window = (hw != NULL) ? 1 : 0;
        g_shm->filters[i].window_visible = ((fl & FILTER_FLAG_WINDOW_DISP) != 0) ? 1 : 0;
        g_shm->filters[i].hwnd32 = (uint32_t)(uintptr_t)hw;
    }

    mcount = plugin_manager_get_menu_item_count();
    if (mcount > BRIDGE_MAX_MENU_ITEMS)
        mcount = BRIDGE_MAX_MENU_ITEMS;
    g_shm->menu_item_count = mcount;
    for (i = 0; i < mcount; i++)
    {
        PluginMenuItem it;
        if (plugin_manager_get_menu_item(i, &it))
        {
            g_shm->menu_items[i].filter_index = it.filter_index;
            lstrcpynA(g_shm->menu_items[i].name, it.name, sizeof(g_shm->menu_items[i].name));
            g_shm->menu_items[i].cmd_id = it.cmd_id;
            g_shm->menu_items[i].def_key = it.def_key;
            g_shm->menu_items[i].flag = it.flag;
        }
    }

    {
        int acount = video_export_get_auo_count();
        if (acount > BRIDGE_MAX_AUOS)
            acount = BRIDGE_MAX_AUOS;
        g_shm->auo_count = acount;
        for (i = 0; i < acount; i++)
        {
            AuoPluginSummary sum;
            if (video_export_get_auo_summary(i, &sum))
            {
                lstrcpynA(g_shm->auos[i].name, sum.name, sizeof(g_shm->auos[i].name));
                memcpy(g_shm->auos[i].filefilter, sum.filefilter,
                       sizeof(g_shm->auos[i].filefilter));
                lstrcpynA(g_shm->auos[i].information, sum.information,
                          sizeof(g_shm->auos[i].information));
                lstrcpynA(g_shm->auos[i].path, sum.path, sizeof(g_shm->auos[i].path));
                g_shm->auos[i].flag = sum.flag;
                g_shm->auos[i].has_config = sum.has_config ? 1 : 0;
            }
        }
    }

    {
        int icount = input_plugin_get_ext_count();
        if (icount > BRIDGE_MAX_AUIS)
            icount = BRIDGE_MAX_AUIS;
        g_shm->aui_count = icount;
        for (i = 0; i < icount; i++)
        {
            InputPluginSummary isum;
            if (input_plugin_get_ext_summary(i, &isum))
            {
                int flen = isum.filefilter_len;
                lstrcpynA(g_shm->auis[i].name, isum.name, sizeof(g_shm->auis[i].name));
                memset(g_shm->auis[i].filefilter, 0, sizeof(g_shm->auis[i].filefilter));
                if (flen > 0)
                {
                    if (flen > (int)sizeof(g_shm->auis[i].filefilter))
                        flen = (int)sizeof(g_shm->auis[i].filefilter);
                    memcpy(g_shm->auis[i].filefilter, isum.filefilter, (size_t)flen);
                }
                g_shm->auis[i].filefilter_len = flen;
                lstrcpynA(g_shm->auis[i].information, isum.information,
                          sizeof(g_shm->auis[i].information));
                lstrcpynA(g_shm->auis[i].path, isum.path, sizeof(g_shm->auis[i].path));
                g_shm->auis[i].flag = isum.flag;
                g_shm->auis[i].has_config = isum.has_config ? 1 : 0;
                g_shm->auis[i].priority = isum.priority;
            }
        }
    }

    {
        int ccount = color_plugin_get_ext_count();
        if (ccount > BRIDGE_MAX_AUCS)
            ccount = BRIDGE_MAX_AUCS;
        g_shm->auc_count = ccount;
        for (i = 0; i < ccount; i++)
        {
            ColorPluginSummary csum;
            if (color_plugin_get_ext_summary(i, &csum))
            {
                lstrcpynA(g_shm->aucs[i].name, csum.name, sizeof(g_shm->aucs[i].name));
                lstrcpynA(g_shm->aucs[i].information, csum.information,
                          sizeof(g_shm->aucs[i].information));
                lstrcpynA(g_shm->aucs[i].path, csum.path, sizeof(g_shm->aucs[i].path));
                g_shm->aucs[i].flag = csum.flag;
            }
        }
    }

    InterlockedIncrement(&g_shm->state_seq);
}

static LRESULT CALLBACK BridgeOwnerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_REAVIUTL_PLUGIN_REDRAW)
    {
        sync_shm_from_local();
        if (!g_in_rpc && g_host_main_hwnd && IsWindow(g_host_main_hwnd))
        {
            PostMessageA(g_host_main_hwnd, WM_REAVIUTL_BRIDGE_NOTIFY, wp, 0);
        }
        return 0;
    }
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ||
        msg == WM_KEYUP || msg == WM_SYSKEYUP)
    {
        BOOL is_down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        if (plugin_manager_handle_key(hwnd, (UINT)wp, is_down, lp))
        {
            sync_shm_from_local();
            if (!g_in_rpc && g_host_main_hwnd && IsWindow(g_host_main_hwnd))
            {
                PostMessageA(g_host_main_hwnd, WM_REAVIUTL_BRIDGE_NOTIFY, 1, 0);
            }
            return 0;
        }
        if (is_down && g_host_main_hwnd && IsWindow(g_host_main_hwnd) &&
            (wp == VK_SPACE || wp == VK_LEFT || wp == VK_RIGHT ||
             wp == VK_HOME || wp == VK_END))
        {
            PostMessageA(g_host_main_hwnd, WM_KEYDOWN, wp, lp);
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static BOOL pipe_write_full(const void *buf, DWORD len)
{
    const BYTE *p = (const BYTE *)buf;
    DWORD rem = len;
    while (rem > 0)
    {
        OVERLAPPED ov;
        DWORD written = 0;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = g_write_ev;
        ResetEvent(g_write_ev);
        if (!WriteFile(g_pipe, p, rem, &written, &ov))
        {
            if (GetLastError() != ERROR_IO_PENDING)
                return FALSE;
            if (!GetOverlappedResult(g_pipe, &ov, &written, TRUE))
                return FALSE;
        }
        if (written == 0)
            return FALSE;
        p += written;
        rem -= written;
    }
    return TRUE;
}

static BOOL pipe_read_rest(void *buf, DWORD already, DWORD total)
{
    BYTE *p = (BYTE *)buf + already;
    DWORD rem = total - already;
    while (rem > 0)
    {
        OVERLAPPED ov;
        DWORD got = 0;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = g_read_ev;
        ResetEvent(g_read_ev);
        if (!ReadFile(g_pipe, p, rem, &got, &ov))
        {
            if (GetLastError() != ERROR_IO_PENDING)
                return FALSE;
            if (!GetOverlappedResult(g_pipe, &ov, &got, TRUE))
                return FALSE;
        }
        if (got == 0)
            return FALSE;
        p += got;
        rem -= got;
    }
    return TRUE;
}

static void handle_rpc(const BridgeRpcReq *req, BridgeRpcResp *resp)
{
    ZeroMemory(resp, sizeof(*resp));

    switch (req->cmd)
    {
    case BRIDGE_CMD_PING:
        resp->ok = 1;
        resp->val0 = 1;
        break;

    case BRIDGE_CMD_LOAD_DIR:
    {
        BOOL f_ok = plugin_manager_init_ex(req->str, g_bridge_hwnd, g_editp);
        resp->val0 = plugin_manager_get_count();
        resp->ok = f_ok ? 1 : 0;
        break;
    }

    case BRIDGE_CMD_LOAD_FILE:
        resp->ok = plugin_manager_load_single_auf(req->str, g_bridge_hwnd, g_editp) ? 1 : 0;
        resp->val0 = plugin_manager_get_count();
        break;

    case BRIDGE_CMD_SCAN_AUO:
        resp->val0 = video_export_scan_auo(req->str, g_bridge_hwnd, g_editp);
        resp->ok = 1;
        break;

    case BRIDGE_CMD_LOAD_AUO:
        resp->ok = video_export_load_single_auo(req->str, g_bridge_hwnd, g_editp) ? 1 : 0;
        resp->val0 = video_export_get_auo_count();
        break;

    case BRIDGE_CMD_SAVE_START:
        if (g_editp)
        {
            plugin_manager_notify_save_start(g_editp, req->arg0, req->arg1, req->arg2);
            resp->ok = 1;
        }
        break;

    case BRIDGE_CMD_SAVE_END:
        if (g_editp)
        {
            plugin_manager_notify_save_end(g_editp, req->arg0);
            resp->ok = 1;
        }
        break;

    case BRIDGE_CMD_AUO_OUTPUT:
        if (g_editp)
        {
            resp->ok = video_export_auo_file(g_bridge_hwnd, g_editp,
                                             req->arg0, req->str, req->arg1)
                           ? 1
                           : 0;
        }
        break;

    case BRIDGE_CMD_AUO_CONFIG:
        resp->ok = video_export_auo_config_dialog(
                       (g_host_main_hwnd && IsWindow(g_host_main_hwnd)) ? g_host_main_hwnd : g_bridge_hwnd,
                       req->arg0)
                       ? 1
                       : 0;
        break;

    case BRIDGE_CMD_OPEN_VIDEO:
        if (g_editp && editp_open_video(g_editp, req->str))
        {
            plugin_manager_notify_file_open(g_editp);
            resp->ok = 1;
        }
        break;

    case BRIDGE_CMD_OPEN_AUDIO:
        if (g_editp && editp_open_audio(g_editp, req->str))
        {
            plugin_manager_notify_update(g_editp);
            resp->ok = 1;
        }
        break;

    case BRIDGE_CMD_CLOSE_VIDEO:
        if (g_editp)
        {
            plugin_manager_notify_file_close(g_editp);
            editp_close_video(g_editp);
            resp->ok = 1;
        }
        break;

    case BRIDGE_CMD_SYNC_STATE:
        if (g_editp)
        {
            if (req->arg1 > 0 && req->arg1 != editp_get_frame_n(g_editp))
            {
                editp_set_frame_n(g_editp, req->arg1);
            }
            if (g_shm && editp_get_frame_n(g_editp) > 0)
            {
                int fn = editp_get_frame_n(g_editp);
                int lim = (fn < REAVIUTL_MAX_FRAME) ? fn : REAVIUTL_MAX_FRAME;
                BYTE *tbl = editp_get_frame_status_table(g_editp, 0);
                if (tbl)
                {
                    memcpy(tbl, g_shm->frame_status, (size_t)lim);
                }
                editp_set_output_path(g_editp, g_shm->output_path);
            }
            editp_set_frame(g_editp, req->arg0);
            editp_set_select_frame(g_editp, req->arg2, req->arg3);
            if (req->arg4 > 0 && req->arg5 > 0)
            {
                editp_set_display_size(g_editp, req->arg4, req->arg5);
            }
            plugin_manager_notify_update(g_editp);
            resp->ok = 1;
        }
        break;

    case BRIDGE_CMD_LOAD_AUP:
        if (g_editp)
        {
            resp->ok = plugin_manager_load_aup(req->str, g_editp) ? 1 : 0;
        }
        break;

    case BRIDGE_CMD_SAVE_AUP:
        if (g_editp)
        {
            resp->ok = plugin_manager_save_aup(req->str, g_editp) ? 1 : 0;
        }
        break;

    case BRIDGE_CMD_SHOW_WINDOW:
        if (req->arg1 == 2)
            resp->ok = plugin_manager_toggle_window(req->arg0) ? 1 : 0;
        else
            resp->ok = plugin_manager_show_window(req->arg0, req->arg1 != 0) ? 1 : 0;
        break;

    case BRIDGE_CMD_SET_ACTIVE:
        if (req->arg1 == 3)
        {
            plugin_manager_disable_all_filters();
            resp->ok = 1;
        }
        else if (req->arg1 == 2)
        {
            resp->ok = plugin_manager_toggle_active(req->arg0) ? 1 : 0;
        }
        else
        {
            resp->ok = plugin_manager_set_active(req->arg0, req->arg1 != 0) ? 1 : 0;
        }
        break;

    case BRIDGE_CMD_FILTER_IO:
        resp->ok = plugin_manager_exec_import_export(req->arg0, req->arg1 != 0) ? 1 : 0;
        break;

    case BRIDGE_CMD_SCAN_INPUT_COLOR:
        color_plugin_init(req->str, g_bridge_hwnd);
        input_plugin_init(req->str, g_bridge_hwnd);
        resp->val0 = input_plugin_get_ext_count();
        resp->val1 = color_plugin_get_ext_count();
        resp->ok = 1;
        break;

    case BRIDGE_CMD_AUI_CONFIG:
    {
        int idx = input_plugin_ext_to_index(req->arg0);
        HWND owner = (g_host_main_hwnd && IsWindow(g_host_main_hwnd)) ? g_host_main_hwnd : g_bridge_hwnd;
        resp->ok = (idx >= 0 && input_plugin_config(idx, owner)) ? 1 : 0;
        break;
    }

    case BRIDGE_CMD_AUI_SET_PRIORITY:
    {
        int idx = input_plugin_ext_to_index(req->arg0);
        if (idx >= 0)
        {
            input_plugin_set_single_priority(idx, req->arg1);
            resp->ok = 1;
        }
        break;
    }

    case BRIDGE_CMD_AUI_OPEN:
    {
        int idx = input_plugin_ext_to_index(req->arg0);
        int slot = -1, s;
        resp->val0 = -1;
        for (s = 0; s < BRIDGE_MAX_OPEN_AUIS; s++)
        {
            if (!g_bridge_aui_handles[s])
            {
                slot = s;
                break;
            }
        }
        if (idx >= 0 && slot >= 0 && g_shm)
        {
            FILE_INFO fi;
            VFWInput *vin = vfw_input_open_aui_index(idx, req->str, &fi, req->arg1);
            if (vin)
            {
                g_bridge_aui_handles[slot] = vin;
                memset(&g_shm->aui_open_info, 0, sizeof(g_shm->aui_open_info));
                g_shm->aui_open_info.has_video = (fi.flag & FILE_INFO_FLAG_VIDEO) ? 1 : 0;
                g_shm->aui_open_info.has_audio = (fi.flag & FILE_INFO_FLAG_AUDIO) ? 1 : 0;
                g_shm->aui_open_info.w = fi.w;
                g_shm->aui_open_info.h = fi.h;
                g_shm->aui_open_info.frame_n = fi.frame_n;
                g_shm->aui_open_info.video_rate = fi.video_rate;
                g_shm->aui_open_info.video_scale = fi.video_scale;
                g_shm->aui_open_info.decode_bpp = fi.video_decode_bit ? fi.video_decode_bit : 24;
                g_shm->aui_open_info.audio_rate = fi.audio_rate;
                g_shm->aui_open_info.audio_ch = fi.audio_ch;
                g_shm->aui_open_info.audio_n = fi.audio_n;
                resp->val0 = slot;
                resp->ok = 1;
            }
        }
        break;
    }

    case BRIDGE_CMD_AUI_CLOSE:
        if (req->arg0 >= 0 && req->arg0 < BRIDGE_MAX_OPEN_AUIS &&
            g_bridge_aui_handles[req->arg0])
        {
            vfw_input_close(g_bridge_aui_handles[req->arg0]);
            g_bridge_aui_handles[req->arg0] = NULL;
            resp->ok = 1;
        }
        break;

    case BRIDGE_CMD_AUI_READ_VIDEO:
        if (req->arg0 >= 0 && req->arg0 < BRIDGE_MAX_OPEN_AUIS &&
            g_bridge_aui_handles[req->arg0] && g_shm)
        {
            resp->ok = vfw_input_read_frame_stride(g_bridge_aui_handles[req->arg0],
                                                   req->arg1,
                                                   g_shm->ycp_frame,
                                                   REAVIUTL_VRAM_W)
                           ? 1
                           : 0;
        }
        break;

    case BRIDGE_CMD_AUI_READ_AUDIO:
        if (req->arg0 >= 0 && req->arg0 < BRIDGE_MAX_OPEN_AUIS &&
            g_bridge_aui_handles[req->arg0] && g_shm && req->arg2 > 0)
        {
            VFWInput *vin = g_bridge_aui_handles[req->arg0];
            int smp = req->arg2;
            int ch = (req->arg4 >= 2) ? 2 : 1;
            if (smp > BRIDGE_MAX_AUDIO_SAMPLES)
                smp = BRIDGE_MAX_AUDIO_SAMPLES;
            if (req->arg3 > 0)
                vfw_input_set_audio_format(vin, req->arg3, ch);
            resp->val0 = vfw_input_read_audio(vin, req->arg1, g_shm->pcm_audio, smp);
            resp->ok = 1;
        }
        break;

    case BRIDGE_CMD_AUI_IS_KEYFRAME:
        if (req->arg0 >= 0 && req->arg0 < BRIDGE_MAX_OPEN_AUIS &&
            g_bridge_aui_handles[req->arg0])
        {
            resp->val0 = vfw_input_is_keyframe(g_bridge_aui_handles[req->arg0], req->arg1) ? 1 : 0;
            resp->ok = 1;
        }
        break;

    case BRIDGE_CMD_EXEC_MENU_ITEM:
        resp->ok = plugin_manager_exec_menu_item_index(req->arg0) ? 1 : 0;
        break;

    case BRIDGE_CMD_FILTER_COMMAND:
        resp->ok = plugin_manager_send_filter_command(req->arg0, req->arg1) ? 1 : 0;
        break;

    case BRIDGE_CMD_IMPORT_EXO:
        resp->ok = plugin_manager_import_exo(req->arg0, req->str, g_editp) ? 1 : 0;
        break;

    case BRIDGE_CMD_MAIN_MOUSE:
        resp->val0 = plugin_manager_forward_main_mouse(
                         (UINT)req->arg0, (WPARAM)req->arg1, req->arg2, req->arg3)
                         ? 1
                         : 0;
        resp->ok = 1;
        break;

    case BRIDGE_CMD_MAIN_KEY:
        resp->val0 = plugin_manager_handle_key(
                         g_bridge_hwnd, (UINT)req->arg0, req->arg1 != 0, (LPARAM)req->arg2)
                         ? 1
                         : 0;
        resp->ok = 1;
        break;

    case BRIDGE_CMD_RENDER_FRAME:
        if (g_editp && g_shm && editp_is_editing(g_editp))
        {
            int w = 0, h = 0, max_w = 0, max_h = 0, y;
            if (req->arg1 != 0)
            {
                // host supplied pre-filtered ycp_frame in shared memory
                PIXEL_YC *dst = editp_get_ycp_edit(g_editp);
                editp_set_frame(g_editp, req->arg0);
                editp_get_dimensions(g_editp, &w, &h, &max_w, &max_h);
                if (dst && w > 0 && h > 0)
                {
                    int copy_w = (w < REAVIUTL_VRAM_W) ? w : REAVIUTL_VRAM_W;
                    int copy_h = (h < REAVIUTL_VRAM_H) ? h : REAVIUTL_VRAM_H;
                    for (y = 0; y < copy_h; y++)
                    {
                        memcpy(&dst[y * max_w],
                               &g_shm->ycp_frame[y * REAVIUTL_VRAM_W],
                               (size_t)copy_w * sizeof(PIXEL_YC));
                    }
                }
                {
                    int fi, fn = plugin_manager_get_count();
                    for (fi = 0; fi < fn; fi++)
                    {
                        FILTER *fp = plugin_manager_get_filter(fi);
                        if (fp && (fp->flag & FILTER_FLAG_ACTIVE) &&
                            !(fp->flag & FILTER_FLAG_AUDIO_FILTER))
                        {
                            plugin_manager_apply_filter(fi, g_editp);
                        }
                    }
                }
                resp->ok = 1;
            }
            else
            {
                resp->ok = plugin_manager_render_video_frame(g_editp, req->arg0) ? 1 : 0;
            }

            if (resp->ok)
            {
                const PIXEL_YC *src = editp_get_ycp_edit(g_editp);
                editp_get_dimensions(g_editp, &w, &h, &max_w, &max_h);
                if (src && w > 0 && h > 0)
                {
                    int copy_w = (w < REAVIUTL_VRAM_W) ? w : REAVIUTL_VRAM_W;
                    int copy_h = (h < REAVIUTL_VRAM_H) ? h : REAVIUTL_VRAM_H;
                    for (y = 0; y < copy_h; y++)
                    {
                        memcpy(&g_shm->ycp_frame[y * REAVIUTL_VRAM_W],
                               &src[y * max_w],
                               (size_t)copy_w * sizeof(PIXEL_YC));
                    }
                }
            }
        }
        break;

    case BRIDGE_CMD_RENDER_AUDIO:
        if (g_editp && g_shm && editp_is_editing(g_editp) && req->arg1 > 0)
        {
            int smp = req->arg1;
            if (smp > BRIDGE_MAX_AUDIO_SAMPLES)
                smp = BRIDGE_MAX_AUDIO_SAMPLES;
            resp->val0 = plugin_manager_apply_audio_filters(
                g_editp, req->arg0, g_shm->pcm_audio, smp, NULL);
            resp->ok = 1;
        }
        break;

    case BRIDGE_CMD_DEBUG_CMD:
        if (lstrcmpiA(req->str, "plugins") == 0)
        {
            plugin_manager_debug_dump();
        }
        else if (lstrcmpiA(req->str, "trace on") == 0)
        {
            debug_console_set_exfunc_trace(TRUE);
            dbg_info("32-bit bridge EXFUNC tracing ENABLED");
        }
        else if (lstrcmpiA(req->str, "trace off") == 0)
        {
            debug_console_set_exfunc_trace(FALSE);
            dbg_info("32-bit bridge EXFUNC tracing DISABLED");
        }
        else if (lstrcmpiA(req->str, "status") == 0 && g_editp)
        {
            int w = 0, h = 0, mw = 0, mh = 0, rate = 0, scale = 0, s = 0, e = 0;
            editp_get_dimensions(g_editp, &w, &h, &mw, &mh);
            editp_get_fps(g_editp, &rate, &scale);
            editp_get_select_frame(g_editp, &s, &e);
            dbg_info("Bridge EDITP=0x%p editing=%d %dx%d (VRAM %dx%d) frame=%d/%d sel=[%d..%d] fps=%d/%d",
                     (void *)g_editp, editp_is_editing(g_editp), w, h, mw, mh,
                     editp_get_frame(g_editp), editp_get_frame_n(g_editp), s, e, rate, scale);
        }
        resp->ok = 1;
        break;

    case BRIDGE_CMD_SHUTDOWN:
        resp->ok = 1;
        g_running = FALSE;
        break;

    default:
        lstrcpynA(resp->err, "unknown bridge command", sizeof(resp->err));
        break;
    }
}

int main(int argc, char **argv)
{
    const char *pipe_name;
    const char *shm_name;
    DWORD parent_pid;
    unsigned long long host_hwnd_val = 0;
    HANDLE hParent = NULL;
    WNDCLASSA wc;
    OVERLAPPED ov_read;
    BridgeRpcReq req;
    BOOL read_pending = FALSE;

    if (argc < 5)
    {
        fprintf(stderr, "Usage: reaviutl_bridge32 <pipe> <shm> <parent_pid> <host_hwnd>\n");
        return 1;
    }

    debug_console_init(TRUE);

    pipe_name = argv[1];
    shm_name = argv[2];
    parent_pid = (DWORD)strtoul(argv[3], NULL, 10);
    host_hwnd_val = _strtoui64(argv[4], NULL, 10);
    g_host_main_hwnd = (HWND)(uintptr_t)host_hwnd_val;

    if (parent_pid != 0)
    {
        hParent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    }

    g_shm_handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm_name);
    if (!g_shm_handle)
        return 2;

    g_shm = (BridgeSharedMem *)MapViewOfFile(g_shm_handle, FILE_MAP_ALL_ACCESS,
                                             0, 0, sizeof(BridgeSharedMem));
    if (!g_shm || g_shm->magic != BRIDGE_MAGIC)
        return 3;
    video_export_set_bridge_telemetry(&g_shm->encode_abort,
                                      &g_shm->encode_now,
                                      &g_shm->encode_total);

    g_pipe = CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE,
                         0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (g_pipe == INVALID_HANDLE_VALUE)
        return 4;

    g_read_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_write_ev = CreateEventA(NULL, TRUE, FALSE, NULL);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = BridgeOwnerWndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "AviUtlBridgeOwnerClass";
    RegisterClassA(&wc);

    g_bridge_hwnd = CreateWindowExA(
        0, "AviUtlBridgeOwnerClass", "AviUtl", WS_OVERLAPPEDWINDOW,
        0, 0, 320, 240, NULL, NULL, GetModuleHandleA(NULL), NULL);

    i18n_init(GetModuleHandleA(NULL));

    g_editp = editp_create_idle(g_bridge_hwnd);
    exfunc_set_editp(g_editp);
    dbg_info("32-bit plugin bridge ready (PID=%lu, bridge_hwnd=0x%p, EDITP=0x%p)",
             (unsigned long)GetCurrentProcessId(), (void *)g_bridge_hwnd, (void *)g_editp);

    ZeroMemory(&ov_read, sizeof(ov_read));
    ov_read.hEvent = g_read_ev;

    while (g_running)
    {
        DWORD got = 0;

        if (!read_pending)
        {
            ResetEvent(g_read_ev);
            ZeroMemory(&req, sizeof(req));
            if (ReadFile(g_pipe, &req, sizeof(req), &got, &ov_read))
            {
                // completed synchronously
                BridgeRpcResp resp;
                if (got < sizeof(req) && !pipe_read_rest(&req, got, sizeof(req)))
                    break;
                g_in_rpc = TRUE;
                handle_rpc(&req, &resp);
                sync_shm_from_local();
                g_in_rpc = FALSE;
                if (!pipe_write_full(&resp, sizeof(resp)))
                    break;
                continue;
            }
            else
            {
                if (GetLastError() != ERROR_IO_PENDING)
                    break;
                read_pending = TRUE;
            }
        }

        // pump all queued Win32 messages first so exedit's UI stays responsive
        {
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                {
                    g_running = FALSE;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (!g_running)
                break;
        }

        // wait for either RPC request, parent termination, or Win32 UI input
        {
            HANDLE waits[2];
            DWORD nwaits = 0;
            DWORD wr;

            waits[nwaits++] = g_read_ev;
            if (hParent)
                waits[nwaits++] = hParent;

            wr = MsgWaitForMultipleObjects(nwaits, waits, FALSE, 250, QS_ALLINPUT);
            if (wr == WAIT_OBJECT_0)
            {
                BridgeRpcResp resp;
                read_pending = FALSE;
                if (!GetOverlappedResult(g_pipe, &ov_read, &got, FALSE) || got == 0)
                    break;
                if (got < sizeof(req) && !pipe_read_rest(&req, got, sizeof(req)))
                    break;
                g_in_rpc = TRUE;
                handle_rpc(&req, &resp);
                sync_shm_from_local();
                g_in_rpc = FALSE;
                if (!pipe_write_full(&resp, sizeof(resp)))
                    break;
            }
            else if (hParent && wr == WAIT_OBJECT_0 + 1)
            {
                // 64-bit host exited, clean shutdown immediately
                break;
            }
        }
    }

    if (read_pending)
    {
        CancelIo(g_pipe);
    }

    {
        int s;
        for (s = 0; s < BRIDGE_MAX_OPEN_AUIS; s++)
        {
            if (g_bridge_aui_handles[s])
            {
                vfw_input_close(g_bridge_aui_handles[s]);
                g_bridge_aui_handles[s] = NULL;
            }
        }
    }
    video_export_shutdown();
    plugin_manager_shutdown();
    input_plugin_shutdown();
    color_plugin_shutdown();
    exfunc_shutdown();
    if (g_editp)
    {
        editp_destroy(g_editp);
        g_editp = NULL;
    }
    if (g_bridge_hwnd && IsWindow(g_bridge_hwnd))
    {
        DestroyWindow(g_bridge_hwnd);
    }
    if (g_pipe != INVALID_HANDLE_VALUE)
        CloseHandle(g_pipe);
    if (g_read_ev)
        CloseHandle(g_read_ev);
    if (g_write_ev)
        CloseHandle(g_write_ev);
    if (g_shm)
        UnmapViewOfFile(g_shm);
    if (g_shm_handle)
        CloseHandle(g_shm_handle);
    if (hParent)
        CloseHandle(hParent);
    return 0;
}
