#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "preview.h"
#include "builtin_filters.h"
#include "darken_theme.h"

#define AVIUTL_BUTTON_CLASS "AviUtlButton"
#define BTN_EXTRA_HICON (0 * (int)sizeof(LONG_PTR))
#define BTN_EXTRA_CMD (1 * (int)sizeof(LONG_PTR))
#define BTN_EXTRA_LPARAM (2 * (int)sizeof(LONG_PTR))
#define BTN_EXTRA_FLAGS (3 * (int)sizeof(LONG_PTR))
#define BTN_REPEAT_TIMER_ID 1

#define WAVE_MAX_W 2048
#define WAVE_H 63
#define WAVE_STRIDE (WAVE_MAX_W * 3) // 6144 = 0x1800 bytes, DWORD-aligned

static HINSTANCE g_hinst = NULL;
static BOOL g_btn_registered = FALSE;
static WNDPROC g_orig_tb_proc = NULL;

static HWND g_hwnd_main = NULL;
static HWND g_hwnd_trackbar = NULL;
static HWND g_hwnd_btn_back = NULL;
static HWND g_hwnd_btn_fwd = NULL;
static HWND g_hwnd_btn_out = NULL;
static HWND g_hwnd_btn_in = NULL;
static HWND g_hwnd_btn_play = NULL;

static HICON g_icon_back = NULL;
static HICON g_icon_fwd = NULL;
static HICON g_icon_in = NULL;
static HICON g_icon_out = NULL;
static HICON g_icon_del = NULL;
static HICON g_icon_del2 = NULL;
static HICON g_icon_mark = NULL;
static HICON g_icon_inter[6] = {NULL};
static HICON g_icon_play = NULL;
static HICON g_icon_stop = NULL;

static AviUtlSystemConfig g_sys_cfg = {
    5, 30, 899, 8991, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0};

static uint32_t g_preview_flags = PREVIEW_FLAG_AUDIO | PREVIEW_FLAG_SIZEAUTO;
static int g_zoom = PREVIEW_ZOOM_100;
static BOOL g_peek_unfiltered = FALSE;

// current frame & audio waveform buffers
static EDITP *g_cur_ep = NULL;
static BITMAPINFO g_bmi;
static void *g_frame_bits = NULL;
static int g_frame_w = 0;
static int g_frame_h = 0;
static short *g_audio_pcm = NULL;
static int g_audio_cap = 0;
static int g_audio_shorts = 0;

// computed blit/crop geometry (editp + 0x3a0..+0x3b7 in 0x00461ea0)
static int g_dst_x = 0;
static int g_dst_y = 0;
static int g_dst_w = 0;
static int g_dst_h = 0;
static int g_src_vis_w = 0;
static int g_src_vis_h = 0;

// preallocated 2048 x 63 bottom-up BGR24 DIB for audio waveform (0x00461ea0)
static unsigned char g_wave_dib[WAVE_STRIDE * WAVE_H];
static BITMAPINFO g_wave_bmi;

static void get_ini_path(char *out_path, size_t out_sz)
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

static void write_ini_int(const char *section, const char *key, int val, const char *ini_path)
{
    char buf[32];
    wsprintfA(buf, "%d", val);
    WritePrivateProfileStringA(section, key, buf, ini_path);
}

// "AviUtlButton" custom 24x24 icon button (0x0040e7d0)

static VOID CALLBACK AviUtlButtonRepeatTimerProc(HWND hwnd, UINT uMsg,
                                                 UINT_PTR idEvent, DWORD dwTime)
{
    DWORD speed = 0;
    UINT interval;
    HWND parent;
    WPARAM cmd_id;
    LPARAM cmd_lp;

    (void)uMsg;
    (void)idEvent;
    (void)dwTime;
    SystemParametersInfoA(SPI_GETKEYBOARDSPEED, 0, &speed, 0);
    interval = (UINT)(1000u / (speed + 2u));
    if (interval < 10)
        interval = 10;
    SetTimer(hwnd, BTN_REPEAT_TIMER_ID, interval, AviUtlButtonRepeatTimerProc);

    parent = GetParent(hwnd);
    if (parent)
    {
        cmd_id = (WPARAM)GetWindowLongPtrA(hwnd, BTN_EXTRA_CMD);
        cmd_lp = (LPARAM)GetWindowLongPtrA(hwnd, BTN_EXTRA_LPARAM);
        PostMessageA(parent, WM_COMMAND, cmd_id, cmd_lp);
    }
}

