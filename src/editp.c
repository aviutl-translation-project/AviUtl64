#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "editp.h"
#include "exfunc.h"
#include "vfw_input.h"

// 0x0048717c, aviutl 1.10 EDIT_HANDLE layout accessed directly by exedit
#pragma pack(push, 1)
typedef struct
{
    uint32_t edit_flag;         // +0x000
    char edit_name[260];        // +0x004
    char output_name[260];      // +0x108
    char project_name[260];     // +0x20c
    int32_t w;                  // +0x310
    int32_t h;                  // +0x314
    int32_t frame_n;            // +0x318
    int32_t select_s;           // +0x31c
    int32_t select_e;           // +0x320
    int32_t pad_324;            // +0x324
    int32_t vram_cur_w;         // +0x328
    int32_t vram_cur_h;         // +0x32c
    int32_t frame;              // +0x330
    uint32_t ycp_disp_ptr32;    // +0x334
    uint32_t bmi_size;          // +0x338
    int32_t disp_w;             // +0x33c biWidth
    int32_t disp_h;             // +0x340 biHeight
    uint16_t bmi_planes;        // +0x344
    uint16_t bmi_bitcount;      // +0x346
    uint32_t bmi_compression;   // +0x348
    uint32_t bmi_size_image;    // +0x34c
    int32_t bmi_xpels;          // +0x350
    int32_t bmi_ypels;          // +0x354
    uint32_t bmi_clr_used;      // +0x358
    uint32_t bmi_clr_important; // +0x35c
    uint8_t pad_360[0x0c];      // +0x360..+0x36b
    uint32_t preview_flags;     // +0x36c
    int32_t win_x;              // +0x370
    int32_t win_y;              // +0x374
    int32_t client_w;           // +0x378
    int32_t client_h;           // +0x37c
    int32_t zoom;               // +0x380
    uint32_t main_hwnd32;       // +0x384
    uint32_t trackbar_hwnd32;   // +0x388
    uint32_t btn_back_hwnd32;   // +0x38c
    uint32_t btn_fwd_hwnd32;    // +0x390
    uint32_t btn_in_hwnd32;     // +0x394
    uint32_t btn_out_hwnd32;    // +0x398
    uint32_t overlay_fourcc;    // +0x39c
    int32_t dst_x;              // +0x3a0
    int32_t dst_y;              // +0x3a4
    int32_t dst_w;              // +0x3a8
    int32_t dst_h;              // +0x3ac
    int32_t src_vis_w;          // +0x3b0
    int32_t src_vis_h;          // +0x3b4
    uint32_t btn_play_hwnd32;   // +0x3b8
    uint8_t pad_3bc[0xac];      // +0x3bc..+0x467
    int32_t video_scale;        // +0x468
    int32_t video_rate;         // +0x46c
    uint8_t pad_470[0x90];      // +0x470..+0x4ff
} AviUtlEditHandleABI;
#pragma pack(pop)

typedef char reaviutl_editp_off_004[(offsetof(AviUtlEditHandleABI, edit_name) == 0x004) ? 1 : -1];
typedef char reaviutl_editp_off_108[(offsetof(AviUtlEditHandleABI, output_name) == 0x108) ? 1 : -1];
typedef char reaviutl_editp_off_20c[(offsetof(AviUtlEditHandleABI, project_name) == 0x20c) ? 1 : -1];
typedef char reaviutl_editp_off_310[(offsetof(AviUtlEditHandleABI, w) == 0x310) ? 1 : -1];
typedef char reaviutl_editp_off_318[(offsetof(AviUtlEditHandleABI, frame_n) == 0x318) ? 1 : -1];
typedef char reaviutl_editp_off_328[(offsetof(AviUtlEditHandleABI, vram_cur_w) == 0x328) ? 1 : -1];
typedef char reaviutl_editp_off_32c[(offsetof(AviUtlEditHandleABI, vram_cur_h) == 0x32c) ? 1 : -1];
typedef char reaviutl_editp_off_330[(offsetof(AviUtlEditHandleABI, frame) == 0x330) ? 1 : -1];
typedef char reaviutl_editp_off_334[(offsetof(AviUtlEditHandleABI, ycp_disp_ptr32) == 0x334) ? 1 : -1];
typedef char reaviutl_editp_off_338[(offsetof(AviUtlEditHandleABI, bmi_size) == 0x338) ? 1 : -1];
typedef char reaviutl_editp_off_33c[(offsetof(AviUtlEditHandleABI, disp_w) == 0x33c) ? 1 : -1];
typedef char reaviutl_editp_off_340[(offsetof(AviUtlEditHandleABI, disp_h) == 0x340) ? 1 : -1];
typedef char reaviutl_editp_off_36c[(offsetof(AviUtlEditHandleABI, preview_flags) == 0x36c) ? 1 : -1];
typedef char reaviutl_editp_off_380[(offsetof(AviUtlEditHandleABI, zoom) == 0x380) ? 1 : -1];
typedef char reaviutl_editp_off_384[(offsetof(AviUtlEditHandleABI, main_hwnd32) == 0x384) ? 1 : -1];
typedef char reaviutl_editp_off_388[(offsetof(AviUtlEditHandleABI, trackbar_hwnd32) == 0x388) ? 1 : -1];
typedef char reaviutl_editp_off_3a0[(offsetof(AviUtlEditHandleABI, dst_x) == 0x3a0) ? 1 : -1];
typedef char reaviutl_editp_off_3b8[(offsetof(AviUtlEditHandleABI, btn_play_hwnd32) == 0x3b8) ? 1 : -1];
typedef char reaviutl_editp_off_468[(offsetof(AviUtlEditHandleABI, video_scale) == 0x468) ? 1 : -1];
typedef char reaviutl_editp_off_46c[(offsetof(AviUtlEditHandleABI, video_rate) == 0x46c) ? 1 : -1];
typedef char reaviutl_editp_abi_sz[(sizeof(AviUtlEditHandleABI) == 0x500) ? 1 : -1];

