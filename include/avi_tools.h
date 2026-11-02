#pragma once

#ifndef AVI_TOOLS_H
#define AVI_TOOLS_H

#include <windows.h>
#include "editp.h"
#include "video_export.h"

#define BATCH_MAX_ITEMS 64
#define AVICHAIN_MAX_FILES 256

typedef void (*AviToolsUiRefreshFn)(HWND owner);

BOOL avi_tools_batch_register_current(HWND owner, EDITP *ep,
                                      const char *out_path, int out_flags,
                                      const AviSaveOptions *opts);
BOOL avi_tools_batch_register_ex(HWND owner, EDITP *ep,
                                 const char *out_path, const char *auo_name,
                                 int out_flags, const AviSaveOptions *opts);

void avi_tools_run_batch_dialog(HWND owner, EDITP *ep, AviToolsUiRefreshFn on_refresh);
void avi_tools_run_batch_dialog_ex(HWND owner, EDITP *ep, AviToolsUiRefreshFn on_refresh,
                                   BOOL auto_start, BOOL auto_close);

BOOL avi_tools_run_avichain_dialog(HWND owner);
BOOL avi_tools_run_avimix_dialog(HWND owner);

#endif
