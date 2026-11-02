#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin_filters.h"
#include "plugin_manager.h"
#include "color_plugin.h"
#include "yc48_convert.h"
#include "exfunc.h"
#include "i18n.h"
#include "aviutl_dlg.h"
#include "debug_console.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static HWND g_host_main_hwnd = NULL;
static FILTER_DLL g_builtin_dlls[BUILTIN_FILTER_COUNT];
static BOOL g_tables_ready = FALSE;
static int g_frame_interlace_override = 0; // 0=std, 1=invert field, 2=odd, 3=even, 4=blend, 5=auto

// string pool for localized CP932 filter/track/check names
static char g_str_pool[256][128];
static int g_str_pool_used = 0;

static void bf_wide_to_sjis(const wchar_t *src, char *dst, int cap)
{
    if (!dst || cap <= 0)
        return;
    dst[0] = '\0';
    if (!src || !src[0])
        return;
    if (WideCharToMultiByte(932, 0, src, -1, dst, cap, NULL, NULL) <= 0)
    {
        WideCharToMultiByte(CP_ACP, 0, src, -1, dst, cap, NULL, NULL);
    }
}

static void bf_sjis_to_wide(const char *src, wchar_t *dst, int cap)
{
    if (!dst || cap <= 0)
        return;
    dst[0] = L'\0';
    if (!src || !src[0])
        return;
    if (MultiByteToWideChar(932, 0, src, -1, dst, cap) <= 0)
    {
        MultiByteToWideChar(CP_ACP, 0, src, -1, dst, cap);
    }
}

static char *alloc_res_sjis(UINT res_id, const char *fallback)
{
    char *slot;
    const wchar_t *ws = i18n_get_wstr(res_id);
    if (g_str_pool_used >= 256)
        return (char *)(fallback ? fallback : "");
    slot = g_str_pool[g_str_pool_used++];
    if (ws && ws[0])
    {
        bf_wide_to_sjis(ws, slot, 128);
    }
    if (!slot[0] && fallback)
    {
        lstrcpynA(slot, fallback, 128);
    }
    return slot;
}

static char *alloc_interlace_name(UINT prefix_res_id)
{
    char *slot;
    wchar_t wfull[128];
    const wchar_t *wp = i18n_get_wstr(prefix_res_id);
    const wchar_t *ws = i18n_get_wstr(16); // "インターレース解除"
    if (g_str_pool_used >= 256)
        return (char *)"Deinterlace";
    slot = g_str_pool[g_str_pool_used++];
    wsprintfW(wfull, L"%s%s", wp ? wp : L"", ws ? ws : L"");
    bf_wide_to_sjis(wfull, slot, 128);
    return slot;
}

static void bf_get_ini_path(char *out_path, size_t out_sz)
{
    char mod_path[MAX_PATH];
    char *slash;
    if (GetModuleFileNameA(NULL, mod_path, MAX_PATH) > 0)
    {
        slash = strrchr(mod_path, '\\');
        if (slash)
            *(slash + 1) = '\0';
        else
            mod_path[0] = '\0';
        _snprintf(out_path, out_sz, "%saviutl.ini", mod_path);
        out_path[out_sz - 1] = '\0';
    }
    else
    {
        lstrcpynA(out_path, ".\\aviutl.ini", (int)out_sz);
    }
}