struct EDITP_T
{
    AviUtlEditHandleABI abi; // +0x000..+0x4ff: 32-bit aviutl layout

    BOOL is_editing;
    BOOL is_saving;
    BOOL has_audio;
    HWND main_hwnd;
    HWND trackbar_hwnd;

    int w, h;
    int org_w, org_h;
    int max_w, max_h;
    int frame_n;
    int frame;
    int select_s, select_e;
    int shift_anchor;
    int fps_scale;
    int fps_rate;
    int audio_rate;
    int audio_ch;

    PIXEL_YC *ycp_edit;
    PIXEL_YC *ycp_temp;
    BYTE *frame_status;
    int frame_status_cap;

    VFWInput *video_source;
    VFWInput *audio_source;
};

static int g_registered_filter_count = 0;
static HFONT g_ui_font = NULL;
static char g_version_info[] = "AviUtl version 1.10";
static char g_empty_str[] = "";

void editp_set_registered_filter_count(int count)
{
    g_registered_filter_count = count >= 0 ? count : 0;
}

int editp_get_registered_filter_count(void)
{
    return g_registered_filter_count;
}

HFONT editp_get_ui_font(void)
{
    if (!g_ui_font)
    {
        g_ui_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                DEFAULT_QUALITY, DEFAULT_PITCH, "MS UI Gothic");
        if (!g_ui_font)
        {
            g_ui_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        }
    }
    return g_ui_font;
}

static void editp_sync_abi(EDITP *ep)
{
    if (!ep)
        return;
    ep->abi.edit_flag = ep->is_editing ? 1u : 0u;
    ep->abi.w = ep->w;
    ep->abi.h = ep->h;
    ep->abi.frame_n = ep->frame_n;
    ep->abi.select_s = ep->select_s;
    ep->abi.select_e = ep->select_e;
    ep->abi.vram_cur_w = ep->w;
    ep->abi.vram_cur_h = ep->h;
    ep->abi.frame = ep->frame;
    ep->abi.bmi_size = sizeof(BITMAPINFOHEADER);
    ep->abi.disp_w = ep->w;
    ep->abi.disp_h = ep->h;
    ep->abi.bmi_planes = 1;
    ep->abi.bmi_bitcount = 24;
    ep->abi.bmi_compression = BI_RGB;
    ep->abi.video_scale = ep->fps_scale > 0 ? ep->fps_scale : 1;
    ep->abi.video_rate = ep->fps_rate > 0 ? ep->fps_rate : 30;
    ep->abi.main_hwnd32 = (uint32_t)(uintptr_t)ep->main_hwnd;
#if !REAVIUTL_X64
    ep->abi.trackbar_hwnd32 = (uint32_t)(uintptr_t)ep->trackbar_hwnd;
    ep->abi.ycp_disp_ptr32 = (uint32_t)(uintptr_t)ep->ycp_edit;
#else
    ep->abi.trackbar_hwnd32 = 0;
    ep->abi.ycp_disp_ptr32 = 0;
#endif
}

static BOOL editp_ensure_frame_status(EDITP *ep, int needed)
{
    int new_cap;
    BYTE *nb;
    if (!ep)
        return FALSE;
    if (needed < 256)
        needed = 256;
    if (needed <= ep->frame_status_cap && ep->frame_status)
    {
        return TRUE;
    }
    new_cap = ep->frame_status_cap ? ep->frame_status_cap : 1024;
    while (new_cap < needed)
    {
        new_cap *= 2;
    }
    if (new_cap > REAVIUTL_MAX_FRAME)
    {
        new_cap = (needed > REAVIUTL_MAX_FRAME) ? needed : REAVIUTL_MAX_FRAME;
    }
    nb = (BYTE *)realloc(ep->frame_status, (size_t)new_cap);
    if (!nb)
        return FALSE;
    if (new_cap > ep->frame_status_cap)
    {
        memset(nb + ep->frame_status_cap, 0,
               (size_t)(new_cap - ep->frame_status_cap));
    }
    ep->frame_status = nb;
    ep->frame_status_cap = new_cap;
    return TRUE;
}

