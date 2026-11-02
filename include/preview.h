#pragma once

#ifndef PREVIEW_H
#define PREVIEW_H

#include <windows.h>
#include <stdint.h>
#include "editp.h"

#define PREVIEW_BOTTOM_BAR_H 32
#define PREVIEW_AUDIO_WAVE_H 64

// main window child control ids (0x00460960)
#define IDC_AVIUTL_TRACKBAR 0x2328
#define IDC_AVIUTL_BTN_BACK 0x2329
#define IDC_AVIUTL_BTN_FORWARD 0x232A
#define IDC_AVIUTL_BTN_OUT 0x232B
#define IDC_AVIUTL_BTN_IN 0x232C
#define IDC_AVIUTL_BTN_PLAY 0x232D

// preview flags at editp + 0x36c (0x004614a0)
#define PREVIEW_FLAG_EDITING 0x00000001u
#define PREVIEW_FLAG_AT_SELECT_S 0x00000002u
#define PREVIEW_FLAG_AT_SELECT_E 0x00000004u
#define PREVIEW_FLAG_AUDIO 0x00000008u
#define PREVIEW_FLAG_PREVIEW 0x00000010u
#define PREVIEW_FLAG_DELETEFRAME 0x00000020u
#define PREVIEW_FLAG_AT_DELFRAME 0x00000040u
#define PREVIEW_FLAG_OVERLAY 0x00000200u
#define PREVIEW_FLAG_ALWAYSTOP 0x00000800u
#define PREVIEW_FLAG_AVIKEYFRAME 0x00001000u
#define PREVIEW_FLAG_RECOMPRESSFRAME 0x00002000u
#define PREVIEW_FLAG_TIME 0x00004000u
#define PREVIEW_FLAG_NOFILTER 0x00008000u
#define PREVIEW_FLAG_DATARATE 0x00010000u
#define PREVIEW_FLAG_SIZEAUTO 0x00080000u

enum PreviewZoom
{
    PREVIEW_ZOOM_25 = 0,
    PREVIEW_ZOOM_50 = 1,
    PREVIEW_ZOOM_100 = 2,
    PREVIEW_ZOOM_200 = 3,
    PREVIEW_ZOOM_300 = 4,
    PREVIEW_ZOOM_400 = 5,
    PREVIEW_ZOOM_WINDOW = 6
};

typedef struct AviUtlSystemConfig
{
    int moveA;
    int moveB;
    int moveC;
    int moveD;
    int startframe;
    int shiftselect;
    int dragdropactive;
    int movieplaymain;
    int trackbarclick;
    int windowsnap;
    int overwritecheck;
    int dragdropdialog;
    int openprojectaup;
    int closedialog;
} AviUtlSystemConfig;

BOOL preview_register_classes(HINSTANCE hInst);
void preview_load_ini(int *out_x, int *out_y, int *out_w, int *out_h, BOOL *out_maximize);
void preview_save_ini(HWND main_hwnd);
void preview_reload_sys_config(HWND main_hwnd);
BOOL preview_init_controls(HWND main_hwnd, HINSTANCE hInst);

HWND preview_get_trackbar_hwnd(void);
const AviUtlSystemConfig *preview_get_sys_config(void);
uint32_t preview_get_flags(void);
int preview_get_zoom(void);
void preview_set_peek_unfiltered(BOOL peek);
BOOL preview_is_effectively_nofilter(EDITP *ep);

void preview_set_playing_state(BOOL playing);
void preview_on_size(HWND main_hwnd, EDITP *ep);
void preview_sync_menu_checks(HWND main_hwnd);
BOOL preview_on_command(HWND main_hwnd, EDITP *ep, int cmd_id, BOOL *out_need_rerender);
void preview_auto_resize(HWND main_hwnd, EDITP *ep, BOOL force);
void preview_update_trackbar(EDITP *ep);

void preview_set_frame_ex(EDITP *ep, int w, int h, void *bgr_bits,
                          const short *audio_pcm, int total_short_samples);
void preview_set_frame(int w, int h, void *bgr_bits);

void preview_layout(HWND hwnd, RECT *out);
BOOL preview_client_to_image(HWND hwnd, int cx, int cy, int *out_img_x, int *out_img_y);
void preview_paint(HWND hwnd, HDC hdc);

#endif
