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
    char path[MAX_PATH];
    int flag;
    BOOL has_config;
    BOOL bridged_x86;
} AuoPluginSummary;

typedef struct AviExInfoTags
{
    char title[260];
    char comment[260];
    char copyright[260];
} AviExInfoTags;

typedef struct AviSaveOptions
{
    BOOL no_recompress_video;
    BOOL no_recompress_audio;
    BOOL no_audio;
    int interleave_frames;
    BOOL output_wav_sidecar;
    BOOL output_log;
    AviExInfoTags exinfo;
} AviSaveOptions;

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

void video_export_populate_menu(HMENU bar, const wchar_t *none_label);

BOOL video_export_avi_dialog(HWND owner, EDITP *ep);
BOOL video_export_avi_file(HWND owner, EDITP *ep, const char *save_path, int out_flags);
BOOL video_export_avi_file_with_options(HWND owner, EDITP *ep, const char *save_path,
                                        int out_flags, const AviSaveOptions *opts);
void video_export_get_default_avi_options(AviSaveOptions *out_opts);
BOOL video_export_run_exinfo_dialog(HWND owner, AviExInfoTags *tags);
BOOL video_export_write_riff_info_tags(const char *avi_path, const AviExInfoTags *tags);
BOOL video_export_run_codec_settings_dialog(HWND owner);

BOOL video_export_wav_dialog(HWND owner, EDITP *ep);
BOOL video_export_wav_file(HWND owner, EDITP *ep, const char *save_path);

BOOL video_export_auo_dialog(HWND owner, EDITP *ep, int auo_idx);
BOOL video_export_auo_file(HWND owner, EDITP *ep, int auo_idx,
                           const char *save_path, int out_flags);

BOOL video_export_choose_video_codec(HWND owner, EDITP *ep);
BOOL video_export_choose_audio_codec(HWND owner, EDITP *ep);
int video_export_get_last_auo_index(void);
void video_export_set_last_auo_index(int auo_idx);
BOOL video_export_auo_config_dialog(HWND owner, int auo_idx);

void video_export_request_abort(void);
BOOL video_export_is_encoding(void);

#endif
