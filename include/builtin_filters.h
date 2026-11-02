#pragma once

#ifndef BUILTIN_FILTERS_H
#define BUILTIN_FILTERS_H

#include <windows.h>
#include "filter.h"
#include "editp.h"

#define BUILTIN_FILTER_COUNT 27

// internal FILTER::flag bits used by aviutl.exe 1.10 (0x0042c930)
#define BUILTIN_FLAG_POPUP_SEP 0x00000080
#define BUILTIN_FLAG_POPUP_APPEND 0x00000100
#define BUILTIN_FLAG_NO_CFG_SUFFIX 0x00000200
#define BUILTIN_FLAG_INTERNAL_RES 0x02000000

// prepare all 27 built-in FILTER_DLL tables and resolve localized string labels
void builtin_filters_prepare_tables(HWND main_hwnd);

// access prepared built-in FILTER_DLL tables (0 .. BUILTIN_FILTER_COUNT - 1)
int builtin_filters_get_count(void);
FILTER_DLL *builtin_filters_get_dll(int builtin_idx);

// returns fixed DAT_00474c38 command slot (0..20), or -1 if assigned sequentially (21+)
int builtin_filters_get_fixed_cmd_slot(int builtin_idx);

// refresh dynamic menu-only tables ([サイズの変更] resizelist & [色変換の設定] .auc list)
void builtin_filters_reload_dynamic_lists(void);

// audio position delay (in frames) when [音声の位置調整] is active
int builtin_filters_get_audio_delay_frames(void);

// framerate decimation ([フレームレートの変更] & [フレームレートの設定])
int builtin_filters_get_framerate_mode(BOOL *out_use_auto24);
BOOL builtin_filters_is_framerate_kept_frame(int rel_frame);
void builtin_filters_adjust_export_fps(int *inout_rate, int *inout_scale);

// deinterlace & field order queries (including per-frame override hook)
int builtin_filters_get_deinterlace_mode(void);
void builtin_filters_set_deinterlace_mode(int mode);
BOOL builtin_filters_is_top_field_first(void);
void builtin_filters_set_frame_interlace_override(int ovr_mode);

// tool window & playback window integration hooks
void builtin_filters_refresh_tool_window(void);
void builtin_filters_notify_menu_rebuilt(HWND main_hwnd);
BOOL builtin_filters_toggle_playback_window_play(HWND main_hwnd, EDITP *ep);
void builtin_filters_stop_playback(void);

#endif
