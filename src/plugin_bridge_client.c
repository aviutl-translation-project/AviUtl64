#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plugin_bridge.h"
#include "debug_console.h"

#if REAVIUTL_X64

static HANDLE g_shm_handle = NULL;
static BridgeSharedMem *g_shm = NULL;
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static HANDLE g_proc = NULL;
static HANDLE g_io_event = NULL;
static BOOL g_active = FALSE;
static LONG g_last_seq = 0;

static BOOL wait_overlapped_msg_safe(HANDLE hPipe, OVERLAPPED *ov,
                                     DWORD *out_bytes, DWORD timeout_ms)
{
    DWORD start = GetTickCount();
    for (;;)
    {
        DWORD elapsed = GetTickCount() - start;
        DWORD remain = (elapsed >= timeout_ms) ? 0 : (timeout_ms - elapsed);
        DWORD wr = MsgWaitForMultipleObjects(1, &ov->hEvent, FALSE,
                                             remain, QS_SENDMESSAGE);
        if (wr == WAIT_OBJECT_0)
        {
            return GetOverlappedResult(hPipe, ov, out_bytes, FALSE);
        }
        if (wr == WAIT_OBJECT_0 + 1)
        {
            MSG msg;
            // dispatch any pending cross-thread/process SendMessage without dequeuing normal posted input messages
            PeekMessageA(&msg, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            continue;
        }
        CancelIo(hPipe);
        return FALSE;
    }
}

static BOOL pipe_write_all(const void *buf, DWORD len, DWORD timeout_ms)
{
    const BYTE *p = (const BYTE *)buf;
    DWORD rem = len;
    while (rem > 0)
    {
        OVERLAPPED ov;
        DWORD written = 0;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = g_io_event;
        ResetEvent(g_io_event);
        if (!WriteFile(g_pipe, p, rem, &written, &ov))
        {
            if (GetLastError() != ERROR_IO_PENDING)
                return FALSE;
            if (!wait_overlapped_msg_safe(g_pipe, &ov, &written, timeout_ms))
                return FALSE;
        }
        if (written == 0)
            return FALSE;
        p += written;
        rem -= written;
    }
    return TRUE;
}

static BOOL pipe_read_all(void *buf, DWORD len, DWORD timeout_ms)
{
    BYTE *p = (BYTE *)buf;
    DWORD rem = len;
    while (rem > 0)
    {
        OVERLAPPED ov;
        DWORD got = 0;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = g_io_event;
        ResetEvent(g_io_event);
        if (!ReadFile(g_pipe, p, rem, &got, &ov))
        {
            if (GetLastError() != ERROR_IO_PENDING)
                return FALSE;
            if (!wait_overlapped_msg_safe(g_pipe, &ov, &got, timeout_ms))
                return FALSE;
        }
        if (got == 0)
            return FALSE;
        p += got;
        rem -= got;
    }
    return TRUE;
}

static BOOL bridge_rpc(const BridgeRpcReq *req, BridgeRpcResp *resp, DWORD timeout_ms)
{
    BridgeRpcResp dummy;
    if (!g_active || g_pipe == INVALID_HANDLE_VALUE)
        return FALSE;
    if (g_proc && WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0)
    {
        g_active = FALSE;
        return FALSE;
    }
    if (!resp)
        resp = &dummy;
    ZeroMemory(resp, sizeof(*resp));
    if (!pipe_write_all(req, sizeof(*req), timeout_ms))
        return FALSE;
    if (!pipe_read_all(resp, sizeof(*resp), timeout_ms))
        return FALSE;
    return resp->ok != 0;
}

// blocking rpc for .auo export and config dialogs while bridge process is alive
static BOOL bridge_rpc_interactive(const BridgeRpcReq *req, BridgeRpcResp *resp,
                                   HWND owner, BridgeEncodeProgressFn on_progress, void *user)
{
    BridgeRpcResp dummy;
    BYTE *p;
    DWORD rem;
    LONG last_now = -1, last_tot = -1;

    if (!g_active || g_pipe == INVALID_HANDLE_VALUE)
        return FALSE;
    if (g_proc && WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0)
    {
        g_active = FALSE;
        return FALSE;
    }
    if (!resp)
        resp = &dummy;
    ZeroMemory(resp, sizeof(*resp));

    if (!pipe_write_all(req, sizeof(*req), 10000))
        return FALSE;

    p = (BYTE *)resp;
    rem = sizeof(*resp);
    while (rem > 0)
    {
        OVERLAPPED ov;
        DWORD got = 0;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = g_io_event;
        ResetEvent(g_io_event);
        if (!ReadFile(g_pipe, p, rem, &got, &ov))
        {
            if (GetLastError() != ERROR_IO_PENDING)
                return FALSE;
            for (;;)
            {
                HANDLE waits[2];
                DWORD nwaits = 0;
                DWORD wr;
                waits[nwaits++] = ov.hEvent;
                if (g_proc)
                    waits[nwaits++] = g_proc;

                wr = MsgWaitForMultipleObjects(nwaits, waits, FALSE, 50,
                                               QS_SENDMESSAGE | QS_KEY | QS_PAINT);
                if (wr == WAIT_OBJECT_0)
                {
                    if (!GetOverlappedResult(g_pipe, &ov, &got, FALSE))
                        return FALSE;
                    break;
                }
                if (g_proc && wr == WAIT_OBJECT_0 + 1)
                {
                    CancelIo(g_pipe);
                    g_active = FALSE;
                    return FALSE;
                }
                {
                    MSG msg;
                    PeekMessageA(&msg, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
                    if (owner && IsWindow(owner))
                    {
                        while (PeekMessageA(&msg, owner, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE))
                        {
                            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && g_shm)
                            {
                                InterlockedExchange(&g_shm->encode_abort, 1);
                            }
                        }
                        if (PeekMessageA(&msg, owner, WM_PAINT, WM_PAINT, PM_REMOVE))
                        {
                            DispatchMessageA(&msg);
                        }
                    }
                }
                if (on_progress && g_shm)
                {
                    LONG c_now = g_shm->encode_now;
                    LONG c_tot = g_shm->encode_total;
                    if (c_tot > 0 && (c_now != last_now || c_tot != last_tot))
                    {
                        last_now = c_now;
                        last_tot = c_tot;
                        on_progress((int)c_now, (int)c_tot, user);
                    }
                }
            }
        }
        if (got == 0)
            return FALSE;
        p += got;
        rem -= got;
    }
    return resp->ok != 0;
}

static BOOL find_bridge_exe(char *out_path, DWORD out_sz)
{
    char exe_dir[MAX_PATH];
    char *slash;
    DWORD attr;

    if (GetModuleFileNameA(NULL, exe_dir, MAX_PATH) > 0)
    {
        slash = strrchr(exe_dir, '\\');
        if (slash)
            *slash = '\0';
        wsprintfA(out_path, "%s\\reaviutl_bridge32.exe", exe_dir);
        attr = GetFileAttributesA(out_path);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
            return TRUE;

        // check parent\build\reaviutl_bridge32.exe if running from build64\Debug
        wsprintfA(out_path, "%s\\..\\build\\reaviutl_bridge32.exe", exe_dir);
        attr = GetFileAttributesA(out_path);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
            return TRUE;
    }

    lstrcpynA(out_path, "build\\reaviutl_bridge32.exe", (int)out_sz);
    attr = GetFileAttributesA(out_path);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

static BOOL ensure_bridge_started(HWND main_hwnd)
{
    char bridge_exe[MAX_PATH];
    char pipe_name[128];
    char shm_name[128];
    char cmdline[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD pid = GetCurrentProcessId();
    static DWORD s_instance = 0;
    DWORD inst;
    OVERLAPPED ov;
    DWORD dummy = 0;

    if (g_active)
        return TRUE;
    if (!find_bridge_exe(bridge_exe, sizeof(bridge_exe)))
    {
        return FALSE;
    }

    inst = ++s_instance;
    wsprintfA(pipe_name, "\\\\.\\pipe\\reaviutl_bridge_%lu_%lu",
              (unsigned long)pid, (unsigned long)inst);
    wsprintfA(shm_name, "Local\\reaviutl_shm_%lu_%lu",
              (unsigned long)pid, (unsigned long)inst);

    g_shm_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                      0, (DWORD)sizeof(BridgeSharedMem), shm_name);
    if (!g_shm_handle)
        return FALSE;

    g_shm = (BridgeSharedMem *)MapViewOfFile(g_shm_handle, FILE_MAP_ALL_ACCESS,
                                             0, 0, sizeof(BridgeSharedMem));
    if (!g_shm)
    {
        CloseHandle(g_shm_handle);
        g_shm_handle = NULL;
        return FALSE;
    }
    ZeroMemory(g_shm, sizeof(BridgeSharedMem));
    g_shm->magic = BRIDGE_MAGIC;
    g_shm->version = BRIDGE_VERSION;

    g_pipe = CreateNamedPipeA(
        pipe_name,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        sizeof(BridgeRpcResp) * 2,
        sizeof(BridgeRpcReq) * 2,
        5000,
        NULL);
    if (g_pipe == INVALID_HANDLE_VALUE)
    {
        plugin_bridge_shutdown();
        return FALSE;
    }

    g_io_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_io_event)
    {
        plugin_bridge_shutdown();
        return FALSE;
    }

    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = g_io_event;
    ConnectNamedPipe(g_pipe, &ov);

    snprintf(cmdline, sizeof(cmdline), "\"%s\" \"%s\" \"%s\" %lu %llu",
             bridge_exe, pipe_name, shm_name,
             (unsigned long)pid,
             (unsigned long long)(uintptr_t)main_hwnd);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    // inherit console or redirected stdio handles into 32-bit bridge process
    {
        BOOL inherit_std = FALSE;
        DWORD create_flags = 0;
        if (!GetConsoleWindow())
        {
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
            create_flags = CREATE_NO_WINDOW;
            if (hOut && hOut != INVALID_HANDLE_VALUE)
            {
                SetHandleInformation(hOut, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
                if (hErr && hErr != INVALID_HANDLE_VALUE)
                {
                    SetHandleInformation(hErr, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
                }
                else
                {
                    hErr = hOut;
                }
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdInput = INVALID_HANDLE_VALUE;
                si.hStdOutput = hOut;
                si.hStdError = hErr;
                inherit_std = TRUE;
            }
        }
        if (!CreateProcessA(bridge_exe, cmdline, NULL, NULL, inherit_std,
                            create_flags, NULL, NULL, &si, &pi))
        {
            dbg_error("Failed to launch 32-bit plugin bridge '%s' (err %lu)",
                      bridge_exe, (unsigned long)GetLastError());
            plugin_bridge_shutdown();
            return FALSE;
        }
    }
    dbg_plugin("Spawned 32-bit plugin bridge (PID=%lu, host_hwnd=0x%p, shm=%s)",
               (unsigned long)pi.dwProcessId, (void *)main_hwnd, shm_name);
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;

    if (GetLastError() == ERROR_IO_PENDING || WaitForSingleObject(g_io_event, 0) != WAIT_OBJECT_0)
    {
        if (!wait_overlapped_msg_safe(g_pipe, &ov, &dummy, 10000))
        {
            plugin_bridge_shutdown();
            return FALSE;
        }
    }

    g_active = TRUE;
    {
        BridgeRpcReq req;
        BridgeRpcResp resp;
        ZeroMemory(&req, sizeof(req));
        req.cmd = BRIDGE_CMD_PING;
        if (!bridge_rpc(&req, &resp, 5000))
        {
            plugin_bridge_shutdown();
            return FALSE;
        }
    }
    return TRUE;
}

BOOL plugin_bridge_is_active(void)
{
    return g_active;
}

BOOL plugin_bridge_load_dir(const char *plugin_dir, HWND main_hwnd, EDITP *ep)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!ensure_bridge_started(main_hwnd))
        return FALSE;
    if (ep && editp_is_editing(ep) && editp_get_edit_path(ep)[0])
    {
        plugin_bridge_open_video(editp_get_edit_path(ep), ep);
    }
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_LOAD_DIR;
    lstrcpynA(req.str, plugin_dir ? plugin_dir : "", sizeof(req.str));
    if (!bridge_rpc(&req, &resp, 15000))
        return FALSE;
    if (ep)
        plugin_bridge_sync_to_bridge(ep);
    return resp.val0 > 0;
}

BOOL plugin_bridge_load_file(const char *auf_path, HWND main_hwnd, EDITP *ep)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!ensure_bridge_started(main_hwnd))
        return FALSE;
    if (ep && editp_is_editing(ep) && editp_get_edit_path(ep)[0])
    {
        plugin_bridge_open_video(editp_get_edit_path(ep), ep);
    }
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_LOAD_FILE;
    lstrcpynA(req.str, auf_path ? auf_path : "", sizeof(req.str));
    if (!bridge_rpc(&req, &resp, 15000))
        return FALSE;
    if (ep)
        plugin_bridge_sync_to_bridge(ep);
    return resp.val0 > 0;
}

