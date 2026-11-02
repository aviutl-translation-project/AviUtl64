#pragma once

#ifndef AVIUTL_DLG_H
#define AVIUTL_DLG_H

#include <windows.h>

#define AVIUTL_DLG_SLOTS 26

#define AVIUTL_DLG_CMD_INIT 0x7FC1
#define AVIUTL_DLG_CMD_EXIT 0x7FC2

typedef struct AviUtlDlgState AviUtlDlgState;

// return TRUE to keep dialog open, FALSE to EndDialog(hdlg, cmd_id)
typedef BOOL (*AviUtlDlgCmdFn)(HWND hdlg, int cmd_id, AviUtlDlgState *st, void *user);

struct AviUtlDlgState
{
    const wchar_t *caption;
    int ints[AVIUTL_DLG_SLOTS];
    wchar_t strs[AVIUTL_DLG_SLOTS][260];
    int checks[AVIUTL_DLG_SLOTS];
    const wchar_t *const *combo_items[AVIUTL_DLG_SLOTS];
    int combo_sel[AVIUTL_DLG_SLOTS];
    wchar_t combo_text[AVIUTL_DLG_SLOTS][260];
    const wchar_t *const *list_items[AVIUTL_DLG_SLOTS];
    int *list_order[AVIUTL_DLG_SLOTS];
    int list_count[AVIUTL_DLG_SLOTS];
    AviUtlDlgCmdFn on_cmd;
    void *user;
};

void aviutl_dlg_init(AviUtlDlgState *st);
void aviutl_dlg_set_str_sjis(AviUtlDlgState *st, int slot, const char *src);
void aviutl_dlg_get_str_sjis(const AviUtlDlgState *st, int slot, char *dst, int dst_cap);

INT_PTR aviutl_dlg_run(HWND owner, const char *res_name, AviUtlDlgState *st);

int aviutl_msgbox_w(HWND owner, const wchar_t *text, const wchar_t *caption, UINT type);
int aviutl_msgbox_sjis(HWND owner, const char *text, const char *caption, UINT type);

#endif

