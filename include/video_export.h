#pragma once

#ifndef VIDEO_EXPORT_H
#define VIDEO_EXPORT_H

#include <windows.h>
#include "output.h"
#include "editp.h"

#define AUO_MENU_PLACEHOLDER_ID 5395 // 0x1513
#define AUO_CMD_BASE 5296            // 0x14b0
#define AUO_MAX_PLUGINS 32

typedef struct AuoPluginSummary
{
    char name[128];
    char filefilter[512];
    char information[256];
    int flag;
    BOOL has_config;
    BOOL bridged_x86;
} AuoPluginSummary;

typedef void (*VideoExportPreviewUpdateFn)(HWND owner);
typedef void (*VideoExportProgressFn)(HWND owner, int now, int total);

void video_export_set_ui_callbacks(VideoExportPreviewUpdateFn on_preview_update,
                                   VideoExportProgressFn on_progress);

void video_export_set_bridge_telemetry(volatile LONG *abort_flag,
                                       volatile LONG *now_val,
                                       volatile LONG *total_val);

int video_export_scan_auo(const char *plugin_dir, HWND main_hwnd, EDITP *ep);
BOOL video_export_load_single_auo(const char *auo_path, HWND main_hwnd, EDITP *ep);
void video_export_shutdown(void);

int video_export_get_auo_count(void);
BOOL video_export_get_auo_summary(int idx, AuoPluginSummary *out_summary);
const char *video_export_get_auo_name(int idx);
int video_export_find_auo_by_name(const char *name);

void video_export_populate_menu(HMENU bar, BOOL is_ja);

BOOL video_export_avi_dialog(HWND owner, EDITP *ep);
BOOL video_export_avi_file(HWND owner, EDITP *ep, const char *save_path, int out_flags);

BOOL video_export_wav_dialog(HWND owner, EDITP *ep);
BOOL video_export_wav_file(HWND owner, EDITP *ep, const char *save_path);

BOOL video_export_auo_dialog(HWND owner, EDITP *ep, int auo_idx);
BOOL video_export_auo_file(HWND owner, EDITP *ep, int auo_idx,
                           const char *save_path, int out_flags);

BOOL video_export_choose_video_codec(HWND owner, EDITP *ep);
BOOL video_export_choose_audio_codec(HWND owner, EDITP *ep);
BOOL video_export_auo_config_dialog(HWND owner, int auo_idx);
void video_export_show_auo_info(HWND owner, BOOL is_ja);

void video_export_request_abort(void);
BOOL video_export_is_encoding(void);

#endif