void plugin_bridge_shutdown(void)
{
    if (g_active && g_pipe != INVALID_HANDLE_VALUE)
    {
        BridgeRpcReq req;
        BridgeRpcResp resp;
        ZeroMemory(&req, sizeof(req));
        req.cmd = BRIDGE_CMD_SHUTDOWN;
        bridge_rpc(&req, &resp, 3000);
    }
    g_active = FALSE;
    if (g_proc)
    {
        if (WaitForSingleObject(g_proc, 2000) != WAIT_OBJECT_0)
        {
            TerminateProcess(g_proc, 0);
        }
        CloseHandle(g_proc);
        g_proc = NULL;
    }
    if (g_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    if (g_io_event)
    {
        CloseHandle(g_io_event);
        g_io_event = NULL;
    }
    if (g_shm)
    {
        UnmapViewOfFile(g_shm);
        g_shm = NULL;
    }
    if (g_shm_handle)
    {
        CloseHandle(g_shm_handle);
        g_shm_handle = NULL;
    }
}

int plugin_bridge_get_filter_count(void)
{
    return (g_active && g_shm) ? g_shm->filter_count : 0;
}

const char *plugin_bridge_get_filter_name(int idx)
{
    if (!g_active || !g_shm || idx < 0 || idx >= g_shm->filter_count)
        return NULL;
    return g_shm->filters[idx].name;
}

int plugin_bridge_get_filter_flags(int idx)
{
    if (!g_active || !g_shm || idx < 0 || idx >= g_shm->filter_count)
        return 0;
    return g_shm->filters[idx].flag;
}

HWND plugin_bridge_get_filter_hwnd(int idx)
{
    if (!g_active || !g_shm || idx < 0 || idx >= g_shm->filter_count)
        return NULL;
    return (HWND)(uintptr_t)g_shm->filters[idx].hwnd32;
}

int plugin_bridge_get_menu_item_count(void)
{
    return (g_active && g_shm) ? g_shm->menu_item_count : 0;
}

BOOL plugin_bridge_get_menu_item(int idx, BridgeMenuItemInfo *out_info)
{
    if (!g_active || !g_shm || !out_info || idx < 0 || idx >= g_shm->menu_item_count)
        return FALSE;
    *out_info = g_shm->menu_items[idx];
    return TRUE;
}

BOOL plugin_bridge_show_window(int idx, int mode)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_SHOW_WINDOW;
    req.arg0 = idx;
    req.arg1 = mode;
    return bridge_rpc(&req, &resp, 5000);
}

