#include <windows.h>
#include <commctrl.h>
#include <string.h>

#include "aviutl_dlg.h"
#include "i18n.h"

#define IDC_DLG_INT_BASE 32000
#define IDC_DLG_STR_BASE 32100
#define IDC_DLG_CHK_BASE 32200
#define IDC_DLG_CMB_BASE 32500
#define IDC_DLG_LST_BASE 32600

#define IDC_DLG_LST_RESET 32700
#define IDC_DLG_LST_UP 32701
#define IDC_DLG_LST_DOWN 32702
#define IDC_DLG_DRAG_ICON 32715
#define IDC_DLG_DRAG_COMMIT 0x7FCC

typedef struct DlgRunCtx
{
    HWND owner;
    AviUtlDlgState *st;
    UINT drag_msg;
    HWND drag_icon_hwnd;
    int drag_src_idx;
    int drag_dst_idx;
    POINT lb_origin;
} DlgRunCtx;

static __declspec(thread) HHOOK g_cbt_hook = NULL;
static __declspec(thread) HWND g_cbt_owner = NULL;

// 0x004111a0: WH_CBT hook to center MessageBox over main/owner window
static LRESULT CALLBACK cbt_center_msgbox_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
    HHOOK hk = g_cbt_hook;
    if (nCode == HCBT_ACTIVATE)
    {
        HWND hwndBox = (HWND)wParam;
        HWND owner = g_cbt_owner;
        if (hk)
        {
            UnhookWindowsHookEx(hk);
            g_cbt_hook = NULL;
        }
        if (hwndBox && owner && IsWindow(owner))
        {
            RECT rc_box, rc_own;
            int x, y;
            GetWindowRect(hwndBox, &rc_box);
            GetWindowRect(owner, &rc_own);
            x = ((rc_box.left - rc_box.right) + rc_own.right + rc_own.left) / 2;
            y = ((rc_box.top - rc_box.bottom) + rc_own.bottom + rc_own.top) / 2;
            if (x < 0)
                x = 0;
            if (y < 0)
                y = 0;
            SetWindowPos(hwndBox, NULL, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    return hk ? CallNextHookEx(hk, nCode, wParam, lParam) : 0;
}

int aviutl_msgbox_w(HWND owner, const wchar_t *text, const wchar_t *caption, UINT type)
{
    int ret;
    if (owner && IsWindow(owner))
    {
        g_cbt_owner = owner;
        g_cbt_hook = SetWindowsHookExW(WH_CBT, cbt_center_msgbox_proc,
                                       NULL, GetCurrentThreadId());
    }
    ret = MessageBoxW(owner, text ? text : L"", caption ? caption : L"AviUtl", type);
    if (g_cbt_hook)
    {
        UnhookWindowsHookEx(g_cbt_hook);
        g_cbt_hook = NULL;
    }
    g_cbt_owner = NULL;
    return ret;
}

int aviutl_msgbox_sjis(HWND owner, const char *text, const char *caption, UINT type)
{
    wchar_t wtext[4096];
    wchar_t wcap[260];

    wtext[0] = L'\0';
    wcap[0] = L'\0';
    if (text && text[0])
    {
        if (MultiByteToWideChar(932, 0, text, -1, wtext, 4096) <= 0)
            MultiByteToWideChar(CP_ACP, 0, text, -1, wtext, 4096);
    }
    if (caption && caption[0])
    {
        if (MultiByteToWideChar(932, 0, caption, -1, wcap, 260) <= 0)
            MultiByteToWideChar(CP_ACP, 0, caption, -1, wcap, 260);
    }
    else
    {
        lstrcpyW(wcap, L"AviUtl");
    }
    return aviutl_msgbox_w(owner, wtext, wcap, type);
}

void aviutl_dlg_init(AviUtlDlgState *st)
{
    if (!st)
        return;
    ZeroMemory(st, sizeof(*st));
}

void aviutl_dlg_set_str_sjis(AviUtlDlgState *st, int slot, const char *src)
{
    if (!st || slot < 0 || slot >= AVIUTL_DLG_SLOTS)
        return;
    st->strs[slot][0] = L'\0';
    if (!src || !src[0])
        return;
    if (MultiByteToWideChar(932, 0, src, -1, st->strs[slot], 260) <= 0)
    {
        MultiByteToWideChar(CP_ACP, 0, src, -1, st->strs[slot], 260);
    }
}

void aviutl_dlg_get_str_sjis(const AviUtlDlgState *st, int slot, char *dst, int dst_cap)
{
    if (!dst || dst_cap <= 0)
        return;
    dst[0] = '\0';
    if (!st || slot < 0 || slot >= AVIUTL_DLG_SLOTS)
        return;
    if (!st->strs[slot][0])
        return;
    if (WideCharToMultiByte(932, 0, st->strs[slot], -1, dst, dst_cap, NULL, NULL) <= 0)
    {
        WideCharToMultiByte(CP_ACP, 0, st->strs[slot], -1, dst, dst_cap, NULL, NULL);
    }
}

// 0x004112f0: refresh ordered listbox slot
static void refresh_listbox_slot(HWND hdlg, AviUtlDlgState *st, int slot, int new_sel)
{
    int id = IDC_DLG_LST_BASE + slot;
    int top_idx, k, pos, n;
    const wchar_t *const *items;
    const int *ord;

    if (!st || slot < 0 || slot >= AVIUTL_DLG_SLOTS)
        return;
    items = st->list_items[slot];
    ord = st->list_order[slot];
    n = st->list_count[slot];
    if (!items || n <= 0)
        return;

    top_idx = (int)SendDlgItemMessageW(hdlg, id, LB_GETTOPINDEX, 0, 0);
    SendDlgItemMessageW(hdlg, id, LB_RESETCONTENT, 0, 0);

    if (ord)
    {
        for (pos = 0; pos < n; pos++)
        {
            for (k = 0; k < n; k++)
            {
                if (ord[k] == pos && items[k])
                {
                    SendDlgItemMessageW(hdlg, id, LB_ADDSTRING, 0, (LPARAM)items[k]);
                    break;
                }
            }
        }
    }
    else
    {
        for (k = 0; k < n && items[k]; k++)
        {
            SendDlgItemMessageW(hdlg, id, LB_ADDSTRING, 0, (LPARAM)items[k]);
        }
    }

    if (top_idx >= 0)
        SendDlgItemMessageW(hdlg, id, LB_SETTOPINDEX, (WPARAM)top_idx, 0);
    if (new_sel >= 0 && new_sel < n)
        SendDlgItemMessageW(hdlg, id, LB_SETCURSEL, (WPARAM)new_sel, 0);
}

static void move_listbox_order(HWND hdlg, AviUtlDlgState *st, int slot, int src_pos, int dst_pos)
{
    int *ord;
    int n, k;

    if (!st || slot < 0 || slot >= AVIUTL_DLG_SLOTS)
        return;
    ord = st->list_order[slot];
    n = st->list_count[slot];
    if (!ord || n <= 1)
        return;
    if (src_pos < 0 || src_pos >= n || dst_pos < 0 || dst_pos >= n || src_pos == dst_pos)
        return;

    for (k = 0; k < n; k++)
    {
        if (ord[k] == src_pos)
        {
            ord[k] = dst_pos;
        }
        else if (src_pos < dst_pos && ord[k] > src_pos && ord[k] <= dst_pos)
        {
            ord[k]--;
        }
        else if (src_pos > dst_pos && ord[k] >= dst_pos && ord[k] < src_pos)
        {
            ord[k]++;
        }
    }
    refresh_listbox_slot(hdlg, st, slot, dst_pos);
}

// 0x004114c0: populate dialog controls from AviUtlDlgState
static void sync_state_to_controls(HWND hdlg, DlgRunCtx *ctx)
{
    AviUtlDlgState *st = ctx->st;
    int i;

    for (i = 0; i < AVIUTL_DLG_SLOTS; i++)
    {
        HWND hCtrl;

        hCtrl = GetDlgItem(hdlg, IDC_DLG_INT_BASE + i);
        if (hCtrl)
        {
            SetDlgItemInt(hdlg, IDC_DLG_INT_BASE + i, (UINT)st->ints[i], TRUE);
        }

        hCtrl = GetDlgItem(hdlg, IDC_DLG_STR_BASE + i);
        if (hCtrl)
        {
            SetDlgItemTextW(hdlg, IDC_DLG_STR_BASE + i, st->strs[i]);
        }

        hCtrl = GetDlgItem(hdlg, IDC_DLG_CHK_BASE + i);
        if (hCtrl)
        {
            int chk = st->checks[i];
            if (chk < 0)
            {
                st->checks[i] = 0;
                EnableWindow(hCtrl, FALSE);
                chk = 0;
            }
            CheckDlgButton(hdlg, IDC_DLG_CHK_BASE + i, chk ? BST_CHECKED : BST_UNCHECKED);
        }

        hCtrl = GetDlgItem(hdlg, IDC_DLG_CMB_BASE + i);
        if (hCtrl)
        {
            const wchar_t *const *citems = st->combo_items[i];
            SendDlgItemMessageW(hdlg, IDC_DLG_CMB_BASE + i, CB_RESETCONTENT, 0, 0);
            if (!citems)
            {
                EnableWindow(hCtrl, FALSE);
            }
            else
            {
                int k = 0;
                while (citems[k])
                {
                    SendDlgItemMessageW(hdlg, IDC_DLG_CMB_BASE + i, CB_ADDSTRING, 0,
                                        (LPARAM)citems[k]);
                    k++;
                }
                if (st->combo_sel[i] < 0)
                {
                    SetDlgItemTextW(hdlg, IDC_DLG_CMB_BASE + i, st->combo_text[i]);
                }
                else
                {
                    SendDlgItemMessageW(hdlg, IDC_DLG_CMB_BASE + i, CB_SETCURSEL,
                                        (WPARAM)st->combo_sel[i], 0);
                }
            }
        }

        hCtrl = GetDlgItem(hdlg, IDC_DLG_LST_BASE + i);
        if (hCtrl && st->list_items[i])
        {
            int cur_sel = (int)SendDlgItemMessageW(hdlg, IDC_DLG_LST_BASE + i, LB_GETCURSEL, 0, 0);
            if (cur_sel < 0)
                cur_sel = 0;
            refresh_listbox_slot(hdlg, st, i, cur_sel);
        }
    }
}

// 0x00411680: read dialog controls back into AviUtlDlgState
static void sync_controls_to_state(HWND hdlg, AviUtlDlgState *st)
{
    int i;

    for (i = 0; i < AVIUTL_DLG_SLOTS; i++)
    {
        if (GetDlgItem(hdlg, IDC_DLG_INT_BASE + i))
        {
            BOOL ok = FALSE;
            st->ints[i] = (int)GetDlgItemInt(hdlg, IDC_DLG_INT_BASE + i, &ok, TRUE);
        }
        if (GetDlgItem(hdlg, IDC_DLG_STR_BASE + i))
        {
            GetDlgItemTextW(hdlg, IDC_DLG_STR_BASE + i, st->strs[i], 260);
        }
        if (GetDlgItem(hdlg, IDC_DLG_CHK_BASE + i))
        {
            st->checks[i] = (IsDlgButtonChecked(hdlg, IDC_DLG_CHK_BASE + i) == BST_CHECKED) ? 1 : 0;
        }
        if (GetDlgItem(hdlg, IDC_DLG_CMB_BASE + i))
        {
            GetDlgItemTextW(hdlg, IDC_DLG_CMB_BASE + i, st->combo_text[i], 260);
            st->combo_sel[i] = (int)SendDlgItemMessageW(hdlg, IDC_DLG_CMB_BASE + i,
                                                        CB_GETCURSEL, 0, 0);
        }
    }
}

// 0x00411800 + 0x004118b0: MakeDragList indicator & drop handling
static BOOL handle_drag_list_msg(HWND hdlg, DlgRunCtx *ctx, DRAGLISTINFO *dli)
{
    if (!ctx || !dli)
        return FALSE;

    switch (dli->uNotification)
    {
    case DL_BEGINDRAG:
    {
        RECT rc_lb;
        GetWindowRect(dli->hWnd, &rc_lb);
        ctx->lb_origin.x = rc_lb.left;
        ctx->lb_origin.y = rc_lb.top;
        ScreenToClient(hdlg, &ctx->lb_origin);
        ctx->drag_src_idx = LBItemFromPt(dli->hWnd, dli->ptCursor, TRUE);
        SetWindowLongPtrW(hdlg, DWLP_MSGRESULT, TRUE);
        return TRUE;
    }

    case DL_DRAGGING:
    {
        int item = LBItemFromPt(dli->hWnd, dli->ptCursor, TRUE);
        ctx->drag_dst_idx = item;
        if (ctx->drag_icon_hwnd)
        {
            if (item >= 0)
            {
                RECT rc_item;
                if (SendMessageW(dli->hWnd, LB_GETITEMRECT, (WPARAM)item, (LPARAM)&rc_item) != LB_ERR)
                {
                    int iy = ctx->lb_origin.y + rc_item.top - 6;
                    int ix = ctx->lb_origin.x - 10;
                    SetWindowPos(ctx->drag_icon_hwnd, HWND_TOP, ix, iy, 16, 16,
                                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
                }
            }
            else
            {
                ShowWindow(ctx->drag_icon_hwnd, SW_HIDE);
            }
        }
        if (item != -1)
        {
            SetCursor(LoadCursorA(NULL, IDC_ARROW));
            SetWindowLongPtrW(hdlg, DWLP_MSGRESULT, 0);
            return TRUE;
        }
        SetWindowLongPtrW(hdlg, DWLP_MSGRESULT, DL_STOPCURSOR);
        return TRUE;
    }

    case DL_DROPPED:
    {
        int item = LBItemFromPt(dli->hWnd, dli->ptCursor, TRUE);
        if (ctx->drag_icon_hwnd)
            ShowWindow(ctx->drag_icon_hwnd, SW_HIDE);
        if (item != -1 && ctx->drag_src_idx != -1 && item != ctx->drag_src_idx)
        {
            ctx->drag_dst_idx = item;
            PostMessageW(hdlg, WM_COMMAND, IDC_DLG_DRAG_COMMIT, 0);
        }
        SetWindowLongPtrW(hdlg, DWLP_MSGRESULT, 0);
        return TRUE;
    }

    case DL_CANCELDRAG:
        if (ctx->drag_icon_hwnd)
            ShowWindow(ctx->drag_icon_hwnd, SW_HIDE);
        SetWindowLongPtrW(hdlg, DWLP_MSGRESULT, 0);
        return TRUE;
    }
    return FALSE;
}

// 0x00411a10: generic AviUtl dialog procedure
static INT_PTR CALLBACK aviutl_generic_dlgproc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    DlgRunCtx *ctx = (DlgRunCtx *)GetWindowLongPtrW(hdlg, GWLP_USERDATA);

    if (ctx && ctx->drag_msg != 0 && msg == ctx->drag_msg && lp != 0)
    {
        return handle_drag_list_msg(hdlg, ctx, (DRAGLISTINFO *)lp);
    }

    if (msg == WM_INITDIALOG)
    {
        ctx = (DlgRunCtx *)lp;
        SetWindowLongPtrW(hdlg, GWLP_USERDATA, (LONG_PTR)ctx);
        if (!ctx || !ctx->st)
            return TRUE;

        if (ctx->owner && IsWindow(ctx->owner))
        {
            RECT rc_dlg, rc_own;
            int x, y;
            GetWindowRect(hdlg, &rc_dlg);
            GetWindowRect(ctx->owner, &rc_own);
            x = ((rc_dlg.left - rc_dlg.right) + rc_own.right + rc_own.left) / 2;
            y = ((rc_dlg.top - rc_dlg.bottom) + rc_own.bottom + rc_own.top) / 2;
            if (x < 0)
                x = 0;
            if (y < 0)
                y = 0;
            SetWindowPos(hdlg, NULL, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        if (ctx->st->caption && ctx->st->caption[0])
        {
            SetWindowTextW(hdlg, ctx->st->caption);
        }

        ctx->drag_icon_hwnd = GetDlgItem(hdlg, IDC_DLG_DRAG_ICON);
        if (ctx->drag_icon_hwnd)
        {
            HICON hDrag = (HICON)LoadImageA(GetModuleHandleA(NULL), "ICON_DRAG",
                                            IMAGE_ICON, 16, 16, 0);
            if (hDrag)
            {
                SendMessageW(ctx->drag_icon_hwnd, STM_SETICON, (WPARAM)hDrag, 0);
            }
            ShowWindow(ctx->drag_icon_hwnd, SW_HIDE);
        }

        sync_state_to_controls(hdlg, ctx);
        if (ctx->st->list_items[0] && ctx->st->list_order[0])
        {
            HWND hLb0 = GetDlgItem(hdlg, IDC_DLG_LST_BASE);
            if (hLb0)
            {
                MakeDragList(hLb0);
            }
        }

        if (ctx->st->on_cmd)
        {
            ctx->st->on_cmd(hdlg, AVIUTL_DLG_CMD_INIT, ctx->st, ctx->st->user);
        }
        return TRUE;
    }

    if (msg == WM_COMMAND && ctx && ctx->st)
    {
        int id = LOWORD(wp);
        int notify_code = HIWORD(wp);

        if (id == IDOK)
        {
            sync_controls_to_state(hdlg, ctx->st);
            if (ctx->st->on_cmd)
            {
                ctx->st->on_cmd(hdlg, AVIUTL_DLG_CMD_EXIT, ctx->st, ctx->st->user);
            }
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL)
        {
            if (ctx->st->on_cmd)
            {
                ctx->st->on_cmd(hdlg, AVIUTL_DLG_CMD_EXIT, ctx->st, ctx->st->user);
            }
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }

        // forward combo/listbox selection changes to custom hook; ignore edit focus noise
        if (notify_code != BN_CLICKED && id >= IDC_DLG_INT_BASE && id < IDC_DLG_LST_RESET)
        {
            BOOL is_cmb_sel = (id >= IDC_DLG_CMB_BASE && id < IDC_DLG_CMB_BASE + AVIUTL_DLG_SLOTS &&
                               notify_code == CBN_SELCHANGE);
            BOOL is_lst_sel = (id >= IDC_DLG_LST_BASE && id < IDC_DLG_LST_BASE + AVIUTL_DLG_SLOTS &&
                               notify_code == LBN_SELCHANGE);
            if (!is_cmb_sel && !is_lst_sel)
            {
                return FALSE;
            }
        }

        if (id == IDC_DLG_DRAG_COMMIT)
        {
            move_listbox_order(hdlg, ctx->st, 0, ctx->drag_src_idx, ctx->drag_dst_idx);
            if (ctx->st->on_cmd)
                ctx->st->on_cmd(hdlg, IDC_DLG_LST_BASE, ctx->st, ctx->st->user);
            return TRUE;
        }
        if (id == IDC_DLG_LST_UP && ctx->st->list_order[0])
        {
            int sel = (int)SendDlgItemMessageW(hdlg, IDC_DLG_LST_BASE, LB_GETCURSEL, 0, 0);
            if (sel > 0)
            {
                move_listbox_order(hdlg, ctx->st, 0, sel, sel - 1);
                if (ctx->st->on_cmd)
                    ctx->st->on_cmd(hdlg, IDC_DLG_LST_BASE, ctx->st, ctx->st->user);
            }
            return TRUE;
        }
        if (id == IDC_DLG_LST_DOWN && ctx->st->list_order[0])
        {
            int sel = (int)SendDlgItemMessageW(hdlg, IDC_DLG_LST_BASE, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel + 1 < ctx->st->list_count[0])
            {
                move_listbox_order(hdlg, ctx->st, 0, sel, sel + 1);
                if (ctx->st->on_cmd)
                    ctx->st->on_cmd(hdlg, IDC_DLG_LST_BASE, ctx->st, ctx->st->user);
            }
            return TRUE;
        }
        if (id == IDC_DLG_LST_RESET && ctx->st->list_order[0])
        {
            int k;
            for (k = 0; k < ctx->st->list_count[0]; k++)
                ctx->st->list_order[0][k] = k;
            refresh_listbox_slot(hdlg, ctx->st, 0, 0);
            if (ctx->st->on_cmd)
                ctx->st->on_cmd(hdlg, IDC_DLG_LST_BASE, ctx->st, ctx->st->user);
            return TRUE;
        }

        if (ctx->st->on_cmd)
        {
            sync_controls_to_state(hdlg, ctx->st);
            if (!ctx->st->on_cmd(hdlg, id, ctx->st, ctx->st->user))
            {
                EndDialog(hdlg, id);
            }
            else
            {
                sync_state_to_controls(hdlg, ctx);
            }
            return TRUE;
        }
    }

    return FALSE;
}

INT_PTR aviutl_dlg_run(HWND owner, const char *res_name, AviUtlDlgState *st)
{
    const DLGTEMPLATE *tpl;
    DlgRunCtx ctx;

    if (!res_name || !st)
        return -1;

    tpl = i18n_load_dialog_template(res_name);
    if (!tpl)
        return -1;

    ZeroMemory(&ctx, sizeof(ctx));
    ctx.owner = owner;
    ctx.st = st;
    ctx.drag_src_idx = -1;
    ctx.drag_dst_idx = -1;
    ctx.drag_msg = RegisterWindowMessageA(DRAGLISTMSGSTRING);

    return DialogBoxIndirectParamW(GetModuleHandleA(NULL), tpl, owner,
                                   aviutl_generic_dlgproc, (LPARAM)&ctx);
}