static LRESULT CALLBACK AviUtlButtonWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        HDC hdc = BeginPaint(hwnd, &ps);
        HICON hIcon = (HICON)GetWindowLongPtrA(hwnd, BTN_EXTRA_HICON);
        GetClientRect(hwnd, &rc);
        if (hIcon)
        {
            DrawIconEx(hdc, 0, 0, hIcon, rc.right, rc.bottom, 0, NULL, DI_NORMAL);
        }
        DrawEdge(hdc, &rc, EDGE_RAISED, BF_RECT);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN:
    {
        RECT rc;
        HDC hdc = GetDC(hwnd);
        HWND parent = GetParent(hwnd);
        LONG_PTR flags = GetWindowLongPtrA(hwnd, BTN_EXTRA_FLAGS);

        GetClientRect(hwnd, &rc);
        DrawEdge(hdc, &rc, EDGE_SUNKEN, BF_RECT);
        ReleaseDC(hwnd, hdc);

        if (parent)
        {
            WPARAM cmd_id = (WPARAM)GetWindowLongPtrA(hwnd, BTN_EXTRA_CMD);
            LPARAM cmd_lp = (LPARAM)GetWindowLongPtrA(hwnd, BTN_EXTRA_LPARAM);
            PostMessageA(parent, WM_COMMAND, cmd_id, cmd_lp);
        }
        SetFocus(hwnd);
        SetCapture(hwnd);

        // auto-repeat while held when bit 0 of flags is clear (< and > buttons)
        if ((flags & 1) == 0)
        {
            DWORD delay = 1;
            UINT initial_ms;
            SystemParametersInfoA(SPI_GETKEYBOARDDELAY, 0, &delay, 0);
            initial_ms = (UINT)(((delay + 1u) * 1000u) >> 2);
            SetTimer(hwnd, BTN_REPEAT_TIMER_ID, initial_ms, AviUtlButtonRepeatTimerProc);
        }
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_KILLFOCUS:
    {
        RECT rc;
        HDC hdc = GetDC(hwnd);
        HWND parent = GetParent(hwnd);

        GetClientRect(hwnd, &rc);
        DrawEdge(hdc, &rc, EDGE_RAISED, BF_RECT);
        ReleaseDC(hwnd, hdc);

        KillTimer(hwnd, BTN_REPEAT_TIMER_ID);
        if (GetCapture() == hwnd)
        {
            ReleaseCapture();
        }
        if (msg == WM_LBUTTONUP && GetFocus() == hwnd && parent)
        {
            SetFocus(parent);
        }
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND create_aviutl_button(HWND parent, HINSTANCE hInst, int child_id,
                                 HICON hIcon, int cmd_id, LONG_PTR flags)
{
    HWND btn = CreateWindowExA(
        0, AVIUTL_BUTTON_CLASS, NULL,
        WS_CHILD | WS_VISIBLE,
        0, 0, 24, 24,
        parent, (HMENU)(UINT_PTR)child_id, hInst, NULL);
    if (btn)
    {
        SetWindowLongPtrA(btn, BTN_EXTRA_HICON, (LONG_PTR)hIcon);
        SetWindowLongPtrA(btn, BTN_EXTRA_CMD, (LONG_PTR)cmd_id);
        SetWindowLongPtrA(btn, BTN_EXTRA_LPARAM, 0);
        SetWindowLongPtrA(btn, BTN_EXTRA_FLAGS, flags);
    }
    return btn;
}

// 0x0045d1d0, subclassed trackbar wndproc (also used by exedit via editp+0x388)

static LRESULT CALLBACK AviUtlTrackbarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_sys_cfg.trackbarclick != 0 && msg == WM_LBUTTONDOWN)
    {
        int x = (short)LOWORD(lp);
        int y = (short)HIWORD(lp);
        RECT rc_chan, rc_thumb;

        ZeroMemory(&rc_chan, sizeof(rc_chan));
        ZeroMemory(&rc_thumb, sizeof(rc_thumb));
        SendMessageA(hwnd, TBM_GETCHANNELRECT, 0, (LPARAM)&rc_chan);
        SendMessageA(hwnd, TBM_GETTHUMBRECT, 0, (LPARAM)&rc_thumb);

        // ignore clicks outside the channel X span or thumb Y span (prevents border page-jumps)
        if (x < rc_chan.left || x > rc_chan.right ||
            y < rc_thumb.top || y > rc_thumb.bottom)
        {
            return 0;
        }

        // click outside current thumb: jump thumb directly under cursor, then enter live drag
        if (x <= rc_thumb.left || x >= rc_thumb.right)
        {
            int rmin = (int)SendMessageA(hwnd, TBM_GETRANGEMIN, 0, 0);
            int rmax = (int)SendMessageA(hwnd, TBM_GETRANGEMAX, 0, 0);
            if (rmax > rmin)
            {
                int half_tw = (rc_thumb.right - rc_thumb.left) / 2;
                int eff_l = rc_chan.left + half_tw + 1;
                int eff_r = rc_chan.right - ((rc_thumb.right - rc_thumb.left) - half_tw) - 1;
                int eff_w = eff_r - eff_l + 1;
                long long span = (long long)rmax - (long long)rmin;
                int pos;

                if (eff_w <= 0)
                {
                    pos = rmin;
                }
                else
                {
                    pos = (int)(((span / 4) + (long long)(x - eff_l) * (span + 1)) / eff_w + rmin);
                }
                if (pos < rmin)
                    pos = rmin;
                if (pos > rmax)
                    pos = rmax;

                SendMessageA(hwnd, TBM_SETPOS, TRUE, (LPARAM)pos);
                SendMessageA(hwnd, TBM_GETTHUMBRECT, 0, (LPARAM)&rc_thumb);
                lp = MAKELPARAM((rc_thumb.left + rc_thumb.right + 1) >> 1, y);
            }
        }
    }
    return CallWindowProcA(g_orig_tb_proc, hwnd, msg, wp, lp);
}

static HICON load_icon_24(HINSTANCE hInst, const char *res_name)
{
    HICON ic = (HICON)LoadImageA(hInst, res_name, IMAGE_ICON, 24, 24, 0);
    return darken_theme_recolor_icon(ic, res_name);
}

BOOL preview_register_classes(HINSTANCE hInst)
{
    WNDCLASSA wc;
    g_hinst = hInst;
    InitCommonControls();

    if (!g_btn_registered)
    {
        ZeroMemory(&wc, sizeof(wc));
        wc.style = 0x2003; // CS_VREDRAW | CS_HREDRAW | CS_BYTEALIGNWINDOW
        wc.lpfnWndProc = AviUtlButtonWndProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 4 * (int)sizeof(LONG_PTR);
        wc.hInstance = hInst;
        wc.hIcon = NULL;
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszMenuName = NULL;
        wc.lpszClassName = AVIUTL_BUTTON_CLASS;
        if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return FALSE;
        }
        g_btn_registered = TRUE;
    }

    g_icon_back = load_icon_24(hInst, "ICON_BACK");
    g_icon_fwd = load_icon_24(hInst, "ICON_FORWARD");
    g_icon_in = load_icon_24(hInst, "ICON_IN");
    g_icon_out = load_icon_24(hInst, "ICON_OUT");
    g_icon_del = load_icon_24(hInst, "ICON_DEL");
    g_icon_del2 = load_icon_24(hInst, "ICON_DEL2");
    g_icon_mark = load_icon_24(hInst, "ICON_MARK");
    g_icon_inter[0] = load_icon_24(hInst, "ICON_I_NORM");
    g_icon_inter[1] = load_icon_24(hInst, "ICON_I_REV");
    g_icon_inter[2] = load_icon_24(hInst, "ICON_I_ODD");
    g_icon_inter[3] = load_icon_24(hInst, "ICON_I_EVEN");
    g_icon_inter[4] = load_icon_24(hInst, "ICON_I_MIX");
    g_icon_inter[5] = load_icon_24(hInst, "ICON_I_AUTO");
    g_icon_play = load_icon_24(hInst, "ICON_PLAY");
    g_icon_stop = load_icon_24(hInst, "ICON_STOP");

    ZeroMemory(&g_wave_bmi, sizeof(g_wave_bmi));
    g_wave_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_wave_bmi.bmiHeader.biWidth = WAVE_MAX_W;
    g_wave_bmi.bmiHeader.biHeight = WAVE_H;
    g_wave_bmi.bmiHeader.biPlanes = 1;
    g_wave_bmi.bmiHeader.biBitCount = 24;
    g_wave_bmi.bmiHeader.biCompression = BI_RGB;
    return TRUE;
}