static BOOL editp_ensure_vram_buffers(EDITP *ep, int req_w, int req_h)
{
    int mw = REAVIUTL_VRAM_W;
    int mh = REAVIUTL_VRAM_H;
    size_t count;

    if (req_w > mw)
        mw = (req_w + 15) & ~15;
    if (req_h > mh)
        mh = (req_h + 15) & ~15;

    if (ep->ycp_edit && ep->ycp_temp && mw <= ep->max_w && mh <= ep->max_h)
    {
        return TRUE;
    }

    free(ep->ycp_edit);
    free(ep->ycp_temp);
    count = (size_t)mw * (size_t)mh;
    ep->ycp_edit = (PIXEL_YC *)calloc(count, sizeof(PIXEL_YC));
    ep->ycp_temp = (PIXEL_YC *)calloc(count, sizeof(PIXEL_YC));
    if (!ep->ycp_edit || !ep->ycp_temp)
    {
        free(ep->ycp_edit);
        free(ep->ycp_temp);
        ep->ycp_edit = NULL;
        ep->ycp_temp = NULL;
        return FALSE;
    }
    ep->max_w = mw;
    ep->max_h = mh;
    return TRUE;
}

EDITP *editp_create_idle(HWND main_hwnd)
{
    EDITP *ep = (EDITP *)calloc(1, sizeof(EDITP));
    if (!ep)
        return NULL;

    ep->main_hwnd = main_hwnd;
    ep->is_editing = FALSE;
    ep->has_audio = FALSE;
    ep->w = 0;
    ep->h = 0;
    ep->frame_n = 0;
    ep->frame = 0;
    ep->select_s = -1;
    ep->select_e = -1;
    ep->shift_anchor = -1;
    ep->fps_scale = 1;
    ep->fps_rate = 30;
    ep->audio_rate = 44100;
    ep->audio_ch = 2;

    if (!editp_ensure_vram_buffers(ep, REAVIUTL_VRAM_W, REAVIUTL_VRAM_H) ||
        !editp_ensure_frame_status(ep, 4096))
    {
        editp_destroy(ep);
        return NULL;
    }
    editp_sync_abi(ep);
    return ep;
}

EDITP *editp_create(int w, int h, int frame_n, int fps_scale, int fps_rate)
{
    EDITP *ep = editp_create_idle(NULL);
    if (!ep)
        return NULL;

    if (w <= 0)
        w = 640;
    if (h <= 0)
        h = 480;
    if (frame_n <= 0)
        frame_n = 1;
    if (fps_scale <= 0)
        fps_scale = 1;
    if (fps_rate <= 0)
        fps_rate = 30;

    if (!editp_ensure_vram_buffers(ep, w, h) ||
        !editp_ensure_frame_status(ep, frame_n))
    {
        editp_destroy(ep);
        return NULL;
    }

    ep->is_editing = TRUE;
    ep->has_audio = TRUE;
    ep->w = w;
    ep->h = h;
    ep->org_w = w;
    ep->org_h = h;
    ep->frame_n = frame_n;
    ep->frame = 0;
    ep->select_s = 0;
    ep->select_e = frame_n - 1;
    ep->shift_anchor = -1;
    ep->fps_scale = fps_scale;
    ep->fps_rate = fps_rate;
    ep->audio_rate = 44100;
    ep->audio_ch = 2;
    memset(ep->frame_status, 0, (size_t)frame_n);
    lstrcpynA(ep->abi.edit_name, "(untitled)", MAX_PATH);
    editp_sync_abi(ep);
    return ep;
}

static void editp_get_ini_path(char *out_path, size_t out_sz)
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