BOOL plugin_bridge_exec_menu_item(int menu_idx)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_EXEC_MENU_ITEM;
    req.arg0 = menu_idx;
    // longer timeout because a menu item like "新規プロジェクトの作成" may run a dialog
    return bridge_rpc(&req, &resp, 120000);
}

BOOL plugin_bridge_send_filter_command(int filter_idx, int filter_cmd_id)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_FILTER_COMMAND;
    req.arg0 = filter_idx;
    req.arg1 = filter_cmd_id;
    return bridge_rpc(&req, &resp, 30000);
}

BOOL plugin_bridge_import_exo(int filter_idx, const char *exo_path, EDITP *ep)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!exo_path || !exo_path[0])
        return FALSE;
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_IMPORT_EXO;
    req.arg0 = filter_idx;
    lstrcpynA(req.str, exo_path, sizeof(req.str));
    if (!bridge_rpc(&req, &resp, 30000))
        return FALSE;
    if (ep)
        plugin_bridge_sync_from_bridge(ep);
    return TRUE;
}

BOOL plugin_bridge_forward_mouse(UINT msg, WPARAM mk_flags, int img_x, int img_y)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_MAIN_MOUSE;
    req.arg0 = (int32_t)msg;
    req.arg1 = (int32_t)mk_flags;
    req.arg2 = img_x;
    req.arg3 = img_y;
    return bridge_rpc(&req, &resp, 2000) && resp.val0 != 0;
}