void preview_load_ini(int *out_x, int *out_y, int *out_w, int *out_h, BOOL *out_maximize)
{
    char ini[MAX_PATH];
    int x, y, w, h, z, mx;
    uint32_t fl = 0;

    get_ini_path(ini, sizeof(ini));

    // [system] section (0x00407b50)
    g_sys_cfg.moveA = GetPrivateProfileIntA("system", "moveA", 5, ini);
    g_sys_cfg.moveB = GetPrivateProfileIntA("system", "moveB", 30, ini);
    g_sys_cfg.moveC = GetPrivateProfileIntA("system", "moveC", 899, ini);
    g_sys_cfg.moveD = GetPrivateProfileIntA("system", "moveD", 8991, ini);
    g_sys_cfg.startframe = GetPrivateProfileIntA("system", "startframe", 1, ini);
    g_sys_cfg.shiftselect = GetPrivateProfileIntA("system", "shiftselect", 1, ini);
    g_sys_cfg.dragdropactive = GetPrivateProfileIntA("system", "dragdropactive", 1, ini);
    g_sys_cfg.movieplaymain = GetPrivateProfileIntA("system", "movieplaymain", 0, ini);
    g_sys_cfg.trackbarclick = GetPrivateProfileIntA("system", "trackbarclick", 1, ini);
    g_sys_cfg.windowsnap = GetPrivateProfileIntA("system", "windowsnap", 1, ini);
    g_sys_cfg.overwritecheck = GetPrivateProfileIntA("system", "overwritecheck", 0, ini);
    g_sys_cfg.dragdropdialog = GetPrivateProfileIntA("system", "dragdropdialog", 0, ini);
    g_sys_cfg.openprojectaup = GetPrivateProfileIntA("system", "openprojectaup", 1, ini);
    g_sys_cfg.closedialog = GetPrivateProfileIntA("system", "closedialog", 0, ini);
    g_sys_cfg.editresume = GetPrivateProfileIntA("system", "editresume", 0, ini);

    // [window] section (0x00460960)
    x = GetPrivateProfileIntA("window", "x", CW_USEDEFAULT, ini);
    y = GetPrivateProfileIntA("window", "y", CW_USEDEFAULT, ini);
    w = GetPrivateProfileIntA("window", "w", 640, ini);
    h = GetPrivateProfileIntA("window", "h", 480, ini);
    z = GetPrivateProfileIntA("window", "zoom", PREVIEW_ZOOM_100, ini);
    mx = GetPrivateProfileIntA("window", "maximize", 0, ini);

    if (w < 160)
        w = 640;
    if (h < 120)
        h = 480;
    if (z < PREVIEW_ZOOM_25 || z > PREVIEW_ZOOM_WINDOW)
        z = PREVIEW_ZOOM_100;
    g_zoom = z;

    if (GetPrivateProfileIntA("window", "audio", 1, ini))
        fl |= PREVIEW_FLAG_AUDIO;
    if (GetPrivateProfileIntA("window", "preview", 0, ini))
        fl |= PREVIEW_FLAG_PREVIEW;
    if (GetPrivateProfileIntA("window", "deleteframe", 0, ini))
        fl |= PREVIEW_FLAG_DELETEFRAME;
    if (GetPrivateProfileIntA("window", "avikeyframe", 0, ini))
        fl |= PREVIEW_FLAG_AVIKEYFRAME;
    if (GetPrivateProfileIntA("window", "recompressframe", 0, ini))
        fl |= PREVIEW_FLAG_RECOMPRESSFRAME;
    if (GetPrivateProfileIntA("window", "overlay", 0, ini))
        fl |= PREVIEW_FLAG_OVERLAY;
    if (GetPrivateProfileIntA("window", "alwaystop", 0, ini))
        fl |= PREVIEW_FLAG_ALWAYSTOP;
    if (GetPrivateProfileIntA("window", "time", 0, ini))
        fl |= PREVIEW_FLAG_TIME;
    if (GetPrivateProfileIntA("window", "nofilter", 0, ini))
        fl |= PREVIEW_FLAG_NOFILTER;
    if (GetPrivateProfileIntA("window", "datarate", 0, ini))
        fl |= PREVIEW_FLAG_DATARATE;
    if (GetPrivateProfileIntA("window", "sizeauto", 1, ini))
        fl |= PREVIEW_FLAG_SIZEAUTO;
    g_preview_flags = fl;

    // guard against off-screen saved coordinates
    if (x != (int)CW_USEDEFAULT && y != (int)CW_USEDEFAULT)
    {
        RECT test_rc;
        SetRect(&test_rc, x, y, x + w, y + h);
        if (MonitorFromRect(&test_rc, MONITOR_DEFAULTTONULL) == NULL)
        {
            x = CW_USEDEFAULT;
            y = CW_USEDEFAULT;
        }
    }

    if (out_x)
        *out_x = x;
    if (out_y)
        *out_y = y;
    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = h;
    if (out_maximize)
        *out_maximize = (mx != 0);
}

void preview_save_ini(HWND main_hwnd)
{
    char ini[MAX_PATH];
    get_ini_path(ini, sizeof(ini));

    // [window] persistence order & semantics from 0x004614a0
    if (main_hwnd && IsWindow(main_hwnd))
    {
        BOOL zoomed = IsZoomed(main_hwnd);
        BOOL iconic = IsIconic(main_hwnd);
        if (!zoomed && !iconic)
        {
            RECT rc;
            GetWindowRect(main_hwnd, &rc);
            write_ini_int("window", "x", rc.left, ini);
            write_ini_int("window", "y", rc.top, ini);
            write_ini_int("window", "w", rc.right - rc.left, ini);
            write_ini_int("window", "h", rc.bottom - rc.top, ini);
            write_ini_int("window", "zoom", g_zoom, ini);
        }
        write_ini_int("window", "maximize", zoomed ? 1 : 0, ini);
    }

    write_ini_int("window", "audio", (g_preview_flags & PREVIEW_FLAG_AUDIO) ? 1 : 0, ini);
    write_ini_int("window", "preview", (g_preview_flags & PREVIEW_FLAG_PREVIEW) ? 1 : 0, ini);
    write_ini_int("window", "deleteframe", (g_preview_flags & PREVIEW_FLAG_DELETEFRAME) ? 1 : 0, ini);
    write_ini_int("window", "avikeyframe", (g_preview_flags & PREVIEW_FLAG_AVIKEYFRAME) ? 1 : 0, ini);
    write_ini_int("window", "recompressframe", (g_preview_flags & PREVIEW_FLAG_RECOMPRESSFRAME) ? 1 : 0, ini);
    write_ini_int("window", "overlay", (g_preview_flags & PREVIEW_FLAG_OVERLAY) ? 1 : 0, ini);
    write_ini_int("window", "alwaystop", (g_preview_flags & PREVIEW_FLAG_ALWAYSTOP) ? 1 : 0, ini);
    write_ini_int("window", "time", (g_preview_flags & PREVIEW_FLAG_TIME) ? 1 : 0, ini);
    write_ini_int("window", "nofilter", (g_preview_flags & PREVIEW_FLAG_NOFILTER) ? 1 : 0, ini);
    write_ini_int("window", "datarate", (g_preview_flags & PREVIEW_FLAG_DATARATE) ? 1 : 0, ini);
    write_ini_int("window", "sizeauto", (g_preview_flags & PREVIEW_FLAG_SIZEAUTO) ? 1 : 0, ini);
}

BOOL preview_init_controls(HWND main_hwnd, HINSTANCE hInst)
{
    g_hwnd_main = main_hwnd;

    // style 0x50000021 = WS_CHILD | WS_VISIBLE | TBS_ENABLESELRANGE | TBS_AUTOTICKS (0x00460960)
    g_hwnd_trackbar = CreateWindowExA(
        0, TRACKBAR_CLASSA, "",
        0x50000021u,
        0, 0, 100, 32,
        main_hwnd, (HMENU)(UINT_PTR)IDC_AVIUTL_TRACKBAR, hInst, NULL);
    if (!g_hwnd_trackbar)
        return FALSE;

    g_orig_tb_proc = (WNDPROC)SetWindowLongPtrA(
        g_hwnd_trackbar, GWLP_WNDPROC, (LONG_PTR)AviUtlTrackbarProc);

    SendMessageA(g_hwnd_trackbar, TBM_SETRANGE, FALSE, MAKELONG(0, 0));
    SendMessageA(g_hwnd_trackbar, TBM_SETPAGESIZE, 0,
                 abs(g_sys_cfg.moveA) > 0 ? abs(g_sys_cfg.moveA) : 1);
    EnableWindow(g_hwnd_trackbar, FALSE);

    // right-aligned 24x24 AviUtlButton cluster (0x00460960)
    g_hwnd_btn_back = create_aviutl_button(main_hwnd, hInst, IDC_AVIUTL_BTN_BACK, g_icon_back, 9001, 0);
    g_hwnd_btn_fwd = create_aviutl_button(main_hwnd, hInst, IDC_AVIUTL_BTN_FORWARD, g_icon_fwd, 9002, 0);
    g_hwnd_btn_out = create_aviutl_button(main_hwnd, hInst, IDC_AVIUTL_BTN_OUT, g_icon_out, 9003, 1);
    g_hwnd_btn_in = create_aviutl_button(main_hwnd, hInst, IDC_AVIUTL_BTN_IN, g_icon_in, 9004, 1);

    if (g_sys_cfg.movieplaymain != 0)
    {
        g_hwnd_btn_play = create_aviutl_button(main_hwnd, hInst, IDC_AVIUTL_BTN_PLAY, g_icon_play, 9005, 1);
    }
    else
    {
        g_hwnd_btn_play = NULL;
    }

    return TRUE;
}