BOOL editp_open_video(EDITP *ep, const char *filepath)
{
    VFWInput *video;
    FILE_INFO fi;
    int proj_w, proj_h, proj_fn, fps_r, fps_s, aud_r, aud_ch;

    if (!ep || !filepath || !filepath[0])
    {
        return FALSE;
    }

    video = vfw_input_open(filepath, &fi);
    if (!video)
    {
        return FALSE;
    }

    proj_w = fi.w;
    proj_h = fi.h;
    fps_s = fi.video_scale > 0 ? fi.video_scale : 1;
    fps_r = fi.video_rate > 0 ? fi.video_rate : 30;
    aud_r = fi.audio_rate > 0 ? fi.audio_rate : 44100;
    aud_ch = fi.audio_ch > 0 ? fi.audio_ch : 2;
    proj_fn = fi.frame_n;

    if ((fi.flag & FILE_INFO_FLAG_VIDEO) == 0 && (fi.flag & FILE_INFO_FLAG_AUDIO) != 0)
    {
        // audio-only input (.mp3 / .wav): synthesize a 640x480 30fps black canvas sized to the audio track length so aviutl timeline scrubbing & playback work
        proj_w = 640;
        proj_h = 480;
        fps_r = 30;
        fps_s = 1;
        proj_fn = (int)(((long long)fi.audio_n * fps_r + (long long)aud_r * fps_s - 1) /
                        ((long long)aud_r * fps_s));
    }
    else if ((fi.flag & FILE_INFO_FLAG_VIDEO) != 0 && strstr(filepath, ".exedit") == NULL)
    {
        char ini[MAX_PATH];
        int load30, loadadj;
        editp_get_ini_path(ini, sizeof(ini));
        load30 = (int)GetPrivateProfileIntA("system", "load30fps", 0, ini);
        loadadj = (int)GetPrivateProfileIntA("system", "loadfpsadjust", 0, ini);

        // 0x00401d10: auto-adjust fps when video & audio durations diverge by >= 0.1s
        if (loadadj != 0 && (fi.flag & FILE_INFO_FLAG_AUDIO) != 0 &&
            proj_fn > 0 && fi.audio_n > 0 && aud_r > 0)
        {
            double vid_sec = ((double)proj_fn * (double)fps_s) / (double)fps_r;
            double aud_sec = (double)fi.audio_n / (double)aud_r;
            if (aud_sec > 0.1 && fabs(vid_sec - aud_sec) >= 0.1)
            {
                double nfps = (double)proj_fn / aud_sec;
                if (nfps > 0.01 && nfps <= 10000.0)
                {
                    fps_r = (int)(nfps * 100000.0 + 0.5);
                    fps_s = 100000;
                }
            }
        }

        // 0x00416b70 lines 219..223: open near-30fps video as NTSC 29.97fps (30000/1001)
        if (load30 != 0)
        {
            double fps_val = (double)fps_r / (double)fps_s;
            if (fps_val >= 29.90 && fps_val <= 30.05)
            {
                fps_r = 30000;
                fps_s = 1001;
            }
        }
    }
    if (proj_fn <= 0)
        proj_fn = 1;

    if (!editp_ensure_vram_buffers(ep, proj_w, proj_h) ||
        !editp_ensure_frame_status(ep, proj_fn > 64 ? proj_fn : 64))
    {
        vfw_input_close(video);
        return FALSE;
    }

    if (ep->audio_source)
    {
        vfw_input_close(ep->audio_source);
        ep->audio_source = NULL;
    }
    if (ep->video_source)
    {
        vfw_input_close(ep->video_source);
    }

    ep->video_source = video;
    ep->is_editing = TRUE;
    ep->has_audio = (fi.flag & FILE_INFO_FLAG_AUDIO) != 0;
    ep->w = proj_w;
    ep->h = proj_h;
    ep->org_w = proj_w;
    ep->org_h = proj_h;
    ep->frame_n = proj_fn;
    ep->frame = 0;
    ep->select_s = 0;
    ep->select_e = ep->frame_n - 1;
    ep->shift_anchor = -1;
    ep->fps_scale = fps_s;
    ep->fps_rate = fps_r;
    ep->audio_rate = aud_r;
    ep->audio_ch = aud_ch;

    vfw_input_set_audio_format(video, ep->audio_rate, ep->audio_ch);

    lstrcpynA(ep->abi.edit_name, filepath, MAX_PATH);
    memset(ep->frame_status, 0, (size_t)ep->frame_n);
    editp_sync_abi(ep);
    exfunc_invalidate_all_caches();
    editp_load_frame(ep, 0);
    return TRUE;
}

BOOL editp_open_audio(EDITP *ep, const char *filepath)
{
    VFWInput *aud;
    FILE_INFO fi;

    if (!ep || !filepath || !filepath[0])
    {
        return FALSE;
    }
    if (!ep->is_editing)
    {
        return editp_open_video(ep, filepath);
    }

    aud = vfw_input_open_ex(filepath, &fi, AVI_FILE_OPEN_FLAG_AUDIO_ONLY);
    if (!aud || (fi.flag & FILE_INFO_FLAG_AUDIO) == 0)
    {
        if (aud)
            vfw_input_close(aud);
        return FALSE;
    }

    if (!ep->has_audio && fi.audio_rate > 0 && fi.audio_ch > 0)
    {
        ep->audio_rate = fi.audio_rate;
        ep->audio_ch = fi.audio_ch;
    }
    vfw_input_set_audio_format(aud,
                               ep->audio_rate > 0 ? ep->audio_rate : 44100,
                               ep->audio_ch > 0 ? ep->audio_ch : 2);

    if (ep->audio_source)
    {
        vfw_input_close(ep->audio_source);
    }
    ep->audio_source = aud;
    ep->has_audio = TRUE;
    return TRUE;
}

void editp_close_video(EDITP *ep)
{
    if (!ep)
        return;
    if (ep->audio_source)
    {
        vfw_input_close(ep->audio_source);
        ep->audio_source = NULL;
    }
    if (ep->video_source)
    {
        vfw_input_close(ep->video_source);
        ep->video_source = NULL;
    }
    ep->is_editing = FALSE;
    ep->has_audio = FALSE;
    ep->w = 0;
    ep->h = 0;
    ep->org_w = 0;
    ep->org_h = 0;
    ep->frame_n = 0;
    ep->frame = 0;
    ep->select_s = -1;
    ep->select_e = -1;
    ep->shift_anchor = -1;
    ep->abi.edit_name[0] = '\0';
    ep->abi.output_name[0] = '\0';
    ep->abi.project_name[0] = '\0';
    if (ep->ycp_edit)
    {
        memset(ep->ycp_edit, 0, (size_t)ep->max_w * (size_t)ep->max_h * sizeof(PIXEL_YC));
    }
    editp_sync_abi(ep);
    exfunc_invalidate_all_caches();
}