BOOL plugin_bridge_forward_key(UINT vk, BOOL is_down, LPARAM key_lparam)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_MAIN_KEY;
    req.arg0 = (int32_t)vk;
    req.arg1 = is_down ? 1 : 0;
    req.arg2 = (int32_t)key_lparam;
    return bridge_rpc(&req, &resp, 5000) && resp.val0 != 0;
}

BOOL plugin_bridge_open_video(const char *filepath, EDITP *ep)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!g_active || !filepath)
        return FALSE;
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_OPEN_VIDEO;
    lstrcpynA(req.str, filepath, sizeof(req.str));
    if (!bridge_rpc(&req, &resp, 15000))
        return FALSE;
    if (ep)
        plugin_bridge_sync_from_bridge(ep);
    return TRUE;
}

BOOL plugin_bridge_open_audio(const char *filepath, EDITP *ep)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!g_active || !filepath)
        return FALSE;
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_OPEN_AUDIO;
    lstrcpynA(req.str, filepath, sizeof(req.str));
    if (!bridge_rpc(&req, &resp, 15000))
        return FALSE;
    if (ep)
        plugin_bridge_sync_from_bridge(ep);
    return TRUE;
}

BOOL plugin_bridge_close_video(EDITP *ep)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!g_active)
        return FALSE;
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_CLOSE_VIDEO;
    if (!bridge_rpc(&req, &resp, 5000))
        return FALSE;
    if (ep)
        plugin_bridge_sync_from_bridge(ep);
    return TRUE;
}

