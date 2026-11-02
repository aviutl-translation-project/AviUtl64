#include <windows.h>
#include <string.h>

#include "winsnap.h"
#include "plugin_manager.h"

#define WINSNAP_MAX_WINDOWS 256
#define WINSNAP_THRESHOLD 10

static int g_enabled_override = -1;
static HWND g_drag_hwnd = NULL;
static BOOL g_drag_is_main = FALSE;
static RECT g_prev_rect;
static int g_snap_dx = WINSNAP_THRESHOLD;
static int g_snap_dy = WINSNAP_THRESHOLD;
static HWND g_win_list[WINSNAP_MAX_WINDOWS];
static BYTE g_docked[WINSNAP_MAX_WINDOWS];
static int g_win_count = 0;

static int iabs_val(int v)
{
    return (v < 0) ? -v : v;
}

static BOOL query_ini_windowsnap(void)
{
    char path[MAX_PATH];
    char *slash;
    path[0] = '\0';
    if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0)
        return TRUE;
    slash = strrchr(path, '\\');
    if (slash)
        lstrcpyA(slash + 1, "aviutl.ini");
    else
        lstrcpyA(path, "aviutl.ini");
    return GetPrivateProfileIntA("system", "windowsnap", 1, path) != 0;
}

void winsnap_set_enabled(BOOL enabled)
{
    g_enabled_override = enabled ? 1 : 0;
}

BOOL winsnap_is_enabled(void)
{
    if (g_enabled_override >= 0)
        return g_enabled_override != 0;
    return query_ini_windowsnap();
}

static void add_candidate_hwnd(HWND h)
{
    int i;
    if (!h || !IsWindow(h) || !IsWindowVisible(h) || IsIconic(h))
        return;
    if (GetParent(h) != NULL)
        return;
    if (g_win_count >= WINSNAP_MAX_WINDOWS)
        return;
    for (i = 0; i < g_win_count; i++)
    {
        if (g_win_list[i] == h)
            return;
    }
    g_win_list[g_win_count++] = h;
}

// 0x004053f0: EnumThreadWindows callback for top-level visible windows
static BOOL CALLBACK enum_thread_win_cb(HWND h, LPARAM lp)
{
    (void)lp;
    if (g_win_count >= WINSNAP_MAX_WINDOWS)
        return FALSE;
    add_candidate_hwnd(h);
    return TRUE;
}

static void collect_snap_windows(HWND dragged, HWND main_hwnd)
{
    int i, fcount;

    g_win_count = 0;
    add_candidate_hwnd(dragged);
    add_candidate_hwnd(main_hwnd);
    EnumThreadWindows(GetCurrentThreadId(), enum_thread_win_cb, 0);

    // include native and bridged plugin windows so x64 host and x86 bridge snap together
    fcount = plugin_manager_get_count();
    for (i = 0; i < fcount && g_win_count < WINSNAP_MAX_WINDOWS; i++)
    {
        add_candidate_hwnd(plugin_manager_get_window(i));
    }
}

// 0x00404cf0: recursively mark windows currently touching `rc_ref` so docked followers don't self-snap
static BOOL mark_docked_recursive(HWND h, const RECT *rc_ref)
{
    RECT rc;
    BOOL touching = FALSE;
    int i;

    if (!h || h == g_drag_hwnd || !GetWindowRect(h, &rc))
        return FALSE;

    if (rc_ref->left < rc.right + 1 && rc.left - 1 < rc_ref->right &&
        (rc.bottom == rc_ref->top || rc.top == rc_ref->bottom ||
         rc.top == rc_ref->top || rc.bottom == rc_ref->bottom))
    {
        touching = TRUE;
    }
    if (rc_ref->top < rc.bottom + 1 && rc.top - 1 < rc_ref->bottom &&
        (rc.right == rc_ref->left || rc.left == rc_ref->right ||
         rc.left == rc_ref->left || rc.right == rc_ref->right))
    {
        touching = TRUE;
    }

    if (!touching)
        return FALSE;

    for (i = 0; i < g_win_count; i++)
    {
        if (g_docked[i] == 0)
        {
            g_docked[i] = 1;
            g_docked[i] = mark_docked_recursive(g_win_list[i], &rc) ? 1 : 0;
        }
    }
    return TRUE;
}