EDITP *editp_create_from_video(const char *filepath)
{
    EDITP *ep = editp_create_idle(NULL);
    if (!ep)
        return NULL;
    if (!editp_open_video(ep, filepath))
    {
        editp_destroy(ep);
        return NULL;
    }
    return ep;
}

void editp_destroy(EDITP *ep)
{
    if (!ep)
        return;
    if (ep->audio_source)
        vfw_input_close(ep->audio_source);
    if (ep->video_source)
        vfw_input_close(ep->video_source);
    free(ep->ycp_edit);
    free(ep->ycp_temp);
    free(ep->frame_status);
    free(ep);
}

void editp_set_main_hwnd(EDITP *ep, HWND hwnd)
{
    if (!ep)
        return;
    ep->main_hwnd = hwnd;
    editp_sync_abi(ep);
}

HWND editp_get_main_hwnd(EDITP *ep)
{
    return ep ? ep->main_hwnd : NULL;
}

BOOL editp_load_frame(EDITP *ep, int frame_num)
{
    int y;
    if (!ep || !ep->is_editing || ep->frame_n <= 0)
    {
        return FALSE;
    }
    if (frame_num < 0)
        frame_num = 0;
    if (frame_num >= ep->frame_n)
        frame_num = ep->frame_n - 1;

    ep->frame = frame_num;
    ep->abi.frame = frame_num;

    if (ep->video_source)
    {
        if (!vfw_input_read_frame_stride(ep->video_source, frame_num,
                                         ep->ycp_edit, ep->max_w))
        {
            // beyond underlying source length (e.g. exedit extended timeline): clear to black
            for (y = 0; y < ep->h; y++)
            {
                memset(ep->ycp_edit + (size_t)y * (size_t)ep->max_w, 0,
                       (size_t)ep->w * sizeof(PIXEL_YC));
            }
        }
    }
    else
    {
        for (y = 0; y < ep->h; y++)
        {
            memset(ep->ycp_edit + (size_t)y * (size_t)ep->max_w, 0,
                   (size_t)ep->w * sizeof(PIXEL_YC));
        }
    }
    return TRUE;
}

BOOL editp_read_source_frame(EDITP *ep, int frame_num, PIXEL_YC *dst, int dst_stride)
{
    int y, cw;
    if (!ep || !ep->is_editing || ep->frame_n <= 0 || !dst || dst_stride <= 0)
    {
        return FALSE;
    }
    if (frame_num < 0)
        frame_num = 0;
    if (frame_num >= ep->frame_n)
        frame_num = ep->frame_n - 1;

    if (ep->video_source)
    {
        if (vfw_input_read_frame_stride(ep->video_source, frame_num, dst, dst_stride))
        {
            return TRUE;
        }
    }
    cw = (ep->org_w > 0 ? ep->org_w : ep->w);
    if (cw > dst_stride)
        cw = dst_stride;
    for (y = 0; y < (ep->org_h > 0 ? ep->org_h : ep->h); y++)
    {
        memset(dst + (size_t)y * (size_t)dst_stride, 0, (size_t)cw * sizeof(PIXEL_YC));
    }
    return TRUE;
}

BOOL editp_is_editing(EDITP *ep)
{
    return ep ? ep->is_editing : FALSE;
}
BOOL editp_is_saving(EDITP *ep)
{
    return ep ? ep->is_saving : FALSE;
}
void editp_set_saving(EDITP *ep, BOOL saving)
{
    if (ep)
        ep->is_saving = saving;
}

PIXEL_YC *editp_get_ycp_edit(EDITP *ep)
{
    return ep ? ep->ycp_edit : NULL;
}
PIXEL_YC *editp_get_ycp_temp(EDITP *ep)
{
    return ep ? ep->ycp_temp : NULL;
}

void editp_swap_ycp(EDITP *ep, PIXEL_YC *new_edit, PIXEL_YC *new_temp)
{
    if (!ep || !new_edit || !new_temp)
        return;
    ep->ycp_edit = new_edit;
    ep->ycp_temp = new_temp;
    editp_sync_abi(ep);
}

void editp_get_dimensions(EDITP *ep, int *w, int *h, int *max_w, int *max_h)
{
    if (!ep)
    {
        if (w)
            *w = 0;
        if (h)
            *h = 0;
        if (max_w)
            *max_w = REAVIUTL_VRAM_W;
        if (max_h)
            *max_h = REAVIUTL_VRAM_H;
        return;
    }
    if (w)
        *w = ep->w;
    if (h)
        *h = ep->h;
    if (max_w)
        *max_w = ep->max_w;
    if (max_h)
        *max_h = ep->max_h;
}

int editp_get_frame(EDITP *ep)
{
    return (ep && ep->is_editing) ? ep->frame : 0;
}