BOOL plugin_bridge_load_aup(const char *aup_path, EDITP *ep)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!g_active || !aup_path || !aup_path[0])
        return FALSE;
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_LOAD_AUP;
    lstrcpynA(req.str, aup_path, sizeof(req.str));
    if (!bridge_rpc(&req, &resp, 30000))
        return FALSE;
    if (ep)
        plugin_bridge_sync_from_bridge(ep);
    return TRUE;
}

BOOL plugin_bridge_save_aup(const char *aup_path, EDITP *ep)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!g_active || !aup_path || !aup_path[0])
        return FALSE;
    if (ep)
    {
        editp_set_project_path(ep, aup_path);
        plugin_bridge_sync_to_bridge(ep);
    }
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_SAVE_AUP;
    lstrcpynA(req.str, aup_path, sizeof(req.str));
    if (!bridge_rpc(&req, &resp, 30000))
        return FALSE;
    if (ep)
        plugin_bridge_sync_from_bridge(ep);
    return TRUE;
}

BOOL plugin_bridge_sync_to_bridge(EDITP *ep)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    int s = 0, e = 0, dw = 0, dh = 0, fn;
    if (!g_active || !g_shm || !ep)
        return FALSE;
    fn = editp_get_frame_n(ep);
    if (fn > 0)
    {
        int lim = (fn < REAVIUTL_MAX_FRAME) ? fn : REAVIUTL_MAX_FRAME;
        const BYTE *tbl = editp_get_frame_status_table(ep, 0);
        if (tbl)
        {
            memcpy(g_shm->frame_status, tbl, (size_t)lim);
        }
    }
    lstrcpynA(g_shm->output_path, editp_get_output_path(ep), sizeof(g_shm->output_path));
    editp_get_select_frame(ep, &s, &e);
    editp_get_display_size(ep, &dw, &dh);
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_SYNC_STATE;
    req.arg0 = editp_get_frame(ep);
    req.arg1 = fn;
    req.arg2 = s;
    req.arg3 = e;
    req.arg4 = dw;
    req.arg5 = dh;
    return bridge_rpc(&req, &resp, 3000);
}

BOOL plugin_bridge_sync_from_bridge(EDITP *ep)
{
    if (!g_active || !g_shm || !ep)
        return FALSE;
    g_last_seq = g_shm->state_seq;

    if (g_shm->is_editing && g_shm->edit_path[0])
    {
        // if exedit opened a new project (e.g. "1280x720_30fps_44100Hz.exedit") or loaded an .aup inside the 32-bit bridge, mirror it in the 64-bit EDITP
        if (!editp_is_editing(ep) ||
            lstrcmpiA(editp_get_edit_path(ep), g_shm->edit_path) != 0)
        {
            editp_open_video(ep, g_shm->edit_path);
        }
        if (g_shm->w > 0 && g_shm->h > 0)
        {
            editp_set_dimensions(ep, g_shm->w, g_shm->h);
        }
        if (g_shm->fps_rate > 0 && g_shm->fps_scale > 0)
        {
            editp_set_fps(ep, g_shm->fps_rate, g_shm->fps_scale);
        }
        if (g_shm->audio_rate > 0 && g_shm->audio_ch > 0)
        {
            editp_set_audio_format(ep, g_shm->audio_rate, g_shm->audio_ch,
                                   g_shm->has_audio != 0);
        }
        if (g_shm->frame_n > 0 && g_shm->frame_n != editp_get_frame_n(ep))
        {
            editp_set_frame_n(ep, g_shm->frame_n);
        }
        if (g_shm->frame_n > 0)
        {
            int lim = (g_shm->frame_n < REAVIUTL_MAX_FRAME) ? g_shm->frame_n : REAVIUTL_MAX_FRAME;
            BYTE *tbl = editp_get_frame_status_table(ep, 0);
            if (tbl)
            {
                memcpy(tbl, g_shm->frame_status, (size_t)lim);
            }
        }
        editp_set_select_frame(ep, g_shm->select_s, g_shm->select_e);
        editp_set_frame(ep, g_shm->frame);
        editp_set_project_path(ep, g_shm->project_path);
        editp_set_output_path(ep, g_shm->output_path);
    }
    else if (!g_shm->is_editing && editp_is_editing(ep))
    {
        editp_close_video(ep);
    }
    return TRUE;
}

