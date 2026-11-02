// 32-bit plugin ipc bridge protocol and client api

#pragma once

#ifndef PLUGIN_BRIDGE_H
#define PLUGIN_BRIDGE_H

#include <windows.h>
#include <stdint.h>
#include "aviutl.h"
#include "editp.h"

#define BRIDGE_MAGIC 0x52415542u // "RAUB"
#define BRIDGE_VERSION 6u
#define BRIDGE_MAX_FILTERS 64
#define BRIDGE_MAX_MENU_ITEMS 256
#define BRIDGE_MAX_AUOS 32
#define BRIDGE_MAX_AUIS 32
#define BRIDGE_MAX_AUCS 32
#define BRIDGE_MAX_OPEN_AUIS 32
#define BRIDGE_MAX_AUDIO_SAMPLES 16384

enum BridgeCmd
{
    BRIDGE_CMD_PING = 1,
    BRIDGE_CMD_LOAD_DIR = 2,
    BRIDGE_CMD_LOAD_FILE = 3,
    BRIDGE_CMD_OPEN_VIDEO = 4,
    BRIDGE_CMD_CLOSE_VIDEO = 5,
    BRIDGE_CMD_SYNC_STATE = 6,
    BRIDGE_CMD_SHOW_WINDOW = 7,
    BRIDGE_CMD_EXEC_MENU_ITEM = 8,
    BRIDGE_CMD_FILTER_COMMAND = 9,
    BRIDGE_CMD_IMPORT_EXO = 10,
    BRIDGE_CMD_MAIN_MOUSE = 11,
    BRIDGE_CMD_MAIN_KEY = 12,
    BRIDGE_CMD_RENDER_FRAME = 13,
    BRIDGE_CMD_LOAD_AUP = 14,
    BRIDGE_CMD_SAVE_AUP = 15,
    BRIDGE_CMD_DEBUG_CMD = 16,
    BRIDGE_CMD_RENDER_AUDIO = 17,
    BRIDGE_CMD_OPEN_AUDIO = 18,
    BRIDGE_CMD_SAVE_START = 19,
    BRIDGE_CMD_SAVE_END = 20,
    BRIDGE_CMD_AUO_OUTPUT = 21,
    BRIDGE_CMD_AUO_CONFIG = 22,
    BRIDGE_CMD_SCAN_AUO = 23,
    BRIDGE_CMD_LOAD_AUO = 24,
    BRIDGE_CMD_SET_ACTIVE = 25,
    BRIDGE_CMD_FILTER_IO = 26,
    BRIDGE_CMD_SCAN_INPUT_COLOR = 27,
    BRIDGE_CMD_AUI_CONFIG = 28,
    BRIDGE_CMD_AUI_SET_PRIORITY = 29,
    BRIDGE_CMD_AUI_OPEN = 30,
    BRIDGE_CMD_AUI_CLOSE = 31,
    BRIDGE_CMD_AUI_READ_VIDEO = 32,
    BRIDGE_CMD_AUI_READ_AUDIO = 33,
    BRIDGE_CMD_AUI_IS_KEYFRAME = 34,
    BRIDGE_CMD_SHUTDOWN = 99
};

#pragma pack(push, 4)

typedef struct BridgeFilterInfo
{
    char name[128];
    char information[256];
    char path[260];
    int32_t flag;
    int32_t has_window;
    int32_t window_visible;
    uint32_t hwnd32;
} BridgeFilterInfo;

typedef struct BridgeMenuItemInfo
{
    int32_t filter_index;
    char name[128];
    int32_t cmd_id;
    int32_t def_key;
    int32_t flag;
} BridgeMenuItemInfo;

typedef struct BridgeAuoInfo
{
    char name[128];
    char filefilter[512];
    char information[256];
    char path[260];
    int32_t flag;
    int32_t has_config;
} BridgeAuoInfo;

typedef struct BridgeAuiInfo
{
    char name[128];
    char filefilter[1024];
    int32_t filefilter_len;
    char information[256];
    char path[260];
    int32_t flag;
    int32_t has_config;
    int32_t priority;
} BridgeAuiInfo;

typedef struct BridgeAucInfo
{
    char name[128];
    char information[256];
    char path[260];
    int32_t flag;
} BridgeAucInfo;

typedef struct BridgeAuiOpenInfo
{
    int32_t has_video;
    int32_t has_audio;
    int32_t w;
    int32_t h;
    int32_t frame_n;
    int32_t video_rate;
    int32_t video_scale;
    int32_t decode_bpp;
    int32_t audio_rate;
    int32_t audio_ch;
    int32_t audio_n;
} BridgeAuiOpenInfo;

typedef struct BridgeSharedMem
{
    uint32_t magic;
    uint32_t version;
    volatile LONG state_seq;

    int32_t is_editing;
    int32_t w;
    int32_t h;
    int32_t max_w;
    int32_t max_h;
    int32_t frame;
    int32_t frame_n;
    int32_t fps_rate;
    int32_t fps_scale;
    int32_t audio_rate;
    int32_t audio_ch;
    int32_t has_audio;
    int32_t select_s;
    int32_t select_e;
    int32_t disp_w;
    int32_t disp_h;
    char edit_path[260];
    char project_path[260];
    char output_path[260];
    uint8_t frame_status[REAVIUTL_MAX_FRAME];

    int32_t filter_count;
    BridgeFilterInfo filters[BRIDGE_MAX_FILTERS];
    int32_t menu_item_count;
    BridgeMenuItemInfo menu_items[BRIDGE_MAX_MENU_ITEMS];

    int32_t auo_count;
    BridgeAuoInfo auos[BRIDGE_MAX_AUOS];
    volatile LONG encode_abort;
    volatile LONG encode_now;
    volatile LONG encode_total;

    int32_t aui_count;
    BridgeAuiInfo auis[BRIDGE_MAX_AUIS];
    int32_t auc_count;
    BridgeAucInfo aucs[BRIDGE_MAX_AUCS];
    BridgeAuiOpenInfo aui_open_info;

    PIXEL_YC ycp_frame[REAVIUTL_VRAM_W * REAVIUTL_VRAM_H];
    int16_t pcm_audio[BRIDGE_MAX_AUDIO_SAMPLES * 2];
} BridgeSharedMem;

