// aviutl input plugin (.aui) sdk types

#pragma once

#ifndef INPUT_H
#define INPUT_H

#include <stddef.h>
#include "aviutl.h"

#define INPUT_INFO_FLAG_VIDEO 0x01
#define INPUT_INFO_FLAG_AUDIO 0x02
#define INPUT_INFO_FLAG_VIDEO_COMPRESSED 0x04
#define INPUT_INFO_FLAG_VIDEO_RANDOM_ACCESS 0x08

#define INPUT_PLUGIN_FLAG_VIDEO 0x01
#define INPUT_PLUGIN_FLAG_AUDIO 0x02

typedef void *INPUT_HANDLE;

#if !REAVIUTL_X64
#pragma pack(push, 4)
#endif

typedef struct INPUT_INFO
{
    int flag;
    int rate, scale;
    int n;
    void *format; // BITMAPINFOHEADER *
    int format_size;
    int audio_n;
    void *audio_format; // WAVEFORMATEX *
    int audio_format_size;
    DWORD handler;
    int reserve[7];
} INPUT_INFO;

typedef struct INPUT_PLUGIN_TABLE
{
    int flag;
    LPSTR name;
    LPSTR filefilter;
    LPSTR information;
    BOOL(__cdecl *func_init)
    (void);
    BOOL(__cdecl *func_exit)
    (void);
    INPUT_HANDLE(__cdecl *func_open)
    (LPSTR file);
    BOOL(__cdecl *func_close)
    (INPUT_HANDLE ih);
    BOOL(__cdecl *func_info_get)
    (INPUT_HANDLE ih, INPUT_INFO *iip);
    int(__cdecl *func_read_video)(INPUT_HANDLE ih, int frame, void *buf);
    int(__cdecl *func_read_audio)(INPUT_HANDLE ih, int start, int length, void *buf);
    BOOL(__cdecl *func_is_keyframe)
    (INPUT_HANDLE ih, int frame);
    BOOL(__cdecl *func_config)
    (HWND hwnd, HINSTANCE dll_hinst);
    int reserve[16];
} INPUT_PLUGIN_TABLE;

#if !REAVIUTL_X64
#pragma pack(pop)

typedef char reaviutl_abi_ii_size[(sizeof(INPUT_INFO) == 0x44) ? 1 : -1];
typedef char reaviutl_abi_ipt_size[(sizeof(INPUT_PLUGIN_TABLE) == 0x74) ? 1 : -1];
#endif

#ifdef __cplusplus
extern "C"
{
#endif
    INPUT_PLUGIN_TABLE *__stdcall GetInputPluginTable(void);
#ifdef __cplusplus
}
#endif

#endif
