// color conversion plugin (.auc) manager

#pragma once

#ifndef COLOR_PLUGIN_H
#define COLOR_PLUGIN_H

#include <windows.h>
#include "aviutl.h"
#include "color.h"

#define COLOR_PLUGIN_MAX 32
#define COLOR_PLUGIN_FLAG_BUILTIN 0x10000000

typedef struct ColorPluginSummary
{
    char name[128];
    char information[256];
    char path[MAX_PATH];
    int flag;
    BOOL bridged_x86;
} ColorPluginSummary;

void color_plugin_init(const char *plugin_dir, HWND main_hwnd);
void color_plugin_shutdown(void);

int color_plugin_get_count(void);
BOOL color_plugin_get_summary(int idx, ColorPluginSummary *out_sum);

int color_plugin_get_ext_count(void);
BOOL color_plugin_get_ext_summary(int ext_idx, ColorPluginSummary *out_sum);

void color_plugin_set_selected_input(int idx);
int color_plugin_get_selected_input(void);
void color_plugin_set_selected_output(int idx);
int color_plugin_get_selected_output(void);

BOOL color_plugin_pixel2yc(PIXEL_YC *ycp, int yc_stride,
                           const void *pixelp, DWORD format,
                           int w, int h, BOOL invert_height);

BOOL color_plugin_yc2pixel(void *pixelp, const PIXEL_YC *ycp, int yc_stride,
                           DWORD format, int w, int h, BOOL invert_height);

#endif