typedef struct BridgeRpcReq
{
    uint32_t cmd;
    int32_t arg0;
    int32_t arg1;
    int32_t arg2;
    int32_t arg3;
    int32_t arg4;
    int32_t arg5;
    char str[260];
} BridgeRpcReq;

typedef struct BridgeRpcResp
{
    int32_t ok;
    int32_t val0;
    int32_t val1;
    char err[256];
} BridgeRpcResp;

#pragma pack(pop)

BOOL plugin_bridge_is_active(void);
BOOL plugin_bridge_load_dir(const char *plugin_dir, HWND main_hwnd, EDITP *ep);
BOOL plugin_bridge_load_file(const char *auf_path, HWND main_hwnd, EDITP *ep);
void plugin_bridge_shutdown(void);

int plugin_bridge_get_filter_count(void);
const char *plugin_bridge_get_filter_name(int idx);
const char *plugin_bridge_get_filter_info(int idx);
const char *plugin_bridge_get_filter_path(int idx);
int plugin_bridge_get_filter_flags(int idx);
HWND plugin_bridge_get_filter_hwnd(int idx);

int plugin_bridge_get_menu_item_count(void);
BOOL plugin_bridge_get_menu_item(int idx, BridgeMenuItemInfo *out_info);

BOOL plugin_bridge_show_window(int idx, int mode);
BOOL plugin_bridge_set_filter_active(int idx, int mode);
BOOL plugin_bridge_filter_io(int filter_idx, BOOL is_export, EDITP *ep);
BOOL plugin_bridge_exec_menu_item(int menu_idx);
BOOL plugin_bridge_send_filter_command(int filter_idx, int filter_cmd_id);
BOOL plugin_bridge_import_exo(int filter_idx, const char *exo_path, EDITP *ep);
BOOL plugin_bridge_forward_mouse(UINT msg, WPARAM mk_flags, int img_x, int img_y);
BOOL plugin_bridge_forward_key(UINT vk, BOOL is_down, LPARAM key_lparam);

BOOL plugin_bridge_open_video(const char *filepath, EDITP *ep);
BOOL plugin_bridge_open_audio(const char *filepath, EDITP *ep);
BOOL plugin_bridge_close_video(EDITP *ep);
BOOL plugin_bridge_load_aup(const char *aup_path, EDITP *ep);
BOOL plugin_bridge_save_aup(const char *aup_path, EDITP *ep);
BOOL plugin_bridge_sync_to_bridge(EDITP *ep);
BOOL plugin_bridge_sync_from_bridge(EDITP *ep);
BOOL plugin_bridge_render_frame(EDITP *ep, int frame_num, BOOL push_host_frame);
int plugin_bridge_render_audio(EDITP *ep, int frame_num, short *buf, int samples, int ch);
BOOL plugin_bridge_debug_cmd(const char *cmd_str);

typedef void (*BridgeEncodeProgressFn)(int now, int total, void *user);

BOOL plugin_bridge_notify_save_start(EDITP *ep, int s, int e, int out_flags);
BOOL plugin_bridge_notify_save_end(EDITP *ep, int out_flags);
BOOL plugin_bridge_scan_auo(const char *plugin_dir, HWND main_hwnd, EDITP *ep);
BOOL plugin_bridge_load_auo(const char *auo_path, HWND main_hwnd, EDITP *ep);
int plugin_bridge_get_auo_count(void);
BOOL plugin_bridge_get_auo_info(int idx, BridgeAuoInfo *out_info);
BOOL plugin_bridge_auo_output(int auo_idx, const char *save_path, int out_flags,
                              EDITP *ep, HWND owner,
                              BridgeEncodeProgressFn on_progress, void *user);
BOOL plugin_bridge_auo_config(int auo_idx, HWND owner);
void plugin_bridge_request_encode_abort(void);

BOOL plugin_bridge_scan_input_color(const char *plugin_dir, HWND main_hwnd);
int plugin_bridge_get_aui_count(void);
BOOL plugin_bridge_get_aui_info(int idx, BridgeAuiInfo *out_info);
BOOL plugin_bridge_aui_config(int idx, HWND owner);
BOOL plugin_bridge_aui_set_priority(int idx, int priority);
int plugin_bridge_aui_open(int idx, const char *filepath, int open_flag, BridgeAuiOpenInfo *out_info);
BOOL plugin_bridge_aui_close(int handle_slot);
BOOL plugin_bridge_aui_read_video(int handle_slot, int frame_num, PIXEL_YC *ycp, int yc_stride, int w, int h);
int plugin_bridge_aui_read_audio(int handle_slot, int sample_num, short *buf, int samples, int req_rate, int req_ch);
BOOL plugin_bridge_aui_is_keyframe(int handle_slot, int frame_num);

int plugin_bridge_get_auc_count(void);
BOOL plugin_bridge_get_auc_info(int idx, BridgeAucInfo *out_info);

#endif
