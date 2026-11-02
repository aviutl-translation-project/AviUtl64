// input plugin (.aui) manager and priority registry

#pragma once

#ifndef INPUT_PLUGIN_H
#define INPUT_PLUGIN_H

#include <windows.h>
#include "aviutl.h"
#include "input.h"

#define INPUT_PLUGIN_MAX 32
#define INPUT_PLUGIN_CMD_CFG_BASE 5496

#define INPUT_PLUGIN_FLAG_BUILTIN 0x00100000
#define INPUT_PLUGIN_FLAG_HIDE_LIST 0x00400000

typedef enum BuiltinInputKind
{
    BUILTIN_INPUT_NONE = 0,
    BUILTIN_INPUT_AVI = 1,
    BUILTIN_INPUT_WAV = 2,
    BUILTIN_INPUT_BMP = 3,
    BUILTIN_INPUT_JPG_PNG = 4,
    BUILTIN_INPUT_VFW = 5,
    BUILTIN_INPUT_AVIS = 6
} BuiltinInputKind;

typedef struct InputPluginSummary
{
    char name[128];
    char filefilter[1024];
    int filefilter_len;
    char information[256];
    char path[MAX_PATH];
    int flag;
    int builtin_kind;
    BOOL has_config;
    int priority;
    BOOL bridged_x86;
    int bridge_aui_idx;
} InputPluginSummary;

typedef struct InputPluginInstance InputPluginInstance;
typedef BOOL (*InputWavConfigDialogFn)(HWND owner, int *inout_w, int *inout_h);

void input_plugin_init(const char *plugin_dir, HWND main_hwnd);
void input_plugin_shutdown(void);

int input_plugin_get_count(void);
BOOL input_plugin_get_summary(int idx, InputPluginSummary *out_sum);

int input_plugin_get_ext_count(void);
BOOL input_plugin_get_ext_summary(int ext_idx, InputPluginSummary *out_sum);
int input_plugin_ext_to_index(int ext_idx);

int input_plugin_get_priority_order(int *out_indices, int max_count, BOOL include_hidden);
void input_plugin_apply_priority_order(const int *ordered_indices, int count, BOOL save_ini);
void input_plugin_set_single_priority(int idx, int priority);

void input_plugin_set_wav_config_handler(InputWavConfigDialogFn fn);
void input_plugin_get_wav_video_size(int *out_w, int *out_h);
void input_plugin_set_wav_video_size(int w, int h, BOOL save_ini);

BOOL input_plugin_config(int idx, HWND owner);
LPSTR input_plugin_get_file_filter(int type);

InputPluginInstance *input_plugin_open_native(int idx, const char *filepath, INPUT_INFO *out_info);
void input_plugin_close_native(InputPluginInstance *inst);
int input_plugin_read_video_native(InputPluginInstance *inst, int frame, void *buf);
int input_plugin_read_audio_native(InputPluginInstance *inst, int start, int length, void *buf);
BOOL input_plugin_is_keyframe_native(InputPluginInstance *inst, int frame);

#endif

