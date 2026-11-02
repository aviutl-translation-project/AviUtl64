// aviutl input plugin (.aui) sdk types

#pragma once

#ifndef INPUT_H
#define INPUT_H

#include "aviutl.h"

#define INPUT_INFO_FLAG_VIDEO 0x01
#define INPUT_INFO_FLAG_AUDIO 0x02
#define INPUT_INFO_FLAG_VIDEO_RANDOM_ACCESS 0x08

typedef void *INPUT_HANDLE;

typedef struct INPUT_INFO
{
    int flag;
    int rate, scale;
    int n;
    void *format; // BITMAPINFOHEADER*
    int format_size;
    int audio_n;
    void *audio_format; // WAVEFORMATEX*
    int audio_format_size;
    DWORD handler;
    int reserve[7];
} INPUT_INFO;

typedef struct INPUT_PLUGIN_TABLE
{
    int flag;
    TCHAR *name;
    TCHAR *filefilter;
    TCHAR *information;
    BOOL (*func_init)
    (void);
    BOOL (*func_exit)
    (void);
    INPUT_HANDLE (*func_open)
    (LPSTR file);
    BOOL (*func_close)
    (INPUT_HANDLE ih);
    BOOL (*func_info_get)
    (INPUT_HANDLE ih, INPUT_INFO *iip);
    int (*func_read_video)(INPUT_HANDLE ih, int frame, void *buf);
    int (*func_read_audio)(INPUT_HANDLE ih, int start, int length, void *buf);
    BOOL (*func_is_keyframe)
    (INPUT_HANDLE ih, int frame);
    BOOL (*func_config)
    (HWND hwnd, HINSTANCE dll_hinst);
} INPUT_PLUGIN_TABLE;

#ifdef __cplusplus
extern "C"
{
#endif
    INPUT_PLUGIN_TABLE *GetInputPluginTable(void);
#ifdef __cplusplus
}
#endif

#endif