int editp_set_frame(EDITP *ep, int n)
{
    if (!ep || !ep->is_editing || ep->frame_n <= 0)
        return 0;
    if (n < 0)
        n = 0;
    if (n >= ep->frame_n)
        n = ep->frame_n - 1;
    ep->frame = n;
    ep->abi.frame = n;
    return ep->frame;
}

int editp_get_frame_n(EDITP *ep)
{
    return (ep && ep->is_editing) ? ep->frame_n : 0;
}

int editp_set_frame_n(EDITP *ep, int n)
{
    if (!ep || !ep->is_editing)
        return 0;
    if (n < 1)
        n = 1;
    if (n > REAVIUTL_MAX_FRAME)
        n = REAVIUTL_MAX_FRAME;
    editp_ensure_frame_status(ep, n);
    if (ep->select_e == ep->frame_n - 1 || ep->select_e >= n)
    {
        ep->select_e = n - 1;
    }
    ep->frame_n = n;
    if (ep->frame >= ep->frame_n)
    {
        ep->frame = ep->frame_n - 1;
    }
    ep->abi.frame_n = ep->frame_n;
    ep->abi.select_e = ep->select_e;
    ep->abi.frame = ep->frame;
    return ep->frame_n;
}

BOOL editp_get_select_frame(EDITP *ep, int *s, int *e)
{
    if (!ep || !ep->is_editing || ep->frame_n <= 0)
    {
        if (s)
            *s = -1;
        if (e)
            *e = -1;
        return FALSE;
    }
    if (s)
        *s = ep->select_s;
    if (e)
        *e = ep->select_e;
    return TRUE;
}

BOOL editp_set_select_frame(EDITP *ep, int s, int e)
{
    if (!ep || !ep->is_editing || ep->frame_n <= 0)
        return FALSE;
    if (s < 0)
        s = 0;
    if (e >= ep->frame_n)
        e = ep->frame_n - 1;
    if (s > e)
        e = s;
    ep->select_s = s;
    ep->select_e = e;
    ep->abi.select_s = s;
    ep->abi.select_e = e;
    return TRUE;
}

BOOL editp_get_frame_status(EDITP *ep, int n, BYTE *fps)
{
    if (!ep || !ep->is_editing || !fps || n < 0 || n >= ep->frame_n || !ep->frame_status)
    {
        return FALSE;
    }
    *fps = ep->frame_status[n];
    return TRUE;
}

BOOL editp_set_frame_status(EDITP *ep, int n, BYTE fps)
{
    if (!ep || !ep->is_editing || n < 0 || n >= ep->frame_n)
    {
        return FALSE;
    }
    if (!editp_ensure_frame_status(ep, n + 1))
    {
        return FALSE;
    }
    ep->frame_status[n] = fps;
    return TRUE;
}

BYTE *editp_get_frame_status_table(EDITP *ep, int type)
{
    (void)type;
    if (!ep || !ep->frame_status)
    {
        return NULL;
    }
    return ep->frame_status;
}

int editp_read_audio_frame(EDITP *ep, int frame_num, short *buf)
{
    int rate, ch, fps_r, fps_s;
    long long s0, s1;
    int count;

    if (!ep || !ep->is_editing)
        return 0;
    rate = ep->audio_rate > 0 ? ep->audio_rate : 44100;
    ch = ep->audio_ch > 0 ? ep->audio_ch : 2;
    fps_r = ep->fps_rate > 0 ? ep->fps_rate : 30;
    fps_s = ep->fps_scale > 0 ? ep->fps_scale : 1;

    s0 = ((long long)frame_num * rate * fps_s) / fps_r;
    s1 = ((long long)(frame_num + 1) * rate * fps_s) / fps_r;
    count = (int)(s1 - s0);
    if (count < 0)
        count = 0;
    if (!buf)
        return count;

    if (ep->audio_source)
    {
        vfw_input_read_audio(ep->audio_source, (int)s0, buf, count);
        return count;
    }
    if (ep->video_source)
    {
        vfw_input_read_audio(ep->video_source, (int)s0, buf, count);
        return count;
    }
    memset(buf, 0, (size_t)count * (size_t)ch * sizeof(short));
    return count;
}

int editp_get_audio_rate(EDITP *ep)
{
    return ep && ep->audio_rate > 0 ? ep->audio_rate : 44100;
}
int editp_get_audio_ch(EDITP *ep)
{
    return ep && ep->audio_ch > 0 ? ep->audio_ch : 2;
}

void editp_set_project_path(EDITP *ep, const char *path)
{
    if (!ep)
        return;
    if (path)
    {
        lstrcpynA(ep->abi.project_name, path, MAX_PATH);
    }
    else
    {
        ep->abi.project_name[0] = '\0';
    }
}

const char *editp_get_project_path(EDITP *ep)
{
    if (!ep)
        return g_empty_str;
    return ep->abi.project_name;
}

void editp_set_output_path(EDITP *ep, const char *path)
{
    if (!ep)
        return;
    if (path)
    {
        lstrcpynA(ep->abi.output_name, path, MAX_PATH);
    }
    else
    {
        ep->abi.output_name[0] = '\0';
    }
}

const char *editp_get_output_path(EDITP *ep)
{
    if (!ep)
        return g_empty_str;
    return ep->abi.output_name;
}

