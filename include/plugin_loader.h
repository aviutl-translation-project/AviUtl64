#pragma once

#ifndef PLUGIN_LOADER_H
#define PLUGIN_LOADER_H

#include <windows.h>
#include "filter.h"

#define PLUGIN_MAX_FILTERS 64

typedef struct PluginReport
{
    void *module;
    int count;
    FILTER_DLL *filters[PLUGIN_MAX_FILTERS];
    DWORD last_error;
    char err[256];
} PluginReport;

int plugin_inspect_auf(const char *path, PluginReport *rep);
void plugin_unload(PluginReport *rep);
void plugin_describe(const FILTER_DLL *f, char *out, int out_sz);

BOOL plugin_setup_runtime_filter(FILTER *fp_out, FILTER_DLL *src,
                                 HINSTANCE hinst, const char *path,
                                 HWND filter_hwnd);
void plugin_free_runtime_filter(FILTER *fp);

BOOL plugin_init_filter(FILTER_DLL *f, HINSTANCE hinst, const char *path,
                        HWND hwnd_parent, char *err, int err_sz);

#endif