// 0x00404e10: magnetic edge snapping on WM_MOVING
static void snap_moving_rect(HWND hwnd, RECT *rc_io)
{
    int i;

    if (!rc_io || hwnd != g_drag_hwnd)
        return;

    if (g_snap_dx != WINSNAP_THRESHOLD)
    {
        rc_io->left -= g_snap_dx;
        rc_io->right -= g_snap_dx;
    }
    if (g_snap_dy != WINSNAP_THRESHOLD)
    {
        rc_io->top -= g_snap_dy;
        rc_io->bottom -= g_snap_dy;
    }

    g_snap_dx = WINSNAP_THRESHOLD;
    g_snap_dy = WINSNAP_THRESHOLD;
    memset(g_docked, 0, sizeof(g_docked));

    if (g_drag_is_main)
    {
        for (i = 0; i < g_win_count; i++)
        {
            if (g_docked[i] == 0)
            {
                g_docked[i] = 1;
                g_docked[i] = mark_docked_recursive(g_win_list[i], &g_prev_rect) ? 1 : 0;
            }
        }
    }

    for (i = 0; i < g_win_count; i++)
    {
        RECT target;
        BOOL ok;

        if (g_docked[i] != 0)
            continue;

        if (g_win_list[i] == g_drag_hwnd)
        {
            // snap to monitor work area containing the dragged window
            HMONITOR hmon = MonitorFromRect(rc_io, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            ZeroMemory(&mi, sizeof(mi));
            mi.cbSize = sizeof(mi);
            if (hmon && GetMonitorInfoA(hmon, &mi))
            {
                target = mi.rcWork;
                ok = TRUE;
            }
            else
            {
                ok = SystemParametersInfoA(SPI_GETWORKAREA, 0, &target, 0);
            }
        }
        else
        {
            ok = GetWindowRect(g_win_list[i], &target);
        }

        if (!ok)
            continue;

        if (rc_io->left < target.right + WINSNAP_THRESHOLD &&
            target.left - WINSNAP_THRESHOLD < rc_io->right)
        {
            int d;
            d = target.top - rc_io->bottom;
            if (iabs_val(d) < iabs_val(g_snap_dy))
                g_snap_dy = d;
            d = target.bottom - rc_io->top;
            if (iabs_val(d) < iabs_val(g_snap_dy))
                g_snap_dy = d;
            d = target.top - rc_io->top;
            if (iabs_val(d) < iabs_val(g_snap_dy))
                g_snap_dy = d;
            d = target.bottom - rc_io->bottom;
            if (iabs_val(d) < iabs_val(g_snap_dy))
                g_snap_dy = d;
        }

        if (rc_io->top < target.bottom + WINSNAP_THRESHOLD &&
            target.top - WINSNAP_THRESHOLD < rc_io->bottom)
        {
            int d;
            d = target.left - rc_io->right;
            if (iabs_val(d) < iabs_val(g_snap_dx))
                g_snap_dx = d;
            d = target.right - rc_io->left;
            if (iabs_val(d) < iabs_val(g_snap_dx))
                g_snap_dx = d;
            d = target.left - rc_io->left;
            if (iabs_val(d) < iabs_val(g_snap_dx))
                g_snap_dx = d;
            d = target.right - rc_io->right;
            if (iabs_val(d) < iabs_val(g_snap_dx))
                g_snap_dx = d;
        }
    }

    if (g_snap_dx != WINSNAP_THRESHOLD)
    {
        rc_io->left += g_snap_dx;
        rc_io->right += g_snap_dx;
    }
    if (g_snap_dy != WINSNAP_THRESHOLD)
    {
        rc_io->top += g_snap_dy;
        rc_io->bottom += g_snap_dy;
    }
}

// 0x004050b0: move docked follower windows transitively when main window moves or resizes
static BOOL move_docked_recursive(HWND h, const RECT *rc_ref,
                                  int dl, int dt, int dr, int db)
{
    RECT rc;
    int mx = 0, my = 0;
    int best_x, best_y, d, near_dx, near_dy;
    int i;

    if (!h || h == g_drag_hwnd || !GetWindowRect(h, &rc))
        return FALSE;

    best_x = iabs_val(rc_ref->left - rc.left);
    near_dx = dl;
    d = iabs_val(rc_ref->left - rc.right);
    if (d < best_x)
        best_x = d;
    d = iabs_val(rc_ref->right - rc.left);
    if (d < best_x)
    {
        best_x = d;
        near_dx = dr;
    }
    d = iabs_val(rc_ref->right - rc.right);
    if (d < best_x)
    {
        near_dx = dr;
    }

    best_y = iabs_val(rc_ref->top - rc.top);
    near_dy = dt;
    d = iabs_val(rc_ref->top - rc.bottom);
    if (d < best_y)
        best_y = d;
    d = iabs_val(rc_ref->bottom - rc.top);
    if (d < best_y)
    {
        best_y = d;
        near_dy = db;
    }
    d = iabs_val(rc_ref->bottom - rc.bottom);
    if (d < best_y)
    {
        near_dy = db;
    }

    if (rc_ref->left < rc.right + 1 && rc.left - 1 < rc_ref->right)
    {
        if (rc.bottom == rc_ref->top)
        {
            mx = near_dx;
            my = dt;
        }
        else if (rc.top == rc_ref->bottom)
        {
            mx = near_dx;
            my = db;
        }
        else if (rc.top == rc_ref->top)
        {
            mx = near_dx;
            my = dt;
        }
        else if (rc.bottom == rc_ref->bottom)
        {
            mx = near_dx;
            my = db;
        }
    }

    if (rc_ref->top < rc.bottom + 1 && rc.top - 1 < rc_ref->bottom)
    {
        if (rc.right == rc_ref->left)
        {
            mx = dl;
            my = near_dy;
        }
        else if (rc.left == rc_ref->right)
        {
            mx = dr;
            my = near_dy;
        }
        else if (rc.left == rc_ref->left)
        {
            mx = dl;
            my = near_dy;
        }
        else if (rc.right == rc_ref->right)
        {
            mx = dr;
            my = near_dy;
        }
    }

    if (mx != 0 || my != 0)
    {
        UINT swp_flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE;
        if (GetWindowThreadProcessId(h, NULL) != GetCurrentThreadId())
        {
            swp_flags |= SWP_ASYNCWINDOWPOS;
        }
        SetWindowPos(h, NULL, rc.left + mx, rc.top + my, 0, 0, swp_flags);

        for (i = 0; i < g_win_count; i++)
        {
            if (g_docked[i] == 0)
            {
                g_docked[i] = 1;
                g_docked[i] = move_docked_recursive(g_win_list[i], &rc, mx, my, mx, my) ? 1 : 0;
            }
        }
        return TRUE;
    }
    return FALSE;
}

// 0x00405300: apply docked follower displacement on WM_MOVE / WM_SIZE of main window
static void on_main_movesize(HWND hwnd)
{
    RECT cur;
    int dl, dt, dr, db, i;

    if (hwnd != g_drag_hwnd || !g_drag_is_main)
        return;
    if (!GetWindowRect(hwnd, &cur))
        return;

    dl = cur.left - g_prev_rect.left;
    dt = cur.top - g_prev_rect.top;
    dr = cur.right - g_prev_rect.right;
    db = cur.bottom - g_prev_rect.bottom;

    if (dl != 0 || dt != 0 || dr != 0 || db != 0)
    {
        memset(g_docked, 0, sizeof(g_docked));
        for (i = 0; i < g_win_count; i++)
        {
            if (g_docked[i] == 0)
            {
                g_docked[i] = 1;
                g_docked[i] = move_docked_recursive(g_win_list[i], &g_prev_rect,
                                                    dl, dt, dr, db)
                                  ? 1
                                  : 0;
            }
        }
        g_prev_rect = cur;
    }
}

BOOL winsnap_handle_message(HWND hwnd, HWND main_hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    if (!winsnap_is_enabled())
    {
        if (msg == WM_EXITSIZEMOVE)
        {
            g_drag_hwnd = NULL;
            g_win_count = 0;
        }
        return FALSE;
    }

    switch (msg)
    {
    case WM_ENTERSIZEMOVE:
        g_snap_dx = WINSNAP_THRESHOLD;
        g_snap_dy = WINSNAP_THRESHOLD;
        GetWindowRect(hwnd, &g_prev_rect);
        collect_snap_windows(hwnd, main_hwnd);
        g_drag_hwnd = hwnd;
        g_drag_is_main = (main_hwnd != NULL && hwnd == main_hwnd);
        return TRUE;

    case WM_EXITSIZEMOVE:
        g_drag_hwnd = NULL;
        g_drag_is_main = FALSE;
        g_win_count = 0;
        return TRUE;

    case WM_MOVING:
        if (lp != 0 && hwnd == g_drag_hwnd)
        {
            snap_moving_rect(hwnd, (RECT *)lp);
            return TRUE;
        }
        return FALSE;

    case WM_MOVE:
    case WM_SIZE:
        if (hwnd == g_drag_hwnd && g_drag_is_main)
        {
            on_main_movesize(hwnd);
        }
        return FALSE;
    }
    return FALSE;
}
