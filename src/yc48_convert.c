#include <string.h>

#include "yc48_convert.h"

static __inline unsigned char clamp_u8(int v)
{
    if (v < 0)
    {
        return 0;
    }
    if (v > 255)
    {
        return 255;
    }
    return (unsigned char)v;
}

static __inline short clamp_s16(int v)
{
    if (v < -32768)
    {
        return -32768;
    }
    if (v > 32767)
    {
        return 32767;
    }
    return (short)v;
}

static __inline void rgb_to_yc_pixel(int r, int g, int b, PIXEL_YC *out)
{
    int y = ((4918 * r + 354) >> 10) + ((9655 * g + 585) >> 10) + ((1875 * b + 523) >> 10);
    int cb = ((-2775 * r + 240) >> 10) + ((-5449 * g + 515) >> 10) + ((8224 * b + 256) >> 10);
    int cr = ((8224 * r + 256) >> 10) + ((-6887 * g + 110) >> 10) + ((-1337 * b + 646) >> 10);
    out->y = clamp_s16(y);
    out->cb = clamp_s16(cb);
    out->cr = clamp_s16(cr);
}

static __inline void yc_to_bgr_bytes(const PIXEL_YC *in, unsigned char *b_out,
                                     unsigned char *g_out, unsigned char *r_out)
{
    int y = in->y;
    int cb = in->cb;
    int cr = in->cr;

    int r = (16320 * y + 22881 * cr + 131072) >> 18;
    int g = (16320 * y - 5616 * cb - 11655 * cr + 131072) >> 18;
    int b = (16320 * y + 28919 * cb + 131072) >> 18;

    *b_out = clamp_u8(b);
    *g_out = clamp_u8(g);
    *r_out = clamp_u8(r);
}

BOOL __cdecl rgb2yc(PIXEL_YC *ycp, PIXEL *pixelp, int w)
{
    int i;
    if (!ycp || !pixelp || w <= 0)
    {
        return FALSE;
    }
    for (i = 0; i < w; i++)
    {
        rgb_to_yc_pixel(pixelp[i].r, pixelp[i].g, pixelp[i].b, &ycp[i]);
    }
    return TRUE;
}

BOOL __cdecl yc2rgb(PIXEL *pixelp, PIXEL_YC *ycp, int w)
{
    int i;
    if (!pixelp || !ycp || w <= 0)
    {
        return FALSE;
    }
    for (i = 0; i < w; i++)
    {
        yc_to_bgr_bytes(&ycp[i], &pixelp[i].b, &pixelp[i].g, &pixelp[i].r);
    }
    return TRUE;
}

BOOL rgb2yc_dib24(PIXEL_YC *ycp, int yc_stride,
                  const unsigned char *dib_bgr, int dib_stride,
                  int w, int h, BOOL bottom_up)
{
    int x, y;
    if (!ycp || !dib_bgr || w <= 0 || h <= 0)
    {
        return FALSE;
    }
    if (yc_stride <= 0)
    {
        yc_stride = w;
    }
    if (dib_stride <= 0)
    {
        dib_stride = (w * 3 + 3) & ~3;
    }
    for (y = 0; y < h; y++)
    {
        int src_y = bottom_up ? (h - 1 - y) : y;
        const unsigned char *src = dib_bgr + (size_t)src_y * (size_t)dib_stride;
        PIXEL_YC *dst = ycp + (size_t)y * (size_t)yc_stride;
        for (x = 0; x < w; x++)
        {
            rgb_to_yc_pixel(src[x * 3 + 2], src[x * 3 + 1], src[x * 3 + 0], &dst[x]);
        }
    }
    return TRUE;
}

BOOL yc2rgb_dib24(unsigned char *dib_bgr, int dib_stride,
                  const PIXEL_YC *ycp, int yc_stride,
                  int w, int h, BOOL bottom_up)
{
    int x, y;
    if (!dib_bgr || !ycp || w <= 0 || h <= 0)
    {
        return FALSE;
    }
    if (yc_stride <= 0)
    {
        yc_stride = w;
    }
    if (dib_stride <= 0)
    {
        dib_stride = (w * 3 + 3) & ~3;
    }
    for (y = 0; y < h; y++)
    {
        int dst_y = bottom_up ? (h - 1 - y) : y;
        unsigned char *dst = dib_bgr + (size_t)dst_y * (size_t)dib_stride;
        const PIXEL_YC *src = ycp + (size_t)y * (size_t)yc_stride;
        for (x = 0; x < w; x++)
        {
            yc_to_bgr_bytes(&src[x], &dst[x * 3 + 0], &dst[x * 3 + 1], &dst[x * 3 + 2]);
        }
    }
    return TRUE;
}

