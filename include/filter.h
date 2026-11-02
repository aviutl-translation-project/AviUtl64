// aviutl filter plugin (.auf) sdk types

#pragma once

#ifndef FILTER_H
#define FILTER_H

#include <stddef.h>
#include "aviutl.h"

#define FILTER_FLAG_ACTIVE 0x00000001
#define FILTER_FLAG_WINDOW_DISP 0x00000002
#define FILTER_FLAG_ALWAYS_ACTIVE 0x00000004
#define FILTER_FLAG_CONFIG_POPUP 0x00000008
#define FILTER_FLAG_CONFIG_CHECK 0x00000010
#define FILTER_FLAG_CONFIG_RADIO 0x00000020
#define FILTER_FLAG_EX_DATA 0x00000400
#define FILTER_FLAG_PRIORITY_HIGHEST 0x00000800
#define FILTER_FLAG_PRIORITY_LOWEST 0x00001000
#define FILTER_FLAG_WINDOW_THICKFRAME 0x00002000
#define FILTER_FLAG_WINDOW_SIZE 0x00004000
#define FILTER_FLAG_DISP_FILTER 0x00008000
#define FILTER_FLAG_REDRAW 0x00020000
#define FILTER_FLAG_EX_INFORMATION 0x00040000
#define FILTER_FLAG_INFORMATION 0x00080000
#define FILTER_FLAG_NO_CONFIG 0x00100000
#define FILTER_FLAG_AUDIO_FILTER 0x00200000
#define FILTER_FLAG_RADIO_BUTTON 0x00400000
#define FILTER_FLAG_WINDOW_HSCROLL 0x00800000
#define FILTER_FLAG_WINDOW_VSCROLL 0x01000000
#define FILTER_FLAG_INTERLACE_FILTER 0x04000000
#define FILTER_FLAG_NO_INIT_DATA 0x08000000
#define FILTER_FLAG_IMPORT 0x10000000
#define FILTER_FLAG_EXPORT 0x20000000
#define FILTER_FLAG_MAIN_MESSAGE 0x40000000

#define FILTER_WINDOW_SIZE_CLIENT 0x10000000
#define FILTER_WINDOW_SIZE_ADD 0x30000000

// messages sent to FILTER.func_WndProc
#define WM_FILTER_UPDATE (0x0400 + 100)
#define WM_FILTER_FILE_OPEN (0x0400 + 101)
#define WM_FILTER_FILE_CLOSE (0x0400 + 102)
#define WM_FILTER_INIT (0x0400 + 103)
#define WM_FILTER_EXIT (0x0400 + 104)
#define WM_FILTER_SAVE_START (0x0400 + 105)
#define WM_FILTER_SAVE_END (0x0400 + 106)
#define WM_FILTER_IMPORT (0x0400 + 107)
#define WM_FILTER_EXPORT (0x0400 + 108)
#define WM_FILTER_CHANGE_ACTIVE (0x0400 + 109)
#define WM_FILTER_CHANGE_WINDOW (0x0400 + 110)
#define WM_FILTER_CHANGE_PARAM (0x0400 + 111)
#define WM_FILTER_CHANGE_EDIT (0x0400 + 112)
#define WM_FILTER_COMMAND (0x0400 + 113)
#define WM_FILTER_FILE_UPDATE (0x0400 + 114)
#define WM_FILTER_MAIN_MOUSE_DOWN (0x0400 + 120)
#define WM_FILTER_MAIN_MOUSE_UP (0x0400 + 121)
#define WM_FILTER_MAIN_MOUSE_MOVE (0x0400 + 122)
#define WM_FILTER_MAIN_KEY_DOWN (0x0400 + 123)
#define WM_FILTER_MAIN_KEY_UP (0x0400 + 124)
#define WM_FILTER_MAIN_MOVESIZE (0x0400 + 125)
#define WM_FILTER_MAIN_MOUSE_DBLCLK (0x0400 + 126)
#define WM_FILTER_MAIN_MOUSE_R_DOWN (0x0400 + 127)
#define WM_FILTER_MAIN_MOUSE_R_UP (0x0400 + 128)
#define WM_FILTER_MAIN_MOUSE_WHEEL (0x0400 + 129)
#define WM_FILTER_MAIN_CONTEXTMENU (0x0400 + 130)

