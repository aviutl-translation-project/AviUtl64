// rgb and yc48 color conversion

#pragma once

#ifndef YC48_CONVERT_H
#define YC48_CONVERT_H

#include <stddef.h>
#include "aviutl.h"

#ifdef __cplusplus
extern "C"
{
#endif

    BOOL __cdecl rgb2yc(PIXEL_YC *ycp, PIXEL *pixelp, int w);
    BOOL __cdecl yc2rgb(PIXEL *pixelp, PIXEL_YC *ycp, int w);

    BOOL rgb2yc_dib24(PIXEL_YC *ycp, int yc_stride,
                      const unsigned char *dib_bgr, int dib_stride,
                      int w, int h, BOOL bottom_up);

    BOOL yc2rgb_dib24(unsigned char *dib_bgr, int dib_stride,
                      const PIXEL_YC *ycp, int yc_stride,
                      int w, int h, BOOL bottom_up);

    // 0x0044fc30 & 0x00450090, packed yuy2 <-> yc48 (bt.601 and bt.709)
    BOOL yuy2_to_yc48(PIXEL_YC *ycp, int yc_stride,
                      const unsigned char *src_yuy2, int src_stride,
                      int w, int h, BOOL bt709);

    BOOL yc48_to_yuy2(unsigned char *dst_yuy2, int dst_stride,
                      const PIXEL_YC *ycp, int yc_stride,
                      int w, int h);

    BOOL yc48_to_yuy2_ex(unsigned char *dst_yuy2, int dst_stride,
                         const PIXEL_YC *ycp, int yc_stride,
                         int w, int h, BOOL bt709);

    // 0x00420e70, format helpers ('DIB ', 'YUY2', 'YC48', and .auc fourccs)
    DWORD yc48_normalize_format(DWORD format);
    size_t yc48_format_frame_size(int w, int h, DWORD format);
    BOOL yc48_convert_to_format(void *dst, const PIXEL_YC *ycp, int yc_stride,
                                int w, int h, DWORD format);

#ifdef __cplusplus
}
#endif

#endif