BOOL plugin_bridge_render_frame(EDITP *ep, int frame_num, BOOL push_host_frame)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    int w = 0, h = 0, max_w = 0, max_h = 0, y;
    PIXEL_YC *dst;

    if (!g_active || !g_shm || !ep)
        return FALSE;

    // ensure bridge has the same open project if host opened it before bridge loaded
    if (editp_is_editing(ep) && !g_shm->is_editing && editp_get_edit_path(ep)[0])
    {
        plugin_bridge_open_video(editp_get_edit_path(ep), ep);
    }

    editp_get_dimensions(ep, &w, &h, &max_w, &max_h);
    if (push_host_frame && w > 0 && h > 0)
    {
        const PIXEL_YC *src = editp_get_ycp_edit(ep);
        int copy_w = (w < REAVIUTL_VRAM_W) ? w : REAVIUTL_VRAM_W;
        int copy_h = (h < REAVIUTL_VRAM_H) ? h : REAVIUTL_VRAM_H;
        for (y = 0; y < copy_h; y++)
        {
            memcpy(&g_shm->ycp_frame[y * REAVIUTL_VRAM_W],
                   &src[y * max_w],
                   (size_t)copy_w * sizeof(PIXEL_YC));
        }
    }

    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_RENDER_FRAME;
    req.arg0 = frame_num;
    req.arg1 = push_host_frame ? 1 : 0;
    if (!bridge_rpc(&req, &resp, 15000))
        return FALSE;

    plugin_bridge_sync_from_bridge(ep);
    editp_get_dimensions(ep, &w, &h, &max_w, &max_h);
    dst = editp_get_ycp_edit(ep);
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
    return TRUE;
}

int plugin_bridge_render_audio(EDITP *ep, int frame_num, short *buf, int samples, int ch)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    int copy_smp, copy_ch;

    if (!g_active || !g_shm || !ep || !buf || samples <= 0)
        return samples;

    if (editp_is_editing(ep) && !g_shm->is_editing && editp_get_edit_path(ep)[0])
    {
        plugin_bridge_open_video(editp_get_edit_path(ep), ep);
    }

    copy_ch = (ch >= 2) ? 2 : 1;
    copy_smp = (samples <= BRIDGE_MAX_AUDIO_SAMPLES) ? samples : BRIDGE_MAX_AUDIO_SAMPLES;

    memcpy(g_shm->pcm_audio, buf, (size_t)copy_smp * (size_t)copy_ch * sizeof(short));

    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_RENDER_AUDIO;
    req.arg0 = frame_num;
    req.arg1 = copy_smp;
    req.arg2 = copy_ch;
    if (!bridge_rpc(&req, &resp, 10000))
    {
        return samples;
    }

    if (resp.val0 > 0 && resp.val0 <= copy_smp)
    {
        copy_smp = resp.val0;
    }
    memcpy(buf, g_shm->pcm_audio, (size_t)copy_smp * (size_t)copy_ch * sizeof(short));
    return copy_smp;
}

BOOL plugin_bridge_debug_cmd(const char *cmd_str)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!g_active || !cmd_str || !cmd_str[0])
        return FALSE;
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_DEBUG_CMD;
    lstrcpynA(req.str, cmd_str, sizeof(req.str));
    return bridge_rpc(&req, &resp, 5000);
}

BOOL plugin_bridge_notify_save_start(EDITP *ep, int s, int e, int out_flags)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!g_active)
        return FALSE;
    if (ep)
        plugin_bridge_sync_to_bridge(ep);
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_SAVE_START;
    req.arg0 = s;
    req.arg1 = e;
    req.arg2 = out_flags;
    return bridge_rpc(&req, &resp, 15000);
}

