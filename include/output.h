// aviutl output plugin (.auo) sdk types

#pragma once

#ifndef OUTPUT_H
#define OUTPUT_H

#include "aviutl.h"

#define OUTPUT_INFO_FLAG_VIDEO 0x00000001
#define OUTPUT_INFO_FLAG_AUDIO 0x00000002
#define OUTPUT_INFO_FLAG_BATCH 0x00000004

#define OUTPUT_INFO_FRAME_FLAG_KEYFRAME 0x00000001
#define OUTPUT_INFO_FRAME_FLAG_COPYFRAME 0x00000002

#define OUTPUT_PLUGIN_TABLE_FLAG_HAS_CONFIG 0x00000100

#if !REAVIUTL_X64
#pragma pack(push, 4)
#endif

// encode session struct passed to func_output (0x5c bytes on x86)
typedef struct OUTPUT_INFO
{
    int flag;                                                            // +0x00
    int w, h;                                                            // +0x04
    int rate, scale;                                                     // +0x0c
    int n;                                                               // +0x14
    int size;                                                            // +0x18
    int audio_rate;                                                      // +0x1c
    int audio_ch;                                                        // +0x20
    int audio_n;                                                         // +0x24
    int audio_size;                                                      // +0x28
    LPSTR savefile;                                                      // +0x2c
    void *(__cdecl *func_get_video)(int frame);                          // +0x30
    void *(__cdecl *func_get_audio)(int start, int length, int *readed); // +0x34
    BOOL(__cdecl *func_is_abort)
    (void); // +0x38
    BOOL(__cdecl *func_rest_time_disp)
    (int now, int total);                   // +0x3c
    int(__cdecl *func_get_flag)(int frame); // +0x40
    BOOL(__cdecl *func_update_preview)
    (void);                                                      // +0x44
    void *(__cdecl *func_get_video_ex)(int frame, DWORD format); // +0x48
    int reserve[4];                                              // +0x4c
} OUTPUT_INFO;

// output plugin table returned by GetOutputPluginTable (0x68 bytes on x86)
typedef struct OUTPUT_PLUGIN_TABLE
{
    int flag;          // +0x00
    LPSTR name;        // +0x04
    LPSTR filefilter;  // +0x08
    LPSTR information; // +0x0c
    BOOL(__cdecl *func_init)
    (void); // +0x10
    BOOL(__cdecl *func_exit)
    (void); // +0x14
    BOOL(__cdecl *func_output)
    (OUTPUT_INFO *oip); // +0x18
    BOOL(__cdecl *func_config)
    (HWND hwnd, HINSTANCE dll_hinst);                    // +0x1c
    int(__cdecl *func_config_get)(void *data, int size); // +0x20
    int(__cdecl *func_config_set)(void *data, int size); // +0x24
    int reserve[16];                                     // +0x28
} OUTPUT_PLUGIN_TABLE;

#if !REAVIUTL_X64
#pragma pack(pop)
typedef char output_info_size_assert[(sizeof(OUTPUT_INFO) == 0x5c) ? 1 : -1];
typedef char output_plugin_table_size_assert[(sizeof(OUTPUT_PLUGIN_TABLE) == 0x68) ? 1 : -1];
#endif

typedef OUTPUT_PLUGIN_TABLE *(__cdecl *GET_OUTPUT_PLUGIN_TABLE_FN)(void);

#ifdef __cplusplus
extern "C"
{
#endif
    OUTPUT_PLUGIN_TABLE *__cdecl GetOutputPluginTable(void);
#ifdef __cplusplus
}
#endif

#endif
