#pragma once

#ifndef EDITP_H
#define EDITP_H

#include <stdint.h>
#include "filter.h"

#define REAVIUTL_VRAM_W 1920
#define REAVIUTL_VRAM_H 1080
#define REAVIUTL_MAX_FRAME 320000
#define REAVIUTL_BUILD 11003 // 0x2afb

typedef struct EDITP_T EDITP;
typedef struct EDITP_T EditpHandle;
typedef struct VFWInput VFWInput;

EDITP *editp_create_idle(HWND main_hwnd);
EDITP *editp_create(int w, int h, int frame_n, int fps_scale, int fps_rate);
EDITP *editp_create_from_video(const char *filepath);
BOOL editp_open_video(EDITP *ep, const char *filepath);
BOOL editp_open_audio(EDITP *ep, const char *filepath);
void editp_close_video(EDITP *ep);
void editp_destroy(EDITP *ep);

void editp_set_main_hwnd(EDITP *ep, HWND hwnd);
HWND editp_get_main_hwnd(EDITP *ep);

BOOL editp_load_frame(EDITP *ep, int frame_num);
BOOL editp_read_source_frame(EDITP *ep, int frame_num, PIXEL_YC *dst, int dst_stride);

BOOL editp_is_editing(EDITP *ep);
BOOL editp_is_saving(EDITP *ep);
void editp_set_saving(EDITP *ep, BOOL saving);

PIXEL_YC *editp_get_ycp_edit(EDITP *ep);
PIXEL_YC *editp_get_ycp_temp(EDITP *ep);
void editp_swap_ycp(EDITP *ep, PIXEL_YC *new_edit, PIXEL_YC *new_temp);

void editp_get_dimensions(EDITP *ep, int *w, int *h, int *max_w, int *max_h);
int editp_get_frame(EDITP *ep);
int editp_set_frame(EDITP *ep, int n);
int editp_get_frame_n(EDITP *ep);
int editp_set_frame_n(EDITP *ep, int n);
BOOL editp_get_select_frame(EDITP *ep, int *s, int *e);
BOOL editp_set_select_frame(EDITP *ep, int s, int e);

BOOL editp_get_frame_status(EDITP *ep, int n, BYTE *fps);
BOOL editp_set_frame_status(EDITP *ep, int n, BYTE fps);
BYTE *editp_get_frame_status_table(EDITP *ep, int type);

int editp_read_audio_frame(EDITP *ep, int frame_num, short *buf);
int editp_get_audio_rate(EDITP *ep);
int editp_get_audio_ch(EDITP *ep);

void editp_set_registered_filter_count(int count);
int editp_get_registered_filter_count(void);
HFONT editp_get_ui_font(void);
void editp_get_sys_info(EDITP *ep, SYS_INFO *si);
BOOL editp_get_file_info(EDITP *ep, FILE_INFO *fi);

void editp_set_project_path(EDITP *ep, const char *path);
const char *editp_get_project_path(EDITP *ep);
void editp_set_output_path(EDITP *ep, const char *path);
const char *editp_get_output_path(EDITP *ep);
const char *editp_get_edit_path(EDITP *ep);
void editp_set_dimensions(EDITP *ep, int w, int h);
void editp_set_display_size(EDITP *ep, int disp_w, int disp_h);
void editp_get_display_size(EDITP *ep, int *disp_w, int *disp_h);
void editp_get_fps(EDITP *ep, int *rate, int *scale);
void editp_set_fps(EDITP *ep, int rate, int scale);
void editp_set_audio_format(EDITP *ep, int audio_rate, int audio_ch, BOOL has_audio);
uint32_t editp_get_trackbar_hwnd32(EDITP *ep);
void editp_set_trackbar_hwnd(EDITP *ep, HWND hwnd_tb);
void editp_sync_preview_state(EDITP *ep, uint32_t flags,
                              int win_x, int win_y, int client_w, int client_h, int zoom,
                              HWND btn_back, HWND btn_fwd, HWND btn_in, HWND btn_out, HWND btn_play,
                              int dst_x, int dst_y, int dst_w, int dst_h,
                              int src_vis_w, int src_vis_h);
BOOL editp_seek_frame_ex(EDITP *ep, int target_frame, BOOL shift_held, BOOL shift_select_cfg);
BOOL editp_has_audio(EDITP *ep);
BOOL editp_is_source_keyframe(EDITP *ep, int frame_num);

#endif