#define SYS_INFO_FLAG_EDIT 0x00000001
#define SYS_INFO_FLAG_VFAPI 0x00000002
#define SYS_INFO_FLAG_USE_SSE 0x00000004
#define SYS_INFO_FLAG_USE_SSE2 0x00000008

#define FILE_INFO_FLAG_VIDEO 0x00000001
#define FILE_INFO_FLAG_AUDIO 0x00000002

#define FILTER_PROC_INFO_FLAG_INVERT_FIELD_ORDER 0x00010000
#define FILTER_PROC_INFO_FLAG_INVERT_INTERLACE 0x00020000

#define FILTER_UPDATE_STATUS_ALL 0x00000000
#define FILTER_UPDATE_STATUS_TRACK 0x00010000
#define FILTER_UPDATE_STATUS_CHECK 0x00020000

#define MID_FILTER_BUTTON 12004

#define ADD_MENU_ITEM_FLAG_KEY_SHIFT 0x00000001
#define ADD_MENU_ITEM_FLAG_KEY_CTRL 0x00000002
#define ADD_MENU_ITEM_FLAG_KEY_ALT 0x00000004
#define ADD_MENU_ITEM_FLAG_KEY_VIEW 0x00000100

#define AVI_FILE_OPEN_FLAG_VIDEO_ONLY 0x00000010
#define AVI_FILE_OPEN_FLAG_AUDIO_ONLY 0x00000020
#define AVI_FILE_OPEN_FLAG_ONLY_YUY2 0x00010000
#define AVI_FILE_OPEN_FLAG_ONLY_RGB24 0x00020000
#define AVI_FILE_OPEN_FLAG_ONLY_RGB32 0x00040000

#define EDIT_OPEN_FLAG_ADD 0x00000002
#define EDIT_OPEN_FLAG_AUDIO 0x00000010
#define EDIT_OPEN_FLAG_VIDEO 0x00000020
#define EDIT_OPEN_FLAG_PROJECT 0x00000200
#define EDIT_OPEN_FLAG_KEY_LOAD 0x00001000
#define EDIT_OPEN_FLAG_DIALOG 0x00010000

#define EDIT_FRAME_EDIT_FLAG_KEYFRAME 0x01
#define EDIT_FRAME_EDIT_FLAG_MARKFRAME 0x02
#define EDIT_FRAME_EDIT_FLAG_DELFRAME 0x04
#define EDIT_FRAME_EDIT_FLAG_NULLFRAME 0x08

#define FRAME_STATUS_TYPE_EDIT_FLAG 0
#define FRAME_STATUS_TYPE_INTER 1

#define EDIT_FRAME_INTER_NORMAL 0
#define EDIT_FRAME_INTER_REVERSE 1
#define EDIT_FRAME_INTER_ODD 2
#define EDIT_FRAME_INTER_EVEN 3
#define EDIT_FRAME_INTER_MIX 4
#define EDIT_FRAME_INTER_AUTO 5

#ifndef _WIN32
typedef unsigned int UINT;
typedef unsigned int WPARAM;
typedef long LPARAM;
#endif

typedef struct FILTER_T FILTER;
typedef struct FILTER_T FILTER_DLL;
typedef struct EXFUNC_T EXFUNC;
typedef void *AVI_FILE_HANDLE;

typedef void(__cdecl *MULTI_THREAD_FUNC)(int thread_id, int thread_num, void *param1, void *param2);

#if !REAVIUTL_X64
#pragma pack(push, 4)
#endif