BOOL plugin_bridge_notify_save_end(EDITP *ep, int out_flags)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!g_active)
        return FALSE;
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_SAVE_END;
    req.arg0 = out_flags;
    if (!bridge_rpc(&req, &resp, 15000))
        return FALSE;
    if (ep)
        plugin_bridge_sync_from_bridge(ep);
    return TRUE;
}

BOOL plugin_bridge_scan_auo(const char *plugin_dir, HWND main_hwnd, EDITP *ep)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!ensure_bridge_started(main_hwnd))
        return FALSE;
    if (ep && editp_is_editing(ep) && !g_shm->is_editing && editp_get_edit_path(ep)[0])
    {
        plugin_bridge_open_video(editp_get_edit_path(ep), ep);
    }
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_SCAN_AUO;
    lstrcpynA(req.str, plugin_dir ? plugin_dir : "", sizeof(req.str));
    if (!bridge_rpc(&req, &resp, 15000))
        return FALSE;
    return resp.val0 > 0;
}

BOOL plugin_bridge_load_auo(const char *auo_path, HWND main_hwnd, EDITP *ep)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!auo_path || !auo_path[0])
        return FALSE;
    if (!ensure_bridge_started(main_hwnd))
        return FALSE;
    if (ep && editp_is_editing(ep) && !g_shm->is_editing && editp_get_edit_path(ep)[0])
    {
        plugin_bridge_open_video(editp_get_edit_path(ep), ep);
    }
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_LOAD_AUO;
    lstrcpynA(req.str, auo_path, sizeof(req.str));
    if (!bridge_rpc(&req, &resp, 15000))
        return FALSE;
    return resp.val0 > 0;
}

int plugin_bridge_get_auo_count(void)
{
    return (g_active && g_shm) ? g_shm->auo_count : 0;
}

BOOL plugin_bridge_get_auo_info(int idx, BridgeAuoInfo *out_info)
{
    if (!g_active || !g_shm || !out_info || idx < 0 || idx >= g_shm->auo_count)
        return FALSE;
    *out_info = g_shm->auos[idx];
    return TRUE;
}

BOOL plugin_bridge_auo_output(int auo_idx, const char *save_path, int out_flags,
                              EDITP *ep, HWND owner,
                              BridgeEncodeProgressFn on_progress, void *user)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    BOOL ok;

    if (!g_active || !g_shm || !save_path || !save_path[0])
        return FALSE;
    if (ep)
    {
        if (editp_is_editing(ep) && !g_shm->is_editing && editp_get_edit_path(ep)[0])
        {
            plugin_bridge_open_video(editp_get_edit_path(ep), ep);
        }
        plugin_bridge_sync_to_bridge(ep);
    }
    InterlockedExchange(&g_shm->encode_abort, 0);
    InterlockedExchange(&g_shm->encode_now, 0);
    InterlockedExchange(&g_shm->encode_total, 0);

    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_AUO_OUTPUT;
    req.arg0 = auo_idx;
    req.arg1 = out_flags;
    lstrcpynA(req.str, save_path, sizeof(req.str));

    ok = bridge_rpc_interactive(&req, &resp, owner, on_progress, user);
    if (ep)
        plugin_bridge_sync_from_bridge(ep);
    return ok;
}

BOOL plugin_bridge_auo_config(int auo_idx, HWND owner)
{
    BridgeRpcReq req;
    BridgeRpcResp resp;
    if (!g_active)
        return FALSE;
    ZeroMemory(&req, sizeof(req));
    req.cmd = BRIDGE_CMD_AUO_CONFIG;
    req.arg0 = auo_idx;
    return bridge_rpc_interactive(&req, &resp, owner, NULL, NULL);
}

void plugin_bridge_request_encode_abort(void)
{
    if (g_active && g_shm)
    {
        InterlockedExchange(&g_shm->encode_abort, 1);
    }
}

#else

