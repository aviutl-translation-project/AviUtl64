#pragma once

#ifndef DARKEN_THEME_H
#define DARKEN_THEME_H

#include <windows.h>

void darken_theme_on_aul_detected(const char *aul_path);
void darken_theme_shutdown(void);

BOOL darken_theme_is_active(void);
void darken_theme_hook_module(HMODULE mod);
void darken_theme_apply_window(HWND hwnd);
HICON darken_theme_recolor_icon(HICON icon, const char *icon_name);

#endif