const char *editp_get_edit_path(EDITP *ep)
{
    if (!ep)
        return g_empty_str;
    return ep->abi.edit_name;
}

void editp_set_dimensions(EDITP *ep, int w, int h)
{
    if (!ep || w <= 0 || h <= 0)
        return;
    editp_ensure_vram_buffers(ep, w, h);
    if (w > ep->max_w)
        w = ep->max_w;
    if (h > ep->max_h)
        h = ep->max_h;
    ep->w = w;
    ep->h = h;
    ep->abi.w = w;
    ep->abi.h = h;
    ep->abi.vram_cur_w = w;
    ep->abi.vram_cur_h = h;
}

void editp_set_display_size(EDITP *ep, int disp_w, int disp_h)
{
    if (!ep || disp_w <= 0 || disp_h <= 0)
        return;
    ep->abi.disp_w = disp_w;
    ep->abi.disp_h = disp_h;
}

void editp_get_display_size(EDITP *ep, int *disp_w, int *disp_h)
{
    if (disp_w)
        *disp_w = ep ? ep->abi.disp_w : 0;
    if (disp_h)
        *disp_h = ep ? ep->abi.disp_h : 0;
}

void editp_get_fps(EDITP *ep, int *rate, int *scale)
{
    if (ep)
    {
        if (ep->abi.video_scale > 0)
            ep->fps_scale = ep->abi.video_scale;
        if (ep->abi.video_rate > 0)
            ep->fps_rate = ep->abi.video_rate;
    }
    if (rate)
        *rate = (ep && ep->fps_rate > 0) ? ep->fps_rate : 30;
    if (scale)
        *scale = (ep && ep->fps_scale > 0) ? ep->fps_scale : 1;
}

void editp_set_fps(EDITP *ep, int rate, int scale)
{
    if (!ep)
        return;
    if (rate <= 0)
        rate = 30;
    if (scale <= 0)
        scale = 1;
    ep->fps_rate = rate;
    ep->fps_scale = scale;
    ep->abi.video_rate = rate;
    ep->abi.video_scale = scale;
}

void editp_set_audio_format(EDITP *ep, int audio_rate, int audio_ch, BOOL has_audio)
{
    if (!ep)
        return;
    if (audio_rate > 0)
        ep->audio_rate = audio_rate;
    if (audio_ch > 0)
        ep->audio_ch = audio_ch;
    ep->has_audio = has_audio ? TRUE : FALSE;
    if (ep->video_source && ep->audio_rate > 0 && ep->audio_ch > 0)
    {
        vfw_input_set_audio_format(ep->video_source, ep->audio_rate, ep->audio_ch);
    }
    if (ep->audio_source && ep->audio_rate > 0 && ep->audio_ch > 0)
    {
        vfw_input_set_audio_format(ep->audio_source, ep->audio_rate, ep->audio_ch);
    }
}

uint32_t editp_get_trackbar_hwnd32(EDITP *ep)
{
    return ep ? ep->abi.trackbar_hwnd32 : 0;
}

void editp_set_trackbar_hwnd(EDITP *ep, HWND hwnd_tb)
{
    if (!ep)
        return;
    ep->trackbar_hwnd = hwnd_tb;
#if !REAVIUTL_X64
    ep->abi.trackbar_hwnd32 = (uint32_t)(uintptr_t)hwnd_tb;
#else
    ep->abi.trackbar_hwnd32 = 0;
#endif
}

void editp_sync_preview_state(EDITP *ep, uint32_t flags,
                              int win_x, int win_y, int client_w, int client_h, int zoom,
                              HWND btn_back, HWND btn_fwd, HWND btn_in, HWND btn_out, HWND btn_play,
                              int dst_x, int dst_y, int dst_w, int dst_h,
                              int src_vis_w, int src_vis_h)
{
    if (!ep)
        return;
    ep->abi.preview_flags = flags;
    ep->abi.win_x = win_x;
    ep->abi.win_y = win_y;
    ep->abi.client_w = client_w;
    ep->abi.client_h = client_h;
    ep->abi.zoom = zoom;
    ep->abi.dst_x = dst_x;
    ep->abi.dst_y = dst_y;
    ep->abi.dst_w = dst_w;
    ep->abi.dst_h = dst_h;
    ep->abi.src_vis_w = src_vis_w;
    ep->abi.src_vis_h = src_vis_h;
#if !REAVIUTL_X64
    ep->abi.btn_back_hwnd32 = (uint32_t)(uintptr_t)btn_back;
    ep->abi.btn_fwd_hwnd32 = (uint32_t)(uintptr_t)btn_fwd;
    ep->abi.btn_in_hwnd32 = (uint32_t)(uintptr_t)btn_in;
    ep->abi.btn_out_hwnd32 = (uint32_t)(uintptr_t)btn_out;
    ep->abi.btn_play_hwnd32 = (uint32_t)(uintptr_t)btn_play;
#else
    (void)btn_back;
    (void)btn_fwd;
    (void)btn_in;
    (void)btn_out;
    (void)btn_play;
#endif
}