// 0x00450090, yc48 to packed yuy2
BOOL yc48_to_yuy2(unsigned char *dst_yuy2, int dst_stride,
                  const PIXEL_YC *ycp, int yc_stride,
                  int w, int h)
{
    int x, y;
    if (!dst_yuy2 || !ycp || w <= 0 || h <= 0)
    {
        return FALSE;
    }
    if (yc_stride <= 0)
    {
        yc_stride = w;
    }
    if (dst_stride <= 0)
    {
        dst_stride = ((w + 1) / 2) * 4;
    }
    for (y = 0; y < h; y++)
    {
        unsigned char *dst = dst_yuy2 + (size_t)y * (size_t)dst_stride;
        const PIXEL_YC *src = ycp + (size_t)y * (size_t)yc_stride;
        for (x = 0; x < w; x += 2)
        {
            const PIXEL_YC *p0 = &src[x];
            const PIXEL_YC *p1 = (x + 1 < w) ? &src[x + 1] : p0;
            int y0 = (((int)clamp_s16((int)p0->y + 301) * 14016) >> 16) >> 2;
            int u0 = (((int)clamp_s16((int)p0->cb + 2350) * 14336) >> 16) >> 2;
            int y1 = (((int)clamp_s16((int)p1->y + 301) * 14016) >> 16) >> 2;
            int v0 = (((int)clamp_s16((int)p0->cr + 2350) * 14336) >> 16) >> 2;
            dst[(x / 2) * 4 + 0] = clamp_u8(y0);
            dst[(x / 2) * 4 + 1] = clamp_u8(u0);
            dst[(x / 2) * 4 + 2] = clamp_u8(y1);
            dst[(x / 2) * 4 + 3] = clamp_u8(v0);
        }
    }
    return TRUE;
}

DWORD yc48_normalize_format(DWORD format)
{
    unsigned int b0 = (format >> 0) & 0xffu;
    unsigned int b1 = (format >> 8) & 0xffu;
    unsigned int b2 = (format >> 16) & 0xffu;
    unsigned int b3 = (format >> 24) & 0xffu;
    DWORD norm;

    if (b0 >= 'a' && b0 <= 'z')
    {
        b0 -= 0x20u;
    }
    if (b1 >= 'a' && b1 <= 'z')
    {
        b1 -= 0x20u;
    }
    if (b2 >= 'a' && b2 <= 'z')
    {
        b2 -= 0x20u;
    }
    if (b3 >= 'a' && b3 <= 'z')
    {
        b3 -= 0x20u;
    }

    norm = (DWORD)(b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
    if (norm == 0x00424944u || norm == 0x20424944u) // "DIB"
    {
        return 0;
    }
    return norm;
}

size_t yc48_format_frame_size(int w, int h, DWORD format)
{
    DWORD norm;
    size_t row_bytes;
    if (w <= 0 || h <= 0)
    {
        return 0;
    }
    norm = yc48_normalize_format(format);
    if (norm == 0x32595559u) // "YUY2"
    {
        row_bytes = (size_t)(((w + 1) / 2) * 4);
    }
    else if (norm == 0x38344359u) // "YC48"
    {
        row_bytes = (size_t)((w * 6 + 3) & ~3);
    }
    else
    {
        row_bytes = (size_t)((w * 3 + 3) & ~3);
    }
    return row_bytes * (size_t)h;
}

BOOL yc48_convert_to_format(void *dst, const PIXEL_YC *ycp, int yc_stride,
                            int w, int h, DWORD format)
{
    DWORD norm;
    if (!dst || !ycp || w <= 0 || h <= 0)
    {
        return FALSE;
    }
    if (yc_stride <= 0)
    {
        yc_stride = w;
    }

    norm = yc48_normalize_format(format);
    if (norm == 0x32595559u) // "YUY2"
    {
        int stride = ((w + 1) / 2) * 4;
        return yc48_to_yuy2((unsigned char *)dst, stride, ycp, yc_stride, w, h);
    }
    if (norm == 0x38344359u) // "YC48"
    {
        int y;
        int dst_stride = (w * 6 + 3) & ~3;
        int row_raw = w * 6;
        for (y = 0; y < h; y++)
        {
            unsigned char *drow = (unsigned char *)dst + (size_t)y * (size_t)dst_stride;
            memcpy(drow, ycp + (size_t)y * (size_t)yc_stride, (size_t)row_raw);
            if (dst_stride > row_raw)
            {
                memset(drow + row_raw, 0, (size_t)(dst_stride - row_raw));
            }
        }
        return TRUE;
    }
    return yc2rgb_dib24((unsigned char *)dst, (w * 3 + 3) & ~3, ycp, yc_stride, w, h, TRUE);
}
