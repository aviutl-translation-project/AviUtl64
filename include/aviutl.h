// core aviutl sdk types

#pragma once

#ifndef AVIUTL_H
#define AVIUTL_H

#ifndef REAVIUTL_X64
#if defined(_WIN64) || defined(__x86_64__) || defined(_M_X64)
#define REAVIUTL_X64 1
#else
#define REAVIUTL_X64 0
#endif
#endif

#ifdef _WIN32
#include <windows.h>
#else
typedef int BOOL;
typedef void *HWND;
typedef void *HINSTANCE;
typedef void *HFONT;
typedef unsigned long DWORD;
typedef char *LPSTR;
#ifndef TCHAR
typedef char TCHAR;
#endif
#endif

// 16-bit YCbCr pixel, 6 bytes
typedef struct PIXEL_YC
{
    short y;  // 0 .. 4096
    short cb; // -2048 .. 2048
    short cr; // -2048 .. 2048
} PIXEL_YC;

// 8-bit BGR pixel
typedef struct PIXEL
{
    unsigned char b, g, r;
} PIXEL;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(PIXEL_YC) == 6, "PIXEL_YC size");
_Static_assert(sizeof(PIXEL) == 3, "PIXEL size");
#endif

#endif