void preview_reload_sys_config(HWND main_hwnd)
{
    char ini[MAX_PATH];
    int page_sz;

    get_ini_path(ini, sizeof(ini));
    g_sys_cfg.moveA = GetPrivateProfileIntA("system", "moveA", 5, ini);
    g_sys_cfg.moveB = GetPrivateProfileIntA("system", "moveB", 30, ini);
    g_sys_cfg.moveC = GetPrivateProfileIntA("system", "moveC", 899, ini);
    g_sys_cfg.moveD = GetPrivateProfileIntA("system", "moveD", 8991, ini);
    g_sys_cfg.startframe = GetPrivateProfileIntA("system", "startframe", 1, ini);
    g_sys_cfg.shiftselect = GetPrivateProfileIntA("system", "shiftselect", 1, ini);
    g_sys_cfg.dragdropactive = GetPrivateProfileIntA("system", "dragdropactive", 1, ini);
    g_sys_cfg.movieplaymain = GetPrivateProfileIntA("system", "movieplaymain", 0, ini);
    g_sys_cfg.trackbarclick = GetPrivateProfileIntA("system", "trackbarclick", 1, ini);
    g_sys_cfg.windowsnap = GetPrivateProfileIntA("system", "windowsnap", 1, ini);
    g_sys_cfg.overwritecheck = GetPrivateProfileIntA("system", "overwritecheck", 0, ini);
    g_sys_cfg.dragdropdialog = GetPrivateProfileIntA("system", "dragdropdialog", 0, ini);
    g_sys_cfg.openprojectaup = GetPrivateProfileIntA("system", "openprojectaup", 1, ini);
    g_sys_cfg.closedialog = GetPrivateProfileIntA("system", "closedialog", 0, ini);
    g_sys_cfg.editresume = GetPrivateProfileIntA("system", "editresume", 0, ini);

    page_sz = abs(g_sys_cfg.moveA);
    if (page_sz < 1)
        page_sz = 1;
    if (g_hwnd_trackbar && IsWindow(g_hwnd_trackbar))
    {
        SendMessageA(g_hwnd_trackbar, TBM_SETPAGESIZE, 0, page_sz);
    }

    if (main_hwnd && IsWindow(main_hwnd))
    {
        if (g_sys_cfg.movieplaymain != 0 && !g_hwnd_btn_play)
        {
            g_hwnd_btn_play = create_aviutl_button(main_hwnd, g_hinst, IDC_AVIUTL_BTN_PLAY,
                                                   g_icon_play, 9005, 1);
        }
        else if (g_sys_cfg.movieplaymain == 0 && g_hwnd_btn_play)
        {
            DestroyWindow(g_hwnd_btn_play);
            g_hwnd_btn_play = NULL;
        }
        preview_on_size(main_hwnd, g_cur_ep);
    }
}

HWND preview_get_trackbar_hwnd(void)
{
    return g_hwnd_trackbar;
}
const AviUtlSystemConfig *preview_get_sys_config(void)
{
    return &g_sys_cfg;
}
uint32_t preview_get_flags(void)
{
    return g_preview_flags;
}
int preview_get_zoom(void)
{
    return g_zoom;
}

void preview_set_peek_unfiltered(BOOL peek)
{
    g_peek_unfiltered = peek;
}

BOOL preview_is_effectively_nofilter(EDITP *ep)
{
    if (g_peek_unfiltered)
        return TRUE;
    if ((g_preview_flags & PREVIEW_FLAG_NOFILTER) && (!ep || !editp_is_saving(ep)))
    {
        return TRUE;
    }
    return FALSE;
}

void preview_set_playing_state(BOOL playing)
{
    if (g_hwnd_btn_play && IsWindow(g_hwnd_btn_play))
    {
        HICON ico = playing ? (g_icon_stop ? g_icon_stop : g_icon_play) : g_icon_play;
        SetWindowLongPtrA(g_hwnd_btn_play, BTN_EXTRA_HICON, (LONG_PTR)ico);
        InvalidateRect(g_hwnd_btn_play, NULL, FALSE);
    }
}

static void sync_ep_state(HWND main_hwnd, EDITP *ep)
{
    RECT win_rc, cli_rc;
    if (!ep || !main_hwnd || !IsWindow(main_hwnd))
        return;
    GetWindowRect(main_hwnd, &win_rc);
    GetClientRect(main_hwnd, &cli_rc);
    editp_sync_preview_state(
        ep, g_preview_flags,
        win_rc.left, win_rc.top, cli_rc.right, cli_rc.bottom, g_zoom,
        g_hwnd_btn_back, g_hwnd_btn_fwd, g_hwnd_btn_in, g_hwnd_btn_out, g_hwnd_btn_play,
        g_dst_x, g_dst_y, g_dst_w, g_dst_h, g_src_vis_w, g_src_vis_h);
}

// 0x004604d0, child control layout

void preview_on_size(HWND main_hwnd, EDITP *ep)
{
    RECT rc;
    int cw, ch, btn_y;

    if (!main_hwnd || !IsWindow(main_hwnd))
        return;
    GetClientRect(main_hwnd, &rc);
    cw = rc.right;
    ch = rc.bottom;
    btn_y = ch - 28; // 4px top margin + 24px button + 4px bottom margin = 32px bar

    if (g_hwnd_btn_play)
    {
        int tb_w = cw - 130;
        if (tb_w < 0)
            tb_w = 0;
        if (g_hwnd_trackbar)
            MoveWindow(g_hwnd_trackbar, 0, ch - 32, tb_w, 32, TRUE);
        if (g_hwnd_btn_play)
            MoveWindow(g_hwnd_btn_play, cw - 124, btn_y, 24, 24, TRUE);
    }
    else
    {
        int tb_w = cw - 106;
        if (tb_w < 0)
            tb_w = 0;
        if (g_hwnd_trackbar)
            MoveWindow(g_hwnd_trackbar, 0, ch - 32, tb_w, 32, TRUE);
    }

    if (g_hwnd_btn_in)
        MoveWindow(g_hwnd_btn_in, cw - 100, btn_y, 24, 24, TRUE);
    if (g_hwnd_btn_out)
        MoveWindow(g_hwnd_btn_out, cw - 76, btn_y, 24, 24, TRUE);
    if (g_hwnd_btn_back)
        MoveWindow(g_hwnd_btn_back, cw - 52, btn_y, 24, 24, TRUE);
    if (g_hwnd_btn_fwd)
        MoveWindow(g_hwnd_btn_fwd, cw - 28, btn_y, 24, 24, TRUE);

    sync_ep_state(main_hwnd, ep ? ep : g_cur_ep);
}

