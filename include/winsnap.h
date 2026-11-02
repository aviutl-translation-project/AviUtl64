#pragma once

#ifndef WINSNAP_H
#define WINSNAP_H

#include <windows.h>

void winsnap_set_enabled(BOOL enabled);
BOOL winsnap_is_enabled(void);

// handle WM_ENTERSIZEMOVE, WM_EXITSIZEMOVE, WM_MOVING, WM_MOVE, WM_SIZE (0x00404cf0..0x00405450)
// returns TRUE if handled and caller should return 0 (or TRUE for WM_MOVING)
BOOL winsnap_handle_message(HWND hwnd, HWND main_hwnd, UINT msg, WPARAM wp, LPARAM lp);

#endif
