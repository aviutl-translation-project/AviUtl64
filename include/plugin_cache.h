// startup plugin metadata cache helpers (0x004045e0 / 0x00404880)

#pragma once

#ifndef PLUGIN_CACHE_H
#define PLUGIN_CACHE_H

#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef enum PluginCacheLookup
{
    PLUGIN_CACHE_MISS = 0,
    PLUGIN_CACHE_HIT_NATIVE = 1,
    PLUGIN_CACHE_HIT_OTHER_ARCH = 2
} PluginCacheLookup;

static const char *plugin_cache_basename_dbcs(const char *path)
{
    const char *base;
    const unsigned char *p;
    if (!path)
        return "";
    base = path;
    p = (const unsigned char *)path;
    while (*p)
    {
        if (IsDBCSLeadByteEx(932, *p))
        {
            if (p[1])
                p += 2;
            else
                break;
        }
        else
        {
            if (*p == '\\' || *p == '/' || *p == ':')
                base = (const char *)(p + 1);
            p++;
        }
    }
    return base;
}

static void plugin_cache_make_section(const char *full_path, char *out_sec, size_t sec_cap)
{
    const unsigned char *s = (const unsigned char *)plugin_cache_basename_dbcs(full_path);
    size_t d = 0;

    if (!out_sec || sec_cap == 0)
        return;
    while (*s && d + 1 < sec_cap)
    {
        if (IsDBCSLeadByteEx(932, *s))
        {
            if (s[1] && d + 2 < sec_cap)
            {
                out_sec[d++] = (char)s[0];
                out_sec[d++] = (char)s[1];
                s += 2;
            }
            else
            {
                break;
            }
        }
        else
        {
            if (*s != '[' && *s != ']')
                out_sec[d++] = (char)*s;
            s++;
        }
    }
    out_sec[d] = '\0';
}

static void plugin_cache_make_check(const WIN32_FIND_DATAA *fd, char *out_chk, size_t chk_cap)
{
    SYSTEMTIME st;
    if (!out_chk || chk_cap == 0)
        return;
    out_chk[0] = '\0';
    if (!fd)
        return;
    ZeroMemory(&st, sizeof(st));
    FileTimeToSystemTime(&fd->ftLastWriteTime, &st);
    _snprintf(out_chk, chk_cap, "%04u/%02u/%02u %02u:%02u:%02u %d",
              (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
              (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
              (int)fd->nFileSizeLow);
    out_chk[chk_cap - 1] = '\0';
}

static BOOL plugin_cache_is_enabled(const char *ini_path)
{
    if (!ini_path || !ini_path[0])
        return FALSE;
    return GetPrivateProfileIntA("system", "plugincache", 0, ini_path) == 1;
}

// 0x004045e0: lookup cached plugin metadata in aviutl.ini
static PluginCacheLookup plugin_cache_try_load(
    const char *ini_path, const char *full_path, const WIN32_FIND_DATAA *fd,
    int expected_arch, int *out_flag, char *out_name, size_t name_cap,
    char *out_filter, size_t filter_cap, int *out_filter_len,
    char *out_info, size_t info_cap)
{
    char sec[MAX_PATH];
    char file_val[MAX_PATH];
    char chk_val[128];
    char expected_chk[128];

    if (!full_path || !full_path[0] || !fd || !plugin_cache_is_enabled(ini_path))
        return PLUGIN_CACHE_MISS;

    plugin_cache_make_section(full_path, sec, sizeof(sec));
    if (!sec[0])
        return PLUGIN_CACHE_MISS;

    GetPrivateProfileStringA(sec, "file", "", file_val, (DWORD)sizeof(file_val), ini_path);
    if (lstrcmpiA(full_path, file_val) != 0)
        return PLUGIN_CACHE_MISS;

    plugin_cache_make_check(fd, expected_chk, sizeof(expected_chk));
    GetPrivateProfileStringA(sec, "check", "", chk_val, (DWORD)sizeof(chk_val), ini_path);
    if (lstrcmpiA(expected_chk, chk_val) != 0)
        return PLUGIN_CACHE_MISS;

    if (expected_arch != 0)
    {
        int cached_arch = (int)GetPrivateProfileIntA(sec, "arch", 32, ini_path);
        if (cached_arch != expected_arch)
            return PLUGIN_CACHE_HIT_OTHER_ARCH;
    }

    if (out_flag)
    {
        *out_flag = (int)GetPrivateProfileIntA(sec, "flag", 0, ini_path);
    }
    if (out_name && name_cap > 0)
    {
        GetPrivateProfileStringA(sec, "name", "", out_name, (DWORD)name_cap, ini_path);
    }
    if (out_filter && filter_cap >= 2)
    {
        char enc[1024];
        size_t si = 0, di = 0;
        memset(out_filter, 0, filter_cap);
        GetPrivateProfileStringA(sec, "filefilter", "", enc, (DWORD)sizeof(enc), ini_path);
        while (enc[si] != '\0' && di + 2 < filter_cap)
        {
            out_filter[di++] = (enc[si] == '\\') ? '\0' : enc[si];
            si++;
        }
        out_filter[di] = '\0';
        out_filter[di + 1 < filter_cap ? di + 1 : filter_cap - 1] = '\0';
        if (out_filter_len)
        {
            *out_filter_len = (di > 0) ? (int)(di + 1) : 0;
        }
    }
    if (out_info && info_cap > 0)
    {
        GetPrivateProfileStringA(sec, "information", "", out_info, (DWORD)info_cap, ini_path);
    }
    return PLUGIN_CACHE_HIT_NATIVE;
}

// 0x00404880: store probed plugin metadata in aviutl.ini
static void plugin_cache_save(
    const char *ini_path, const char *full_path, const WIN32_FIND_DATAA *fd,
    int arch, const int *flag_ptr, const char *name,
    const char *double_nul_filter, const char *info)
{
    char sec[MAX_PATH];
    char chk[128];
    char num_buf[32];

    if (!full_path || !full_path[0] || !fd || !plugin_cache_is_enabled(ini_path))
        return;
    if (GetPrivateProfileIntA("system", "tempconfig", 0, ini_path) != 0)
        return;

    plugin_cache_make_section(full_path, sec, sizeof(sec));
    if (!sec[0])
        return;

    plugin_cache_make_check(fd, chk, sizeof(chk));
    WritePrivateProfileStringA(sec, "file", full_path, ini_path);
    WritePrivateProfileStringA(sec, "check", chk, ini_path);

    if (arch > 0)
    {
        wsprintfA(num_buf, "%d", arch);
        WritePrivateProfileStringA(sec, "arch", num_buf, ini_path);
    }
    if (flag_ptr)
    {
        wsprintfA(num_buf, "%d", *flag_ptr);
        WritePrivateProfileStringA(sec, "flag", num_buf, ini_path);
    }
    if (name)
    {
        WritePrivateProfileStringA(sec, "name", name, ini_path);
    }
    if (double_nul_filter)
    {
        char enc[1024];
        size_t si = 0, di = 0;
        while (double_nul_filter[si] != '\0' && di + 2 < sizeof(enc))
        {
            while (double_nul_filter[si] != '\0' && di + 2 < sizeof(enc))
            {
                enc[di++] = double_nul_filter[si++];
            }
            enc[di++] = '\\';
            si++;
        }
        enc[di] = '\0';
        WritePrivateProfileStringA(sec, "filefilter", enc, ini_path);
    }
    if (info)
    {
        WritePrivateProfileStringA(sec, "information", info, ini_path);
    }
}

#endif