BOOL plugin_bridge_is_active(void)
{
    return FALSE;
}
BOOL plugin_bridge_load_dir(const char *d, HWND h, EDITP *e)
{
    (void)d;
    (void)h;
    (void)e;
    return FALSE;
}
BOOL plugin_bridge_load_file(const char *f, HWND h, EDITP *e)
{
    (void)f;
    (void)h;
    (void)e;
    return FALSE;
}
void plugin_bridge_shutdown(void)
{
}
int plugin_bridge_get_filter_count(void)
{
    return 0;
}
const char *plugin_bridge_get_filter_name(int i)
{
    (void)i;
    return NULL;
}
int plugin_bridge_get_filter_flags(int i)
{
    (void)i;
    return 0;
}
HWND plugin_bridge_get_filter_hwnd(int i)
{
    (void)i;
    return NULL;
}
int plugin_bridge_get_menu_item_count(void)
{
    return 0;
}
BOOL plugin_bridge_get_menu_item(int i, BridgeMenuItemInfo *o)
{
    (void)i;
    (void)o;
    return FALSE;
}
BOOL plugin_bridge_show_window(int i, int m)
{
    (void)i;
    (void)m;
    return FALSE;
}
BOOL plugin_bridge_exec_menu_item(int i)
{
    (void)i;
    return FALSE;
}
BOOL plugin_bridge_send_filter_command(int i, int c)
{
    (void)i;
    (void)c;
    return FALSE;
}
BOOL plugin_bridge_import_exo(int i, const char *p, EDITP *e)
{
    (void)i;
    (void)p;
    (void)e;
    return FALSE;
}
BOOL plugin_bridge_forward_mouse(UINT m, WPARAM w, int x, int y)
{
    (void)m;
    (void)w;
    (void)x;
    (void)y;
    return FALSE;
}
BOOL plugin_bridge_forward_key(UINT v, BOOL d, LPARAM l)
{
    (void)v;
    (void)d;
    (void)l;
    return FALSE;
}
BOOL plugin_bridge_open_video(const char *f, EDITP *e)
{
    (void)f;
    (void)e;
    return FALSE;
}
BOOL plugin_bridge_open_audio(const char *f, EDITP *e)
{
    (void)f;
    (void)e;
    return FALSE;
}
BOOL plugin_bridge_close_video(EDITP *e)
{
    (void)e;
    return FALSE;
}
BOOL plugin_bridge_load_aup(const char *p, EDITP *e)
{
    (void)p;
    (void)e;
    return FALSE;
}
BOOL plugin_bridge_save_aup(const char *p, EDITP *e)
{
    (void)p;
    (void)e;
    return FALSE;
}
BOOL plugin_bridge_sync_to_bridge(EDITP *e)
{
    (void)e;
    return FALSE;
}
BOOL plugin_bridge_sync_from_bridge(EDITP *e)
{
    (void)e;
    return FALSE;
}
BOOL plugin_bridge_render_frame(EDITP *e, int n, BOOL p)
{
    (void)e;
    (void)n;
    (void)p;
    return FALSE;
}
int plugin_bridge_render_audio(EDITP *e, int n, short *b, int s, int c)
{
    (void)e;
    (void)n;
    (void)b;
    (void)c;
    return s;
}
BOOL plugin_bridge_debug_cmd(const char *c)
{
    (void)c;
    return FALSE;
}
BOOL plugin_bridge_notify_save_start(EDITP *e, int s, int end_f, int fl)
{
    (void)e;
    (void)s;
    (void)end_f;
    (void)fl;
    return FALSE;
}
BOOL plugin_bridge_notify_save_end(EDITP *e, int fl)
{
    (void)e;
    (void)fl;
    return FALSE;
}
BOOL plugin_bridge_scan_auo(const char *d, HWND h, EDITP *e)
{
    (void)d;
    (void)h;
    (void)e;
    return FALSE;
}
BOOL plugin_bridge_load_auo(const char *p, HWND h, EDITP *e)
{
    (void)p;
    (void)h;
    (void)e;
    return FALSE;
}
int plugin_bridge_get_auo_count(void)
{
    return 0;
}
BOOL plugin_bridge_get_auo_info(int i, BridgeAuoInfo *o)
{
    (void)i;
    (void)o;
    return FALSE;
}
BOOL plugin_bridge_auo_output(int i, const char *p, int fl, EDITP *e, HWND o, BridgeEncodeProgressFn cb, void *u)
{
    (void)i;
    (void)p;
    (void)fl;
    (void)e;
    (void)o;
    (void)cb;
    (void)u;
    return FALSE;
}
BOOL plugin_bridge_auo_config(int i, HWND o)
{
    (void)i;
    (void)o;
    return FALSE;
}
void plugin_bridge_request_encode_abort(void)
{
}

#endif