void preview_sync_menu_checks(HWND main_hwnd)
{
    HMENU hMenu;
    if (!main_hwnd)
        return;
    hMenu = GetMenu(main_hwnd);
    if (!hMenu)
        return;

    CheckMenuRadioItem(hMenu, 9200, 9206, 9200 + g_zoom, MF_BYCOMMAND);
    CheckMenuItem(hMenu, 9104, MF_BYCOMMAND | ((g_preview_flags & PREVIEW_FLAG_AUDIO) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, 9105, MF_BYCOMMAND | ((g_preview_flags & PREVIEW_FLAG_PREVIEW) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, 9106, MF_BYCOMMAND | ((g_preview_flags & PREVIEW_FLAG_DELETEFRAME) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, 9109, MF_BYCOMMAND | ((g_preview_flags & PREVIEW_FLAG_AVIKEYFRAME) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, 9110, MF_BYCOMMAND | ((g_preview_flags & PREVIEW_FLAG_RECOMPRESSFRAME) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, 9111, MF_BYCOMMAND | ((g_preview_flags & PREVIEW_FLAG_OVERLAY) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, 9113, MF_BYCOMMAND | ((g_preview_flags & PREVIEW_FLAG_ALWAYSTOP) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, 9114, MF_BYCOMMAND | ((g_preview_flags & PREVIEW_FLAG_TIME) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, 9115, MF_BYCOMMAND | ((g_preview_flags & PREVIEW_FLAG_DATARATE) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, 9116, MF_BYCOMMAND | ((g_preview_flags & PREVIEW_FLAG_SIZEAUTO) ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hMenu, 5144, MF_BYCOMMAND | ((g_preview_flags & PREVIEW_FLAG_NOFILTER) ? MF_CHECKED : MF_UNCHECKED));
}

// 0x00462b90, auto window resize ("sizeauto")

void preview_auto_resize(HWND main_hwnd, EDITP *ep, BOOL force)
{
    int w = 0, h = 0, mw = 0, mh = 0;
    int target_cw, target_ch;
    RECT rc_cli, rc_win, rc_adj, rc_work;
    DWORD style, ex_style;
    int cur_extra_w, cur_extra_h, win_w, win_h;

    if (!main_hwnd || !IsWindow(main_hwnd) || !ep || !editp_is_editing(ep))
        return;
    if (!force && !(g_preview_flags & PREVIEW_FLAG_SIZEAUTO))
        return;
    if (g_zoom == PREVIEW_ZOOM_WINDOW)
        return;
    if (IsZoomed(main_hwnd) || IsIconic(main_hwnd))
        return;

    editp_get_dimensions(ep, &w, &h, &mw, &mh);
    if (w <= 0 || h <= 0)
        return;

    if (g_zoom < PREVIEW_ZOOM_100)
    {
        int div = 4 - g_zoom * 2; // 4 for 25%, 2 for 50%
        target_cw = w / div;
        target_ch = h / div;
    }
    else
    {
        int mul = g_zoom - 1; // 1, 2, 3, 4
        target_cw = w * mul;
        target_ch = h * mul;
    }

    target_ch += PREVIEW_BOTTOM_BAR_H;
    if (g_preview_flags & PREVIEW_FLAG_AUDIO)
    {
        target_ch += PREVIEW_AUDIO_WAVE_H;
    }

    if (target_cw < 200)
        target_cw = 200;

    // account for non-client borders and multi-row menu bar wrap
    GetClientRect(main_hwnd, &rc_cli);
    GetWindowRect(main_hwnd, &rc_win);
    style = (DWORD)GetWindowLongA(main_hwnd, GWL_STYLE);
    ex_style = (DWORD)GetWindowLongA(main_hwnd, GWL_EXSTYLE);
    SetRect(&rc_adj, 0, 0, target_cw, target_ch);
    AdjustWindowRectEx(&rc_adj, style, GetMenu(main_hwnd) != NULL, ex_style);

    win_w = rc_adj.right - rc_adj.left;
    win_h = rc_adj.bottom - rc_adj.top;

    // preserve any extra menu wrap height if current window already has wrapped menu
    cur_extra_w = (rc_win.right - rc_win.left) - rc_cli.right;
    cur_extra_h = (rc_win.bottom - rc_win.top) - rc_cli.bottom;
    if (cur_extra_w > 0 && win_w < target_cw + cur_extra_w)
        win_w = target_cw + cur_extra_w;
    if (cur_extra_h > 0 && win_h < target_ch + cur_extra_h)
        win_h = target_ch + cur_extra_h;

    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &rc_work, 0))
    {
        int max_ww = rc_work.right - rc_work.left;
        int max_wh = rc_work.bottom - rc_work.top;
        if (win_w > max_ww)
            win_w = max_ww;
        if (win_h > max_wh)
            win_h = max_wh;
    }

    SetWindowPos(main_hwnd, NULL, 0, 0, win_w, win_h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

BOOL preview_on_command(HWND main_hwnd, EDITP *ep, int cmd_id, BOOL *out_need_rerender)
{
    BOOL rerender = FALSE;
    if (out_need_rerender)
        *out_need_rerender = FALSE;

    if (cmd_id >= 9200 && cmd_id <= 9206)
    {
        g_zoom = cmd_id - 9200;
        preview_sync_menu_checks(main_hwnd);
        preview_auto_resize(main_hwnd, ep, FALSE);
        sync_ep_state(main_hwnd, ep);
        InvalidateRect(main_hwnd, NULL, FALSE);
        return TRUE;
    }

    switch (cmd_id)
    {
    case 9107: // zoom In (Numpad +)
        if (g_zoom == PREVIEW_ZOOM_WINDOW)
            g_zoom = PREVIEW_ZOOM_100;
        else if (g_zoom < PREVIEW_ZOOM_400)
            g_zoom++;
        preview_sync_menu_checks(main_hwnd);
        preview_auto_resize(main_hwnd, ep, FALSE);
        sync_ep_state(main_hwnd, ep);
        InvalidateRect(main_hwnd, NULL, FALSE);
        return TRUE;

    case 9108: // zoom Out (Numpad -)
        if (g_zoom == PREVIEW_ZOOM_WINDOW)
            g_zoom = PREVIEW_ZOOM_100;
        else if (g_zoom > PREVIEW_ZOOM_25)
            g_zoom--;
        preview_sync_menu_checks(main_hwnd);
        preview_auto_resize(main_hwnd, ep, FALSE);
        sync_ep_state(main_hwnd, ep);
        InvalidateRect(main_hwnd, NULL, FALSE);
        return TRUE;

    case 9104: // show Audio Waveform
        g_preview_flags ^= PREVIEW_FLAG_AUDIO;
        rerender = TRUE;
        preview_auto_resize(main_hwnd, ep, FALSE);
        break;

    case 9105: // preview While Saving
        g_preview_flags ^= PREVIEW_FLAG_PREVIEW;
        break;

    case 9106: // show Decimated / Delete Frames
        g_preview_flags ^= PREVIEW_FLAG_DELETEFRAME;
        break;

    case 9109: // show Source Keyframe ("*")
        g_preview_flags ^= PREVIEW_FLAG_AVIKEYFRAME;
        break;

    case 9110: // show Recompress Needed ("!")
        g_preview_flags ^= PREVIEW_FLAG_RECOMPRESSFRAME;
        break;

    case 9111: // overlay Display
        g_preview_flags ^= PREVIEW_FLAG_OVERLAY;
        break;

    case 9113: // always on Top
        g_preview_flags ^= PREVIEW_FLAG_ALWAYSTOP;
        SetWindowPos(main_hwnd,
                     (g_preview_flags & PREVIEW_FLAG_ALWAYSTOP) ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        break;

    case 9114: // show Time [HH:MM:SS.cc]
        g_preview_flags ^= PREVIEW_FLAG_TIME;
        break;

    case 9115: // show Data Rate While Saving
        g_preview_flags ^= PREVIEW_FLAG_DATARATE;
        break;

    case 9116: // auto Resize Window
        g_preview_flags ^= PREVIEW_FLAG_SIZEAUTO;
        if (g_preview_flags & PREVIEW_FLAG_SIZEAUTO)
        {
            preview_auto_resize(main_hwnd, ep, TRUE);
        }
        break;

    case 5144: // filter Only While Saving (nofilter)
        g_preview_flags ^= PREVIEW_FLAG_NOFILTER;
        rerender = TRUE;
        break;

    default:
        return FALSE;
    }

    preview_sync_menu_checks(main_hwnd);
    sync_ep_state(main_hwnd, ep);
    if (out_need_rerender)
        *out_need_rerender = rerender;
    InvalidateRect(main_hwnd, NULL, FALSE);
    return TRUE;
}

void preview_update_trackbar(EDITP *ep)
{
    if (!g_hwnd_trackbar || !IsWindow(g_hwnd_trackbar))
        return;

    if (!ep || !editp_is_editing(ep) || editp_get_frame_n(ep) <= 0)
    {
        SendMessageA(g_hwnd_trackbar, TBM_CLEARSEL, TRUE, 0);
        SendMessageA(g_hwnd_trackbar, TBM_SETRANGEMIN, FALSE, 0);
        SendMessageA(g_hwnd_trackbar, TBM_SETRANGEMAX, TRUE, 0);
        SendMessageA(g_hwnd_trackbar, TBM_SETPOS, TRUE, 0);
        EnableWindow(g_hwnd_trackbar, FALSE);
        return;
    }

    {
        int fn = editp_get_frame_n(ep);
        int rmax = (fn > 1) ? (fn - 1) : 0;
        int cur = editp_get_frame(ep);
        int s = -1, e = -1;
        int tic_freq = (fn > 20) ? (fn / 20) : 1;

        EnableWindow(g_hwnd_trackbar, TRUE);
        SendMessageA(g_hwnd_trackbar, TBM_SETRANGEMIN, FALSE, 0);
        SendMessageA(g_hwnd_trackbar, TBM_SETRANGEMAX, FALSE, rmax);
        SendMessageA(g_hwnd_trackbar, TBM_SETTICFREQ, (WPARAM)tic_freq, 0);
        SendMessageA(g_hwnd_trackbar, TBM_SETPAGESIZE, 0, g_sys_cfg.moveA > 0 ? g_sys_cfg.moveA : 5);

        if (editp_get_select_frame(ep, &s, &e) && s >= 0 && e >= s)
        {
            SendMessageA(g_hwnd_trackbar, TBM_SETSELSTART, FALSE, s);
            SendMessageA(g_hwnd_trackbar, TBM_SETSELEND, TRUE, e);
        }
        else
        {
            SendMessageA(g_hwnd_trackbar, TBM_CLEARSEL, TRUE, 0);
        }
        SendMessageA(g_hwnd_trackbar, TBM_SETPOS, TRUE, cur);
    }
}

// 0x00461ea0, frame blit, waveform, and status icons

void preview_set_frame_ex(EDITP *ep, int w, int h, void *bgr_bits,
                          const short *audio_pcm, int total_short_samples)
{
    g_cur_ep = ep;
    ZeroMemory(&g_bmi, sizeof(g_bmi));

    if (!ep || !editp_is_editing(ep) || !bgr_bits || w <= 0 || h <= 0)
    {
        g_frame_bits = NULL;
        g_frame_w = 0;
        g_frame_h = 0;
        g_audio_shorts = 0;
        g_preview_flags &= ~(PREVIEW_FLAG_EDITING | PREVIEW_FLAG_AT_SELECT_S |
                             PREVIEW_FLAG_AT_SELECT_E | PREVIEW_FLAG_AT_DELFRAME);
        preview_update_trackbar(ep);
        if (g_hwnd_main)
            sync_ep_state(g_hwnd_main, ep);
        return;
    }

    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = w;
    g_bmi.bmiHeader.biHeight = h; // bottom-up DIB
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 24;
    g_bmi.bmiHeader.biCompression = BI_RGB;
    g_frame_bits = bgr_bits;
    g_frame_w = w;
    g_frame_h = h;

    // copy PCM16 samples for current frame's waveform
    if (audio_pcm && total_short_samples > 0)
    {
        if (total_short_samples > g_audio_cap || !g_audio_pcm)
        {
            short *nb = (short *)realloc(g_audio_pcm, (size_t)total_short_samples * sizeof(short));
            if (nb)
            {
                g_audio_pcm = nb;
                g_audio_cap = total_short_samples;
            }
        }
        if (g_audio_pcm && total_short_samples <= g_audio_cap)
        {
            memcpy(g_audio_pcm, audio_pcm, (size_t)total_short_samples * sizeof(short));
            g_audio_shorts = total_short_samples;
        }
        else
        {
            g_audio_shorts = 0;
        }
    }
    else
    {
        g_audio_shorts = 0;
    }

    // update dynamic frame status bits (0x0041d090)
    {
        int cur = editp_get_frame(ep);
        int s = -1, e = -1;
        BYTE st = 0;

        g_preview_flags |= PREVIEW_FLAG_EDITING;
        editp_get_select_frame(ep, &s, &e);
        if (cur == s)
            g_preview_flags |= PREVIEW_FLAG_AT_SELECT_S;
        else
            g_preview_flags &= ~PREVIEW_FLAG_AT_SELECT_S;

        if (cur == e)
            g_preview_flags |= PREVIEW_FLAG_AT_SELECT_E;
        else
            g_preview_flags &= ~PREVIEW_FLAG_AT_SELECT_E;

        if (editp_get_frame_status(ep, cur, &st) && (st & EDIT_FRAME_EDIT_FLAG_DELFRAME))
        {
            g_preview_flags |= PREVIEW_FLAG_AT_DELFRAME;
        }
        else
        {
            g_preview_flags &= ~PREVIEW_FLAG_AT_DELFRAME;
        }
    }

    preview_update_trackbar(ep);
}

void preview_set_frame(int w, int h, void *bgr_bits)
{
    preview_set_frame_ex(g_cur_ep, w, h, bgr_bits, NULL, 0);
}

void preview_layout(HWND hwnd, RECT *out)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    out->left = 0;
    out->top = 0;
    out->right = rc.right;
    out->bottom = rc.bottom - PREVIEW_BOTTOM_BAR_H;
    if (out->bottom < 0)
        out->bottom = 0;
}

static void compute_zoom_geometry(int client_w, int avail_h, int w, int h)
{
    if (w <= 0 || h <= 0 || client_w <= 0 || avail_h <= 0)
    {
        g_dst_x = g_dst_y = g_dst_w = g_dst_h = 0;
        g_src_vis_w = w;
        g_src_vis_h = h;
        return;
    }

    if (g_zoom == PREVIEW_ZOOM_WINDOW)
    {
        // aspect-preserving WindowSize fit (0x00461ea0)
        int fw, fh;
        if ((long long)client_w * h < (long long)avail_h * w)
        {
            fw = client_w;
            fh = (int)(((long long)client_w * h) / w);
        }
        else
        {
            fh = avail_h;
            fw = (int)(((long long)avail_h * w) / h);
        }
        if (fw < 1)
            fw = 1;
        if (fh < 1)
            fh = 1;
        g_dst_x = (client_w - fw) / 2;
        g_dst_y = (avail_h - fh) / 2;
        g_dst_w = fw;
        g_dst_h = fh;
        g_src_vis_w = w;
        g_src_vis_h = h;
        return;
    }

    // fixed zoom 25%, 50%, 100%, 200%, 300%, 400% with top-left crop when larger than client
    if (g_zoom < PREVIEW_ZOOM_100)
    {
        int div = 4 - g_zoom * 2; // 4 for 25%, 2 for 50%
        int scaled_w = w / div;
        int scaled_h = h / div;
        int vis_w = w;
        int vis_h = h;
        int dx = (client_w - scaled_w) / 2;
        int dy = (avail_h - scaled_h) / 2;

        if (dx < 0)
        {
            dx = 0;
            vis_w = client_w * div;
            if (vis_w > w)
                vis_w = w;
        }
        if (dy < 0)
        {
            dy = 0;
            vis_h = avail_h * div;
            if (vis_h > h)
                vis_h = h;
        }
        g_dst_x = dx;
        g_dst_y = dy;
        g_src_vis_w = vis_w;
        g_src_vis_h = vis_h;
        g_dst_w = vis_w / div;
        g_dst_h = vis_h / div;
    }
    else
    {
        int mul = g_zoom - 1; // 1, 2, 3, 4
        int scaled_w = w * mul;
        int scaled_h = h * mul;
        int vis_w = w;
        int vis_h = h;
        int dx = (client_w - scaled_w) / 2;
        int dy = (avail_h - scaled_h) / 2;

        if (dx < 0)
        {
            dx = 0;
            vis_w = client_w / mul;
            if (vis_w < 1)
                vis_w = 1;
            if (vis_w > w)
                vis_w = w;
        }
        if (dy < 0)
        {
            dy = 0;
            vis_h = avail_h / mul;
            if (vis_h < 1)
                vis_h = 1;
            if (vis_h > h)
                vis_h = h;
        }
        g_dst_x = dx;
        g_dst_y = dy;
        g_src_vis_w = vis_w;
        g_src_vis_h = vis_h;
        g_dst_w = vis_w * mul;
        g_dst_h = vis_h * mul;
    }
}

// 0x00412300, client to image coordinate mapping
BOOL preview_client_to_image(HWND hwnd, int cx, int cy, int *out_img_x, int *out_img_y)
{
    RECT rc;
    int avail_h, ix, iy;

    if (!g_frame_bits || g_frame_w <= 0 || g_frame_h <= 0)
        return FALSE;
    GetClientRect(hwnd, &rc);
    avail_h = rc.bottom - PREVIEW_BOTTOM_BAR_H;
    if (g_preview_flags & PREVIEW_FLAG_AUDIO)
    {
        avail_h -= PREVIEW_AUDIO_WAVE_H;
    }
    compute_zoom_geometry(rc.right, avail_h, g_frame_w, g_frame_h);
    if (g_dst_w <= 0 || g_dst_h <= 0)
        return FALSE;

    ix = (int)(((long long)(cx - g_dst_x) * g_src_vis_w) / g_dst_w);
    iy = (int)(((long long)(cy - g_dst_y) * g_src_vis_h) / g_dst_h);

    if (out_img_x)
        *out_img_x = ix;
    if (out_img_y)
        *out_img_y = iy;
    return (cy >= 0 && cy < avail_h);
}

// render 63px green-on-dark-gray PCM16 waveform (0x00461ea0)
static void paint_audio_waveform(HDC hdc, int client_w, int client_h)
{
    RECT sep_rc;
    int wave_w = (client_w < WAVE_MAX_W) ? client_w : WAVE_MAX_W;
    int x, row;

    if (client_h <= 95 || wave_w <= 0)
        return;

    // 1px COLOR_BTNFACE separator at y = client_h - 96
    SetRect(&sep_rc, 0, client_h - 96, client_w, client_h - 95);
    FillRect(hdc, &sep_rc, (HBRUSH)(COLOR_BTNFACE + 1));

    if (!g_audio_pcm || g_audio_shorts <= 0 || !g_cur_ep || !editp_has_audio(g_cur_ep))
    {
        // solid 0x3f3f3f dark gray when no audio stream/samples (0x00461ea0)
        for (row = 0; row < WAVE_H; row++)
        {
            memset(g_wave_dib + (size_t)row * WAVE_STRIDE, 0x3f, (size_t)wave_w * 3u);
        }
    }
    else
    {
        for (row = 0; row < WAVE_H; row++)
        {
            memset(g_wave_dib + (size_t)row * WAVE_STRIDE, 0x3f, (size_t)wave_w * 3u);
        }
        for (x = 0; x < wave_w; x++)
        {
            int i0 = (int)(((long long)x * g_audio_shorts) / wave_w);
            int i1 = (int)(((long long)(x + 1) * g_audio_shorts) / wave_w);
            int max_abs = 0, s_idx, amp, r0, r1;

            if (i1 <= i0)
                i1 = i0 + 1;
            if (i1 > g_audio_shorts)
                i1 = g_audio_shorts;

            for (s_idx = i0; s_idx < i1; s_idx++)
            {
                int v = (int)g_audio_pcm[s_idx];
                if (v < 0)
                    v = -v;
                if (v > max_abs)
                    max_abs = v;
            }

            amp = max_abs >> 10; // 0..32 clamped to 0..31
            if (amp > 31)
                amp = 31;
            r0 = 31 - amp;
            r1 = 31 + amp;

            // 0x3fff3f: B=0x3f, G=0xff, R=0x3f (draws 1px green center line when amp == 0)
            for (row = r0; row <= r1; row++)
            {
                unsigned char *px = g_wave_dib + (size_t)row * WAVE_STRIDE + (size_t)x * 3u;
                px[0] = 0x3f;
                px[1] = 0xff;
                px[2] = 0x3f;
            }
        }
    }

    SetDIBitsToDevice(hdc, 0, client_h - 95, (DWORD)wave_w, WAVE_H,
                      0, 0, 0, WAVE_H, g_wave_dib, &g_wave_bmi, DIB_RGB_COLORS);

    if (client_w > wave_w)
    {
        RECT right_rc;
        HBRUSH dark_br = CreateSolidBrush(RGB(0x3f, 0x3f, 0x3f));
        SetRect(&right_rc, wave_w, client_h - 95, client_w, client_h - 32);
        FillRect(hdc, &right_rc, dark_br);
        DeleteObject(dark_br);
    }
}

static void draw_corner_badge(HDC hdc, int x, int y, HICON hIcon)
{
    RECT rc;
    if (!hIcon)
        return;
    SetRect(&rc, x, y, x + 24, y + 24);
    DrawIconEx(hdc, x, y, hIcon, 24, 24, 0, NULL, DI_NORMAL);
    // flags from 0x00461ea0: BDR_RAISEDINNER (4), BF_FLAT | BF_RECT (0x80f)
    DrawEdge(hdc, &rc, BDR_RAISEDINNER, BF_FLAT | BF_RECT);
}

void preview_paint(HWND hwnd, HDC hdc)
{
    RECT cli_rc, bar_rc;
    HBRUSH btn_face = (HBRUSH)(COLOR_BTNFACE + 1);
    int client_w, client_h, avail_h;

    GetClientRect(hwnd, &cli_rc);
    client_w = cli_rc.right;
    client_h = cli_rc.bottom;

    // bottom 32px control bar background (0x00412300)
    SetRect(&bar_rc, 0, client_h - PREVIEW_BOTTOM_BAR_H, client_w, client_h);
    if (bar_rc.top < 0)
        bar_rc.top = 0;
    FillRect(hdc, &bar_rc, btn_face);

    // idle state (no file/project open): solid COLOR_BTNFACE, no text or checkerboard
    if (!g_frame_bits || g_frame_w <= 0 || g_frame_h <= 0 ||
        !(g_preview_flags & PREVIEW_FLAG_EDITING))
    {
        RECT empty_rc;
        SetRect(&empty_rc, 0, 0, client_w, bar_rc.top);
        if (empty_rc.bottom > empty_rc.top)
        {
            FillRect(hdc, &empty_rc, btn_face);
        }
        g_dst_x = g_dst_y = g_dst_w = g_dst_h = 0;
        sync_ep_state(hwnd, g_cur_ep);
        return;
    }

    // optional 64px audio waveform strip [client_h - 96 .. client_h - 32)
    avail_h = client_h - PREVIEW_BOTTOM_BAR_H;
    if (g_preview_flags & PREVIEW_FLAG_AUDIO)
    {
        paint_audio_waveform(hdc, client_w, client_h);
        avail_h = client_h - (PREVIEW_BOTTOM_BAR_H + PREVIEW_AUDIO_WAVE_H);
    }
    if (avail_h <= 0)
    {
        g_dst_x = g_dst_y = g_dst_w = g_dst_h = 0;
        sync_ep_state(hwnd, g_cur_ep);
        return;
    }

    // compute zoom & top-left crop geometry and fill 4 COLOR_BTNFACE letterbox bars
    compute_zoom_geometry(client_w, avail_h, g_frame_w, g_frame_h);
    {
        RECT r;
        if (g_dst_y > 0)
        {
            SetRect(&r, 0, 0, client_w, g_dst_y);
            FillRect(hdc, &r, btn_face);
        }
        if (g_dst_y + g_dst_h < avail_h)
        {
            SetRect(&r, 0, g_dst_y + g_dst_h, client_w, avail_h);
            FillRect(hdc, &r, btn_face);
        }
        if (g_dst_x > 0)
        {
            SetRect(&r, 0, g_dst_y, g_dst_x, g_dst_y + g_dst_h);
            FillRect(hdc, &r, btn_face);
        }
        if (g_dst_x + g_dst_w < client_w)
        {
            SetRect(&r, g_dst_x + g_dst_w, g_dst_y, client_w, g_dst_y + g_dst_h);
            FillRect(hdc, &r, btn_face);
        }
    }

    // blit bottom-up BGR24 DIB (top-left crop -> YSrc = g_frame_h - g_src_vis_h)
    if (g_dst_w > 0 && g_dst_h > 0)
    {
        int y_src = g_frame_h - g_src_vis_h;
        if (g_zoom == PREVIEW_ZOOM_100)
        {
            SetDIBitsToDevice(hdc,
                              g_dst_x, g_dst_y,
                              (DWORD)g_src_vis_w, (DWORD)g_src_vis_h,
                              0, y_src, 0, (UINT)g_frame_h,
                              g_frame_bits, &g_bmi, DIB_RGB_COLORS);
        }
        else
        {
            if (g_zoom == PREVIEW_ZOOM_WINDOW)
            {
                SetStretchBltMode(hdc, HALFTONE);
                SetBrushOrgEx(hdc, 0, 0, NULL);
            }
            else
            {
                SetStretchBltMode(hdc, COLORONCOLOR);
            }
            StretchDIBits(hdc,
                          g_dst_x, g_dst_y, g_dst_w, g_dst_h,
                          0, y_src, g_src_vis_w, g_src_vis_h,
                          g_frame_bits, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
        }

        // corner status badges: ICON_IN, ICON_MARK, ICON_DEL/DEL2, ICON_I_*, ICON_OUT (0x00461ea0)
        if (avail_h >= 36 && client_w >= 64)
        {
            int left_x = g_dst_x + 6;
            int top_y = g_dst_y + 6;
            int cur_f = g_cur_ep ? editp_get_frame(g_cur_ep) : 0;
            int sel_s = 0, sel_e = 0;
            BYTE fst = 0;
            BYTE finter = g_cur_ep ? editp_get_frame_inter(g_cur_ep, cur_f) : 0;

            if (g_cur_ep)
            {
                editp_get_select_frame(g_cur_ep, &sel_s, &sel_e);
                editp_get_frame_status(g_cur_ep, cur_f, &fst);
            }
            if (g_preview_flags & PREVIEW_FLAG_AT_SELECT_S)
            {
                draw_corner_badge(hdc, left_x, top_y, g_icon_in);
                left_x += 24;
            }
            if (fst & EDIT_FRAME_EDIT_FLAG_MARKFRAME)
            {
                draw_corner_badge(hdc, left_x, top_y, g_icon_mark);
                left_x += 24;
            }
            if ((g_preview_flags & (PREVIEW_FLAG_DELETEFRAME | PREVIEW_FLAG_AT_DELFRAME)) ==
                (PREVIEW_FLAG_DELETEFRAME | PREVIEW_FLAG_AT_DELFRAME))
            {
                draw_corner_badge(hdc, left_x, top_y, g_icon_del);
                left_x += 24;
            }
            else if ((g_preview_flags & PREVIEW_FLAG_DELETEFRAME) &&
                     cur_f >= sel_s &&
                     !builtin_filters_is_framerate_kept_frame(cur_f - sel_s))
            {
                draw_corner_badge(hdc, left_x, top_y, g_icon_del2);
                left_x += 24;
            }
            if (finter >= 1 && finter <= 5 && g_icon_inter[finter])
            {
                draw_corner_badge(hdc, left_x, top_y, g_icon_inter[finter]);
            }
            if (g_preview_flags & PREVIEW_FLAG_AT_SELECT_E)
            {
                draw_corner_badge(hdc, client_w - g_dst_x - 30, top_y, g_icon_out);
            }
        }
    }

    sync_ep_state(hwnd, g_cur_ep);
}
