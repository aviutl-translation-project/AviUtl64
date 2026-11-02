#pragma once

#ifndef KEYCFG_H
#define KEYCFG_H

#include <windows.h>

// initialize default shortcuts (built-in 1.10 + plugin add_menu_item) and load aviutl.key
void keycfg_init_and_load(HWND main_hwnd);

// open the "KEYCFGLIST" shortcut key configuration dialog (menu ID 102)
void keycfg_run_dialog(HWND main_hwnd);

// translate WM_KEYDOWN / WM_SYSKEYDOWN via active shortcut table (0x004335f0)
// returns TRUE if a matching shortcut posted WM_COMMAND to main_hwnd
BOOL keycfg_translate_keydown(HWND main_hwnd, UINT msg, UINT vk);

// explicit binary file save/load helpers (returns serialized byte count; 0 means matches defaults)
int keycfg_serialize(HWND main_hwnd, unsigned char *out_buf, int out_cap);
BOOL keycfg_load_from_memory(HWND main_hwnd, const unsigned char *buf, int len);

#endif