// per-frame work packet passed to func_proc (0x68 bytes on x86)
typedef struct FILTER_PROC_INFO
{
    int flag;           // +0x00
    PIXEL_YC *ycp_edit; // +0x04 row stride is max_w
    PIXEL_YC *ycp_temp; // +0x08
    int w, h;           // +0x0c
    int max_w, max_h;   // +0x14
    int frame;          // +0x1c
    int frame_n;        // +0x20
    int org_w, org_h;   // +0x24
    short *audiop;      // +0x2c interleaved PCM16
    int audio_n;        // +0x30 samples per channel
    int audio_ch;       // +0x34 channel count
    PIXEL *pixelp;      // +0x38 unused
    void *editp;        // +0x3c
    int yc_size;        // +0x40
    int line_size;      // +0x44 max_w * 6
    int reserve[8];     // +0x48
} FILTER_PROC_INFO;

// global host and project info (0x4c bytes on x86)
typedef struct SYS_INFO
{
    int flag;           // +0x00
    LPSTR info;         // +0x04
    int filter_n;       // +0x08
    int min_w, min_h;   // +0x0c
    int max_w, max_h;   // +0x14
    int max_frame;      // +0x1c
    LPSTR edit_name;    // +0x20
    LPSTR project_name; // +0x24
    LPSTR output_name;  // +0x28
    int vram_w, vram_h; // +0x2c
    int vram_byte_size; // +0x34
    int vram_yc_size;   // +0x38
    HFONT hfont;        // +0x3c
    int build;          // +0x40 11003 for v1.10
    int reserve[2];     // +0x44
} SYS_INFO;

// media file metadata (0x40 bytes on x86)
typedef struct FILE_INFO
{
    int flag;                  // +0x00
    LPSTR name;                // +0x04
    int w, h;                  // +0x08
    int video_rate;            // +0x10
    int video_scale;           // +0x14
    int audio_rate;            // +0x18
    int audio_ch;              // +0x1c
    int frame_n;               // +0x20
    DWORD video_decode_format; // +0x24
    int video_decode_bit;      // +0x28
    int audio_n;               // +0x2c
    int reserve[4];            // +0x30
} FILE_INFO;