// 0x0041d260, frame seek with Shift-drag selection anchor
BOOL editp_seek_frame_ex(EDITP *ep, int target_frame, BOOL shift_held, BOOL shift_select_cfg)
{
    int prev_frame;
    BOOL sel_changed = FALSE;

    if (!ep || !ep->is_editing || ep->frame_n <= 0)
        return FALSE;
    if (target_frame < 0)
        target_frame = 0;
    if (target_frame >= ep->frame_n)
        target_frame = ep->frame_n - 1;

    prev_frame = ep->frame;
    if (shift_select_cfg && shift_held)
    {
        int s, e;
        if (ep->shift_anchor < 0)
        {
            ep->shift_anchor = prev_frame;
        }
        if (target_frame < ep->shift_anchor)
        {
            s = target_frame;
            e = ep->shift_anchor;
        }
        else
        {
            s = ep->shift_anchor;
            e = target_frame;
        }
        if (s != ep->select_s || e != ep->select_e)
        {
            ep->select_s = s;
            ep->select_e = e;
            ep->abi.select_s = s;
            ep->abi.select_e = e;
            sel_changed = TRUE;
        }
    }
    else
    {
        ep->shift_anchor = -1;
    }

    ep->frame = target_frame;
    ep->abi.frame = target_frame;
    return (target_frame != prev_frame) || sel_changed;
}

BOOL editp_has_audio(EDITP *ep)
{
    return (ep && ep->is_editing && ep->has_audio) ? TRUE : FALSE;
}

BOOL editp_is_source_keyframe(EDITP *ep, int frame_num)
{
    if (!ep || !ep->is_editing || frame_num < 0 || frame_num >= ep->frame_n)
    {
        return FALSE;
    }
    if (ep->frame_status && (ep->frame_status[frame_num] & EDIT_FRAME_EDIT_FLAG_KEYFRAME))
    {
        return TRUE;
    }
    if (ep->video_source)
    {
        return vfw_input_is_keyframe(ep->video_source, frame_num);
    }
    return TRUE;
}

void editp_get_sys_info(EDITP *ep, SYS_INFO *si)
{
    int vram_w = (ep && ep->max_w > REAVIUTL_VRAM_W) ? ep->max_w : REAVIUTL_VRAM_W;
    int vram_h = (ep && ep->max_h > REAVIUTL_VRAM_H) ? ep->max_h : REAVIUTL_VRAM_H;

    if (!si)
        return;

    // 0x00422120, populate +0x00..+0x44 without writing past 0x48
    si->flag = SYS_INFO_FLAG_USE_SSE | SYS_INFO_FLAG_USE_SSE2;
    if (ep && ep->is_editing)
    {
        si->flag |= SYS_INFO_FLAG_EDIT;
    }
    si->info = g_version_info;
    si->filter_n = g_registered_filter_count;
    si->min_w = 32;
    si->min_h = 32;
    si->max_w = vram_w;
    si->max_h = vram_h;
    si->max_frame = REAVIUTL_MAX_FRAME;
    si->edit_name = ep ? ep->abi.edit_name : g_empty_str;
    si->project_name = ep ? ep->abi.project_name : g_empty_str;
    si->output_name = ep ? ep->abi.output_name : g_empty_str;
    si->vram_w = vram_w;
    si->vram_h = vram_h;
    si->vram_byte_size = sizeof(PIXEL_YC);
    si->vram_yc_size = vram_w * (int)sizeof(PIXEL_YC);
    si->hfont = editp_get_ui_font();
    si->build = REAVIUTL_BUILD;
    si->reserve[0] = 0;
}

BOOL editp_get_file_info(EDITP *ep, FILE_INFO *fi)
{
    if (!fi)
        return FALSE;
    memset(fi, 0, sizeof(FILE_INFO));
    if (!ep || !ep->is_editing || ep->frame_n <= 0)
    {
        fi->name = g_empty_str;
        return FALSE;
    }

    // sync any direct writes exedit made to editp+0x468 / editp+0x46c (0x1003ac30)
    if (ep->abi.video_scale > 0)
        ep->fps_scale = ep->abi.video_scale;
    if (ep->abi.video_rate > 0)
        ep->fps_rate = ep->abi.video_rate;

    fi->flag = FILE_INFO_FLAG_VIDEO | FILE_INFO_FLAG_AUDIO;
    fi->name = ep->abi.edit_name;
    fi->w = ep->w;
    fi->h = ep->h;
    fi->video_rate = ep->fps_rate > 0 ? ep->fps_rate : 30;
    fi->video_scale = ep->fps_scale > 0 ? ep->fps_scale : 1;
    fi->audio_rate = ep->audio_rate > 0 ? ep->audio_rate : 44100;
    fi->audio_ch = ep->audio_ch > 0 ? ep->audio_ch : 2;
    fi->frame_n = ep->frame_n;
    fi->video_decode_format = 0;
    fi->video_decode_bit = 24;
    fi->audio_n = (int)(((long long)fi->frame_n * fi->audio_rate * fi->video_scale) /
                        fi->video_rate);
    return TRUE;
}
