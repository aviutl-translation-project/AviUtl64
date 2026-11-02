#pragma once

#ifndef PLUGIN_MANAGER_H
#define PLUGIN_MANAGER_H

#include <windows.h>
#include "filter.h"
#include "editp.h"

#define WM_REAVIUTL_PLUGIN_REDRAW (WM_APP + 0x40)
#define WM_REAVIUTL_BRIDGE_NOTIFY (WM_APP + 0x50)

#define PLUGIN_CMD_WINDOW_BASE 30000
#define PLUGIN_CMD_MENU_ITEM_BASE 31000

typedef struct PluginMenuItem
{
    int filter_index;
    char name[128];
    HWND hwnd;
    int cmd_id;
    int def_key;
    int flag;
} PluginMenuItem;

BOOL plugin_manager_init_ex(const char *plugin_dir, HWND main_hwnd, EDITP *editp);
BOOL plugin_manager_init(const char *plugin_dir);
BOOL plugin_manager_load_single_auf(const char *auf_path, HWND main_hwnd, EDITP *editp);
void plugin_manager_shutdown(void);

int plugin_manager_get_count(void);
const char *plugin_manager_get_name(int index);
int plugin_manager_get_flags(int index);
FILTER *plugin_manager_get_filter(int index);
HWND plugin_manager_get_window(int index);
int plugin_manager_find_by_name(const char *name);
BOOL plugin_manager_is_bridged(void);

int plugin_manager_get_menu_item_count(void);
BOOL plugin_manager_get_menu_item(int index, PluginMenuItem *out_item);
BOOL plugin_manager_exec_menu_item_index(int menu_index);

HMENU plugin_manager_build_menu(void);

BOOL plugin_manager_show_window(int index, BOOL show);
BOOL plugin_manager_toggle_window(int index);

BOOL plugin_manager_handle_command(HWND hwnd, int cmd_id);
BOOL plugin_manager_handle_key(HWND hwnd, UINT vk, BOOL is_down, LPARAM key_lparam);

BOOL plugin_manager_forward_main_mouse(UINT filter_mouse_msg, WPARAM mk_flags,
                                       int img_x, int img_y);
BOOL plugin_manager_forward_main_misc(UINT filter_msg, WPARAM wp, LPARAM lp);

void plugin_manager_modify_title(EDITP *ep, int frame, char *title, int max_title);

int plugin_manager_render_audio_frame(EDITP *ep, int frame_num, short *buf, BOOL nofilter);
int plugin_manager_apply_audio_filters(EDITP *ep, int frame_num, short *buf,
                                       int samples, const FILTER *stop_before_fp);

BOOL plugin_manager_send_filter_command(int filter_index, int filter_cmd_id);
BOOL plugin_manager_import_exo(int filter_index, const char *exo_path, EDITP *ep);
BOOL plugin_manager_load_aup(const char *aup_path, EDITP *ep);
BOOL plugin_manager_save_aup(const char *aup_path, EDITP *ep);

void plugin_manager_notify_file_open(EDITP *ep);
void plugin_manager_notify_file_close(EDITP *ep);
void plugin_manager_notify_update(EDITP *ep);
void plugin_manager_notify_save_start(EDITP *ep, int s, int e, int out_flags);
void plugin_manager_notify_save_end(EDITP *ep, int out_flags);
BOOL plugin_manager_has_active_audio_filter(void);

BOOL plugin_manager_sync_from_bridge(EDITP *ep);

BOOL plugin_manager_render_video_frame(EDITP *ep, int frame_num);
BOOL plugin_manager_apply_filter(int filter_id, void *editp);

void plugin_manager_debug_dump(void);

#endif
