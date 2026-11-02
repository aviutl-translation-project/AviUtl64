// aviutl color conversion plugin (.auc) sdk types

#pragma once

#ifndef COLOR_H
#define COLOR_H

#include <stddef.h>
#include "aviutl.h"
#include "filter.h"

#define COLOR_PROC_INFO_FLAG_INVERT_HEIGHT 1
#define COLOR_PROC_INFO_FLAG_USE_SSE 256
#define COLOR_PROC_INFO_FLAG_USE_SSE2 512

#if !REAVIUTL_X64
#pragma pack(push, 4)
#endif

typedef struct COLOR_PROC_INFO
{
    int flag;
    PIXEL_YC *ycp;
    void *pixelp;
    DWORD format;
    int w, h;
    int line_size;
    int yc_size;
    BOOL(__cdecl *exec_multi_thread_func)
    (MULTI_THREAD_FUNC func, void *param1, void *param2);
    int reserve[16];
} COLOR_PROC_INFO;

typedef struct COLOR_PLUGIN_TABLE
{
    int flag;
    LPSTR name;
    LPSTR information;
    BOOL(__cdecl *func_init)
    (void);
    BOOL(__cdecl *func_exit)
    (void);
    BOOL(__cdecl *func_pixel2yc)
    (COLOR_PROC_INFO *cpip);
    BOOL(__cdecl *func_yc2pixel)
    (COLOR_PROC_INFO *cpip);
    int reserve[16];
} COLOR_PLUGIN_TABLE;

#if !REAVIUTL_X64
#pragma pack(pop)

typedef char reaviutl_abi_cpi_size[(sizeof(COLOR_PROC_INFO) == 0x64) ? 1 : -1];
typedef char reaviutl_abi_cpt_size[(sizeof(COLOR_PLUGIN_TABLE) == 0x5c) ? 1 : -1];
#endif

#ifdef __cplusplus
extern "C"
{
#endif
    COLOR_PLUGIN_TABLE *__stdcall GetColorPluginTable(void);
    COLOR_PLUGIN_TABLE *__stdcall GetColorPluginTableYUY2(void);
#ifdef __cplusplus
}
#endif

#endif