// host callback table (0x004a8c78 in aviutl 1.10, 0x140 bytes on x86)
struct EXFUNC_T
{
    void *(__cdecl *get_ycp_ofs)(void *editp, int n, int ofs); // +0x000
    void *(__cdecl *get_ycp)(void *editp, int n);              // +0x004
    void *(__cdecl *get_pixelp)(void *editp, int n);           // +0x008
    int(__cdecl *get_audio)(void *editp, int n, void *buf);    // +0x00c
    BOOL(__cdecl *is_editing)
    (void *editp); // +0x010
    BOOL(__cdecl *is_saving)
    (void *editp);                          // +0x014
    int(__cdecl *get_frame)(void *editp);   // +0x018
    int(__cdecl *get_frame_n)(void *editp); // +0x01c
    BOOL(__cdecl *get_frame_size)
    (void *editp, int *w, int *h);                 // +0x020
    int(__cdecl *set_frame)(void *editp, int n);   // +0x024
    int(__cdecl *set_frame_n)(void *editp, int n); // +0x028
    BOOL(__cdecl *copy_frame)
    (void *editp, int d, int s); // +0x02c
    BOOL(__cdecl *copy_video)
    (void *editp, int d, int s); // +0x030
    BOOL(__cdecl *copy_audio)
    (void *editp, int d, int s); // +0x034
    BOOL(__cdecl *copy_clip)
    (HWND hwnd, void *pixelp, int w, int h); // +0x038
    BOOL(__cdecl *paste_clip)
    (HWND hwnd, void *editp, int n); // +0x03c
    BOOL(__cdecl *get_frame_status)
    (void *editp, int n, BYTE *fps); // +0x040
    BOOL(__cdecl *set_frame_status)
    (void *editp, int n, BYTE fps); // +0x044
    BOOL(__cdecl *is_saveframe)
    (void *editp, int n); // +0x048
    BOOL(__cdecl *is_keyframe)
    (void *editp, int n); // +0x04c
    BOOL(__cdecl *is_recompress)
    (void *editp, int n); // +0x050
    BOOL(__cdecl *filter_window_update)
    (FILTER *fp); // +0x054
    BOOL(__cdecl *is_filter_window_disp)
    (FILTER *fp); // +0x058
    BOOL(__cdecl *get_file_info)
    (void *editp, FILE_INFO *fip); // +0x05c
    LPSTR(__cdecl *get_config_name)
    (void *editp, int n); // +0x060
    BOOL(__cdecl *is_filter_active)
    (FILTER *fp); // +0x064
    BOOL(__cdecl *get_pixel_filtered)
    (void *editp, int n, void *pixelp, int *w, int *h);              // +0x068
    int(__cdecl *get_audio_filtered)(void *editp, int n, void *buf); // +0x06c
    BOOL(__cdecl *get_select_frame)
    (void *editp, int *s, int *e); // +0x070
    BOOL(__cdecl *set_select_frame)
    (void *editp, int s, int e); // +0x074
    BOOL(__cdecl *rgb2yc)
    (PIXEL_YC *ycp, PIXEL *pixelp, int w); // +0x078
    BOOL(__cdecl *yc2rgb)
    (PIXEL *pixelp, PIXEL_YC *ycp, int w); // +0x07c
    BOOL(__cdecl *dlg_get_load_name)
    (LPSTR name, LPSTR filter, LPSTR def); // +0x080
    BOOL(__cdecl *dlg_get_save_name)
    (LPSTR name, LPSTR filter, LPSTR def);                    // +0x084
    int(__cdecl *ini_load_int)(FILTER *fp, LPSTR key, int n); // +0x088
    int(__cdecl *ini_save_int)(FILTER *fp, LPSTR key, int n); // +0x08c
    BOOL(__cdecl *ini_load_str)
    (FILTER *fp, LPSTR key, LPSTR str, LPSTR def); // +0x090
    BOOL(__cdecl *ini_save_str)
    (FILTER *fp, LPSTR key, LPSTR str); // +0x094
    BOOL(__cdecl *get_source_file_info)
    (void *editp, FILE_INFO *fip, int source_file_id); // +0x098
    BOOL(__cdecl *get_source_video_number)
    (void *editp, int n, int *source_file_id, int *source_video_number); // +0x09c
    BOOL(__cdecl *get_sys_info)
    (void *editp, SYS_INFO *sip);                                                      // +0x0a0
    FILTER *(__cdecl *get_filterp)(int filter_id);                                     // +0x0a4
    void *(__cdecl *get_ycp_filtering)(FILTER *fp, void *editp, int n, void *reserve); // +0x0a8
    int(__cdecl *get_audio_filtering)(FILTER *fp, void *editp, int n, void *buf);      // +0x0ac
    BOOL(__cdecl *set_ycp_filtering_cache_size)
    (FILTER *fp, int w, int h, int d, int flag);                              // +0x0b0
    void *(__cdecl *get_ycp_filtering_cache)(FILTER *fp, void *editp, int n); // +0x0b4
    void *(__cdecl *get_ycp_source_cache)(void *editp, int n, int ofs);       // +0x0b8
    void *(__cdecl *get_disp_pixelp)(void *editp, DWORD format);              // +0x0bc
    BOOL(__cdecl *get_pixel_source)
    (void *editp, int n, void *pixelp, DWORD format); // +0x0c0
    BOOL(__cdecl *get_pixel_filtered_ex)
    (void *editp, int n, void *pixelp, int *w, int *h, DWORD format);                                // +0x0c4
    PIXEL_YC *(__cdecl *get_ycp_filtering_cache_ex)(FILTER *fp, void *editp, int n, int *w, int *h); // +0x0c8
    BOOL(__cdecl *exec_multi_thread_func)
    (MULTI_THREAD_FUNC func, void *param1, void *param2); // +0x0cc
    PIXEL_YC *(__cdecl *create_yc)(void);                 // +0x0d0
    void(__cdecl *delete_yc)(PIXEL_YC *ycp);              // +0x0d4
    BOOL(__cdecl *load_image)
    (PIXEL_YC *ycp, LPSTR file, int *w, int *h, int flag);                                                                       // +0x0d8
    void(__cdecl *resize_yc)(PIXEL_YC *ycp, int w, int h, PIXEL_YC *src, int sx, int sy, int sw, int sh);                        // +0x0dc
    void(__cdecl *copy_yc)(PIXEL_YC *ycp, int x, int y, PIXEL_YC *src, int sx, int sy, int sw, int sh, int tr);                  // +0x0e0
    void(__cdecl *draw_text)(PIXEL_YC *ycp, int x, int y, LPSTR text, int r, int g, int b, int tr, HFONT hfont, int *w, int *h); // +0x0e4
    AVI_FILE_HANDLE(__cdecl *avi_file_open)
    (LPSTR file, FILE_INFO *fip, int flag);             // +0x0e8
    void(__cdecl *avi_file_close)(AVI_FILE_HANDLE afh); // +0x0ec
    BOOL(__cdecl *avi_file_read_video)
    (AVI_FILE_HANDLE afh, PIXEL_YC *ycp, int n);                              // +0x0f0
    int(__cdecl *avi_file_read_audio)(AVI_FILE_HANDLE afh, void *buf, int n); // +0x0f4
    void *(__cdecl *avi_file_get_video_pixelp)(AVI_FILE_HANDLE afh, int n);   // +0x0f8
    LPSTR(__cdecl *get_avi_file_filter)
    (int type);                                                                                      // +0x0fc
    int(__cdecl *avi_file_read_audio_sample)(AVI_FILE_HANDLE afh, int start, int length, void *buf); // +0x100
    int(__cdecl *avi_file_set_audio_sample_rate)(AVI_FILE_HANDLE afh, int audio_rate, int audio_ch); // +0x104
    BYTE *(__cdecl *get_frame_status_table)(void *editp, int type);                                  // +0x108
    BOOL(__cdecl *set_undo)
    (void *editp); // +0x10c
    BOOL(__cdecl *add_menu_item)
    (FILTER *fp, LPSTR name, HWND hwnd, int id, int def_key, int flag); // +0x110
    BOOL(__cdecl *edit_open)
    (void *editp, LPSTR file, int flag); // +0x114
    BOOL(__cdecl *edit_close)
    (void *editp); // +0x118
    BOOL(__cdecl *edit_output)
    (void *editp, LPSTR file, int flag, LPSTR type); // +0x11c
    BOOL(__cdecl *set_config)
    (void *editp, int n, LPSTR name); // +0x120
    int reserve[7];                   // +0x124
};