static int clamp_i32(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static SHORT clamp_s16(int v)
{
    if (v < -32768)
        return (SHORT)-32768;
    if (v > 32767)
        return (SHORT)32767;
    return (SHORT)v;
}

static FILTER *get_live_builtin_fp(int builtin_idx)
{
    int idx;
    if (builtin_idx < 0 || builtin_idx >= BUILTIN_FILTER_COUNT)
        return NULL;
    if (builtin_idx < plugin_manager_get_count() && plugin_manager_is_builtin(builtin_idx))
    {
        FILTER *fp = plugin_manager_get_filter(builtin_idx);
        if (fp)
            return fp;
    }
    if (g_builtin_dlls[builtin_idx].name)
    {
        idx = plugin_manager_find_by_name(g_builtin_dlls[builtin_idx].name);
        if (idx >= 0)
            return plugin_manager_get_filter(idx);
    }
    return NULL;
}

// parse "640x480,352x480,..." from [system] resizelist
typedef struct ResizePreset
{
    int w;
    int h;
    char label[32];
} ResizePreset;

static int load_resize_presets(ResizePreset *out_list, int max_items)
{
    char ini[MAX_PATH], raw[512];
    const char *p;
    int count = 0;

    bf_get_ini_path(ini, sizeof(ini));
    GetPrivateProfileStringA("system", "resizelist",
                             "640x480,352x480,352x240,320x240", raw, sizeof(raw), ini);
    p = raw;
    while (*p && count < max_items)
    {
        int w = 0, h = 0;
        while (*p == ' ' || *p == ',' || *p == ';')
            p++;
        if (!*p)
            break;
        while (*p >= '0' && *p <= '9')
        {
            w = w * 10 + (*p - '0');
            p++;
        }
        if (*p == 'x' || *p == 'X' || *p == '*')
            p++;
        while (*p >= '0' && *p <= '9')
        {
            h = h * 10 + (*p - '0');
            p++;
        }
        while (*p && *p != ',' && *p != ';')
            p++;
        if (w >= 16 && h >= 16)
        {
            w &= ~1;
            h &= ~1;
            out_list[count].w = w;
            out_list[count].h = h;
            wsprintfA(out_list[count].label, "%dx%d", w, h);
            count++;
        }
    }
    return count;
}

// separable Lanczos-3 PIXEL_YC resampler (used by clipping & resize and size change)

typedef struct LanczosTapSpan
{
    int start;
    int count;
    int weights[16]; // Q14 fixed-point (sum == 16384)
} LanczosTapSpan;

static double lanczos3_kernel(double x)
{
    double ax = fabs(x);
    if (ax < 1e-7)
        return 1.0;
    if (ax >= 3.0)
        return 0.0;
    return (sin(M_PI * x) * sin(M_PI * x / 3.0)) / ((M_PI * x) * (M_PI * x / 3.0));
}

static LanczosTapSpan *build_lanczos_spans(int src_len, int dst_len)
{
    LanczosTapSpan *spans;
    double scale, support, filter_scale;
    int d;

    if (src_len <= 0 || dst_len <= 0)
        return NULL;
    spans = (LanczosTapSpan *)calloc((size_t)dst_len, sizeof(LanczosTapSpan));
    if (!spans)
        return NULL;

    scale = (double)src_len / (double)dst_len;
    filter_scale = (scale > 1.0) ? scale : 1.0;
    support = 3.0 * filter_scale;

    for (d = 0; d < dst_len; d++)
    {
        double center = ((double)d + 0.5) * scale - 0.5;
        int left = (int)floor(center - support);
        int right = (int)ceil(center + support);
        double fweights[16];
        double sum = 0.0;
        int count = 0, s, k, isum = 0, max_k = 0;

        if (right - left + 1 > 16)
        {
            left = (int)floor(center) - 7;
            right = left + 15;
        }
        spans[d].start = left;
        for (s = left; s <= right && count < 16; s++)
        {
            double w = lanczos3_kernel((center - (double)s) / filter_scale);
            fweights[count++] = w;
            sum += w;
        }
        spans[d].count = count;
        if (fabs(sum) < 1e-9)
        {
            spans[d].count = 1;
            spans[d].start = clamp_i32((int)(center + 0.5), 0, src_len - 1);
            spans[d].weights[0] = 16384;
            continue;
        }
        for (k = 0; k < count; k++)
        {
            int iw = (int)floor((fweights[k] / sum) * 16384.0 + 0.5);
            spans[d].weights[k] = iw;
            isum += iw;
            if (fabs(fweights[k]) > fabs(fweights[max_k]))
                max_k = k;
        }
        spans[d].weights[max_k] += (16384 - isum);
    }
    return spans;
}

typedef struct ResizeMtCtx
{
    const PIXEL_YC *src;
    PIXEL_YC *mid;
    PIXEL_YC *dst;
    int src_stride;
    int mid_stride;
    int dst_stride;
    int src_w, src_h;
    int dst_w, dst_h;
    const LanczosTapSpan *x_spans;
    const LanczosTapSpan *y_spans;
} ResizeMtCtx;

static void __cdecl resize_horiz_mt(int tid, int tnum, void *p1, void *p2)
{
    ResizeMtCtx *ctx = (ResizeMtCtx *)p1;
    int y, x, k;
    (void)p2;
    for (y = tid; y < ctx->src_h; y += tnum)
    {
        const PIXEL_YC *srow = ctx->src + (size_t)y * (size_t)ctx->src_stride;
        PIXEL_YC *drow = ctx->mid + (size_t)y * (size_t)ctx->mid_stride;
        for (x = 0; x < ctx->dst_w; x++)
        {
            const LanczosTapSpan *sp = &ctx->x_spans[x];
            int sy = 8192, scb = 8192, scr = 8192;
            for (k = 0; k < sp->count; k++)
            {
                int sx = clamp_i32(sp->start + k, 0, ctx->src_w - 1);
                int w = sp->weights[k];
                sy += (int)srow[sx].y * w;
                scb += (int)srow[sx].cb * w;
                scr += (int)srow[sx].cr * w;
            }
            drow[x].y = clamp_s16(sy >> 14);
            drow[x].cb = clamp_s16(scb >> 14);
            drow[x].cr = clamp_s16(scr >> 14);
        }
    }
}

static void __cdecl resize_vert_mt(int tid, int tnum, void *p1, void *p2)
{
    ResizeMtCtx *ctx = (ResizeMtCtx *)p1;
    int y, x, k;
    (void)p2;
    for (y = tid; y < ctx->dst_h; y += tnum)
    {
        const LanczosTapSpan *sp = &ctx->y_spans[y];
        PIXEL_YC *drow = ctx->dst + (size_t)y * (size_t)ctx->dst_stride;
        for (x = 0; x < ctx->dst_w; x++)
        {
            int sy = 8192, scb = 8192, scr = 8192;
            for (k = 0; k < sp->count; k++)
            {
                int row_idx = clamp_i32(sp->start + k, 0, ctx->src_h - 1);
                const PIXEL_YC *p = &ctx->mid[(size_t)row_idx * (size_t)ctx->mid_stride + (size_t)x];
                int w = sp->weights[k];
                sy += (int)p->y * w;
                scb += (int)p->cb * w;
                scr += (int)p->cr * w;
            }
            drow[x].y = clamp_s16(sy >> 14);
            drow[x].cb = clamp_s16(scb >> 14);
            drow[x].cr = clamp_s16(scr >> 14);
        }
    }
}

static BOOL resize_yc48_lanczos3(FILTER *fp, const PIXEL_YC *src, int src_stride,
                                 int src_w, int src_h,
                                 PIXEL_YC *dst, int dst_stride,
                                 int dst_w, int dst_h)
{
    LanczosTapSpan *xs = NULL, *ys = NULL;
    PIXEL_YC *mid = NULL;
    ResizeMtCtx ctx;

    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return FALSE;

    if (src_w == dst_w && src_h == dst_h)
    {
        int y;
        for (y = 0; y < dst_h; y++)
        {
            memcpy(dst + (size_t)y * (size_t)dst_stride,
                   src + (size_t)y * (size_t)src_stride,
                   (size_t)dst_w * sizeof(PIXEL_YC));
        }
        return TRUE;
    }

    xs = build_lanczos_spans(src_w, dst_w);
    ys = build_lanczos_spans(src_h, dst_h);
    mid = (PIXEL_YC *)malloc((size_t)dst_w * (size_t)src_h * sizeof(PIXEL_YC));
    if (!xs || !ys || !mid)
    {
        free(xs);
        free(ys);
        free(mid);
        return FALSE;
    }

    ZeroMemory(&ctx, sizeof(ctx));
    ctx.src = src;
    ctx.mid = mid;
    ctx.dst = dst;
    ctx.src_stride = src_stride;
    ctx.mid_stride = dst_w;
    ctx.dst_stride = dst_stride;
    ctx.src_w = src_w;
    ctx.src_h = src_h;
    ctx.dst_w = dst_w;
    ctx.dst_h = dst_h;
    ctx.x_spans = xs;
    ctx.y_spans = ys;

    if (fp && fp->exfunc && fp->exfunc->exec_multi_thread_func)
    {
        fp->exfunc->exec_multi_thread_func(resize_horiz_mt, &ctx, NULL);
        fp->exfunc->exec_multi_thread_func(resize_vert_mt, &ctx, NULL);
    }
    else
    {
        resize_horiz_mt(0, 1, &ctx, NULL);
        resize_vert_mt(0, 1, &ctx, NULL);
    }

    free(mid);
    free(ys);
    free(xs);
    return TRUE;
}

// [3] spatial noise reduction (ノイズ除去フィルタ, 0x0043af80 / 0x00435350)

typedef struct SpatialNrCtx
{
    const PIXEL_YC *src;
    PIXEL_YC *dst;
    int stride;
    int w, h;
    int range;
    int thresh_yc;
    int weight_q8;
} SpatialNrCtx;

static int tent_contrib(int diff, int thresh)
{
    int ad = (diff < 0) ? -diff : diff;
    int ex = (ad > thresh) ? (ad - thresh) : 0;
    int mag = ad - 2 * ex;
    if (mag <= 0)
        return 0;
    return (diff < 0) ? -mag : mag;
}

static void __cdecl spatial_nr_mt(int tid, int tnum, void *p1, void *p2)
{
    SpatialNrCtx *c = (SpatialNrCtx *)p1;
    int y, x, dy, dx;
    int r = c->range;
    (void)p2;

    for (y = tid; y < c->h; y += tnum)
    {
        PIXEL_YC *drow = c->dst + (size_t)y * (size_t)c->stride;
        for (x = 0; x < c->w; x++)
        {
            const PIXEL_YC *cp = &c->src[(size_t)y * (size_t)c->stride + (size_t)x];
            int sy = 0, scb = 0, scr = 0;
            for (dy = -r; dy <= r; dy++)
            {
                int ny = clamp_i32(y + dy, 0, c->h - 1);
                const PIXEL_YC *nrow = c->src + (size_t)ny * (size_t)c->stride;
                for (dx = -r; dx <= r; dx++)
                {
                    int nx;
                    const PIXEL_YC *np;
                    if (dx == 0 && dy == 0)
                        continue;
                    // match aviutl 5x5 (21-tap) and 7x7 (37-tap) clipped corner diamond
                    if (r == 2 && abs(dx) == 2 && abs(dy) == 2)
                        continue;
                    if (r == 3 && abs(dx) + abs(dy) > 4)
                        continue;
                    nx = clamp_i32(x + dx, 0, c->w - 1);
                    np = &nrow[nx];
                    sy += tent_contrib((int)np->y - (int)cp->y, c->thresh_yc);
                    scb += tent_contrib((int)np->cb - (int)cp->cb, c->thresh_yc);
                    scr += tent_contrib((int)np->cr - (int)cp->cr, c->thresh_yc);
                }
            }
            drow[x].y = clamp_s16((int)cp->y + ((sy * c->weight_q8) >> 8));
            drow[x].cb = clamp_s16((int)cp->cb + ((scb * c->weight_q8) >> 8));
            drow[x].cr = clamp_s16((int)cp->cr + ((scr * c->weight_q8) >> 8));
        }
    }
}

static BOOL __cdecl proc_spatial_nr(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    static const int k_div[4] = {1, 9, 21, 37};
    SpatialNrCtx ctx;
    PIXEL_YC *tmp;
    int strength, range, thresh;

    if (!fp || !fpip || !fp->track || fpip->w <= 0 || fpip->h <= 0)
        return FALSE;
    strength = fp->track[0];
    range = clamp_i32(fp->track[1], 1, 3);
    thresh = fp->track[2];
    if (strength <= 0 || thresh <= 0)
        return TRUE;

    ctx.src = fpip->ycp_edit;
    ctx.dst = fpip->ycp_temp;
    ctx.stride = fpip->max_w;
    ctx.w = fpip->w;
    ctx.h = fpip->h;
    ctx.range = range;
    ctx.thresh_yc = thresh * 4;
    ctx.weight_q8 = (strength << 8) / k_div[range];

    if (fp->exfunc && fp->exfunc->exec_multi_thread_func)
        fp->exfunc->exec_multi_thread_func(spatial_nr_mt, &ctx, NULL);
    else
        spatial_nr_mt(0, 1, &ctx, NULL);

    tmp = fpip->ycp_edit;
    fpip->ycp_edit = fpip->ycp_temp;
    fpip->ycp_temp = tmp;
    return TRUE;
}

// [4] temporal noise reduction (ノイズ除去(時間軸)フィルタ, 0x0043f170 / 0x0043bed0)

typedef struct TemporalNrCtx
{
    const PIXEL_YC *frames[7];
    int frame_strides[7];
    int frame_w[7];
    int frame_h[7];
    int count;
    int center_idx;
    const PIXEL_YC *cur_src;
    PIXEL_YC *dst;
    int edit_stride;
    int w, h;
    int thresh8;
    int weight_q8;
} TemporalNrCtx;

static int temporal_tent(int diff0, int diff4, int thresh8)
{
    int a0 = abs(diff0);
    int a4 = abs(diff4);
    int e0 = (a0 * 2 > thresh8) ? (a0 * 2 - thresh8) : 0;
    int e4 = (a4 > thresh8 * 2) ? ((a4 - thresh8 * 2) >> 1) : 0;
    int mag = a0 - e0 - e4;
    if (mag <= 0)
        return 0;
    return (diff0 < 0) ? -mag : mag;
}

static void __cdecl temporal_nr_mt(int tid, int tnum, void *p1, void *p2)
{
    TemporalNrCtx *c = (TemporalNrCtx *)p1;
    int y, x, k;
    (void)p2;

    for (y = tid; y < c->h; y += tnum)
    {
        int ym = (y > 0) ? (y - 1) : 0;
        int yp = (y + 1 < c->h) ? (y + 1) : (c->h - 1);
        PIXEL_YC *drow = c->dst + (size_t)y * (size_t)c->edit_stride;

        for (x = 0; x < c->w; x++)
        {
            int xm = (x > 0) ? (x - 1) : 0;
            int xp = (x + 1 < c->w) ? (x + 1) : (c->w - 1);
            const PIXEL_YC *p0 = &c->cur_src[(size_t)y * (size_t)c->edit_stride + (size_t)x];
            int s0_y = (int)c->cur_src[(size_t)ym * (size_t)c->edit_stride + (size_t)x].y +
                       (int)c->cur_src[(size_t)yp * (size_t)c->edit_stride + (size_t)x].y +
                       (int)c->cur_src[(size_t)y * (size_t)c->edit_stride + (size_t)xm].y +
                       (int)c->cur_src[(size_t)y * (size_t)c->edit_stride + (size_t)xp].y;
            int s0_cb = (int)c->cur_src[(size_t)ym * (size_t)c->edit_stride + (size_t)x].cb +
                        (int)c->cur_src[(size_t)yp * (size_t)c->edit_stride + (size_t)x].cb +
                        (int)c->cur_src[(size_t)y * (size_t)c->edit_stride + (size_t)xm].cb +
                        (int)c->cur_src[(size_t)y * (size_t)c->edit_stride + (size_t)xp].cb;
            int s0_cr = (int)c->cur_src[(size_t)ym * (size_t)c->edit_stride + (size_t)x].cr +
                        (int)c->cur_src[(size_t)yp * (size_t)c->edit_stride + (size_t)x].cr +
                        (int)c->cur_src[(size_t)y * (size_t)c->edit_stride + (size_t)xm].cr +
                        (int)c->cur_src[(size_t)y * (size_t)c->edit_stride + (size_t)xp].cr;
            int acc_y = 0, acc_cb = 0, acc_cr = 0;

            for (k = 0; k < c->count; k++)
            {
                const PIXEL_YC *fb = c->frames[k];
                int st = c->frame_strides[k];
                const PIXEL_YC *pk;
                int sk_y, sk_cb, sk_cr;

                if (k == c->center_idx || !fb || x >= c->frame_w[k] || y >= c->frame_h[k])
                    continue;
                pk = &fb[(size_t)y * (size_t)st + (size_t)x];
                sk_y = (int)fb[(size_t)ym * (size_t)st + (size_t)x].y +
                       (int)fb[(size_t)yp * (size_t)st + (size_t)x].y +
                       (int)fb[(size_t)y * (size_t)st + (size_t)xm].y +
                       (int)fb[(size_t)y * (size_t)st + (size_t)xp].y;
                sk_cb = (int)fb[(size_t)ym * (size_t)st + (size_t)x].cb +
                        (int)fb[(size_t)yp * (size_t)st + (size_t)x].cb +
                        (int)fb[(size_t)y * (size_t)st + (size_t)xm].cb +
                        (int)fb[(size_t)y * (size_t)st + (size_t)xp].cb;
                sk_cr = (int)fb[(size_t)ym * (size_t)st + (size_t)x].cr +
                        (int)fb[(size_t)yp * (size_t)st + (size_t)x].cr +
                        (int)fb[(size_t)y * (size_t)st + (size_t)xm].cr +
                        (int)fb[(size_t)y * (size_t)st + (size_t)xp].cr;

                acc_y += temporal_tent((int)pk->y - (int)p0->y, sk_y - s0_y, c->thresh8);
                acc_cb += temporal_tent((int)pk->cb - (int)p0->cb, sk_cb - s0_cb, c->thresh8);
                acc_cr += temporal_tent((int)pk->cr - (int)p0->cr, sk_cr - s0_cr, c->thresh8);
            }

            drow[x].y = clamp_s16((int)p0->y + ((acc_y * c->weight_q8) >> 8));
            drow[x].cb = clamp_s16((int)p0->cb + ((acc_cb * c->weight_q8) >> 8));
            drow[x].cr = clamp_s16((int)p0->cr + ((acc_cr * c->weight_q8) >> 8));
        }
    }
}

static BOOL __cdecl proc_temporal_nr(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    TemporalNrCtx ctx;
    PIXEL_YC *tmp;
    int strength, range, thresh, cache_stride, dt;

    if (!fp || !fpip || !fp->track || !fp->exfunc || fpip->w <= 0 || fpip->h <= 0)
        return FALSE;
    strength = fp->track[0];
    range = clamp_i32(fp->track[1], 1, 3);
    thresh = fp->track[2];
    if (strength <= 0 || thresh <= 0 || fpip->frame_n <= 1)
        return TRUE;

    cache_stride = (fpip->w + 8 + 15) & ~15;
    if (!fp->exfunc->set_ycp_filtering_cache_size(fp, cache_stride, fpip->h, 2 * range + 1, 1))
        return TRUE;

    ZeroMemory(&ctx, sizeof(ctx));
    ctx.count = 2 * range + 1;
    ctx.center_idx = range;
    for (dt = -range; dt <= range; dt++)
    {
        int k = dt + range;
        int tf = clamp_i32(fpip->frame + dt, 0, fpip->frame_n - 1);
        int cw = 0, ch = 0;
        if (dt == 0)
            continue;
        ctx.frames[k] = fp->exfunc->get_ycp_filtering_cache_ex(fp, fpip->editp, tf, &cw, &ch);
        ctx.frame_strides[k] = cache_stride;
        ctx.frame_w[k] = cw;
        ctx.frame_h[k] = ch;
    }

    ctx.cur_src = fpip->ycp_edit;
    ctx.dst = fpip->ycp_temp;
    ctx.edit_stride = fpip->max_w;
    ctx.w = fpip->w;
    ctx.h = fpip->h;
    ctx.thresh8 = thresh * 8;
    ctx.weight_q8 = (strength << 8) / (2 * range + 1);

    if (fp->exfunc->exec_multi_thread_func)
        fp->exfunc->exec_multi_thread_func(temporal_nr_mt, &ctx, NULL);
    else
        temporal_nr_mt(0, 1, &ctx, NULL);

    tmp = fpip->ycp_edit;
    fpip->ycp_edit = fpip->ycp_temp;
    fpip->ycp_temp = tmp;
    return TRUE;
}

// [5] sharpen (シャープフィルタ) & [6] Blur (ぼかしフィルタ)

typedef struct BlurSharpCtx
{
    const PIXEL_YC *src;
    PIXEL_YC *dst;
    int stride;
    int w, h;
    int range;
    int strength;
    int low_yc;
    int span_yc;
    BOOL is_sharpen;
} BlurSharpCtx;

static int band_clamp_delta(int diff, int low_yc, int span_yc)
{
    int ad = (diff < 0) ? -diff : diff;
    int mag = ad - low_yc;
    if (mag <= 0)
        return 0;
    if (mag > span_yc)
        mag = span_yc;
    return (diff < 0) ? -mag : mag;
}

static void __cdecl blur_sharp_mt(int tid, int tnum, void *p1, void *p2)
{
    static const int k_neighbors[4] = {4, 8, 20, 36};
    BlurSharpCtx *c = (BlurSharpCtx *)p1;
    int y, x, dy, dx;
    int r = c->range;
    int n_cnt = k_neighbors[r];
    (void)p2;

    for (y = tid; y < c->h; y += tnum)
    {
        PIXEL_YC *drow = c->dst + (size_t)y * (size_t)c->stride;
        for (x = 0; x < c->w; x++)
        {
            const PIXEL_YC *cp = &c->src[(size_t)y * (size_t)c->stride + (size_t)x];
            int sy = 0, scb = 0, scr = 0;
            int dy_avg, dcb_avg, dcr_avg;

            if (r == 0)
            {
                int xm = (x > 0) ? (x - 1) : 0;
                int xp = (x + 1 < c->w) ? (x + 1) : (c->w - 1);
                int ym = (y > 0) ? (y - 1) : 0;
                int yp = (y + 1 < c->h) ? (y + 1) : (c->h - 1);
                sy = (int)c->src[(size_t)ym * (size_t)c->stride + (size_t)x].y +
                     (int)c->src[(size_t)yp * (size_t)c->stride + (size_t)x].y +
                     (int)c->src[(size_t)y * (size_t)c->stride + (size_t)xm].y +
                     (int)c->src[(size_t)y * (size_t)c->stride + (size_t)xp].y;
                scb = (int)c->src[(size_t)ym * (size_t)c->stride + (size_t)x].cb +
                      (int)c->src[(size_t)yp * (size_t)c->stride + (size_t)x].cb +
                      (int)c->src[(size_t)y * (size_t)c->stride + (size_t)xm].cb +
                      (int)c->src[(size_t)y * (size_t)c->stride + (size_t)xp].cb;
                scr = (int)c->src[(size_t)ym * (size_t)c->stride + (size_t)x].cr +
                      (int)c->src[(size_t)yp * (size_t)c->stride + (size_t)x].cr +
                      (int)c->src[(size_t)y * (size_t)c->stride + (size_t)xm].cr +
                      (int)c->src[(size_t)y * (size_t)c->stride + (size_t)xp].cr;
            }
            else
            {
                for (dy = -r; dy <= r; dy++)
                {
                    int ny = clamp_i32(y + dy, 0, c->h - 1);
                    const PIXEL_YC *nrow = c->src + (size_t)ny * (size_t)c->stride;
                    for (dx = -r; dx <= r; dx++)
                    {
                        int nx;
                        const PIXEL_YC *np;
                        if (dx == 0 && dy == 0)
                            continue;
                        if (r == 2 && abs(dx) == 2 && abs(dy) == 2)
                            continue;
                        if (r == 3 && abs(dx) + abs(dy) > 4)
                            continue;
                        nx = clamp_i32(x + dx, 0, c->w - 1);
                        np = &nrow[nx];
                        sy += (int)np->y;
                        scb += (int)np->cb;
                        scr += (int)np->cr;
                    }
                }
            }

            dy_avg = band_clamp_delta((sy / n_cnt) - (int)cp->y, c->low_yc, c->span_yc);
            dcb_avg = band_clamp_delta((scb / n_cnt) - (int)cp->cb, c->low_yc, c->span_yc);
            dcr_avg = band_clamp_delta((scr / n_cnt) - (int)cp->cr, c->low_yc, c->span_yc);

            if (c->is_sharpen)
            {
                drow[x].y = clamp_s16((int)cp->y - ((dy_avg * c->strength) >> 7));
                drow[x].cb = clamp_s16((int)cp->cb - ((dcb_avg * c->strength) >> 7));
                drow[x].cr = clamp_s16((int)cp->cr - ((dcr_avg * c->strength) >> 7));
            }
            else
            {
                int bscale = (c->strength * n_cnt * 256) / ((n_cnt + 1) * 256);
                drow[x].y = clamp_s16((int)cp->y + ((dy_avg * bscale) >> 8));
                drow[x].cb = clamp_s16((int)cp->cb + ((dcb_avg * bscale) >> 8));
                drow[x].cr = clamp_s16((int)cp->cr + ((dcr_avg * bscale) >> 8));
            }
        }
    }
}

static BOOL run_blur_or_sharpen(FILTER *fp, FILTER_PROC_INFO *fpip, BOOL is_sharpen)
{
    BlurSharpCtx ctx;
    PIXEL_YC *tmp;
    int strength, range, lower, upper;

    if (!fp || !fpip || !fp->track || fpip->w <= 0 || fpip->h <= 0)
        return FALSE;
    strength = fp->track[0];
    range = clamp_i32(fp->track[1], is_sharpen ? 1 : 0, 3);
    lower = fp->track[2];
    upper = fp->track[3];
    if (strength <= 0 || lower >= upper)
        return TRUE;

    ctx.src = fpip->ycp_edit;
    ctx.dst = fpip->ycp_temp;
    ctx.stride = fpip->max_w;
    ctx.w = fpip->w;
    ctx.h = fpip->h;
    ctx.range = range;
    ctx.strength = strength;
    ctx.low_yc = lower * 4;
    ctx.span_yc = (upper - lower) * 4;
    ctx.is_sharpen = is_sharpen;

    if (fp->exfunc && fp->exfunc->exec_multi_thread_func)
        fp->exfunc->exec_multi_thread_func(blur_sharp_mt, &ctx, NULL);
    else
        blur_sharp_mt(0, 1, &ctx, NULL);

    tmp = fpip->ycp_edit;
    fpip->ycp_edit = fpip->ycp_temp;
    fpip->ycp_temp = tmp;
    return TRUE;
}

static BOOL __cdecl proc_sharpen(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    return run_blur_or_sharpen(fp, fpip, TRUE);
}

static BOOL __cdecl proc_blur(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    return run_blur_or_sharpen(fp, fpip, FALSE);
}

// [7] clipping & resize (クリッピング＆リサイズ, 0x0040eaa0 / 0x0040edb0)

typedef struct ClipResizeExData
{
    int w;
    int h;
    int keep_aspect;
} ClipResizeExData;

static ClipResizeExData g_clip_resize_ex = {0, 0, 0};
static const ClipResizeExData g_clip_resize_def = {0, 0, 0};
static HWND g_clip_combo = NULL;
static HWND g_clip_aspect_chk = NULL;
static BOOL g_clip_syncing = FALSE;

#define IDC_CLIP_COMBO 12004
#define IDC_CLIP_ASPECT 12005

static void sync_clip_resize_controls(FILTER *fp)
{
    ClipResizeExData *ex;
    ResizePreset presets[32];
    int pcnt, i;
    wchar_t wtxt[64];

    if (!fp || !fp->ex_data_ptr || !g_clip_combo || !IsWindow(g_clip_combo))
        return;
    ex = (ClipResizeExData *)fp->ex_data_ptr;
    g_clip_syncing = TRUE;

    SendMessageW(g_clip_combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_clip_combo, CB_ADDSTRING, 0, (LPARAM)i18n_get_wstr(12)); // "なし"
    pcnt = load_resize_presets(presets, 32);
    for (i = 0; i < pcnt; i++)
    {
        bf_sjis_to_wide(presets[i].label, wtxt, 64);
        SendMessageW(g_clip_combo, CB_ADDSTRING, 0, (LPARAM)wtxt);
    }

    if (ex->w <= 0 || ex->h <= 0)
    {
        SendMessageW(g_clip_combo, CB_SETCURSEL, 0, 0);
    }
    else
    {
        int matched = -1;
        for (i = 0; i < pcnt; i++)
        {
            if (presets[i].w == ex->w && presets[i].h == ex->h)
            {
                matched = i + 1;
                break;
            }
        }
        if (matched >= 0)
        {
            SendMessageW(g_clip_combo, CB_SETCURSEL, (WPARAM)matched, 0);
        }
        else
        {
            wsprintfW(wtxt, L"%dx%d", ex->w, ex->h);
            SetWindowTextW(g_clip_combo, wtxt);
        }
    }

    if (g_clip_aspect_chk && IsWindow(g_clip_aspect_chk))
    {
        SendMessageW(g_clip_aspect_chk, BM_SETCHECK,
                     ex->keep_aspect ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    g_clip_syncing = FALSE;
}

static BOOL __cdecl upd_clip_resize(FILTER *fp, int status)
{
    (void)status;
    sync_clip_resize_controls(fp);
    return TRUE;
}

static BOOL __cdecl wndproc_clip_resize(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                        void *editp, FILTER *fp)
{
    (void)lp;
    (void)editp;

    switch (msg)
    {
    case WM_FILTER_INIT:
    {
        HINSTANCE hInst = GetModuleHandleA(NULL);
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND hLbl = CreateWindowExW(0, L"STATIC", i18n_get_wstr(34),
                                    WS_CHILD | WS_VISIBLE,
                                    6, 114, 48, 16, hwnd, NULL, hInst, NULL);
        if (hLbl)
            SendMessageW(hLbl, WM_SETFONT, (WPARAM)hFont, 0);

        g_clip_combo = CreateWindowExW(0, L"COMBOBOX", L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWN,
                                       56, 110, 96, 200, hwnd,
                                       (HMENU)(UINT_PTR)IDC_CLIP_COMBO, hInst, NULL);
        if (g_clip_combo)
            SendMessageW(g_clip_combo, WM_SETFONT, (WPARAM)hFont, 0);

        g_clip_aspect_chk = CreateWindowExW(0, L"BUTTON", i18n_get_wstr(63),
                                            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                            158, 113, 150, 16, hwnd,
                                            (HMENU)(UINT_PTR)IDC_CLIP_ASPECT, hInst, NULL);
        if (g_clip_aspect_chk)
            SendMessageW(g_clip_aspect_chk, WM_SETFONT, (WPARAM)hFont, 0);

        sync_clip_resize_controls(fp);
        return FALSE;
    }

    case WM_FILTER_UPDATE:
    case WM_FILTER_CHANGE_PARAM:
        sync_clip_resize_controls(fp);
        return FALSE;

    case WM_COMMAND:
        if (!g_clip_syncing && fp && fp->ex_data_ptr)
        {
            ClipResizeExData *ex = (ClipResizeExData *)fp->ex_data_ptr;
            int id = LOWORD(wp);
            int code = HIWORD(wp);

            if (id == IDC_CLIP_ASPECT && code == BN_CLICKED)
            {
                ex->keep_aspect = (SendMessageW(g_clip_aspect_chk, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
                exfunc_invalidate_all_caches();
                return TRUE;
            }
            if (id == IDC_CLIP_COMBO && (code == CBN_SELCHANGE || code == CBN_EDITCHANGE))
            {
                wchar_t wbuf[64] = {0};
                char buf[64] = {0};
                int w = 0, h = 0;

                if (code == CBN_SELCHANGE)
                {
                    int sel = (int)SendMessageW(g_clip_combo, CB_GETCURSEL, 0, 0);
                    if (sel <= 0)
                    {
                        ex->w = 0;
                        ex->h = 0;
                        exfunc_invalidate_all_caches();
                        return TRUE;
                    }
                    SendMessageW(g_clip_combo, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)wbuf);
                }
                else
                {
                    GetWindowTextW(g_clip_combo, wbuf, 64);
                }
                bf_wide_to_sjis(wbuf, buf, sizeof(buf));
                if (sscanf(buf, "%dx%d", &w, &h) == 2 || sscanf(buf, "%dX%d", &w, &h) == 2)
                {
                    if (w >= 16 && h >= 16)
                    {
                        ex->w = w & ~1;
                        ex->h = h & ~1;
                        exfunc_invalidate_all_caches();
                        return TRUE;
                    }
                }
                else if (!buf[0])
                {
                    ex->w = 0;
                    ex->h = 0;
                    exfunc_invalidate_all_caches();
                    return TRUE;
                }
            }
        }
        break;
    }
    return FALSE;
}

static BOOL __cdecl proc_clip_resize(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    ClipResizeExData *ex;
    int top, bottom, left, right, cw, ch, y;
    int dst_w, dst_h;
    const PIXEL_YC *crop_src;
    PIXEL_YC *tmp;

    if (!fp || !fpip || !fp->track || fpip->w <= 0 || fpip->h <= 0)
        return FALSE;

    ex = (ClipResizeExData *)fp->ex_data_ptr;
    top = clamp_i32(fp->track[0], 0, fpip->h - 4);
    bottom = clamp_i32(fp->track[1], 0, fpip->h - 4 - top);
    left = clamp_i32(fp->track[2], 0, fpip->w - 4);
    right = clamp_i32(fp->track[3], 0, fpip->w - 4 - left);

    cw = (fpip->w - left - right) & ~1;
    ch = (fpip->h - top - bottom) & ~1;
    if (cw < 4)
        cw = 4;
    if (ch < 4)
        ch = 4;

    crop_src = fpip->ycp_edit + (size_t)top * (size_t)fpip->max_w + (size_t)left;
    dst_w = (ex && ex->w >= 16) ? (ex->w & ~1) : cw;
    dst_h = (ex && ex->h >= 16) ? (ex->h & ~1) : ch;
    if (dst_w > fpip->max_w)
        dst_w = fpip->max_w & ~1;
    if (dst_h > fpip->max_h)
        dst_h = fpip->max_h & ~1;

    if (ex && ex->keep_aspect && ex->w >= 16 && ex->h >= 16)
    {
        // preserve cropped aspect ratio inside target dimensions
        if ((long long)cw * dst_h > (long long)ch * dst_w)
        {
            dst_h = (int)(((long long)dst_w * ch + cw / 2) / cw) & ~1;
            if (dst_h < 4)
                dst_h = 4;
        }
        else
        {
            dst_w = (int)(((long long)dst_h * cw + ch / 2) / ch) & ~1;
            if (dst_w < 4)
                dst_w = 4;
        }
    }

    if (top == 0 && bottom == 0 && left == 0 && right == 0 && dst_w == fpip->w && dst_h == fpip->h)
        return TRUE;

    if (dst_w == cw && dst_h == ch)
    {
        for (y = 0; y < ch; y++)
        {
            memcpy(fpip->ycp_temp + (size_t)y * (size_t)fpip->max_w,
                   crop_src + (size_t)y * (size_t)fpip->max_w,
                   (size_t)cw * sizeof(PIXEL_YC));
        }
    }
    else
    {
        if (!resize_yc48_lanczos3(fp, crop_src, fpip->max_w, cw, ch,
                                  fpip->ycp_temp, fpip->max_w, dst_w, dst_h))
        {
            return TRUE;
        }
    }

    tmp = fpip->ycp_edit;
    fpip->ycp_edit = fpip->ycp_temp;
    fpip->ycp_temp = tmp;
    fpip->w = dst_w;
    fpip->h = dst_h;
    return TRUE;
}

// [8] edge fill (縁塗りつぶし, 0x0040f170)

static BOOL __cdecl proc_edge_fill(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    int t, b, l, r, w, h, st, y, x;
    BOOL center_mode, edge_color;
    int src_x0, src_y0, inner_w, inner_h, dst_x0, dst_y0;
    PIXEL_YC *tmp;

    if (!fp || !fpip || !fp->track || !fp->check || fpip->w <= 0 || fpip->h <= 0)
        return FALSE;

    t = fp->track[0];
    b = fp->track[1];
    l = fp->track[2];
    r = fp->track[3];
    if (t == 0 && b == 0 && l == 0 && r == 0)
        return TRUE;

    w = fpip->w;
    h = fpip->h;
    st = fpip->max_w;
    center_mode = (fp->check[0] != 0);
    edge_color = (fp->check[1] != 0);

    src_x0 = clamp_i32(l > 0 ? l : 0, 0, w);
    src_y0 = clamp_i32(t > 0 ? t : 0, 0, h);
    inner_w = clamp_i32(w - (l > 0 ? l : 0) - (r > 0 ? r : 0), 0, w);
    inner_h = clamp_i32(h - (t > 0 ? t : 0) - (b > 0 ? b : 0), 0, h);

    if (center_mode)
    {
        dst_x0 = (w - inner_w) / 2;
        dst_y0 = (h - inner_h) / 2;
    }
    else
    {
        dst_x0 = (l > 0) ? l : (-l);
        dst_y0 = (t > 0) ? t : (-t);
        dst_x0 = clamp_i32(dst_x0, 0, w);
        dst_y0 = clamp_i32(dst_y0, 0, h);
        if (dst_x0 + inner_w > w)
            inner_w = w - dst_x0;
        if (dst_y0 + inner_h > h)
            inner_h = h - dst_y0;
    }

    for (y = 0; y < h; y++)
    {
        memset(fpip->ycp_temp + (size_t)y * (size_t)st, 0, (size_t)w * sizeof(PIXEL_YC));
    }

    if (inner_w > 0 && inner_h > 0)
    {
        for (y = 0; y < inner_h; y++)
        {
            memcpy(fpip->ycp_temp + (size_t)(dst_y0 + y) * (size_t)st + (size_t)dst_x0,
                   fpip->ycp_edit + (size_t)(src_y0 + y) * (size_t)st + (size_t)src_x0,
                   (size_t)inner_w * sizeof(PIXEL_YC));
        }

        if (edge_color)
        {
            for (y = dst_y0; y < dst_y0 + inner_h; y++)
            {
                PIXEL_YC *row = fpip->ycp_temp + (size_t)y * (size_t)st;
                PIXEL_YC lp = row[dst_x0];
                PIXEL_YC rp = row[dst_x0 + inner_w - 1];
                for (x = 0; x < dst_x0; x++)
                    row[x] = lp;
                for (x = dst_x0 + inner_w; x < w; x++)
                    row[x] = rp;
            }
            for (y = 0; y < dst_y0; y++)
            {
                memcpy(fpip->ycp_temp + (size_t)y * (size_t)st,
                       fpip->ycp_temp + (size_t)dst_y0 * (size_t)st,
                       (size_t)w * sizeof(PIXEL_YC));
            }
            for (y = dst_y0 + inner_h; y < h; y++)
            {
                memcpy(fpip->ycp_temp + (size_t)y * (size_t)st,
                       fpip->ycp_temp + (size_t)(dst_y0 + inner_h - 1) * (size_t)st,
                       (size_t)w * sizeof(PIXEL_YC));
            }
        }
    }

    tmp = fpip->ycp_edit;
    fpip->ycp_edit = fpip->ycp_temp;
    fpip->ycp_temp = tmp;
    return TRUE;
}

// [9] color correction (色調補正, 0x0040f6f0 / 0x00448250)

typedef struct ColorCorrCtx
{
    PIXEL_YC *ycp;
    int stride;
    int w, h;
    int y_lut[4097];
    int y_off;
    int y_gain_q14;
    BOOL use_y_lut;
    int c00_q14, c01_q14, c10_q14, c11_q14;
} ColorCorrCtx;

static void __cdecl color_corr_mt(int tid, int tnum, void *p1, void *p2)
{
    ColorCorrCtx *c = (ColorCorrCtx *)p1;
    int y, x;
    (void)p2;

    for (y = tid; y < c->h; y += tnum)
    {
        PIXEL_YC *row = c->ycp + (size_t)y * (size_t)c->stride;
        for (x = 0; x < c->w; x++)
        {
            int yv = (int)row[x].y;
            int cb = (int)row[x].cb;
            int cr = (int)row[x].cr;
            int ny, ncb, ncr;

            if (c->use_y_lut && yv >= 0 && yv <= 4096)
            {
                ny = c->y_lut[yv];
            }
            else
            {
                ny = ((yv * c->y_gain_q14) >> 14) + c->y_off;
            }
            ncb = (cb * c->c00_q14 + cr * c->c01_q14 + 8192) >> 14;
            ncr = (cb * c->c10_q14 + cr * c->c11_q14 + 8192) >> 14;

            row[x].y = clamp_s16(ny);
            row[x].cb = clamp_s16(ncb);
            row[x].cr = clamp_s16(ncr);
        }
    }
}

static BOOL __cdecl proc_color_corr(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    ColorCorrCtx ctx;
    int bright, cont, gamma, luma, sat, hue, i;
    double sat_f, ang, cs, sn, exp_g;

    if (!fp || !fpip || !fp->track || fpip->w <= 0 || fpip->h <= 0)
        return FALSE;
    bright = fp->track[0];
    cont = fp->track[1];
    gamma = fp->track[2];
    luma = fp->track[3];
    sat = fp->track[4];
    hue = fp->track[5];
    if ((bright | cont | gamma | luma | sat | hue) == 0)
        return TRUE;

    ctx.ycp = fpip->ycp_edit;
    ctx.stride = fpip->max_w;
    ctx.w = fpip->w;
    ctx.h = fpip->h;
    ctx.y_off = (((cont + 256) * bright * 16) >> 8) - cont * 8;
    ctx.y_gain_q14 = clamp_i32((luma + 256 + cont) * 64, 0, 0x7fff);
    ctx.use_y_lut = TRUE;

    exp_g = (gamma >= 0) ? (256.0 / (256.0 + (double)gamma))
                         : ((256.0 - (double)gamma) / 256.0);
    for (i = 0; i <= 4096; i++)
    {
        double v = ((double)(i * ctx.y_gain_q14) / 16384.0) + (double)ctx.y_off;
        if (gamma != 0 && v > 0.0)
        {
            double norm = v / 4096.0;
            if (norm < 1.0)
                v = pow(norm, exp_g) * 4096.0;
        }
        ctx.y_lut[i] = clamp_i32((int)floor(v + 0.5), -32768, 32767);
    }

    sat_f = (double)(sat + 256) / 256.0;
    ang = ((double)hue * M_PI) / 256.0;
    cs = cos(ang) * sat_f;
    sn = sin(ang) * sat_f;
    ctx.c00_q14 = (int)floor(cs * 16384.0 + 0.5);
    ctx.c01_q14 = (int)floor(-sn * 16384.0 + 0.5);
    ctx.c10_q14 = (int)floor(sn * 16384.0 + 0.5);
    ctx.c11_q14 = (int)floor(cs * 16384.0 + 0.5);

    if (fp->exfunc && fp->exfunc->exec_multi_thread_func)
        fp->exfunc->exec_multi_thread_func(color_corr_mt, &ctx, NULL);
    else
        color_corr_mt(0, 1, &ctx, NULL);

    return TRUE;
}

// [10] extended color correction (拡張色調補正, 0x0040f820 / 0x0040f740 / 0x004463b0)

static BOOL __cdecl upd_ext_color_corr(FILTER *fp, int status)
{
    if (!fp || !fp->track || !fp->check)
        return FALSE;

    if ((status & 0xffff0000) == FILTER_UPDATE_STATUS_CHECK)
    {
        int cidx = status & 0xffff;
        if (cidx == 1 && fp->check[1] && fp->check[2])
        {
            fp->check[2] = 0;
            if (fp->exfunc && fp->exfunc->filter_window_update)
                fp->exfunc->filter_window_update(fp);
        }
        else if (cidx == 2 && fp->check[2] && fp->check[1])
        {
            fp->check[1] = 0;
            if (fp->exfunc && fp->exfunc->filter_window_update)
                fp->exfunc->filter_window_update(fp);
        }
    }

    // RGB sync (check[0]): tracks 6..8=R, 9..11=G, 12..14=B
    if (fp->check[0] && (status & 0xffff0000) == FILTER_UPDATE_STATUS_TRACK)
    {
        int tidx = status & 0xffff;
        if (tidx >= 6 && tidx <= 14)
        {
            int grp = (tidx - 6) % 3;
            int val = fp->track[tidx];
            fp->track[6 + grp] = val;
            fp->track[9 + grp] = val;
            fp->track[12 + grp] = val;
            if (fp->exfunc && fp->exfunc->filter_window_update)
                fp->exfunc->filter_window_update(fp);
        }
    }
    return TRUE;
}

typedef struct ExtColorCorrCtx
{
    PIXEL_YC *ycp;
    int stride;
    int w, h;
    int y_offs, y_gain_q12;
    int cb_offs, cb_gain_q12;
    int cr_offs, cr_gain_q12;
    BOOL has_rgb;
    int r_lut[4097];
    int g_lut[4097];
    int b_lut[4097];
} ExtColorCorrCtx;

static void build_ext_rgb_lut(int lut[4097], int offs, int gain, int gamm)
{
    int i;
    double exp_g = (gamm >= 0) ? (256.0 / (256.0 + (double)gamm))
                               : ((256.0 - (double)gamm) / 256.0);
    double gf = (double)(gain + 256) / 256.0;
    double of = (double)(offs * 16);

    for (i = 0; i <= 4096; i++)
    {
        double v = (double)i * gf + of;
        if (gamm != 0 && v > 0.0 && v < 4096.0)
        {
            v = pow(v / 4096.0, exp_g) * 4096.0;
        }
        lut[i] = clamp_i32((int)floor(v + 0.5), 0, 4096);
    }
}

static void __cdecl ext_color_corr_mt(int tid, int tnum, void *p1, void *p2)
{
    ExtColorCorrCtx *c = (ExtColorCorrCtx *)p1;
    int y, x;
    (void)p2;

    for (y = tid; y < c->h; y += tnum)
    {
        PIXEL_YC *row = c->ycp + (size_t)y * (size_t)c->stride;
        for (x = 0; x < c->w; x++)
        {
            int yv = (((int)row[x].y + c->y_offs) * c->y_gain_q12 + 2048) >> 12;
            int cb = (((int)row[x].cb + c->cb_offs) * c->cb_gain_q12 + 2048) >> 12;
            int cr = (((int)row[x].cr + c->cr_offs) * c->cr_gain_q12 + 2048) >> 12;

            if (c->has_rgb)
            {
                // BT.601 12-bit YC48 <-> RGB12 (exact inverse of aviutl's yc2rgb/rgb2yc)
                int r12 = clamp_i32(yv + ((5743 * cr + 2048) >> 12), 0, 4096);
                int g12 = clamp_i32(yv - ((1410 * cb + 2925 * cr + 2048) >> 12), 0, 4096);
                int b12 = clamp_i32(yv + ((7258 * cb + 2048) >> 12), 0, 4096);

                r12 = c->r_lut[r12];
                g12 = c->g_lut[g12];
                b12 = c->b_lut[b12];

                yv = (1225 * r12 + 2404 * g12 + 467 * b12 + 2048) >> 12;
                cb = (-691 * r12 - 1357 * g12 + 2048 * b12 + 2048) >> 12;
                cr = (2048 * r12 - 1715 * g12 - 333 * b12 + 2048) >> 12;
            }

            row[x].y = clamp_s16(yv);
            row[x].cb = clamp_s16(cb);
            row[x].cr = clamp_s16(cr);
        }
    }
}

static BOOL __cdecl proc_ext_color_corr(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    ExtColorCorrCtx ctx;
    int i, any_nz = 0;

    if (!fp || !fpip || !fp->track || !fp->check || fpip->w <= 0 || fpip->h <= 0)
        return FALSE;
    for (i = 0; i < 15; i++)
        any_nz |= fp->track[i];
    if (!any_nz && !fp->check[1] && !fp->check[2])
        return TRUE;

    ZeroMemory(&ctx, sizeof(ctx));
    ctx.ycp = fpip->ycp_edit;
    ctx.stride = fpip->max_w;
    ctx.w = fpip->w;
    ctx.h = fpip->h;

    ctx.y_offs = fp->track[0] * 16;
    ctx.y_gain_q12 = (fp->track[1] + 256) * 16;
    ctx.cb_offs = fp->track[2] * 8;
    ctx.cb_gain_q12 = (fp->track[3] + 256) * 16;
    ctx.cr_offs = fp->track[4] * 8;
    ctx.cr_gain_q12 = (fp->track[5] + 256) * 16;

    // 0x0040f820 exact TV->PC and PC->TV scale presets
    if (fp->check[1]) // TV -> PC
    {
        ctx.y_offs -= 0x100;
        ctx.y_gain_q12 = (ctx.y_gain_q12 * 255 + 109) / 219;
        ctx.cb_gain_q12 = (ctx.cb_gain_q12 * 255 + 112) / 224;
        ctx.cr_gain_q12 = (ctx.cr_gain_q12 * 255 + 112) / 224;
    }
    else if (fp->check[2]) // PC -> TV
    {
        ctx.y_gain_q12 = (ctx.y_gain_q12 * 219 + 127) / 255;
        ctx.y_offs += 0x100;
        ctx.cb_gain_q12 = (ctx.cb_gain_q12 * 224 + 127) / 255;
        ctx.cr_gain_q12 = (ctx.cr_gain_q12 * 224 + 127) / 255;
    }

    ctx.has_rgb = (fp->track[6] | fp->track[7] | fp->track[8] |
                   fp->track[9] | fp->track[10] | fp->track[11] |
                   fp->track[12] | fp->track[13] | fp->track[14]) != 0;
    if (ctx.has_rgb)
    {
        build_ext_rgb_lut(ctx.r_lut, fp->track[6], fp->track[7], fp->track[8]);
        build_ext_rgb_lut(ctx.g_lut, fp->track[9], fp->track[10], fp->track[11]);
        build_ext_rgb_lut(ctx.b_lut, fp->track[12], fp->track[13], fp->track[14]);
    }

    if (fp->exfunc && fp->exfunc->exec_multi_thread_func)
        fp->exfunc->exec_multi_thread_func(ext_color_corr_mt, &ctx, NULL);
    else
        ext_color_corr_mt(0, 1, &ctx, NULL);

    return TRUE;
}

// [11] audio position (音声の位置調整), [12] Volume (音量の調整), [13] Maximize Volume

static BOOL __cdecl wndproc_audio_pos(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      void *editp, FILTER *fp)
{
    (void)wp;
    (void)lp;

    if (msg == WM_FILTER_UPDATE || msg == WM_FILTER_CHANGE_PARAM ||
        msg == WM_FILTER_FILE_OPEN || msg == WM_FILTER_FILE_CLOSE)
    {
        if (hwnd && IsWindow(hwnd))
            InvalidateRect(hwnd, NULL, TRUE);
        return FALSE;
    }
    if (msg == WM_PAINT && hwnd && fp && fp->track)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        int rate = 30, scale = 1, ms = 0;
        char txt[64];
        HFONT oldf;

        GetClientRect(hwnd, &rc);
        if (editp)
            editp_get_fps((EDITP *)editp, &rate, &scale);
        if (rate <= 0)
            rate = 30;
        if (scale <= 0)
            scale = 1;
        ms = (int)(((long long)fp->track[0] * 1000LL * (long long)scale) / (long long)rate);
        wsprintfA(txt, "%dms", ms);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        oldf = (HFONT)SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
        rc.top = rc.bottom - 18;
        rc.right -= 8;
        DrawTextA(hdc, txt, -1, &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldf);
        EndPaint(hwnd, &ps);
        return FALSE;
    }
    return FALSE;
}

int builtin_filters_get_audio_delay_frames(void)
{
    FILTER *fp = get_live_builtin_fp(11);
    if (!fp || !(fp->flag & FILTER_FLAG_ACTIVE) || !fp->track)
        return 0;
    return fp->track[0];
}

static BOOL __cdecl proc_audio_vol(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    int gain, total, i;
    if (!fp || !fpip || !fp->track || !fpip->audiop || fpip->audio_n <= 0)
        return TRUE;
    gain = fp->track[0] + 256;
    if (gain == 256)
        return TRUE;
    total = fpip->audio_n * (fpip->audio_ch > 0 ? fpip->audio_ch : 2);
    for (i = 0; i < total; i++)
    {
        fpip->audiop[i] = clamp_s16(((int)fpip->audiop[i] * gain) >> 8);
    }
    return TRUE;
}

static int g_max_vol_peak = 0;

static BOOL __cdecl save_start_max_vol(FILTER *fp, int s, int e, void *editp)
{
    short *buf;
    int f, peak = 0;

    g_max_vol_peak = 0;
    if (!fp || !fp->exfunc || !fp->exfunc->get_audio_filtering || !editp)
        return TRUE;
    if (!(fp->flag & FILTER_FLAG_ACTIVE))
        return TRUE;

    buf = (short *)malloc(65536 * sizeof(short));
    if (!buf)
        return TRUE;

    for (f = s; f <= e; f++)
    {
        BYTE st = 0;
        int smp, ch, total, i;
        if (editp_get_frame_status((EDITP *)editp, f, &st) && (st & EDIT_FRAME_EDIT_FLAG_DELFRAME))
            continue;
        smp = fp->exfunc->get_audio_filtering(fp, editp, f, buf);
        ch = editp_get_audio_ch((EDITP *)editp);
        total = smp * (ch > 0 ? ch : 2);
        if (total > 65536)
            total = 65536;
        for (i = 0; i < total; i++)
        {
            int av = abs((int)buf[i]);
            if (av > peak)
                peak = av;
        }
    }
    free(buf);
    g_max_vol_peak = peak;
    return TRUE;
}

static BOOL __cdecl save_end_max_vol(FILTER *fp, void *editp)
{
    (void)fp;
    (void)editp;
    g_max_vol_peak = 0;
    return TRUE;
}

static BOOL __cdecl proc_max_vol(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    int level, total, i;
    long long num, den;

    if (!fp || !fpip || !fp->track || !fpip->audiop || fpip->audio_n <= 0)
        return TRUE;
    if (!fp->exfunc || !fp->exfunc->is_saving || !fp->exfunc->is_saving(fpip->editp))
        return TRUE;
    level = fp->track[0];
    if (level <= 0 || g_max_vol_peak <= 0)
        return TRUE;

    num = 32767LL * (long long)level;
    den = (long long)g_max_vol_peak * 256LL;
    total = fpip->audio_n * (fpip->audio_ch > 0 ? fpip->audio_ch : 2);
    for (i = 0; i < total; i++)
    {
        long long v = ((long long)fpip->audiop[i] * num) / den;
        fpip->audiop[i] = clamp_s16((int)v);
    }
    return TRUE;
}

// [14] size change (サイズの変更, 0x00454050 / 0x00453f30 + "SIZE" dialog)

typedef struct SizeChangeExData
{
    int w;
    int h;
} SizeChangeExData;

static SizeChangeExData g_size_change_ex = {640, 480};
static char g_size_check_labels[40][64];
static char *g_size_check_ptrs[40];
static int g_size_check_defs[40];
static ResizePreset g_size_presets[36];
static int g_size_preset_count = 0;
static int g_size_last_valid_sel = 0;

static void rebuild_size_change_items(void)
{
    int i, n = 0;
    const wchar_t *w_none = i18n_get_wstr(12);
    const wchar_t *w_custom = i18n_get_wstr(101);
    char custom_base[48];

    bf_wide_to_sjis(w_none, g_size_check_labels[n], 64);
    g_size_check_ptrs[n] = g_size_check_labels[n];
    g_size_check_defs[n] = 1;
    n++;

    g_size_preset_count = load_resize_presets(g_size_presets, 36);
    for (i = 0; i < g_size_preset_count && n < 38; i++)
    {
        lstrcpynA(g_size_check_labels[n], g_size_presets[i].label, 64);
        g_size_check_ptrs[n] = g_size_check_labels[n];
        g_size_check_defs[n] = 0;
        n++;
    }

    bf_wide_to_sjis(w_custom, custom_base, sizeof(custom_base));
    if (g_size_change_ex.w >= 16 && g_size_change_ex.h >= 16)
    {
        _snprintf(g_size_check_labels[n], 64, "%s (%dx%d)",
                  custom_base, g_size_change_ex.w, g_size_change_ex.h);
        g_size_check_labels[n][63] = '\0';
    }
    else
    {
        lstrcpynA(g_size_check_labels[n], custom_base, 64);
    }
    g_size_check_ptrs[n] = g_size_check_labels[n];
    g_size_check_defs[n] = 0;
    n++;

    g_builtin_dlls[14].check_n = n;
    g_builtin_dlls[14].check_name = g_size_check_ptrs;
    g_builtin_dlls[14].check_default = g_size_check_defs;
}

static BOOL __cdecl upd_size_change(FILTER *fp, int status)
{
    int sel = -1, i;
    if (!fp || !fp->check || fp->check_n <= 0)
        return FALSE;

    for (i = 0; i < fp->check_n; i++)
    {
        if (fp->check[i])
        {
            sel = i;
            break;
        }
    }

    // last item ("指定サイズ") opens "SIZE" dialog
    if ((status & 0xffff0000) == FILTER_UPDATE_STATUS_CHECK && sel == fp->check_n - 1)
    {
        AviUtlDlgState st;
        SizeChangeExData *ex = (SizeChangeExData *)fp->ex_data_ptr;
        HWND owner = (g_host_main_hwnd && IsWindow(g_host_main_hwnd)) ? g_host_main_hwnd : NULL;

        aviutl_dlg_init(&st);
        st.ints[0] = (ex && ex->w >= 16) ? ex->w : 640;
        st.ints[1] = (ex && ex->h >= 16) ? ex->h : 480;
        if (aviutl_dlg_run(owner, "SIZE", &st) == IDOK &&
            st.ints[0] >= 16 && st.ints[1] >= 16)
        {
            if (ex)
            {
                ex->w = st.ints[0] & ~1;
                ex->h = st.ints[1] & ~1;
                g_size_change_ex = *ex;
            }
            g_size_last_valid_sel = sel;
            rebuild_size_change_items();
        }
        else
        {
            for (i = 0; i < fp->check_n; i++)
                fp->check[i] = (i == g_size_last_valid_sel) ? 1 : 0;
        }
    }
    else if (sel >= 0)
    {
        g_size_last_valid_sel = sel;
    }

    builtin_filters_refresh_tool_window();
    return TRUE;
}

static BOOL __cdecl proc_size_change(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    int sel = 0, i, dst_w = 0, dst_h = 0;
    PIXEL_YC *tmp;

    if (!fp || !fpip || !fp->check || fpip->w <= 0 || fpip->h <= 0)
        return FALSE;
    for (i = 0; i < fp->check_n; i++)
    {
        if (fp->check[i])
        {
            sel = i;
            break;
        }
    }
    if (sel <= 0)
        return TRUE;

    if (sel - 1 < g_size_preset_count)
    {
        dst_w = g_size_presets[sel - 1].w;
        dst_h = g_size_presets[sel - 1].h;
    }
    else if (fp->ex_data_ptr)
    {
        SizeChangeExData *ex = (SizeChangeExData *)fp->ex_data_ptr;
        dst_w = ex->w;
        dst_h = ex->h;
    }
    dst_w = clamp_i32(dst_w & ~1, 0, fpip->max_w & ~1);
    dst_h = clamp_i32(dst_h & ~1, 0, fpip->max_h & ~1);
    if (dst_w < 16 || dst_h < 16 || (dst_w == fpip->w && dst_h == fpip->h))
        return TRUE;

    if (!resize_yc48_lanczos3(fp, fpip->ycp_edit, fpip->max_w, fpip->w, fpip->h,
                              fpip->ycp_temp, fpip->max_w, dst_w, dst_h))
    {
        return TRUE;
    }
    tmp = fpip->ycp_edit;
    fpip->ycp_edit = fpip->ycp_temp;
    fpip->ycp_temp = tmp;
    fpip->w = dst_w;
    fpip->h = dst_h;
    return TRUE;
}

// [15] framerate change (フレームレートの変更) & [16] Framerate Settings

static BOOL __cdecl upd_framerate_change(FILTER *fp, int status)
{
    (void)fp;
    (void)status;
    builtin_filters_refresh_tool_window();
    return TRUE;
}

int builtin_filters_get_framerate_mode(BOOL *out_use_auto24)
{
    FILTER *fp15 = get_live_builtin_fp(15);
    FILTER *fp16 = get_live_builtin_fp(16);
    int mode = 0, i;

    if (out_use_auto24)
        *out_use_auto24 = (fp16 && fp16->check && fp16->check_n > 0 && fp16->check[0] != 0);

    if (fp15 && fp15->check)
    {
        for (i = 0; i < fp15->check_n && i < 6; i++)
        {
            if (fp15->check[i])
            {
                mode = i;
                break;
            }
        }
    }
    return mode;
}

BOOL builtin_filters_is_framerate_kept_frame(int rel_frame)
{
    int mode = builtin_filters_get_framerate_mode(NULL);
    int m5, m3;
    if (rel_frame < 0)
        rel_frame = 0;
    m5 = rel_frame % 5;
    m3 = rel_frame % 3;

    switch (mode)
    {
    case 1: // 4/5 (24 <- 30)
        return m5 != 4;
    case 2: // 2/3 (20 <- 30)
        return m3 != 2;
    case 3: // 1/2 (15 <- 30)
        return (rel_frame % 2) == 0;
    case 4: // 2/5 (12 <- 30)
        return (m5 == 0 || m5 == 2);
    case 5: // 1/3 (10 <- 30)
        return m3 == 0;
    default:
        return TRUE;
    }
}

static long long gcd_ll(long long a, long long b)
{
    while (b != 0)
    {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a > 0 ? a : 1;
}

void builtin_filters_adjust_export_fps(int *inout_rate, int *inout_scale)
{
    static const int k_num[6] = {1, 4, 2, 1, 2, 1};
    static const int k_den[6] = {1, 5, 3, 2, 5, 3};
    int mode = builtin_filters_get_framerate_mode(NULL);
    long long r, s, g;

    if (!inout_rate || !inout_scale || mode <= 0 || mode >= 6)
        return;
    r = (long long)(*inout_rate > 0 ? *inout_rate : 30) * k_num[mode];
    s = (long long)(*inout_scale > 0 ? *inout_scale : 1) * k_den[mode];
    g = gcd_ll(r, s);
    r /= g;
    s /= g;
    if (r > 0 && s > 0 && r <= 0x7fffffffLL && s <= 0x7fffffffLL)
    {
        *inout_rate = (int)r;
        *inout_scale = (int)s;
    }
}

// [17] deinterlace, [18] field order, and [19..24] interlace sub-filters

static unsigned char g_deinterlace_ex[256];
static const unsigned char g_deinterlace_def[256] = {0};

int builtin_filters_get_deinterlace_mode(void)
{
    FILTER *fp = get_live_builtin_fp(17);
    int i;
    if (!fp || !fp->check)
        return 0;
    for (i = 0; i < fp->check_n && i < 6; i++)
    {
        if (fp->check[i])
            return i;
    }
    return 0;
}

void builtin_filters_set_deinterlace_mode(int mode)
{
    int i;
    if (mode < 0 || mode >= 6)
        return;
    plugin_manager_select_config_check(17, mode);
    (void)i;
}

BOOL builtin_filters_is_top_field_first(void)
{
    FILTER *fp = get_live_builtin_fp(18);
    if (!fp || !fp->check || fp->check_n < 2)
        return TRUE;
    return fp->check[1] != 0;
}

void builtin_filters_set_frame_interlace_override(int ovr_mode)
{
    g_frame_interlace_override = clamp_i32(ovr_mode, 0, 5);
}

static int get_effective_interlace_subindex(BOOL *out_tff)
{
    int mode = builtin_filters_get_deinterlace_mode();
    BOOL tff = builtin_filters_is_top_field_first();

    if (g_frame_interlace_override == 1)
        tff = !tff;
    else if (g_frame_interlace_override == 2)
        mode = 1; // odd
    else if (g_frame_interlace_override == 3)
        mode = 2; // even
    else if (g_frame_interlace_override == 4)
        mode = 3; // blend
    else if (g_frame_interlace_override == 5)
        mode = 5; // auto

    if (out_tff)
        *out_tff = tff;
    return 19 + clamp_i32(mode, 0, 5);
}

static BOOL __cdecl upd_deinterlace(FILTER *fp, int status)
{
    (void)fp;
    (void)status;
    builtin_filters_refresh_tool_window();
    return TRUE;
}

static BOOL __cdecl upd_field_order(FILTER *fp, int status)
{
    (void)fp;
    (void)status;
    exfunc_invalidate_all_caches();
    return TRUE;
}

static BOOL __cdecl upd_interlace_odd_even(FILTER *fp, int status)
{
    (void)fp;
    (void)status;
    exfunc_invalidate_all_caches();
    return TRUE;
}

static BOOL __cdecl proc_interlace_none(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    (void)fp;
    (void)fpip;
    return TRUE;
}

// interpolate odd (keep_parity=1 -> keep odd rows 1,3,5..) or even (keep_parity=0 -> keep even rows 0,2,4..)
static BOOL run_odd_or_even_deinterlace(FILTER *fp, FILTER_PROC_INFO *fpip, int sub_idx, int keep_parity)
{
    int w, h, st, y, x;
    BOOL pos_interp, adapt_interp;

    if (!fp || !fpip || fpip->w <= 0 || fpip->h < 2)
        return TRUE;
    if (get_effective_interlace_subindex(NULL) != sub_idx)
        return TRUE;

    w = fpip->w;
    h = fpip->h;
    st = fpip->max_w;
    pos_interp = (fp->check && fp->check_n > 0) ? (fp->check[0] != 0) : TRUE;
    adapt_interp = (fp->check && fp->check_n > 1) ? (fp->check[1] != 0) : TRUE;

    for (y = 0; y < h; y++)
    {
        if ((y & 1) == keep_parity)
            continue;
        {
            int ya = (y > 0) ? (y - 1) : ((1 < h) ? 1 : 0);
            int yb = (y + 1 < h) ? (y + 1) : ((y > 0) ? (y - 1) : 0);
            PIXEL_YC *cur = fpip->ycp_edit + (size_t)y * (size_t)st;
            const PIXEL_YC *ra = fpip->ycp_edit + (size_t)ya * (size_t)st;
            const PIXEL_YC *rb = fpip->ycp_edit + (size_t)yb * (size_t)st;

            if (!pos_interp)
            {
                memcpy(cur, ra, (size_t)w * sizeof(PIXEL_YC));
                continue;
            }
            for (x = 0; x < w; x++)
            {
                if (adapt_interp)
                {
                    int comb = abs(((int)ra[x].y + (int)rb[x].y) / 2 - (int)cur[x].y);
                    if (comb < 48)
                        continue;
                }
                cur[x].y = (SHORT)(((int)ra[x].y + (int)rb[x].y + 1) >> 1);
                cur[x].cb = (SHORT)(((int)ra[x].cb + (int)rb[x].cb + 1) >> 1);
                cur[x].cr = (SHORT)(((int)ra[x].cr + (int)rb[x].cr + 1) >> 1);
            }
        }
    }
    return TRUE;
}

static BOOL __cdecl proc_interlace_odd(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    return run_odd_or_even_deinterlace(fp, fpip, 20, 1);
}

static BOOL __cdecl proc_interlace_even(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    return run_odd_or_even_deinterlace(fp, fpip, 21, 0);
}

static BOOL __cdecl proc_interlace_blend(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    int w, h, st, y, x;
    PIXEL_YC *tmp;

    if (!fp || !fpip || fpip->w <= 0 || fpip->h < 2)
        return TRUE;
    if (get_effective_interlace_subindex(NULL) != 22)
        return TRUE;

    w = fpip->w;
    h = fpip->h;
    st = fpip->max_w;
    for (y = 0; y < h; y++)
    {
        int ym = (y > 0) ? (y - 1) : 0;
        int yp = (y + 1 < h) ? (y + 1) : (h - 1);
        const PIXEL_YC *rm = fpip->ycp_edit + (size_t)ym * (size_t)st;
        const PIXEL_YC *rc = fpip->ycp_edit + (size_t)y * (size_t)st;
        const PIXEL_YC *rp = fpip->ycp_edit + (size_t)yp * (size_t)st;
        PIXEL_YC *dst = fpip->ycp_temp + (size_t)y * (size_t)st;
        for (x = 0; x < w; x++)
        {
            dst[x].y = (SHORT)(((int)rm[x].y + 2 * (int)rc[x].y + (int)rp[x].y + 2) >> 2);
            dst[x].cb = (SHORT)(((int)rm[x].cb + 2 * (int)rc[x].cb + (int)rp[x].cb + 2) >> 2);
            dst[x].cr = (SHORT)(((int)rm[x].cr + 2 * (int)rc[x].cr + (int)rp[x].cr + 2) >> 2);
        }
    }
    tmp = fpip->ycp_edit;
    fpip->ycp_edit = fpip->ycp_temp;
    fpip->ycp_temp = tmp;
    return TRUE;
}

static BOOL __cdecl proc_interlace_auto24(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    BOOL tff = TRUE;
    int thresh, blend_comb, w, h, st, y, x;
    const PIXEL_YC *prev_src = NULL;

    if (!fp || !fpip || fpip->w <= 0 || fpip->h < 2)
        return TRUE;
    if (get_effective_interlace_subindex(&tff) != 23)
        return TRUE;

    thresh = (fp->track && fp->track_n > 0) ? (fp->track[0] * 4) : 256;
    blend_comb = (fp->check && fp->check_n > 0) ? fp->check[0] : 0;
    w = fpip->w;
    h = fpip->h;
    st = fpip->max_w;

    if (fp->exfunc && fp->exfunc->get_ycp_source_cache && fpip->frame > 0)
    {
        prev_src = (const PIXEL_YC *)fp->exfunc->get_ycp_source_cache(fpip->editp, fpip->frame - 1, 0);
    }

    for (y = 0; y < h; y++)
    {
        int drop_parity = tff ? 1 : 0;
        if ((y & 1) == drop_parity)
        {
            int ya = (y > 0) ? (y - 1) : 0;
            int yb = (y + 1 < h) ? (y + 1) : (h - 1);
            PIXEL_YC *cur = fpip->ycp_edit + (size_t)y * (size_t)st;
            const PIXEL_YC *ra = fpip->ycp_edit + (size_t)ya * (size_t)st;
            const PIXEL_YC *rb = fpip->ycp_edit + (size_t)yb * (size_t)st;
            const PIXEL_YC *pr = prev_src ? (prev_src + (size_t)y * (size_t)st) : NULL;

            for (x = 0; x < w; x++)
            {
                int mid_y = ((int)ra[x].y + (int)rb[x].y) >> 1;
                int comb = abs((int)cur[x].y - mid_y);
                if (comb <= thresh)
                    continue;
                if (pr && abs((int)pr[x].y - mid_y) < comb / 2)
                {
                    cur[x] = pr[x];
                }
                else if (blend_comb)
                {
                    cur[x].y = (SHORT)(((int)cur[x].y + mid_y + 1) >> 1);
                    cur[x].cb = (SHORT)(((int)cur[x].cb + (((int)ra[x].cb + (int)rb[x].cb) >> 1) + 1) >> 1);
                    cur[x].cr = (SHORT)(((int)cur[x].cr + (((int)ra[x].cr + (int)rb[x].cr) >> 1) + 1) >> 1);
                }
                else
                {
                    cur[x].y = (SHORT)mid_y;
                    cur[x].cb = (SHORT)(((int)ra[x].cb + (int)rb[x].cb + 1) >> 1);
                    cur[x].cr = (SHORT)(((int)ra[x].cr + (int)rb[x].cr + 1) >> 1);
                }
            }
        }
    }
    return TRUE;
}

static BOOL __cdecl proc_interlace_auto(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    BOOL tff = TRUE;
    int w, h, st, y, x, drop_parity;
    const PIXEL_YC *prev_src = NULL;

    if (!fp || !fpip || fpip->w <= 0 || fpip->h < 2)
        return TRUE;
    if (get_effective_interlace_subindex(&tff) != 24)
        return TRUE;

    w = fpip->w;
    h = fpip->h;
    st = fpip->max_w;
    drop_parity = tff ? 1 : 0;

    if (fp->exfunc && fp->exfunc->get_ycp_source_cache && fpip->frame > 0)
    {
        prev_src = (const PIXEL_YC *)fp->exfunc->get_ycp_source_cache(fpip->editp, fpip->frame - 1, 0);
    }

    for (y = 0; y < h; y++)
    {
        int ya, yb;
        PIXEL_YC *cur;
        const PIXEL_YC *ra, *rb, *pr;

        if ((y & 1) != drop_parity)
            continue;
        ya = (y > 0) ? (y - 1) : 0;
        yb = (y + 1 < h) ? (y + 1) : (h - 1);
        cur = fpip->ycp_edit + (size_t)y * (size_t)st;
        ra = fpip->ycp_edit + (size_t)ya * (size_t)st;
        rb = fpip->ycp_edit + (size_t)yb * (size_t)st;
        pr = prev_src ? (prev_src + (size_t)y * (size_t)st) : NULL;

        for (x = 0; x < w; x++)
        {
            int mid_y = ((int)ra[x].y + (int)rb[x].y) >> 1;
            int comb = abs((int)cur[x].y - mid_y);
            int motion = pr ? abs((int)cur[x].y - (int)pr[x].y) : comb;
            if (comb > 96 && motion > 64)
            {
                cur[x].y = (SHORT)mid_y;
                cur[x].cb = (SHORT)(((int)ra[x].cb + (int)rb[x].cb + 1) >> 1);
                cur[x].cr = (SHORT)(((int)ra[x].cr + (int)rb[x].cr + 1) >> 1);
            }
        }
    }
    return TRUE;
}

// [25] & [26] color conversion settings (色変換の設定 [入力] / [出力], 0x00424720)

static char g_col_in_labels[COLOR_PLUGIN_MAX + 2][128];
static char *g_col_in_ptrs[COLOR_PLUGIN_MAX + 2];
static int g_col_in_defs[COLOR_PLUGIN_MAX + 2];

static char g_col_out_labels[COLOR_PLUGIN_MAX + 2][128];
static char *g_col_out_ptrs[COLOR_PLUGIN_MAX + 2];
static int g_col_out_defs[COLOR_PLUGIN_MAX + 2];

static void rebuild_color_conv_items(void)
{
    int i, cnt = color_plugin_get_count();
    int sel_in = color_plugin_get_selected_input();
    int sel_out = color_plugin_get_selected_output();
    int n_in = 0, n_out = 0;

    bf_wide_to_sjis(i18n_get_wstr(137), g_col_in_labels[0], 128);
    g_col_in_ptrs[0] = g_col_in_labels[0];
    g_col_in_defs[0] = -1; // grayed header "[ 入力 ]"
    n_in = 1;

    bf_wide_to_sjis(i18n_get_wstr(138), g_col_out_labels[0], 128);
    g_col_out_ptrs[0] = g_col_out_labels[0];
    g_col_out_defs[0] = -1; // grayed header "[ 出力 ]"
    n_out = 1;

    for (i = 0; i < cnt && n_in < COLOR_PLUGIN_MAX + 1; i++)
    {
        ColorPluginSummary sum;
        if (!color_plugin_get_summary(i, &sum))
            continue;
        lstrcpynA(g_col_in_labels[n_in], sum.name, 128);
        g_col_in_ptrs[n_in] = g_col_in_labels[n_in];
        g_col_in_defs[n_in] = (i == sel_in) ? 1 : 0;
        n_in++;

        lstrcpynA(g_col_out_labels[n_out], sum.name, 128);
        g_col_out_ptrs[n_out] = g_col_out_labels[n_out];
        g_col_out_defs[n_out] = (i == sel_out) ? 1 : 0;
        n_out++;
    }

    g_builtin_dlls[25].check_n = n_in;
    g_builtin_dlls[25].check_name = g_col_in_ptrs;
    g_builtin_dlls[25].check_default = g_col_in_defs;

    g_builtin_dlls[26].check_n = n_out;
    g_builtin_dlls[26].check_name = g_col_out_ptrs;
    g_builtin_dlls[26].check_default = g_col_out_defs;
}

static BOOL __cdecl upd_color_conv_in(FILTER *fp, int status)
{
    int i;
    (void)status;
    if (fp && fp->check)
    {
        for (i = 1; i < fp->check_n; i++)
        {
            if (fp->check[i])
            {
                color_plugin_set_selected_input(i - 1);
                break;
            }
        }
    }
    exfunc_invalidate_all_caches();
    return TRUE;
}

static BOOL __cdecl upd_color_conv_out(FILTER *fp, int status)
{
    int i;
    (void)status;
    if (fp && fp->check)
    {
        for (i = 1; i < fp->check_n; i++)
        {
            if (fp->check[i])
            {
                color_plugin_set_selected_output(i - 1);
                break;
            }
        }
    }
    exfunc_invalidate_all_caches();
    return TRUE;
}

// [2] histogram (ヒストグラム, 0x00432400 / 0x00432630)

#define HIST_W 256
#define HIST_H 242 // 3 * 80 + 2

static int g_hist_mode = 0; // 0 = RGB, 1 = YCbCr
static unsigned char g_hist_dib[HIST_W * HIST_H * 3];
static BOOL g_hist_has_image = FALSE;

static void hist_set_menu_checks(HWND hwnd)
{
    HMENU m = hwnd ? GetMenu(hwnd) : NULL;
    if (!m)
        return;
    CheckMenuRadioItem(m, 100, 101, (UINT)(100 + (g_hist_mode ? 1 : 0)), MF_BYCOMMAND);
}

static void hist_render_buffer(const PIXEL_YC *ycp, int stride, int w, int h)
{
    unsigned int bins[3][256];
    int x, y, b, c;

    memset(bins, 0, sizeof(bins));
    memset(g_hist_dib, 16, sizeof(g_hist_dib));

    // separator lines at y=80 and y=161
    for (x = 0; x < HIST_W; x++)
    {
        unsigned char *s0 = &g_hist_dib[(80 * HIST_W + x) * 3];
        unsigned char *s1 = &g_hist_dib[(161 * HIST_W + x) * 3];
        s0[0] = s0[1] = s0[2] = 96;
        s1[0] = s1[1] = s1[2] = 96;
    }

    if (!ycp || w <= 0 || h <= 0)
    {
        g_hist_has_image = TRUE;
        return;
    }

    for (y = 0; y < h; y++)
    {
        const PIXEL_YC *row = ycp + (size_t)y * (size_t)stride;
        if (g_hist_mode == 0)
        {
            for (x = 0; x < w; x++)
            {
                int yv = (int)row[x].y;
                int cb = (int)row[x].cb;
                int cr = (int)row[x].cr;
                int r = clamp_i32(((yv * 255 + 2048) + ((5743 * cr * 255) >> 12)) >> 12, 0, 255);
                int g = clamp_i32(((yv * 255 + 2048) - (((1410 * cb + 2925 * cr) * 255) >> 12)) >> 12, 0, 255);
                int bl = clamp_i32(((yv * 255 + 2048) + ((7258 * cb * 255) >> 12)) >> 12, 0, 255);
                bins[0][r]++;
                bins[1][g]++;
                bins[2][bl]++;
            }
        }
        else
        {
            for (x = 0; x < w; x++)
            {
                int y8 = clamp_i32(16 + (((int)row[x].y * 219 + 2048) >> 12), 0, 255);
                int cb8 = clamp_i32(128 + (((int)row[x].cb * 224 + 2048) >> 12), 0, 255);
                int cr8 = clamp_i32(128 + (((int)row[x].cr * 224 + 2048) >> 12), 0, 255);
                bins[0][y8]++;
                bins[1][cb8]++;
                bins[2][cr8]++;
            }
        }
    }

    for (b = 0; b < 3; b++)
    {
        int band_top = b * 81;
        int div_k = (g_hist_mode == 1 && b > 0) ? 3 : 6;
        unsigned char col_b = 220, col_g = 220, col_r = 220;

        if (g_hist_mode == 0)
        {
            col_r = (b == 0) ? 255 : 64;
            col_g = (b == 1) ? 255 : 64;
            col_b = (b == 2) ? 255 : 64;
        }
        else if (b == 0)
        {
            col_r = 240;
            col_g = 240;
            col_b = 200;
        }
        else if (b == 1)
        {
            col_r = 80;
            col_g = 190;
            col_b = 255;
        }
        else
        {
            col_r = 255;
            col_g = 110;
            col_b = 180;
        }

        // subtle reference lines in YCbCr mode (16, 128, 235/240)
        if (g_hist_mode == 1)
        {
            int ref_hi = (b == 0) ? 235 : 240;
            for (y = 0; y < 80; y++)
            {
                unsigned char *p16 = &g_hist_dib[((band_top + y) * HIST_W + 16) * 3];
                unsigned char *phi = &g_hist_dib[((band_top + y) * HIST_W + ref_hi) * 3];
                p16[0] = p16[1] = p16[2] = 48;
                phi[0] = phi[1] = phi[2] = 48;
                if (b > 0)
                {
                    unsigned char *p128 = &g_hist_dib[((band_top + y) * HIST_W + 128) * 3];
                    p128[0] = p128[1] = p128[2] = 56;
                }
            }
        }

        for (c = 0; c < 256; c++)
        {
            long long num = ((long long)bins[b][c] * 256LL * 80LL) / (long long)div_k;
            long long den = (long long)w * (long long)h;
            int bar_h = (den > 0) ? (int)(num / den) : 0;
            if (bins[b][c] > 0 && bar_h < 1)
                bar_h = 1;
            if (bar_h > 80)
                bar_h = 80;
            for (y = 80 - bar_h; y < 80; y++)
            {
                unsigned char *px = &g_hist_dib[((band_top + y) * HIST_W + c) * 3];
                px[0] = col_b;
                px[1] = col_g;
                px[2] = col_r;
            }
        }
    }
    g_hist_has_image = TRUE;
}

static BOOL __cdecl proc_histogram(FILTER *fp, FILTER_PROC_INFO *fpip)
{
    if (!fp || !fpip || !(fp->flag & FILTER_FLAG_WINDOW_DISP) || !fp->hwnd)
        return TRUE;
    hist_render_buffer(fpip->ycp_edit, fpip->max_w, fpip->w, fpip->h);
    InvalidateRect(fp->hwnd, NULL, FALSE);
    return TRUE;
}

static void hist_refresh_from_editp(HWND hwnd, EDITP *ep)
{
    if (!hwnd || !IsWindow(hwnd))
        return;
    if (ep && editp_is_editing(ep))
    {
        int w = 0, h = 0, mw = 0, mh = 0;
        editp_get_dimensions(ep, &w, &h, &mw, &mh);
        hist_render_buffer(editp_get_ycp_edit(ep), mw, w, h);
    }
    else
    {
        hist_render_buffer(NULL, 0, 0, 0);
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static BOOL __cdecl wndproc_histogram(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      void *editp, FILTER *fp)
{
    char ini[MAX_PATH];
    (void)lp;

    switch (msg)
    {
    case WM_FILTER_INIT:
        if (hwnd && IsWindow(hwnd))
        {
            HMENU hm = i18n_load_menu("HIST");
            RECT rc = {0, 0, HIST_W, HIST_H};
            DWORD st = (DWORD)GetWindowLongA(hwnd, GWL_STYLE);
            DWORD exst = (DWORD)GetWindowLongA(hwnd, GWL_EXSTYLE);
            bf_get_ini_path(ini, sizeof(ini));
            g_hist_mode = GetPrivateProfileIntA(fp && fp->name ? fp->name : "Histogram",
                                                "mode", 0, ini) ? 1 : 0;
            if (hm)
                SetMenu(hwnd, hm);
            AdjustWindowRectEx(&rc, st, hm != NULL, exst);
            SetWindowPos(hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            hist_set_menu_checks(hwnd);
            hist_render_buffer(NULL, 0, 0, 0);
        }
        return FALSE;

    case WM_FILTER_EXIT:
        bf_get_ini_path(ini, sizeof(ini));
        WritePrivateProfileStringA(fp && fp->name ? fp->name : "Histogram",
                                   "mode", g_hist_mode ? "1" : "0", ini);
        return FALSE;

    case WM_FILTER_CHANGE_WINDOW:
    case WM_FILTER_FILE_OPEN:
    case WM_FILTER_FILE_CLOSE:
    case WM_FILTER_UPDATE:
        if (fp && (fp->flag & FILTER_FLAG_WINDOW_DISP))
            hist_refresh_from_editp(hwnd, (EDITP *)editp);
        return FALSE;

    case WM_COMMAND:
        if (LOWORD(wp) == 100 || LOWORD(wp) == 101)
        {
            g_hist_mode = (LOWORD(wp) == 101) ? 1 : 0;
            hist_set_menu_checks(hwnd);
            hist_refresh_from_editp(hwnd, (EDITP *)editp);
            return FALSE;
        }
        break;

    case WM_PAINT:
        if (hwnd)
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            BITMAPINFO bmi;
            ZeroMemory(&bmi, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = HIST_W;
            bmi.bmiHeader.biHeight = -HIST_H; // top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 24;
            bmi.bmiHeader.biCompression = BI_RGB;
            if (!g_hist_has_image)
                hist_render_buffer(NULL, 0, 0, 0);
            SetDIBitsToDevice(hdc, 0, 0, HIST_W, HIST_H, 0, 0, 0, HIST_H,
                              g_hist_dib, &bmi, DIB_RGB_COLORS);
            EndPaint(hwnd, &ps);
            return FALSE;
        }
        break;
    }
    return FALSE;
}

// [1] tool window (ツールウィンドウ, 0x0045caf0 / 0x0045c390 / 0x0045c7a0)

#define MAX_TOOL_FLT_ROWS 128
#define IDC_TOOL_ICON_BASE 46000
#define IDC_TOOL_COMBO_INTER 46050
#define IDC_TOOL_COMBO_FPS 46051
#define IDC_TOOL_COMBO_SIZE 46052
#define IDC_TOOL_CHK_BASE 46100
#define IDC_TOOL_CFG_BASE 46300

typedef struct ToolIconSpec
{
    const char *icon_res;
    int main_cmd;
    const char *fallback;
} ToolIconSpec;

static const ToolIconSpec k_tool_icons[] = {
    {"ICON_LEFT", 9001, "<"},
    {"ICON_RIGHT", 9002, ">"},
    {"ICON_BACK", 5175, "|<"},
    {"ICON_FORWARD", 5174, ">|"},
    {"ICON_IN", 5110, "["},
    {"ICON_OUT", 5111, "]"},
    {"ICON_MARK", 5130, "M"},
    {"ICON_DEL", 5101, "X"},
    {"ICON_Z_1", 9201, "50"},
    {"ICON_Z_2", 9202, "1x"},
    {"ICON_Z_3", 9203, "2x"},
    {"ICON_PLAY", 1099, "P"},
};

#define TOOL_ICON_COUNT ((int)(sizeof(k_tool_icons) / sizeof(k_tool_icons[0])))

static HWND g_tool_hwnd = NULL;
static int g_tool_cols = 1; // 1..4
static int g_tool_show = 7; // bit 1=icons, 2=combos, 4=filters
static HWND g_tool_icon_btns[TOOL_ICON_COUNT];
static HWND g_tool_lbl_combos[3];
static HWND g_tool_combos[3];
static HWND g_tool_flt_chk[MAX_TOOL_FLT_ROWS];
static HWND g_tool_flt_cfg[MAX_TOOL_FLT_ROWS];
static int g_tool_flt_idx[MAX_TOOL_FLT_ROWS];
static int g_tool_flt_count = 0;
static BOOL g_tool_syncing = FALSE;

static void destroy_tool_children(void)
{
    int i;
    for (i = 0; i < TOOL_ICON_COUNT; i++)
    {
        if (g_tool_icon_btns[i] && IsWindow(g_tool_icon_btns[i]))
            DestroyWindow(g_tool_icon_btns[i]);
        g_tool_icon_btns[i] = NULL;
    }
    for (i = 0; i < 3; i++)
    {
        if (g_tool_lbl_combos[i] && IsWindow(g_tool_lbl_combos[i]))
            DestroyWindow(g_tool_lbl_combos[i]);
        if (g_tool_combos[i] && IsWindow(g_tool_combos[i]))
            DestroyWindow(g_tool_combos[i]);
        g_tool_lbl_combos[i] = NULL;
        g_tool_combos[i] = NULL;
    }
    for (i = 0; i < g_tool_flt_count; i++)
    {
        if (g_tool_flt_chk[i] && IsWindow(g_tool_flt_chk[i]))
            DestroyWindow(g_tool_flt_chk[i]);
        if (g_tool_flt_cfg[i] && IsWindow(g_tool_flt_cfg[i]))
            DestroyWindow(g_tool_flt_cfg[i]);
        g_tool_flt_chk[i] = NULL;
        g_tool_flt_cfg[i] = NULL;
    }
    g_tool_flt_count = 0;
}

static BOOL is_tool_listed_filter(int fl)
{
    if (fl & (FILTER_FLAG_NO_CONFIG | FILTER_FLAG_CONFIG_POPUP |
              FILTER_FLAG_CONFIG_CHECK | FILTER_FLAG_CONFIG_RADIO |
              FILTER_FLAG_ALWAYS_ACTIVE | FILTER_FLAG_INTERLACE_FILTER |
              FILTER_FLAG_DISP_FILTER))
    {
        return FALSE;
    }
    return TRUE;
}

static void populate_tool_setting_combo(HWND hCombo, int builtin_idx)
{
    int n, c, sel = 0;
    if (!hCombo || !IsWindow(hCombo))
        return;
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    n = plugin_manager_get_config_check_count(builtin_idx);
    for (c = 0; c < n; c++)
    {
        const char *nm = plugin_manager_get_config_check_name(builtin_idx, c);
        wchar_t wnm[128];
        bf_sjis_to_wide(nm ? nm : "", wnm, 128);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)wnm);
        if (plugin_manager_get_config_check_value(builtin_idx, c) > 0)
            sel = c;
    }
    SendMessageW(hCombo, CB_SETCURSEL, (WPARAM)sel, 0);
}

static void sync_tool_window_states(void)
{
    int i;
    if (!g_tool_hwnd || !IsWindow(g_tool_hwnd))
        return;

    g_tool_syncing = TRUE;
    if (g_tool_show & 2)
    {
        populate_tool_setting_combo(g_tool_combos[0], 17);
        populate_tool_setting_combo(g_tool_combos[1], 15);
        populate_tool_setting_combo(g_tool_combos[2], 14);
    }
    for (i = 0; i < g_tool_flt_count; i++)
    {
        int fidx = g_tool_flt_idx[i];
        int fl = plugin_manager_get_flags(fidx);
        if (g_tool_flt_chk[i] && IsWindow(g_tool_flt_chk[i]))
        {
            SendMessageW(g_tool_flt_chk[i], BM_SETCHECK,
                         (fl & FILTER_FLAG_ACTIVE) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }
    g_tool_syncing = FALSE;
}

static void rebuild_tool_window_layout(void)
{
    HINSTANCE hInst = GetModuleHandleA(NULL);
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int cols = clamp_i32(g_tool_cols, 1, 4);
    int col_w = 252;
    int client_w = cols * col_w + 4;
    int y_top = 4;
    int i;
    RECT rc;
    DWORD st, exst;

    if (!g_tool_hwnd || !IsWindow(g_tool_hwnd))
        return;

    destroy_tool_children();
    g_tool_syncing = TRUE;

    // icon bar (show & 1)
    if (g_tool_show & 1)
    {
        for (i = 0; i < TOOL_ICON_COUNT; i++)
        {
            HICON hIco = (HICON)LoadImageA(hInst, k_tool_icons[i].icon_res, IMAGE_ICON, 16, 16, 0);
            g_tool_icon_btns[i] = CreateWindowExA(
                0, "BUTTON", k_tool_icons[i].fallback,
                WS_CHILD | WS_VISIBLE | (hIco ? BS_ICON : BS_PUSHBUTTON),
                4 + i * 20, y_top, 20, 20, g_tool_hwnd,
                (HMENU)(UINT_PTR)(IDC_TOOL_ICON_BASE + i), hInst, NULL);
            if (g_tool_icon_btns[i])
            {
                if (hIco)
                    SendMessageA(g_tool_icon_btns[i], BM_SETIMAGE, IMAGE_ICON, (LPARAM)hIco);
                else
                    SendMessageA(g_tool_icon_btns[i], WM_SETFONT, (WPARAM)hFont, 0);
            }
        }
        y_top += 24;
    }

    // quick settings combos (show & 2): Deinterlace (17), Framerate (15), Size (14)
    if (g_tool_show & 2)
    {
        static const UINT k_lbl_res[3] = {74, 97, 100};
        static const int k_flt_ids[3] = {17, 15, 14};
        for (i = 0; i < 3; i++)
        {
            g_tool_lbl_combos[i] = CreateWindowExW(
                0, L"STATIC", i18n_get_wstr(k_lbl_res[i]),
                WS_CHILD | WS_VISIBLE,
                6, y_top + 3, 110, 16, g_tool_hwnd, NULL, hInst, NULL);
            if (g_tool_lbl_combos[i])
                SendMessageW(g_tool_lbl_combos[i], WM_SETFONT, (WPARAM)hFont, 0);

            g_tool_combos[i] = CreateWindowExW(
                0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                118, y_top, 132, 200, g_tool_hwnd,
                (HMENU)(UINT_PTR)(IDC_TOOL_COMBO_INTER + i), hInst, NULL);
            if (g_tool_combos[i])
            {
                SendMessageW(g_tool_combos[i], WM_SETFONT, (WPARAM)hFont, 0);
                populate_tool_setting_combo(g_tool_combos[i], k_flt_ids[i]);
            }
            y_top += 22;
        }
        y_top += 2;
    }

    // configurable filters in execution priority order (show & 4)
    if (g_tool_show & 4)
    {
        int cand[MAX_TOOL_FLT_ROWS];
        int vid_ord[MAX_TOOL_FLT_ROWS], aud_ord[MAX_TOOL_FLT_ROWS];
        BOOL seen[256] = {0};
        int vn, an, total, n_cand = 0, rows_per_col;
        const wchar_t *w_cfg = i18n_get_wstr(107); // "の設定" -> button label "設定"
        const wchar_t *btn_lbl = (w_cfg && lstrlenW(w_cfg) >= 2) ? (w_cfg + 1) : L"Cfg";

        vn = plugin_manager_get_execution_order(FALSE, vid_ord, MAX_TOOL_FLT_ROWS);
        for (i = 0; i < vn && n_cand < MAX_TOOL_FLT_ROWS; i++)
        {
            int idx = vid_ord[i];
            if (idx >= 0 && idx < 256 && is_tool_listed_filter(plugin_manager_get_flags(idx)))
            {
                cand[n_cand++] = idx;
                seen[idx] = TRUE;
            }
        }
        an = plugin_manager_get_execution_order(TRUE, aud_ord, MAX_TOOL_FLT_ROWS);
        for (i = 0; i < an && n_cand < MAX_TOOL_FLT_ROWS; i++)
        {
            int idx = aud_ord[i];
            if (idx >= 0 && idx < 256 && !seen[idx] && is_tool_listed_filter(plugin_manager_get_flags(idx)))
            {
                cand[n_cand++] = idx;
                seen[idx] = TRUE;
            }
        }
        // include filters without func_proc (e.g. Audio Position, ExEdit)
        total = plugin_manager_get_count();
        for (i = 0; i < total && i < 256 && n_cand < MAX_TOOL_FLT_ROWS; i++)
        {
            if (!seen[i] && is_tool_listed_filter(plugin_manager_get_flags(i)))
            {
                cand[n_cand++] = i;
                seen[i] = TRUE;
            }
        }

        rows_per_col = (n_cand + cols - 1) / cols;
        if (rows_per_col < 1)
            rows_per_col = 1;

        for (i = 0; i < n_cand; i++)
        {
            int fidx = cand[i];
            int c_col = i / rows_per_col;
            int c_row = i % rows_per_col;
            int x0 = 4 + c_col * col_w;
            int y0 = y_top + c_row * 19;
            int fl = plugin_manager_get_flags(fidx);
            wchar_t wnm[256];

            bf_sjis_to_wide(plugin_manager_get_name(fidx), wnm, 256);
            g_tool_flt_idx[i] = fidx;
            g_tool_flt_chk[i] = CreateWindowExW(
                0, L"BUTTON", wnm,
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                x0, y0, 206, 18, g_tool_hwnd,
                (HMENU)(UINT_PTR)(IDC_TOOL_CHK_BASE + i), hInst, NULL);
            if (g_tool_flt_chk[i])
            {
                SendMessageW(g_tool_flt_chk[i], WM_SETFONT, (WPARAM)hFont, 0);
                if (fl & FILTER_FLAG_ACTIVE)
                    SendMessageW(g_tool_flt_chk[i], BM_SETCHECK, BST_CHECKED, 0);
            }

            g_tool_flt_cfg[i] = CreateWindowExW(
                0, L"BUTTON", btn_lbl,
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                x0 + 208, y0, 40, 17, g_tool_hwnd,
                (HMENU)(UINT_PTR)(IDC_TOOL_CFG_BASE + i), hInst, NULL);
            if (g_tool_flt_cfg[i])
                SendMessageW(g_tool_flt_cfg[i], WM_SETFONT, (WPARAM)hFont, 0);
        }
        g_tool_flt_count = n_cand;
        y_top += rows_per_col * 19 + 4;
    }

    if (y_top < 28)
        y_top = 28;

    SetRect(&rc, 0, 0, client_w, y_top);
    st = (DWORD)GetWindowLongA(g_tool_hwnd, GWL_STYLE);
    exst = (DWORD)GetWindowLongA(g_tool_hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&rc, st, FALSE, exst);
    SetWindowPos(g_tool_hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_tool_hwnd, NULL, TRUE);
    g_tool_syncing = FALSE;
}

void builtin_filters_refresh_tool_window(void)
{
    sync_tool_window_states();
}

void builtin_filters_notify_menu_rebuilt(HWND main_hwnd)
{
    if (main_hwnd)
        g_host_main_hwnd = main_hwnd;
    if (g_tool_hwnd && IsWindow(g_tool_hwnd))
        sync_tool_window_states();
}

static BOOL __cdecl wndproc_tool_window(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                        void *editp, FILTER *fp)
{
    char ini[MAX_PATH], vbuf[32];
    (void)editp;

    switch (msg)
    {
    case WM_FILTER_INIT:
        g_tool_hwnd = hwnd;
        bf_get_ini_path(ini, sizeof(ini));
        g_tool_cols = clamp_i32(GetPrivateProfileIntA(fp && fp->name ? fp->name : "ToolWindow",
                                                      "line", 1, ini), 1, 4);
        g_tool_show = GetPrivateProfileIntA(fp && fp->name ? fp->name : "ToolWindow",
                                            "show", 7, ini) & 7;
        if (g_tool_show == 0)
            g_tool_show = 7;
        rebuild_tool_window_layout();
        return FALSE;

    case WM_FILTER_EXIT:
        bf_get_ini_path(ini, sizeof(ini));
        wsprintfA(vbuf, "%d", g_tool_cols);
        WritePrivateProfileStringA(fp && fp->name ? fp->name : "ToolWindow", "line", vbuf, ini);
        wsprintfA(vbuf, "%d", g_tool_show);
        WritePrivateProfileStringA(fp && fp->name ? fp->name : "ToolWindow", "show", vbuf, ini);
        destroy_tool_children();
        g_tool_hwnd = NULL;
        return FALSE;

    case WM_FILTER_CHANGE_WINDOW:
    case WM_FILTER_UPDATE:
    case WM_FILTER_FILE_OPEN:
        if (fp && (fp->flag & FILTER_FLAG_WINDOW_DISP))
            rebuild_tool_window_layout();
        return FALSE;

    case WM_CONTEXTMENU:
    {
        HMENU hm = i18n_load_menu("CONTEXT_TOOL");
        HMENU sub = hm ? GetSubMenu(hm, 0) : NULL;
        if (sub)
        {
            POINT pt;
            int cmd;
            CheckMenuRadioItem(sub, 100, 103, (UINT)(100 + g_tool_cols - 1), MF_BYCOMMAND);
            CheckMenuItem(sub, 110, MF_BYCOMMAND | ((g_tool_show & 1) ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(sub, 111, MF_BYCOMMAND | ((g_tool_show & 2) ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(sub, 112, MF_BYCOMMAND | ((g_tool_show & 4) ? MF_CHECKED : MF_UNCHECKED));
            GetCursorPos(&pt);
            cmd = (int)TrackPopupMenu(sub, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      pt.x, pt.y, 0, hwnd, NULL);
            if (cmd >= 100 && cmd <= 103)
            {
                g_tool_cols = (cmd - 100) + 1;
                rebuild_tool_window_layout();
            }
            else if (cmd >= 110 && cmd <= 112)
            {
                int bit = 1 << (cmd - 110);
                if ((g_tool_show ^ bit) != 0)
                {
                    g_tool_show ^= bit;
                    rebuild_tool_window_layout();
                }
            }
        }
        if (hm)
            DestroyMenu(hm);
        return TRUE;
    }

    case WM_COMMAND:
        if (!g_tool_syncing)
        {
            int id = LOWORD(wp);
            int code = HIWORD(wp);

            if (id >= IDC_TOOL_ICON_BASE && id < IDC_TOOL_ICON_BASE + TOOL_ICON_COUNT)
            {
                if (g_host_main_hwnd && IsWindow(g_host_main_hwnd))
                    PostMessageA(g_host_main_hwnd, WM_COMMAND,
                                 (WPARAM)k_tool_icons[id - IDC_TOOL_ICON_BASE].main_cmd, 0);
                return FALSE;
            }
            if (id >= IDC_TOOL_COMBO_INTER && id <= IDC_TOOL_COMBO_SIZE && code == CBN_SELCHANGE)
            {
                static const int k_flt_ids[3] = {17, 15, 14};
                int which = id - IDC_TOOL_COMBO_INTER;
                int sel = (int)SendMessageW(g_tool_combos[which], CB_GETCURSEL, 0, 0);
                if (sel >= 0)
                {
                    plugin_manager_select_config_check(k_flt_ids[which], sel);
                    sync_tool_window_states();
                }
                return FALSE;
            }
            if (id >= IDC_TOOL_CHK_BASE && id < IDC_TOOL_CHK_BASE + g_tool_flt_count)
            {
                int row = id - IDC_TOOL_CHK_BASE;
                int slot = plugin_manager_get_cmd_slot(g_tool_flt_idx[row]);
                if (g_host_main_hwnd && IsWindow(g_host_main_hwnd) && slot >= 0)
                    SendMessageA(g_host_main_hwnd, WM_COMMAND,
                                 (WPARAM)(PLUGIN_CMD_TOGGLE_BASE + slot), 0);
                return FALSE;
            }
            if (id >= IDC_TOOL_CFG_BASE && id < IDC_TOOL_CFG_BASE + g_tool_flt_count)
            {
                int row = id - IDC_TOOL_CFG_BASE;
                int slot = plugin_manager_get_cmd_slot(g_tool_flt_idx[row]);
                if (g_host_main_hwnd && IsWindow(g_host_main_hwnd) && slot >= 0)
                    SendMessageA(g_host_main_hwnd, WM_COMMAND,
                                 (WPARAM)(PLUGIN_CMD_WINDOW_BASE + slot), 0);
                return FALSE;
            }
        }
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_host_main_hwnd && IsWindow(g_host_main_hwnd))
            PostMessageA(g_host_main_hwnd, msg, wp, lp);
        break;
    }
    return FALSE;
}

// [0] playback window (再生ウィンドウ, 0x00451590)

#define PLAY_BAR_H 28
#define PLAY_TIMER_ID 0x504C
#define IDC_PLAY_TRACKBAR 47000
#define IDC_PLAY_BTN_PLAY 47001
#define IDC_PLAY_BTN_STOP 47002
#define IDC_PLAY_BTN_VOL 47003
#define PLAY_AUDIO_BUFS 4
#define PLAY_AUDIO_BYTES 8192

static HWND g_play_hwnd = NULL;
static HWND g_play_tb = NULL;
static HWND g_play_btn_play = NULL;
static HWND g_play_btn_stop = NULL;
static HWND g_play_btn_vol = NULL;
static HICON g_ico_play = NULL;
static HICON g_ico_pause = NULL;
static HICON g_ico_stop = NULL;
static HICON g_ico_vol = NULL;

static BOOL g_pw_playing = FALSE;
static BOOL g_pw_repeat = FALSE;
static BOOL g_pw_select_only = FALSE;
static BOOL g_pw_nofilter = FALSE;
static BOOL g_pw_overlay = FALSE;
static int g_pw_size_cmd = 111; // 100%
static int g_pw_volume = 256;   // 0..256
static int g_pw_cur_frame = 0;
static int g_pw_start_frame = 0;
static int g_pw_end_frame = 0;
static DWORD g_pw_start_tick = 0;

static unsigned char *g_pw_dib = NULL;
static size_t g_pw_dib_cap = 0;
static int g_pw_img_w = 0;
static int g_pw_img_h = 0;

static HWAVEOUT g_pw_wave = NULL;
static WAVEHDR g_pw_whdr[PLAY_AUDIO_BUFS];
static char g_pw_wbuf[PLAY_AUDIO_BUFS][PLAY_AUDIO_BYTES];
static BOOL g_pw_wprep[PLAY_AUDIO_BUFS];
static int g_pw_aud_cursor = 0;
static short *g_pw_pcm_tmp = NULL;
static int g_pw_pcm_cap = 0;
static int g_pw_pcm_off = 0;
static int g_pw_pcm_rem = 0;

static void pw_sync_menus(void)
{
    HMENU m = g_play_hwnd ? GetMenu(g_play_hwnd) : NULL;
    if (!m)
        return;
    CheckMenuItem(m, 107, MF_BYCOMMAND | (g_pw_select_only ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, 103, MF_BYCOMMAND | (g_pw_repeat ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, 104, MF_BYCOMMAND | (g_pw_nofilter ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, 106, MF_BYCOMMAND | (g_pw_overlay ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(m, 110, 117, (UINT)g_pw_size_cmd, MF_BYCOMMAND);
}

static void pw_layout_controls(HWND hwnd)
{
    RECT rc;
    int cw, ch, y0;
    if (!hwnd || !IsWindow(hwnd))
        return;
    GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    y0 = ch - PLAY_BAR_H + 4;
    if (y0 < 0)
        y0 = 0;

    if (g_play_btn_play)
        MoveWindow(g_play_btn_play, 4, y0, 20, 20, TRUE);
    if (g_play_btn_stop)
        MoveWindow(g_play_btn_stop, 26, y0, 20, 20, TRUE);
    if (g_play_btn_vol)
        MoveWindow(g_play_btn_vol, 48, y0, 20, 20, TRUE);
    if (g_play_tb)
        MoveWindow(g_play_tb, 72, y0 - 1, (cw > 78) ? (cw - 76) : 16, 22, TRUE);
}

static void pw_apply_size_preset(EDITP *ep)
{
    int base_w = 640, base_h = 480, mw = 0, mh = 0;
    int tw = 320, th = 240;
    RECT rc;
    DWORD st, exst;

    if (!g_play_hwnd || !IsWindow(g_play_hwnd) || g_pw_size_cmd == 116)
        return;
    if (ep && editp_is_editing(ep))
    {
        editp_get_dimensions(ep, &base_w, &base_h, &mw, &mh);
        if (base_w <= 0)
            base_w = 640;
        if (base_h <= 0)
            base_h = 480;
    }

    switch (g_pw_size_cmd)
    {
    case 117:
        tw = base_w / 4;
        th = base_h / 4;
        break;
    case 110:
        tw = base_w / 2;
        th = base_h / 2;
        break;
    case 111:
        tw = base_w;
        th = base_h;
        break;
    case 112:
        tw = base_w * 2;
        th = base_h * 2;
        break;
    case 113:
        tw = 640;
        th = 480;
        break;
    case 114:
        tw = 320;
        th = 240;
        break;
    case 115:
        tw = 160;
        th = 120;
        break;
    default:
        return;
    }
    if (tw < 160)
        tw = 160;
    if (th < 90)
        th = 90;

    SetRect(&rc, 0, 0, tw, th + PLAY_BAR_H);
    st = (DWORD)GetWindowLongA(g_play_hwnd, GWL_STYLE);
    exst = (DWORD)GetWindowLongA(g_play_hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&rc, st, GetMenu(g_play_hwnd) != NULL, exst);
    SetWindowPos(g_play_hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    pw_layout_controls(g_play_hwnd);
}

static void pw_render_frame_to_dib(EDITP *ep, int frame_num)
{
    int w = 0, h = 0, mw = 0, mh = 0, stride;
    size_t need;
    int saved_f;

    if (!ep || !editp_is_editing(ep))
    {
        g_pw_img_w = 0;
        g_pw_img_h = 0;
        return;
    }

    saved_f = editp_get_frame(ep);
    if (g_pw_nofilter)
    {
        editp_load_frame(ep, frame_num);
    }
    else if (!plugin_manager_render_video_frame(ep, frame_num))
    {
        editp_load_frame(ep, frame_num);
    }
    editp_get_dimensions(ep, &w, &h, &mw, &mh);
    if (w > 0 && h > 0)
    {
        stride = (w * 3 + 3) & ~3;
        need = (size_t)stride * (size_t)h;
        if (need > g_pw_dib_cap || !g_pw_dib)
        {
            unsigned char *nb = (unsigned char *)realloc(g_pw_dib, need);
            if (nb)
            {
                g_pw_dib = nb;
                g_pw_dib_cap = need;
            }
        }
        if (g_pw_dib && g_pw_dib_cap >= need)
        {
            yc2rgb_dib24(g_pw_dib, stride, editp_get_ycp_edit(ep), mw, w, h, TRUE);
            g_pw_img_w = w;
            g_pw_img_h = h;
        }
    }
    if (saved_f != frame_num)
        editp_set_frame(ep, saved_f);

    if (g_play_tb && IsWindow(g_play_tb))
    {
        int fn = editp_get_frame_n(ep);
        SendMessageA(g_play_tb, TBM_SETRANGEMIN, FALSE, 0);
        SendMessageA(g_play_tb, TBM_SETRANGEMAX, TRUE, fn > 1 ? fn - 1 : 0);
        SendMessageA(g_play_tb, TBM_SETPOS, TRUE, frame_num);
    }
    if (g_play_hwnd && IsWindow(g_play_hwnd))
    {
        RECT vrc;
        GetClientRect(g_play_hwnd, &vrc);
        vrc.bottom = (vrc.bottom > PLAY_BAR_H) ? (vrc.bottom - PLAY_BAR_H) : vrc.bottom;
        InvalidateRect(g_play_hwnd, &vrc, FALSE);
    }
}

static void pw_fill_wave_buf(EDITP *ep, WAVEHDR *wh)
{
    int filled = 0;
    int ch = editp_get_audio_ch(ep) > 0 ? editp_get_audio_ch(ep) : 2;

    if (!g_pw_playing || !g_pw_wave || !wh || !ep || !editp_is_editing(ep))
        return;

    while (filled < PLAY_AUDIO_BYTES)
    {
        if (g_pw_pcm_rem <= 0)
        {
            if (g_pw_aud_cursor > g_pw_end_frame)
            {
                memset(wh->lpData + filled, 0, (size_t)(PLAY_AUDIO_BYTES - filled));
                filled = PLAY_AUDIO_BYTES;
                break;
            }
            {
                int f = g_pw_aud_cursor++;
                int smp = editp_read_audio_frame(ep, f, NULL);
                int sh = (smp > 0 ? smp : 2048) * ch + 64;
                if (sh > g_pw_pcm_cap || !g_pw_pcm_tmp)
                {
                    short *nb = (short *)realloc(g_pw_pcm_tmp, (size_t)sh * sizeof(short));
                    if (!nb)
                        break;
                    g_pw_pcm_tmp = nb;
                    g_pw_pcm_cap = sh;
                }
                if (smp > 0)
                {
                    int got = plugin_manager_render_audio_frame(ep, f, g_pw_pcm_tmp, g_pw_nofilter);
                    int k, tot = got * ch;
                    if (g_pw_volume != 256)
                    {
                        for (k = 0; k < tot; k++)
                            g_pw_pcm_tmp[k] = clamp_s16(((int)g_pw_pcm_tmp[k] * g_pw_volume) >> 8);
                    }
                    g_pw_pcm_off = 0;
                    g_pw_pcm_rem = tot * (int)sizeof(short);
                }
                else
                {
                    g_pw_pcm_rem = 0;
                }
            }
        }
        if (g_pw_pcm_rem > 0)
        {
            int take = PLAY_AUDIO_BYTES - filled;
            if (take > g_pw_pcm_rem)
                take = g_pw_pcm_rem;
            memcpy(wh->lpData + filled, (const char *)g_pw_pcm_tmp + g_pw_pcm_off, (size_t)take);
            filled += take;
            g_pw_pcm_off += take;
            g_pw_pcm_rem -= take;
        }
    }
    wh->dwBufferLength = (DWORD)(filled > 0 ? filled : PLAY_AUDIO_BYTES);
    wh->dwFlags &= ~WHDR_DONE;
    waveOutWrite(g_pw_wave, wh, sizeof(*wh));
}

static void pw_stop_audio(void)
{
    int i;
    if (!g_pw_wave)
        return;
    waveOutReset(g_pw_wave);
    for (i = 0; i < PLAY_AUDIO_BUFS; i++)
    {
        if (g_pw_wprep[i])
        {
            waveOutUnprepareHeader(g_pw_wave, &g_pw_whdr[i], sizeof(WAVEHDR));
            g_pw_wprep[i] = FALSE;
        }
    }
    waveOutClose(g_pw_wave);
    g_pw_wave = NULL;
    g_pw_pcm_rem = 0;
    g_pw_pcm_off = 0;
}

static void pw_start_audio(HWND hwnd, EDITP *ep, int start_f)
{
    WAVEFORMATEX wfx;
    int i;
    pw_stop_audio();
    if (!ep || !editp_has_audio(ep) || g_pw_volume <= 0)
        return;

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)(editp_get_audio_ch(ep) > 0 ? editp_get_audio_ch(ep) : 2);
    wfx.nSamplesPerSec = (DWORD)(editp_get_audio_rate(ep) > 0 ? editp_get_audio_rate(ep) : 44100);
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(wfx.nChannels * 2);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    if (waveOutOpen(&g_pw_wave, WAVE_MAPPER, &wfx, (DWORD_PTR)hwnd, 0, CALLBACK_WINDOW) != MMSYSERR_NOERROR)
    {
        g_pw_wave = NULL;
        return;
    }
    g_pw_aud_cursor = start_f;
    g_pw_pcm_off = 0;
    g_pw_pcm_rem = 0;
    for (i = 0; i < PLAY_AUDIO_BUFS; i++)
    {
        ZeroMemory(&g_pw_whdr[i], sizeof(WAVEHDR));
        g_pw_whdr[i].lpData = g_pw_wbuf[i];
        g_pw_whdr[i].dwBufferLength = PLAY_AUDIO_BYTES;
        if (waveOutPrepareHeader(g_pw_wave, &g_pw_whdr[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
        {
            g_pw_wprep[i] = TRUE;
            pw_fill_wave_buf(ep, &g_pw_whdr[i]);
        }
    }
}

void builtin_filters_stop_playback(void)
{
    EDITP *ep = exfunc_get_editp();
    if (!g_pw_playing)
        return;
    g_pw_playing = FALSE;
    if (g_play_hwnd && IsWindow(g_play_hwnd))
        KillTimer(g_play_hwnd, PLAY_TIMER_ID);
    pw_stop_audio();
    if (g_play_btn_play && g_ico_play)
        SendMessageA(g_play_btn_play, BM_SETIMAGE, IMAGE_ICON, (LPARAM)g_ico_play);
    if (ep && editp_is_editing(ep))
        plugin_manager_render_video_frame(ep, editp_get_frame(ep));
}

static void pw_start_playback(EDITP *ep, BOOL from_range_start)
{
    int fn, s = 0, e = 0;
    if (!g_play_hwnd || !IsWindow(g_play_hwnd) || !ep || !editp_is_editing(ep))
        return;
    fn = editp_get_frame_n(ep);
    if (fn <= 0)
        return;

    if (g_pw_select_only && editp_get_select_frame(ep, &s, &e) && s >= 0 && e >= s && e < fn)
    {
        g_pw_start_frame = from_range_start ? s : clamp_i32(g_pw_cur_frame, s, e);
        g_pw_end_frame = e;
    }
    else
    {
        g_pw_start_frame = from_range_start ? 0 : clamp_i32(editp_get_frame(ep), 0, fn - 1);
        g_pw_end_frame = fn - 1;
    }
    if (g_pw_start_frame >= g_pw_end_frame)
        g_pw_start_frame = (g_pw_select_only && s >= 0) ? s : 0;

    g_pw_cur_frame = g_pw_start_frame;
    g_pw_playing = TRUE;
    g_pw_start_tick = GetTickCount();
    if (g_play_btn_play && g_ico_pause)
        SendMessageA(g_play_btn_play, BM_SETIMAGE, IMAGE_ICON, (LPARAM)g_ico_pause);
    pw_start_audio(g_play_hwnd, ep, g_pw_start_frame);
    pw_render_frame_to_dib(ep, g_pw_cur_frame);
    SetTimer(g_play_hwnd, PLAY_TIMER_ID, 15, NULL);
}

BOOL builtin_filters_toggle_playback_window_play(HWND main_hwnd, EDITP *ep)
{
    int idx = plugin_manager_find_by_cmd_slot(1); // slot 1 = Playback Window
    (void)main_hwnd;
    if (idx < 0 || !ep || !editp_is_editing(ep))
        return FALSE;
    if (!(plugin_manager_get_flags(idx) & FILTER_FLAG_WINDOW_DISP))
    {
        plugin_manager_show_window(idx, TRUE);
    }
    if (g_pw_playing)
        builtin_filters_stop_playback();
    else
        pw_start_playback(ep, FALSE);
    return TRUE;
}

static BOOL __cdecl wndproc_playback_window(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                            void *editp, FILTER *fp)
{
    EDITP *ep = (EDITP *)(editp ? editp : exfunc_get_editp());
    char ini[MAX_PATH], vbuf[32];

    switch (msg)
    {
    case WM_FILTER_INIT:
    {
        HINSTANCE hInst = GetModuleHandleA(NULL);
        HMENU hm = i18n_load_menu("PLAY");
        const char *sec = (fp && fp->name) ? fp->name : "PlaybackWindow";

        g_play_hwnd = hwnd;
        bf_get_ini_path(ini, sizeof(ini));
        g_pw_repeat = GetPrivateProfileIntA(sec, "repeat", 0, ini) != 0;
        g_pw_select_only = GetPrivateProfileIntA(sec, "select", 0, ini) != 0;
        g_pw_nofilter = GetPrivateProfileIntA(sec, "nofilter", 0, ini) != 0;
        g_pw_overlay = GetPrivateProfileIntA(sec, "overlay", 0, ini) != 0;
        g_pw_size_cmd = clamp_i32(GetPrivateProfileIntA(sec, "size", 111, ini), 110, 117);
        g_pw_volume = clamp_i32(GetPrivateProfileIntA(sec, "volume", 256, ini), 0, 256);

        if (hm)
            SetMenu(hwnd, hm);
        g_ico_play = (HICON)LoadImageA(hInst, "ICON_PLAY", IMAGE_ICON, 16, 16, 0);
        g_ico_pause = (HICON)LoadImageA(hInst, "ICON_PAUSE", IMAGE_ICON, 16, 16, 0);
        g_ico_stop = (HICON)LoadImageA(hInst, "ICON_STOP", IMAGE_ICON, 16, 16, 0);
        g_ico_vol = (HICON)LoadImageA(hInst, "ICON_VOL", IMAGE_ICON, 16, 16, 0);

        g_play_btn_play = CreateWindowExA(0, "BUTTON", ">",
                                          WS_CHILD | WS_VISIBLE | (g_ico_play ? BS_ICON : BS_PUSHBUTTON),
                                          4, 4, 20, 20, hwnd, (HMENU)(UINT_PTR)IDC_PLAY_BTN_PLAY, hInst, NULL);
        if (g_play_btn_play && g_ico_play)
            SendMessageA(g_play_btn_play, BM_SETIMAGE, IMAGE_ICON, (LPARAM)g_ico_play);

        g_play_btn_stop = CreateWindowExA(0, "BUTTON", "[]",
                                          WS_CHILD | WS_VISIBLE | (g_ico_stop ? BS_ICON : BS_PUSHBUTTON),
                                          26, 4, 20, 20, hwnd, (HMENU)(UINT_PTR)IDC_PLAY_BTN_STOP, hInst, NULL);
        if (g_play_btn_stop && g_ico_stop)
            SendMessageA(g_play_btn_stop, BM_SETIMAGE, IMAGE_ICON, (LPARAM)g_ico_stop);

        g_play_btn_vol = CreateWindowExA(0, "BUTTON", "V",
                                         WS_CHILD | WS_VISIBLE | (g_ico_vol ? BS_ICON : BS_PUSHBUTTON),
                                         48, 4, 20, 20, hwnd, (HMENU)(UINT_PTR)IDC_PLAY_BTN_VOL, hInst, NULL);
        if (g_play_btn_vol && g_ico_vol)
            SendMessageA(g_play_btn_vol, BM_SETIMAGE, IMAGE_ICON, (LPARAM)g_ico_vol);

        g_play_tb = CreateWindowExA(0, TRACKBAR_CLASSA, "",
                                    WS_CHILD | WS_VISIBLE | TBS_NOTICKS,
                                    72, 4, 220, 22, hwnd, (HMENU)(UINT_PTR)IDC_PLAY_TRACKBAR, hInst, NULL);

        pw_sync_menus();
        pw_layout_controls(hwnd);
        return FALSE;
    }

    case WM_FILTER_EXIT:
    {
        const char *sec = (fp && fp->name) ? fp->name : "PlaybackWindow";
        builtin_filters_stop_playback();
        bf_get_ini_path(ini, sizeof(ini));
        WritePrivateProfileStringA(sec, "repeat", g_pw_repeat ? "1" : "0", ini);
        WritePrivateProfileStringA(sec, "select", g_pw_select_only ? "1" : "0", ini);
        WritePrivateProfileStringA(sec, "nofilter", g_pw_nofilter ? "1" : "0", ini);
        WritePrivateProfileStringA(sec, "overlay", g_pw_overlay ? "1" : "0", ini);
        wsprintfA(vbuf, "%d", g_pw_size_cmd);
        WritePrivateProfileStringA(sec, "size", vbuf, ini);
        wsprintfA(vbuf, "%d", g_pw_volume);
        WritePrivateProfileStringA(sec, "volume", vbuf, ini);

        free(g_pw_dib);
        g_pw_dib = NULL;
        g_pw_dib_cap = 0;
        free(g_pw_pcm_tmp);
        g_pw_pcm_tmp = NULL;
        g_pw_pcm_cap = 0;
        g_play_hwnd = NULL;
        return FALSE;
    }

    case WM_FILTER_FILE_OPEN:
        builtin_filters_stop_playback();
        g_pw_cur_frame = ep ? editp_get_frame(ep) : 0;
        if (fp && (fp->flag & FILTER_FLAG_WINDOW_DISP))
        {
            pw_apply_size_preset(ep);
            pw_render_frame_to_dib(ep, g_pw_cur_frame);
        }
        return FALSE;

    case WM_FILTER_FILE_CLOSE:
    case WM_FILTER_SAVE_START:
        builtin_filters_stop_playback();
        g_pw_img_w = 0;
        g_pw_img_h = 0;
        if (hwnd && IsWindow(hwnd))
            InvalidateRect(hwnd, NULL, TRUE);
        return FALSE;

    case WM_FILTER_CHANGE_WINDOW:
        if (fp && !(fp->flag & FILTER_FLAG_WINDOW_DISP))
        {
            builtin_filters_stop_playback();
        }
        else if (fp && (fp->flag & FILTER_FLAG_WINDOW_DISP))
        {
            g_pw_cur_frame = ep ? editp_get_frame(ep) : 0;
            pw_render_frame_to_dib(ep, g_pw_cur_frame);
        }
        return FALSE;

    case WM_FILTER_UPDATE:
        if (!g_pw_playing && fp && (fp->flag & FILTER_FLAG_WINDOW_DISP) && ep)
        {
            g_pw_cur_frame = editp_get_frame(ep);
            pw_render_frame_to_dib(ep, g_pw_cur_frame);
        }
        return FALSE;

    case WM_SIZE:
        pw_layout_controls(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return FALSE;

    case WM_HSCROLL:
        if ((HWND)lp == g_play_tb && ep && editp_is_editing(ep))
        {
            int pos = (int)SendMessageA(g_play_tb, TBM_GETPOS, 0, 0);
            if (g_pw_playing)
                builtin_filters_stop_playback();
            g_pw_cur_frame = clamp_i32(pos, 0, editp_get_frame_n(ep) - 1);
            pw_render_frame_to_dib(ep, g_pw_cur_frame);
        }
        return FALSE;

    case MM_WOM_DONE:
        if (g_pw_playing && g_pw_wave && (HWAVEOUT)wp == g_pw_wave)
            pw_fill_wave_buf(ep, (WAVEHDR *)lp);
        return FALSE;

    case WM_TIMER:
        if (wp == PLAY_TIMER_ID && g_pw_playing && ep && editp_is_editing(ep))
        {
            int rate = 30, scale = 1, target;
            DWORD elapsed;
            editp_get_fps(ep, &rate, &scale);
            if (rate <= 0)
                rate = 30;
            if (scale <= 0)
                scale = 1;
            elapsed = GetTickCount() - g_pw_start_tick;
            target = g_pw_start_frame +
                     (int)(((unsigned long long)elapsed * (unsigned long long)rate) /
                           (1000ULL * (unsigned long long)scale));
            if (target > g_pw_end_frame)
            {
                if (g_pw_repeat)
                {
                    pw_start_playback(ep, TRUE);
                }
                else
                {
                    g_pw_cur_frame = g_pw_end_frame;
                    pw_render_frame_to_dib(ep, g_pw_cur_frame);
                    builtin_filters_stop_playback();
                }
            }
            else if (target != g_pw_cur_frame)
            {
                g_pw_cur_frame = target;
                pw_render_frame_to_dib(ep, g_pw_cur_frame);
            }
        }
        return FALSE;

    case WM_CONTEXTMENU:
    {
        HMENU hm = i18n_load_menu("CONTEXT4");
        HMENU sub = hm ? GetSubMenu(hm, 0) : NULL;
        if (sub)
        {
            POINT pt;
            int cmd;
            CheckMenuItem(sub, 107, MF_BYCOMMAND | (g_pw_select_only ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuItem(sub, 103, MF_BYCOMMAND | (g_pw_repeat ? MF_CHECKED : MF_UNCHECKED));
            CheckMenuRadioItem(sub, 110, 117, (UINT)g_pw_size_cmd, MF_BYCOMMAND);
            GetCursorPos(&pt);
            cmd = (int)TrackPopupMenu(sub, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                      pt.x, pt.y, 0, hwnd, NULL);
            if (cmd > 0)
                SendMessageA(hwnd, WM_COMMAND, (WPARAM)cmd, 0);
        }
        if (hm)
            DestroyMenu(hm);
        return TRUE;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        if (id == 105 || id == IDC_PLAY_BTN_PLAY)
        {
            if (g_pw_playing)
                builtin_filters_stop_playback();
            else
                pw_start_playback(ep, FALSE);
            return FALSE;
        }
        if (id == 101 || id == IDC_PLAY_BTN_STOP)
        {
            builtin_filters_stop_playback();
            if (ep && editp_is_editing(ep))
            {
                int s = 0, e = 0;
                g_pw_cur_frame = (g_pw_select_only && editp_get_select_frame(ep, &s, &e) && s >= 0) ? s : 0;
                pw_render_frame_to_dib(ep, g_pw_cur_frame);
            }
            return FALSE;
        }
        if (id == 108 && ep && editp_is_editing(ep))
        {
            editp_set_frame(ep, g_pw_cur_frame);
            plugin_manager_notify_update(ep);
            if (g_host_main_hwnd && IsWindow(g_host_main_hwnd))
                PostMessageA(g_host_main_hwnd, WM_REAVIUTL_PLUGIN_REDRAW, 2, 0);
            return FALSE;
        }
        if (id == 107)
        {
            g_pw_select_only = !g_pw_select_only;
            pw_sync_menus();
            return FALSE;
        }
        if (id == 103)
        {
            g_pw_repeat = !g_pw_repeat;
            pw_sync_menus();
            return FALSE;
        }
        if (id == 104)
        {
            g_pw_nofilter = !g_pw_nofilter;
            pw_sync_menus();
            if (!g_pw_playing && ep && editp_is_editing(ep))
                pw_render_frame_to_dib(ep, g_pw_cur_frame);
            return FALSE;
        }
        if (id == 106)
        {
            g_pw_overlay = !g_pw_overlay;
            pw_sync_menus();
            return FALSE;
        }
        if (id >= 110 && id <= 117)
        {
            g_pw_size_cmd = id;
            pw_sync_menus();
            pw_apply_size_preset(ep);
            InvalidateRect(hwnd, NULL, FALSE);
            return FALSE;
        }
        if (id == 109)
        {
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            return FALSE;
        }
        if (id == IDC_PLAY_BTN_VOL)
        {
            static const int k_vols[5] = {256, 192, 128, 64, 0};
            static const wchar_t *k_vlbls[5] = {L"100%", L"75%", L"50%", L"25%", L"0% (Mute)"};
            HMENU vm = CreatePopupMenu();
            if (vm)
            {
                POINT pt;
                int k, pick;
                for (k = 0; k < 5; k++)
                {
                    UINT mf = MF_STRING | ((abs(g_pw_volume - k_vols[k]) <= 16) ? MF_CHECKED : 0);
                    AppendMenuW(vm, mf, (UINT_PTR)(200 + k), k_vlbls[k]);
                }
                GetCursorPos(&pt);
                pick = (int)TrackPopupMenu(vm, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                           pt.x, pt.y, 0, hwnd, NULL);
                if (pick >= 200 && pick < 205)
                    g_pw_volume = k_vols[pick - 200];
                DestroyMenu(vm);
            }
            return FALSE;
        }
        break;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc, vid_rc, bar_rc;
        GetClientRect(hwnd, &rc);
        vid_rc = rc;
        vid_rc.bottom = (rc.bottom > PLAY_BAR_H) ? (rc.bottom - PLAY_BAR_H) : rc.bottom;
        bar_rc = rc;
        bar_rc.top = vid_rc.bottom;

        FillRect(hdc, &bar_rc, (HBRUSH)(COLOR_BTNFACE + 1));
        if (g_pw_dib && g_pw_img_w > 0 && g_pw_img_h > 0 &&
            vid_rc.right > vid_rc.left && vid_rc.bottom > vid_rc.top)
        {
            BITMAPINFO bmi;
            int vw = vid_rc.right - vid_rc.left;
            int vh = vid_rc.bottom - vid_rc.top;
            int dw = vw, dh = vh, dx = 0, dy = 0;

            if ((long long)vw * g_pw_img_h > (long long)vh * g_pw_img_w)
            {
                dw = (int)(((long long)vh * g_pw_img_w) / g_pw_img_h);
                dx = (vw - dw) / 2;
            }
            else
            {
                dh = (int)(((long long)vw * g_pw_img_h) / g_pw_img_w);
                dy = (vh - dh) / 2;
            }

            if (dx > 0 || dy > 0)
                FillRect(hdc, &vid_rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

            ZeroMemory(&bmi, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = g_pw_img_w;
            bmi.bmiHeader.biHeight = g_pw_img_h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 24;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(hdc, COLORONCOLOR);
            StretchDIBits(hdc, dx, dy, dw, dh,
                          0, 0, g_pw_img_w, g_pw_img_h,
                          g_pw_dib, &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        else
        {
            FillRect(hdc, &vid_rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(hwnd, &ps);
        return FALSE;
    }
    }
    return FALSE;
}

// built-in FILTER_DLL table construction and DAT_00474c38 slot lookup

static char *g_snr_tn[3];
static int g_snr_td[3] = {256, 2, 24};
static int g_snr_ts[3] = {0, 1, 0};
static int g_snr_te[3] = {256, 3, 256};

static char *g_tnr_tn[3];
static int g_tnr_td[3] = {256, 3, 36};
static int g_tnr_ts[3] = {0, 1, 0};
static int g_tnr_te[3] = {256, 3, 256};

static char *g_shp_tn[4];
static int g_shp_td[4] = {256, 2, 128, 1024};
static int g_shp_ts[4] = {0, 1, 0, 0};
static int g_shp_te[4] = {256, 3, 1024, 1024};

static char *g_blr_tn[4];
static int g_blr_td[4] = {256, 2, 0, 1024};
static int g_blr_ts[4] = {0, 0, 0, 0};
static int g_blr_te[4] = {256, 3, 1024, 1024};

static char *g_clp_tn[4];
static int g_clp_td[4] = {0, 0, 0, 0};
static int g_clp_ts[4] = {0, 0, 0, 0};
static int g_clp_te[4] = {1024, 1024, 1024, 1024};

static char *g_edg_tn[4];
static int g_edg_td[4] = {0, 0, 0, 0};
static int g_edg_ts[4] = {-1024, -1024, -1024, -1024};
static int g_edg_te[4] = {1024, 1024, 1024, 1024};
static char *g_edg_cn[2];
static int g_edg_cd[2] = {0, 0};

static char *g_cc_tn[6];
static int g_cc_td[6] = {0, 0, 0, 0, 0, 0};
static int g_cc_ts[6] = {-256, -256, -256, -256, -256, -256};
static int g_cc_te[6] = {256, 256, 256, 256, 256, 256};

static char *g_ecc_tn[15] = {
    "Y(offs)", "Y(gain)", "Cb(offs)", "Cb(gain)", "Cr(offs)", "Cr(gain)",
    "R(offs)", "R(gain)", "R(gamm)", "G(offs)", "G(gain)", "G(gamm)",
    "B(offs)", "B(gain)", "B(gamm)"};
static int g_ecc_td[15] = {0};
static int g_ecc_ts[15] = {
    -256, -256, -256, -256, -256, -256, -256, -256, -256, -256, -256, -256, -256, -256, -256};
static int g_ecc_te[15] = {
    256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256};
static char *g_ecc_cn[3];
static int g_ecc_cd[3] = {0, 0, 0};

static char *g_apos_tn[1];
static int g_apos_td[1] = {0};
static int g_apos_ts[1] = {-999};
static int g_apos_te[1] = {999};

static char *g_avol_tn[1];
static int g_avol_td[1] = {0};
static int g_avol_ts[1] = {-256};
static int g_avol_te[1] = {256};

static char *g_amx_tn[1];
static int g_amx_td[1] = {256};
static int g_amx_ts[1] = {0};
static int g_amx_te[1] = {256};

// [15] framerate change & [16] framerate settings
static char *g_fps_cn[6];
static int g_fps_cd[6] = {1, 0, 0, 0, 0, 0};
static char *g_fps_cfg_cn[1];
static int g_fps_cfg_cd[1] = {0};

// [17] deinterlace & [18] field order
static char *g_deint_cn[6];
static int g_deint_cd[6] = {1, 0, 0, 0, 0, 0};
static char *g_fo_cn[2];
static int g_fo_cd[2] = {0, 1};

static char *g_oe_cn[2];
static int g_oe_cd[2] = {1, 1};

static char *g_a24_tn[2];
static int g_a24_td[2] = {64, 16};
static int g_a24_ts[2] = {0, 0};
static int g_a24_te[2] = {256, 256};
static char *g_a24_cn[1];
static int g_a24_cd[1] = {0};

// exact DAT_00474c38 fixed command slots (0..20) for PTR_DAT_004824f0[0..26]
static const int k_builtin_fixed_slots[BUILTIN_FILTER_COUNT] = {
    1,  // [ 0] Playback Window (res 96)  -> 11002
    2,  // [ 1] Tool Window (res 106)     -> 11003
    3,  // [ 2] Histogram (res 73)        -> 11004
    4,  // [ 3] Spatial NR (res 94)       -> 10005 / 11005
    5,  // [ 4] Temporal NR (res 95)      -> 10006 / 11006
    10, // [ 5] Sharpen (res 102)         -> 10011 / 11011
    11, // [ 6] Blur (res 61)             -> 10012 / 11012
    7,  // [ 7] Clipping & Resize (res 62)-> 10008 / 11008
    8,  // [ 8] Edge Fill (res 122)       -> 10009 / 11009
    12, // [ 9] Color Correction (res 66) -> 10013 / 11013
    13, // [10] Ext Color Corr (res 67)   -> 10014 / 11014
    14, // [11] Audio Position (res 115)  -> 10015 / 11015
    15, // [12] Volume (res 114)          -> 10016 / 11016
    16, // [13] Maximize Volume (res 60)  -> 10017 / 11017
    17, // [14] Size Change (res 100)     -> slot 17
    18, // [15] Framerate Change (res 97) -> slot 18
    -1, // [16] Framerate Settings (res 98)
    19, // [17] Deinterlace (res 74)      -> slot 19
    20, // [18] Field Order (res 109)     -> slot 20
    -1, // [19] None Interlace
    -1, // [20] Odd Interlace
    -1, // [21] Even Interlace
    -1, // [22] Blend Interlace
    -1, // [23] Auto24fps Interlace
    -1, // [24] Auto Interlace
    -1, // [25] Color Conversion In
    -1  // [26] Color Conversion Out ("__COLOR_OUT")
};

int builtin_filters_get_fixed_cmd_slot(int builtin_idx)
{
    if (builtin_idx < 0 || builtin_idx >= BUILTIN_FILTER_COUNT)
        return -1;
    return k_builtin_fixed_slots[builtin_idx];
}

void builtin_filters_reload_dynamic_lists(void)
{
    rebuild_size_change_items();
    rebuild_color_conv_items();
}

void builtin_filters_prepare_tables(HWND main_hwnd)
{
    FILTER_DLL *f;
    g_host_main_hwnd = main_hwnd;
    g_str_pool_used = 0;
    ZeroMemory(g_builtin_dlls, sizeof(g_builtin_dlls));

    f = &g_builtin_dlls[0];
    f->flag = 0x0200f004;
    f->x = 0x148;
    f->y = 0x139;
    f->name = alloc_res_sjis(96, "Playback");
    f->func_WndProc = wndproc_playback_window;

    f = &g_builtin_dlls[1];
    f->flag = 0x0200d004;
    f->x = 0x102;
    f->y = 0x2e;
    f->name = alloc_res_sjis(106, "ToolWindow");
    f->func_WndProc = wndproc_tool_window;

    f = &g_builtin_dlls[2];
    f->flag = 0x0200d004;
    f->x = 0x10000100;
    f->y = 0x100000f2;
    f->name = alloc_res_sjis(73, "Histogram");
    f->func_proc = proc_histogram;
    f->func_WndProc = wndproc_histogram;

    g_snr_tn[0] = alloc_res_sjis(42, "Strength");
    g_snr_tn[1] = alloc_res_sjis(43, "Range");
    g_snr_tn[2] = alloc_res_sjis(44, "Threshold");
    f = &g_builtin_dlls[3];
    f->flag = 0x02000000;
    f->name = alloc_res_sjis(94, "NoiseReduction");
    f->track_n = 3;
    f->track_name = g_snr_tn;
    f->track_default = g_snr_td;
    f->track_s = g_snr_ts;
    f->track_e = g_snr_te;
    f->func_proc = proc_spatial_nr;

    g_tnr_tn[0] = g_snr_tn[0];
    g_tnr_tn[1] = g_snr_tn[1];
    g_tnr_tn[2] = g_snr_tn[2];
    f = &g_builtin_dlls[4];
    f->flag = 0x0a000800;
    f->name = alloc_res_sjis(95, "TemporalNR");
    f->track_n = 3;
    f->track_name = g_tnr_tn;
    f->track_default = g_tnr_td;
    f->track_s = g_tnr_ts;
    f->track_e = g_tnr_te;
    f->func_proc = proc_temporal_nr;

    g_shp_tn[0] = g_snr_tn[0];
    g_shp_tn[1] = g_snr_tn[1];
    g_shp_tn[2] = alloc_res_sjis(45, "Lower");
    g_shp_tn[3] = alloc_res_sjis(46, "Upper");
    f = &g_builtin_dlls[5];
    f->flag = 0x02000000;
    f->name = alloc_res_sjis(102, "Sharpen");
    f->track_n = 4;
    f->track_name = g_shp_tn;
    f->track_default = g_shp_td;
    f->track_s = g_shp_ts;
    f->track_e = g_shp_te;
    f->func_proc = proc_sharpen;

    g_blr_tn[0] = g_snr_tn[0];
    g_blr_tn[1] = g_snr_tn[1];
    g_blr_tn[2] = g_shp_tn[2];
    g_blr_tn[3] = g_shp_tn[3];
    f = &g_builtin_dlls[6];
    f->flag = 0x02000000;
    f->name = alloc_res_sjis(61, "Blur");
    f->track_n = 4;
    f->track_name = g_blr_tn;
    f->track_default = g_blr_td;
    f->track_s = g_blr_ts;
    f->track_e = g_blr_te;
    f->func_proc = proc_blur;

    g_clp_tn[0] = alloc_res_sjis(49, "Top");
    g_clp_tn[1] = alloc_res_sjis(50, "Bottom");
    g_clp_tn[2] = alloc_res_sjis(51, "Left");
    g_clp_tn[3] = alloc_res_sjis(52, "Right");
    f = &g_builtin_dlls[7];
    f->flag = 0x02004400;
    f->x = 0x30000000;
    f->y = 0x3000001a;
    f->name = alloc_res_sjis(62, "ClipResize");
    f->track_n = 4;
    f->track_name = g_clp_tn;
    f->track_default = g_clp_td;
    f->track_s = g_clp_ts;
    f->track_e = g_clp_te;
    f->func_proc = proc_clip_resize;
    f->func_update = upd_clip_resize;
    f->func_WndProc = wndproc_clip_resize;
    f->ex_data_ptr = &g_clip_resize_ex;
    f->ex_data_size = (int)sizeof(g_clip_resize_ex);
    f->ex_data_def = (void *)&g_clip_resize_def;

    g_edg_tn[0] = g_clp_tn[0];
    g_edg_tn[1] = g_clp_tn[1];
    g_edg_tn[2] = g_clp_tn[2];
    g_edg_tn[3] = g_clp_tn[3];
    g_edg_cn[0] = alloc_res_sjis(128, "Center");
    g_edg_cn[1] = alloc_res_sjis(124, "EdgeColor");
    f = &g_builtin_dlls[8];
    f->flag = 0x02000000;
    f->name = alloc_res_sjis(122, "EdgeFill");
    f->track_n = 4;
    f->track_name = g_edg_tn;
    f->track_default = g_edg_td;
    f->track_s = g_edg_ts;
    f->track_e = g_edg_te;
    f->check_n = 2;
    f->check_name = g_edg_cn;
    f->check_default = g_edg_cd;
    f->func_proc = proc_edge_fill;

    g_cc_tn[0] = alloc_res_sjis(53, "Bright");
    g_cc_tn[1] = alloc_res_sjis(54, "Contrast");
    g_cc_tn[2] = alloc_res_sjis(56, "Gamma");
    g_cc_tn[3] = alloc_res_sjis(55, "Luma");
    g_cc_tn[4] = alloc_res_sjis(57, "Sat");
    g_cc_tn[5] = alloc_res_sjis(58, "Hue");
    f = &g_builtin_dlls[9];
    f->flag = 0x02001000;
    f->name = alloc_res_sjis(66, "ColorCorrection");
    f->track_n = 6;
    f->track_name = g_cc_tn;
    f->track_default = g_cc_td;
    f->track_s = g_cc_ts;
    f->track_e = g_cc_te;
    f->func_proc = proc_color_corr;

    g_ecc_cn[0] = alloc_res_sjis(69, "RGB Sync");
    g_ecc_cn[1] = alloc_res_sjis(68, "TV -> PC");
    g_ecc_cn[2] = alloc_res_sjis(125, "PC -> TV");
    f = &g_builtin_dlls[10];
    f->flag = 0x02001000;
    f->name = alloc_res_sjis(67, "ExtColorCorrection");
    f->track_n = 15;
    f->track_name = g_ecc_tn;
    f->track_default = g_ecc_td;
    f->track_s = g_ecc_ts;
    f->track_e = g_ecc_te;
    f->check_n = 3;
    f->check_name = g_ecc_cn;
    f->check_default = g_ecc_cd;
    f->func_proc = proc_ext_color_corr;
    f->func_update = upd_ext_color_corr;

    g_apos_tn[0] = alloc_res_sjis(116, "Pos");
    f = &g_builtin_dlls[11];
    f->flag = 0x02204000;
    f->x = 0x30000000;
    f->y = 0x30000012;
    f->name = alloc_res_sjis(115, "AudioPos");
    f->track_n = 1;
    f->track_name = g_apos_tn;
    f->track_default = g_apos_td;
    f->track_s = g_apos_ts;
    f->track_e = g_apos_te;
    f->func_WndProc = wndproc_audio_pos;

    g_avol_tn[0] = alloc_res_sjis(59, "Level");
    f = &g_builtin_dlls[12];
    f->flag = 0x02200000;
    f->name = alloc_res_sjis(114, "Volume");
    f->track_n = 1;
    f->track_name = g_avol_tn;
    f->track_default = g_avol_td;
    f->track_s = g_avol_ts;
    f->track_e = g_avol_te;
    f->func_proc = proc_audio_vol;

    g_amx_tn[0] = g_avol_tn[0];
    f = &g_builtin_dlls[13];
    f->flag = 0x02200800;
    f->name = alloc_res_sjis(60, "MaxVolume");
    f->track_n = 1;
    f->track_name = g_amx_tn;
    f->track_default = g_amx_td;
    f->track_s = g_amx_ts;
    f->track_e = g_amx_te;
    f->func_proc = proc_max_vol;
    f->func_save_start = save_start_max_vol;
    f->func_save_end = save_end_max_vol;

    f = &g_builtin_dlls[14];
    f->flag = 0x0200062c;
    f->name = alloc_res_sjis(100, "SizeChange");
    f->func_proc = proc_size_change;
    f->func_update = upd_size_change;
    f->ex_data_ptr = &g_size_change_ex;
    f->ex_data_size = (int)sizeof(g_size_change_ex);

    g_fps_cn[0] = alloc_res_sjis(12, "None");
    g_fps_cn[1] = "24fps <- 30fps  (4/5)";
    g_fps_cn[2] = "20fps <- 30fps  (2/3)";
    g_fps_cn[3] = "15fps <- 30fps  (1/2)";
    g_fps_cn[4] = "12fps <- 30fps  (2/5)";
    g_fps_cn[5] = "10fps <- 30fps  (1/3)";
    f = &g_builtin_dlls[15];
    f->flag = 0x020002ac;
    f->name = alloc_res_sjis(97, "FramerateChange");
    f->check_n = 6;
    f->check_name = g_fps_cn;
    f->check_default = g_fps_cd;
    f->func_update = upd_framerate_change;

    g_fps_cfg_cn[0] = alloc_res_sjis(99, "Auto24fps");
    f = &g_builtin_dlls[16];
    f->flag = 0x02000314;
    f->name = alloc_res_sjis(98, "FramerateSettings");
    f->check_n = 1;
    f->check_name = g_fps_cfg_cn;
    f->check_default = g_fps_cfg_cd;

    g_deint_cn[0] = alloc_res_sjis(12, "None");
    g_deint_cn[1] = alloc_res_sjis(17, "Odd");
    g_deint_cn[2] = alloc_res_sjis(18, "Even");
    g_deint_cn[3] = alloc_res_sjis(19, "Blend");
    g_deint_cn[4] = alloc_res_sjis(22, "Auto24fps");
    g_deint_cn[5] = alloc_res_sjis(21, "Auto");
    f = &g_builtin_dlls[17];
    f->flag = 0x020006ac;
    f->name = alloc_res_sjis(74, "Deinterlace");
    f->check_n = 6;
    f->check_name = g_deint_cn;
    f->check_default = g_deint_cd;
    f->func_update = upd_deinterlace;
    f->ex_data_ptr = g_deinterlace_ex;
    f->ex_data_size = (int)sizeof(g_deinterlace_ex);
    f->ex_data_def = (void *)g_deinterlace_def;

    g_fo_cn[0] = alloc_res_sjis(110, "Bottom -> Top");
    g_fo_cn[1] = alloc_res_sjis(111, "Top -> Bottom");
    f = &g_builtin_dlls[18];
    f->flag = 0x020003a4;
    f->name = alloc_res_sjis(109, "FieldOrder");
    f->check_n = 2;
    f->check_name = g_fo_cn;
    f->check_default = g_fo_cd;
    f->func_update = upd_field_order;

    f = &g_builtin_dlls[19];
    f->flag = 0x06100004;
    f->name = alloc_interlace_name(12);
    f->func_proc = proc_interlace_none;

    g_oe_cn[0] = alloc_res_sjis(82, "PosInterp");
    g_oe_cn[1] = alloc_res_sjis(83, "AdaptInterp");
    f = &g_builtin_dlls[20];
    f->flag = 0x06000004;
    f->name = alloc_interlace_name(17);
    f->check_n = 2;
    f->check_name = g_oe_cn;
    f->check_default = g_oe_cd;
    f->func_proc = proc_interlace_odd;
    f->func_update = upd_interlace_odd_even;

    f = &g_builtin_dlls[21];
    f->flag = 0x06000004;
    f->name = alloc_interlace_name(18);
    f->check_n = 2;
    f->check_name = g_oe_cn;
    f->check_default = g_oe_cd;
    f->func_proc = proc_interlace_even;
    f->func_update = upd_interlace_odd_even;

    f = &g_builtin_dlls[22];
    f->flag = 0x06100004;
    f->name = alloc_interlace_name(19);
    f->func_proc = proc_interlace_blend;

    g_a24_tn[0] = g_snr_tn[2];
    g_a24_tn[1] = g_snr_tn[1];
    g_a24_cn[0] = alloc_res_sjis(80, "BlendComb");
    f = &g_builtin_dlls[23];
    f->flag = 0x06000004;
    f->name = alloc_interlace_name(22);
    f->track_n = 2;
    f->track_name = g_a24_tn;
    f->track_default = g_a24_td;
    f->track_s = g_a24_ts;
    f->track_e = g_a24_te;
    f->check_n = 1;
    f->check_name = g_a24_cn;
    f->check_default = g_a24_cd;
    f->func_proc = proc_interlace_auto24;
    f->func_update = upd_field_order;

    f = &g_builtin_dlls[24];
    f->flag = 0x06100004;
    f->name = alloc_interlace_name(21);
    f->func_proc = proc_interlace_auto;

    f = &g_builtin_dlls[25];
    f->flag = 0x020002ac;
    f->name = alloc_res_sjis(136, "ColorConversion");
    f->func_update = upd_color_conv_in;

    f = &g_builtin_dlls[26];
    f->flag = 0x02000324;
    f->name = "__COLOR_OUT";
    f->func_update = upd_color_conv_out;

    builtin_filters_reload_dynamic_lists();
    g_tables_ready = TRUE;
}

int builtin_filters_get_count(void)
{
    return g_tables_ready ? BUILTIN_FILTER_COUNT : 0;
}

FILTER_DLL *builtin_filters_get_dll(int builtin_idx)
{
    if (!g_tables_ready || builtin_idx < 0 || builtin_idx >= BUILTIN_FILTER_COUNT)
        return NULL;
    return &g_builtin_dlls[builtin_idx];
}
