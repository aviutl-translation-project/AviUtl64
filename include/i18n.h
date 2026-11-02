#pragma once

#ifndef I18N_H
#define I18N_H

#include <windows.h>

void i18n_init(HINSTANCE hInst);
void i18n_shutdown(void);

LANGID i18n_get_active_langid(void);
BOOL i18n_is_japanese(void);

const wchar_t *i18n_get_wstr(UINT id);
const char *i18n_get_str(UINT id);

HMENU i18n_load_menu(const char *res_name);
const DLGTEMPLATE *i18n_load_dialog_template(const char *res_name);

BOOL i18n_show_lang_dialog(HWND owner);
void i18n_show_lang_info(HWND owner);

#endif