// filter descriptor and runtime state (0x8c bytes on x86)
struct FILTER_T
{
    int flag;           // +0x00
    int x, y;           // +0x04
    TCHAR *name;        // +0x0c
    int track_n;        // +0x10
    TCHAR **track_name; // +0x14
    int *track_default; // +0x18
    int *track_s;       // +0x1c
    int *track_e;       // +0x20
    int check_n;        // +0x24
    TCHAR **check_name; // +0x28
    int *check_default; // +0x2c
    BOOL(__cdecl *func_proc)
    (FILTER *fp, FILTER_PROC_INFO *fpip); // +0x30
    BOOL(__cdecl *func_init)
    (FILTER *fp); // +0x34
    BOOL(__cdecl *func_exit)
    (FILTER *fp); // +0x38
    BOOL(__cdecl *func_update)
    (FILTER *fp, int status); // +0x3c
    BOOL(__cdecl *func_WndProc)
    (HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void *editp, FILTER *fp); // +0x40
    int *track;                                                                       // +0x44
    int *check;                                                                       // +0x48
    void *ex_data_ptr;                                                                // +0x4c
    int ex_data_size;                                                                 // +0x50
    TCHAR *information;                                                               // +0x54
    BOOL(__cdecl *func_save_start)
    (FILTER *fp, int s, int e, void *editp); // +0x58
    BOOL(__cdecl *func_save_end)
    (FILTER *fp, void *editp); // +0x5c
    EXFUNC *exfunc;            // +0x60
    HWND hwnd;                 // +0x64
    HINSTANCE dll_hinst;       // +0x68
    void *ex_data_def;         // +0x6c
    BOOL(__cdecl *func_is_saveframe)
    (FILTER *fp, void *editp, int saveno, int frame, int fps, int edit_flag, int inter); // +0x70
    BOOL(__cdecl *func_project_load)
    (FILTER *fp, void *editp, void *data, int size); // +0x74
    BOOL(__cdecl *func_project_save)
    (FILTER *fp, void *editp, void *data, int *size); // +0x78
    BOOL(__cdecl *func_modify_title)
    (FILTER *fp, void *editp, int frame, LPSTR title, int max_title); // +0x7c
    TCHAR *dll_path;                                                  // +0x80
    int reserve[2];                                                   // +0x84
};

#if !REAVIUTL_X64
#pragma pack(pop)

typedef char reaviutl_abi_fpip_size[(sizeof(FILTER_PROC_INFO) == 0x68) ? 1 : -1];
typedef char reaviutl_abi_fi_size[(sizeof(FILE_INFO) == 0x40) ? 1 : -1];
typedef char reaviutl_abi_si_build[(offsetof(SYS_INFO, build) == 0x40) ? 1 : -1];
typedef char reaviutl_abi_si_size[(sizeof(SYS_INFO) == 0x4c) ? 1 : -1];
typedef char reaviutl_abi_exf_fi[(offsetof(EXFUNC, get_file_info) == 0x05c) ? 1 : -1];
typedef char reaviutl_abi_exf_ini[(offsetof(EXFUNC, ini_load_int) == 0x088) ? 1 : -1];
typedef char reaviutl_abi_exf_si[(offsetof(EXFUNC, get_sys_info) == 0x0a0) ? 1 : -1];
typedef char reaviutl_abi_exf_fp[(offsetof(EXFUNC, get_filterp) == 0x0a4) ? 1 : -1];
typedef char reaviutl_abi_exf_mt[(offsetof(EXFUNC, exec_multi_thread_func) == 0x0cc) ? 1 : -1];
typedef char reaviutl_abi_exf_avi[(offsetof(EXFUNC, avi_file_open) == 0x0e8) ? 1 : -1];
typedef char reaviutl_abi_exf_menu[(offsetof(EXFUNC, add_menu_item) == 0x110) ? 1 : -1];
typedef char reaviutl_abi_exf_size[(sizeof(EXFUNC) == 0x140) ? 1 : -1];
typedef char reaviutl_abi_flt_exfunc[(offsetof(FILTER, exfunc) == 0x60) ? 1 : -1];
typedef char reaviutl_abi_flt_hwnd[(offsetof(FILTER, hwnd) == 0x64) ? 1 : -1];
typedef char reaviutl_abi_flt_hinst[(offsetof(FILTER, dll_hinst) == 0x68) ? 1 : -1];
typedef char reaviutl_abi_flt_size[(sizeof(FILTER) == 0x8c) ? 1 : -1];
#endif

#ifdef __cplusplus
extern "C"
{
#endif
    FILTER *__stdcall GetFilterTable(void);
    FILTER **__stdcall GetFilterTableList(void);
    FILTER *__stdcall GetFilterTableYUY2(void);
    FILTER **__stdcall GetFilterTableListYUY2(void);
#ifdef __cplusplus
}
#endif

#endif
