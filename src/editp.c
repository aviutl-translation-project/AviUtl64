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
#include "yc48_convert.h"

#define EDITP_MAX_MEM_FRAMES 256

typedef struct EditpSourceSlot
{
    BOOL in_use;
    char filepath[MAX_PATH];
    FILE_INFO fi;
    VFWInput *input;
} EditpSourceSlot;

typedef struct EditpMemFrame
{
    PIXEL_YC *ycp;
    int w;
    int h;
} EditpMemFrame;

typedef struct EditpUndoState
{
    BOOL valid;
    BOOL has_virtual_edits;
    int frame_n;
    int frame;
    int select_s;
    int select_e;
    uint32_t *vid_src_id;
    uint32_t *vid_src_frame;
    uint32_t *aud_src_id;
    uint32_t *aud_src_frame;
    BYTE *frame_status;
    BYTE *frame_inter;
    BYTE *frame_config;
} EditpUndoState;

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
    BOOL has_virtual_edits;
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
    BYTE *frame_inter;
    BYTE *frame_config;
    uint32_t *vid_src_id;
    uint32_t *vid_src_frame;
    uint32_t *aud_src_id;
    uint32_t *aud_src_frame;
    int frame_status_cap;

    VFWInput *video_source;
    VFWInput *audio_source;
    EditpSourceSlot sources[EDITP_MAX_SOURCES];
    int source_count;

    EditpMemFrame mem_frames[EDITP_MAX_MEM_FRAMES];
    int mem_frame_count;

    PIXEL_YC *copied_yc;
    int copied_w;
    int copied_h;

    EditpUndoState undo;
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

static void editp_free_undo(EditpUndoState *u)
{
    if (!u)
        return;
    free(u->vid_src_id);
    free(u->vid_src_frame);
    free(u->aud_src_id);
    free(u->aud_src_frame);
    free(u->frame_status);
    free(u->frame_inter);
    free(u->frame_config);
    memset(u, 0, sizeof(*u));
}

static void editp_free_mem_frames(EDITP *ep)
{
    int i;
    if (!ep)
        return;
    for (i = 0; i < ep->mem_frame_count && i < EDITP_MAX_MEM_FRAMES; i++)
    {
        free(ep->mem_frames[i].ycp);
        ep->mem_frames[i].ycp = NULL;
        ep->mem_frames[i].w = 0;
        ep->mem_frames[i].h = 0;
    }
    ep->mem_frame_count = 0;
}

static void editp_close_all_sources(EDITP *ep)
{
    int i;
    if (!ep)
        return;
    if (ep->audio_source)
    {
        vfw_input_close(ep->audio_source);
        ep->audio_source = NULL;
    }
    for (i = 0; i < EDITP_MAX_SOURCES; i++)
    {
        if (ep->sources[i].in_use)
        {
            if (ep->sources[i].input)
            {
                vfw_input_close(ep->sources[i].input);
            }
            memset(&ep->sources[i], 0, sizeof(ep->sources[i]));
        }
    }
    ep->video_source = NULL;
    ep->source_count = 0;
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
    int old_cap, new_cap, i;
    BYTE *nst, *nin, *ncf;
    uint32_t *nvid, *nvf, *naid, *naf;

    if (!ep)
        return FALSE;
    if (needed < 256)
        needed = 256;
    if (needed <= ep->frame_status_cap &&
        ep->frame_status && ep->frame_inter && ep->frame_config &&
        ep->vid_src_id && ep->vid_src_frame && ep->aud_src_id && ep->aud_src_frame)
    {
        return TRUE;
    }

    old_cap = ep->frame_status_cap;
    new_cap = old_cap ? old_cap : 1024;
    while (new_cap < needed)
    {
        new_cap *= 2;
    }
    if (new_cap > REAVIUTL_MAX_FRAME)
    {
        new_cap = (needed > REAVIUTL_MAX_FRAME) ? needed : REAVIUTL_MAX_FRAME;
    }

    nst = (BYTE *)realloc(ep->frame_status, (size_t)new_cap);
    if (!nst)
        return FALSE;
    ep->frame_status = nst;

    nin = (BYTE *)realloc(ep->frame_inter, (size_t)new_cap);
    if (!nin)
        return FALSE;
    ep->frame_inter = nin;

    ncf = (BYTE *)realloc(ep->frame_config, (size_t)new_cap);
    if (!ncf)
        return FALSE;
    ep->frame_config = ncf;

    nvid = (uint32_t *)realloc(ep->vid_src_id, (size_t)new_cap * sizeof(uint32_t));
    if (!nvid)
        return FALSE;
    ep->vid_src_id = nvid;

    nvf = (uint32_t *)realloc(ep->vid_src_frame, (size_t)new_cap * sizeof(uint32_t));
    if (!nvf)
        return FALSE;
    ep->vid_src_frame = nvf;

    naid = (uint32_t *)realloc(ep->aud_src_id, (size_t)new_cap * sizeof(uint32_t));
    if (!naid)
        return FALSE;
    ep->aud_src_id = naid;

    naf = (uint32_t *)realloc(ep->aud_src_frame, (size_t)new_cap * sizeof(uint32_t));
    if (!naf)
        return FALSE;
    ep->aud_src_frame = naf;

    if (new_cap > old_cap)
    {
        uint32_t def_aud_id = ep->audio_source ? EDITP_EXT_AUDIO_ID : 0u;
        size_t tail = (size_t)(new_cap - old_cap);
        memset(ep->frame_status + old_cap, 0, tail);
        memset(ep->frame_inter + old_cap, 0, tail);
        memset(ep->frame_config + old_cap, 0, tail);
        memset(ep->vid_src_id + old_cap, 0, tail * sizeof(uint32_t));
        for (i = old_cap; i < new_cap; i++)
        {
            ep->vid_src_frame[i] = (uint32_t)i;
            ep->aud_src_id[i] = def_aud_id;
            ep->aud_src_frame[i] = (uint32_t)i;
        }
    }
    ep->frame_status_cap = new_cap;
    return TRUE;
}

static void editp_init_identity_tables(EDITP *ep, int count)
{
    int i;
    uint32_t def_aud_id;
    if (!ep || count <= 0)
        return;
    def_aud_id = ep->audio_source ? EDITP_EXT_AUDIO_ID : 0u;
    memset(ep->frame_status, 0, (size_t)count);
    memset(ep->frame_inter, 0, (size_t)count);
    memset(ep->frame_config, 0, (size_t)count);
    for (i = 0; i < count; i++)
    {
        ep->vid_src_id[i] = 0u;
        ep->vid_src_frame[i] = (uint32_t)i;
        ep->aud_src_id[i] = def_aud_id;
        ep->aud_src_frame[i] = (uint32_t)i;
    }
    ep->has_virtual_edits = FALSE;
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
    editp_init_identity_tables(ep, frame_n);
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

// 0x00416b70: increment trailing digit run before extension for sequential AVI loading
static BOOL build_next_seq_filepath(const char *cur_path, char *out_next, size_t cap)
{
    const char *base = cur_path;
    const unsigned char *p = (const unsigned char *)cur_path;
    const char *dot = NULL;
    const char *dig_end;
    const char *dig_start;
    long val;
    int width, prefix_len;
    char num_buf[32];

    if (!cur_path || !out_next || cap < MAX_PATH)
        return FALSE;

    while (*p)
    {
        if (IsDBCSLeadByte(*p))
        {
            if (p[1])
                p += 2;
            else
                break;
        }
        else
        {
            if (*p == '\\' || *p == '/' || *p == ':')
            {
                base = (const char *)(p + 1);
                dot = NULL;
            }
            else if (*p == '.')
            {
                dot = (const char *)p;
            }
            p++;
        }
    }
    if (!dot || dot <= base)
        return FALSE;

    dig_end = dot;
    dig_start = dig_end;
    while (dig_start > base && dig_start[-1] >= '0' && dig_start[-1] <= '9')
    {
        dig_start--;
    }
    if (dig_start == dig_end || (dig_end - dig_start) > 9)
        return FALSE;

    width = (int)(dig_end - dig_start);
    val = strtol(dig_start, NULL, 10);
    if (val < 0)
        return FALSE;

    wsprintfA(num_buf, "%0*ld", width, val + 1);
    prefix_len = (int)(dig_start - cur_path);
    if (prefix_len + (int)strlen(num_buf) + (int)strlen(dot) + 1 >= (int)cap)
        return FALSE;

    memcpy(out_next, cur_path, (size_t)prefix_len);
    out_next[prefix_len] = '\0';
    lstrcatA(out_next, num_buf);
    lstrcatA(out_next, dot);
    return GetFileAttributesA(out_next) != INVALID_FILE_ATTRIBUTES;
}

static BOOL editp_append_single_clip(EDITP *ep, const char *filepath,
                                     const EditpLoadOptions *opts, BOOL record_undo)
{
    int slot = -1, s;
    VFWInput *video = NULL;
    FILE_INFO fi;
    char ini[MAX_PATH];
    int fps_noconv = 0;
    int clip_r, clip_s, clip_fn, add_frames, i;
    BOOL is_60 = (opts && opts->load_60fps);
    BOOL into_sel = (opts && opts->into_selection);

    if (!ep || !ep->is_editing || !filepath || !filepath[0])
        return FALSE;

    for (s = 1; s < EDITP_MAX_SOURCES; s++)
    {
        if (!ep->sources[s].in_use)
        {
            slot = s;
            break;
        }
    }
    if (slot < 0)
        return FALSE;

    video = vfw_input_open(filepath, &fi);
    if (!video)
        return FALSE;

    clip_s = fi.video_scale > 0 ? fi.video_scale : 1;
    clip_r = fi.video_rate > 0 ? fi.video_rate : 30;
    clip_fn = fi.frame_n > 0 ? fi.frame_n : 1;

    editp_get_ini_path(ini, sizeof(ini));
    fps_noconv = (int)GetPrivateProfileIntA("system", "fpsnoconvert", 0, ini);

    if (opts && opts->fps_adjust && (fi.flag & FILE_INFO_FLAG_AUDIO) != 0 &&
        clip_fn > 0 && fi.audio_n > 0 && fi.audio_rate > 0)
    {
        double aud_sec = (double)fi.audio_n / (double)fi.audio_rate;
        if (aud_sec > 0.1)
        {
            double nfps = (double)clip_fn / aud_sec;
            if (nfps > 0.01 && nfps <= 10000.0)
            {
                clip_r = (int)(nfps * 100000.0 + 0.5);
                clip_s = 100000;
            }
        }
    }
    if (opts && opts->fps_rate > 0 && opts->fps_scale > 0)
    {
        clip_r = opts->fps_rate;
        clip_s = opts->fps_scale;
    }

    if (is_60)
    {
        clip_fn *= 2;
        clip_r *= 2;
    }

    if (fps_noconv != 0 || (clip_r == ep->fps_rate && clip_s == ep->fps_scale))
    {
        add_frames = clip_fn;
    }
    else
    {
        double dur = ((double)clip_fn * (double)clip_s) / (double)clip_r;
        add_frames = (int)(dur * ((double)ep->fps_rate / (double)ep->fps_scale) + 0.5);
        if (add_frames < 1)
            add_frames = 1;
    }

    if (ep->audio_rate > 0 && ep->audio_ch > 0)
    {
        vfw_input_set_audio_format(video, ep->audio_rate, ep->audio_ch);
    }

    if (into_sel && ep->select_s >= 0 && ep->select_e >= ep->select_s && ep->select_e < ep->frame_n)
    {
        int sel_s = ep->select_s;
        int sel_e = ep->select_e;
        int tail_n = ep->frame_n - (sel_e + 1);
        int new_total = sel_s + add_frames + tail_n;

        if (new_total > REAVIUTL_MAX_FRAME)
        {
            vfw_input_close(video);
            return FALSE;
        }
        if (record_undo)
            editp_push_undo(ep);
        if (!editp_ensure_frame_status(ep, new_total))
        {
            vfw_input_close(video);
            return FALSE;
        }
        if (tail_n > 0 && sel_s + add_frames != sel_e + 1)
        {
            size_t tsz = (size_t)tail_n;
            memmove(ep->vid_src_id + sel_s + add_frames, ep->vid_src_id + sel_e + 1, tsz * sizeof(uint32_t));
            memmove(ep->vid_src_frame + sel_s + add_frames, ep->vid_src_frame + sel_e + 1, tsz * sizeof(uint32_t));
            memmove(ep->aud_src_id + sel_s + add_frames, ep->aud_src_id + sel_e + 1, tsz * sizeof(uint32_t));
            memmove(ep->aud_src_frame + sel_s + add_frames, ep->aud_src_frame + sel_e + 1, tsz * sizeof(uint32_t));
            memmove(ep->frame_status + sel_s + add_frames, ep->frame_status + sel_e + 1, tsz);
            memmove(ep->frame_inter + sel_s + add_frames, ep->frame_inter + sel_e + 1, tsz);
            memmove(ep->frame_config + sel_s + add_frames, ep->frame_config + sel_e + 1, tsz);
        }
        for (i = 0; i < add_frames; i++)
        {
            int dst = sel_s + i;
            int sf = (fps_noconv != 0 || (clip_r == ep->fps_rate && clip_s == ep->fps_scale))
                         ? i
                         : (int)(((long long)i * clip_r * ep->fps_scale) /
                                 ((long long)ep->fps_rate * clip_s));
            int raw_sf = is_60 ? (sf / 2) : sf;
            if (raw_sf >= fi.frame_n && fi.frame_n > 0)
                raw_sf = fi.frame_n - 1;
            ep->vid_src_id[dst] = (uint32_t)slot;
            ep->vid_src_frame[dst] = (uint32_t)raw_sf;
            ep->aud_src_id[dst] = (uint32_t)slot;
            ep->aud_src_frame[dst] = (uint32_t)raw_sf;
            ep->frame_status[dst] = 0;
            ep->frame_inter[dst] = is_60 ? (BYTE)((sf & 1) ? EDIT_FRAME_INTER_EVEN : EDIT_FRAME_INTER_ODD) : 0;
            ep->frame_config[dst] = 0;
        }
        ep->frame_n = new_total;
        ep->select_s = sel_s;
        ep->select_e = sel_s + add_frames - 1;
        ep->frame = sel_s;
    }
    else
    {
        int old_fn = ep->frame_n;
        int new_total = old_fn + add_frames;
        if (new_total > REAVIUTL_MAX_FRAME)
        {
            vfw_input_close(video);
            return FALSE;
        }
        if (record_undo)
            editp_push_undo(ep);
        if (!editp_ensure_frame_status(ep, new_total))
        {
            vfw_input_close(video);
            return FALSE;
        }
        for (i = 0; i < add_frames; i++)
        {
            int dst = old_fn + i;
            int sf = (fps_noconv != 0 || (clip_r == ep->fps_rate && clip_s == ep->fps_scale))
                         ? i
                         : (int)(((long long)i * clip_r * ep->fps_scale) /
                                 ((long long)ep->fps_rate * clip_s));
            int raw_sf = is_60 ? (sf / 2) : sf;
            if (raw_sf >= fi.frame_n && fi.frame_n > 0)
                raw_sf = fi.frame_n - 1;
            ep->vid_src_id[dst] = (uint32_t)slot;
            ep->vid_src_frame[dst] = (uint32_t)raw_sf;
            ep->aud_src_id[dst] = (uint32_t)slot;
            ep->aud_src_frame[dst] = (uint32_t)raw_sf;
            ep->frame_status[dst] = 0;
            ep->frame_inter[dst] = is_60 ? (BYTE)((sf & 1) ? EDIT_FRAME_INTER_EVEN : EDIT_FRAME_INTER_ODD) : 0;
            ep->frame_config[dst] = 0;
        }
        if (ep->select_e == old_fn - 1)
            ep->select_e = new_total - 1;
        ep->frame_n = new_total;
    }

    ep->sources[slot].in_use = TRUE;
    lstrcpynA(ep->sources[slot].filepath, filepath, MAX_PATH);
    ep->sources[slot].fi = fi;
    ep->sources[slot].fi.name = ep->sources[slot].filepath;
    ep->sources[slot].input = video;
    if (slot >= ep->source_count)
        ep->source_count = slot + 1;
    if (fi.flag & FILE_INFO_FLAG_AUDIO)
        ep->has_audio = TRUE;
    ep->has_virtual_edits = TRUE;

    editp_sync_abi(ep);
    exfunc_invalidate_all_caches();
    return TRUE;
}

BOOL editp_open_video_ex(EDITP *ep, const char *filepath, const EditpLoadOptions *opts)
{
    VFWInput *video;
    FILE_INFO fi;
    int proj_w, proj_h, proj_fn, fps_r, fps_s, aud_r, aud_ch;
    BOOL is_60 = (opts && opts->load_60fps);

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
        is_60 = FALSE;
    }
    else if ((fi.flag & FILE_INFO_FLAG_VIDEO) != 0 && strstr(filepath, ".exedit") == NULL)
    {
        char ini[MAX_PATH];
        int load30, loadadj;
        editp_get_ini_path(ini, sizeof(ini));
        load30 = (int)GetPrivateProfileIntA("system", "load30fps", 0, ini);
        loadadj = (int)GetPrivateProfileIntA("system", "loadfpsadjust", 0, ini);
        if (opts && opts->fps_adjust)
            loadadj = 1;

        // 0x00401d10: auto-adjust fps when video & audio durations diverge by >= 0.1s
        if (loadadj != 0 && (fi.flag & FILE_INFO_FLAG_AUDIO) != 0 &&
            proj_fn > 0 && fi.audio_n > 0 && aud_r > 0)
        {
            double vid_sec = ((double)proj_fn * (double)fps_s) / (double)fps_r;
            double aud_sec = (double)fi.audio_n / (double)aud_r;
            if (aud_sec > 0.1 && ((opts && opts->fps_adjust) || fabs(vid_sec - aud_sec) >= 0.1))
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

        if (opts && opts->fps_rate > 0 && opts->fps_scale > 0)
        {
            fps_r = opts->fps_rate;
            fps_s = opts->fps_scale;
        }
    }
    if (proj_fn <= 0)
        proj_fn = 1;

    if (is_60)
    {
        proj_fn *= 2;
        fps_r *= 2;
    }

    if (!editp_ensure_vram_buffers(ep, proj_w, proj_h) ||
        !editp_ensure_frame_status(ep, proj_fn > 64 ? proj_fn : 64))
    {
        vfw_input_close(video);
        return FALSE;
    }

    editp_close_all_sources(ep);
    editp_free_mem_frames(ep);
    editp_free_undo(&ep->undo);

    ep->video_source = video;
    ep->sources[0].in_use = TRUE;
    lstrcpynA(ep->sources[0].filepath, filepath, MAX_PATH);
    ep->sources[0].fi = fi;
    ep->sources[0].fi.name = ep->sources[0].filepath;
    ep->sources[0].input = video;
    ep->source_count = 1;

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
    editp_init_identity_tables(ep, ep->frame_n);
    if (is_60)
    {
        int i;
        for (i = 0; i < ep->frame_n; i++)
        {
            ep->vid_src_frame[i] = (uint32_t)(i / 2);
            ep->aud_src_frame[i] = (uint32_t)(i / 2);
            ep->frame_inter[i] = (BYTE)((i & 1) ? EDIT_FRAME_INTER_EVEN : EDIT_FRAME_INTER_ODD);
        }
        ep->has_virtual_edits = TRUE;
    }

    if (opts && opts->seq_avi)
    {
        char cur_seq[MAX_PATH], next_seq[MAX_PATH];
        EditpLoadOptions seq_opts = *opts;
        int seg;
        seq_opts.seq_avi = FALSE;
        seq_opts.into_selection = FALSE;
        lstrcpynA(cur_seq, filepath, MAX_PATH);
        for (seg = 0; seg < EDITP_MAX_SOURCES - 1; seg++)
        {
            if (!build_next_seq_filepath(cur_seq, next_seq, sizeof(next_seq)))
                break;
            if (!editp_append_single_clip(ep, next_seq, &seq_opts, FALSE))
                break;
            lstrcpynA(cur_seq, next_seq, MAX_PATH);
        }
        ep->select_s = 0;
        ep->select_e = ep->frame_n - 1;
    }

    editp_sync_abi(ep);
    exfunc_invalidate_all_caches();
    editp_load_frame(ep, 0);
    return TRUE;
}

BOOL editp_open_video(EDITP *ep, const char *filepath)
{
    return editp_open_video_ex(ep, filepath, NULL);
}

BOOL editp_append_video_ex(EDITP *ep, const char *filepath, const EditpLoadOptions *opts)
{
    if (!ep || !filepath || !filepath[0])
        return FALSE;
    if (!ep->is_editing)
        return editp_open_video_ex(ep, filepath, opts);

    if (!editp_append_single_clip(ep, filepath, opts, TRUE))
        return FALSE;

    if (opts && opts->seq_avi)
    {
        char cur_seq[MAX_PATH], next_seq[MAX_PATH];
        EditpLoadOptions seq_opts = *opts;
        int seg;
        seq_opts.seq_avi = FALSE;
        seq_opts.into_selection = FALSE;
        lstrcpynA(cur_seq, filepath, MAX_PATH);
        for (seg = 0; seg < EDITP_MAX_SOURCES - 1; seg++)
        {
            if (!build_next_seq_filepath(cur_seq, next_seq, sizeof(next_seq)))
                break;
            if (!editp_append_single_clip(ep, next_seq, &seq_opts, FALSE))
                break;
            lstrcpynA(cur_seq, next_seq, MAX_PATH);
        }
    }
    editp_load_frame(ep, ep->frame);
    return TRUE;
}

BOOL editp_append_video(EDITP *ep, const char *filepath)
{
    return editp_append_video_ex(ep, filepath, NULL);
}

BOOL editp_open_audio(EDITP *ep, const char *filepath)
{
    VFWInput *aud;
    FILE_INFO fi;
    int i;

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
    if (ep->aud_src_id && ep->aud_src_frame)
    {
        for (i = 0; i < ep->frame_n; i++)
        {
            ep->aud_src_id[i] = EDITP_EXT_AUDIO_ID;
            ep->aud_src_frame[i] = (uint32_t)i;
        }
    }
    return TRUE;
}

void editp_close_video(EDITP *ep)
{
    if (!ep)
        return;
    editp_close_all_sources(ep);
    editp_free_mem_frames(ep);
    editp_free_undo(&ep->undo);
    ep->is_editing = FALSE;
    ep->has_audio = FALSE;
    ep->has_virtual_edits = FALSE;
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
    editp_close_all_sources(ep);
    editp_free_mem_frames(ep);
    editp_free_undo(&ep->undo);
    free(ep->copied_yc);
    free(ep->ycp_edit);
    free(ep->ycp_temp);
    free(ep->frame_status);
    free(ep->frame_inter);
    free(ep->frame_config);
    free(ep->vid_src_id);
    free(ep->vid_src_frame);
    free(ep->aud_src_id);
    free(ep->aud_src_frame);
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

static BOOL read_mapped_video_frame(EDITP *ep, int frame_num, PIXEL_YC *dst,
                                    int dst_stride, int dst_w, int dst_h)
{
    uint32_t vid_id;
    int vid_f, y, cw;

    if (!ep || !dst || dst_stride <= 0 || dst_w <= 0 || dst_h <= 0)
        return FALSE;

    vid_id = (ep->vid_src_id && frame_num < ep->frame_status_cap)
                 ? ep->vid_src_id[frame_num]
                 : 0u;
    vid_f = (ep->vid_src_frame && frame_num < ep->frame_status_cap)
                ? (int)ep->vid_src_frame[frame_num]
                : frame_num;

    if (vid_id >= EDITP_MEM_SRC_BASE)
    {
        int mslot = (int)(vid_id - EDITP_MEM_SRC_BASE);
        if (mslot >= 0 && mslot < ep->mem_frame_count && ep->mem_frames[mslot].ycp)
        {
            const EditpMemFrame *mf = &ep->mem_frames[mslot];
            int copy_w = (mf->w < dst_w) ? mf->w : dst_w;
            int copy_h = (mf->h < dst_h) ? mf->h : dst_h;
            if (copy_w > dst_stride)
                copy_w = dst_stride;
            cw = (dst_w < dst_stride) ? dst_w : dst_stride;
            for (y = 0; y < dst_h; y++)
            {
                if (y < copy_h)
                {
                    memcpy(dst + (size_t)y * (size_t)dst_stride,
                           mf->ycp + (size_t)y * (size_t)mf->w,
                           (size_t)copy_w * sizeof(PIXEL_YC));
                    if (copy_w < cw)
                    {
                        memset(dst + (size_t)y * (size_t)dst_stride + copy_w, 0,
                               (size_t)(cw - copy_w) * sizeof(PIXEL_YC));
                    }
                }
                else
                {
                    memset(dst + (size_t)y * (size_t)dst_stride, 0,
                           (size_t)cw * sizeof(PIXEL_YC));
                }
            }
            return TRUE;
        }
    }
    else if (vid_id < EDITP_MAX_SOURCES && ep->sources[vid_id].in_use && ep->sources[vid_id].input)
    {
        EditpSourceSlot *ss = &ep->sources[vid_id];
        int sw = ss->fi.w;
        int sh = ss->fi.h;
        if (vid_id == 0 || (sw == dst_w && sh == dst_h) || sw <= 0 || sh <= 0)
        {
            if (vfw_input_read_frame_stride(ss->input, vid_f, dst, dst_stride))
            {
                return TRUE;
            }
        }
        else
        {
            PIXEL_YC *tmp = (PIXEL_YC *)malloc((size_t)sw * (size_t)sh * sizeof(PIXEL_YC));
            if (tmp)
            {
                if (vfw_input_read_frame_stride(ss->input, vid_f, tmp, sw))
                {
                    int copy_w = (sw < dst_w) ? sw : dst_w;
                    int copy_h = (sh < dst_h) ? sh : dst_h;
                    if (copy_w > dst_stride)
                        copy_w = dst_stride;
                    cw = (dst_w < dst_stride) ? dst_w : dst_stride;
                    for (y = 0; y < dst_h; y++)
                    {
                        if (y < copy_h)
                        {
                            memcpy(dst + (size_t)y * (size_t)dst_stride,
                                   tmp + (size_t)y * (size_t)sw,
                                   (size_t)copy_w * sizeof(PIXEL_YC));
                            if (copy_w < cw)
                            {
                                memset(dst + (size_t)y * (size_t)dst_stride + copy_w, 0,
                                       (size_t)(cw - copy_w) * sizeof(PIXEL_YC));
                            }
                        }
                        else
                        {
                            memset(dst + (size_t)y * (size_t)dst_stride, 0,
                                   (size_t)cw * sizeof(PIXEL_YC));
                        }
                    }
                    free(tmp);
                    return TRUE;
                }
                free(tmp);
            }
        }
    }

    cw = (dst_w < dst_stride) ? dst_w : dst_stride;
    for (y = 0; y < dst_h; y++)
    {
        memset(dst + (size_t)y * (size_t)dst_stride, 0, (size_t)cw * sizeof(PIXEL_YC));
    }
    return TRUE;
}

BOOL editp_load_frame(EDITP *ep, int frame_num)
{
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
    if (ep->org_w > 0 && ep->org_h > 0)
    {
        ep->w = ep->org_w;
        ep->h = ep->org_h;
        ep->abi.w = ep->w;
        ep->abi.h = ep->h;
        ep->abi.vram_cur_w = ep->w;
        ep->abi.vram_cur_h = ep->h;
    }

    return read_mapped_video_frame(ep, frame_num, ep->ycp_edit, ep->max_w, ep->w, ep->h);
}

BOOL editp_read_source_frame(EDITP *ep, int frame_num, PIXEL_YC *dst, int dst_stride)
{
    int cw, ch;
    if (!ep || !ep->is_editing || ep->frame_n <= 0 || !dst || dst_stride <= 0)
    {
        return FALSE;
    }
    if (frame_num < 0)
        frame_num = 0;
    if (frame_num >= ep->frame_n)
        frame_num = ep->frame_n - 1;

    cw = (ep->org_w > 0 ? ep->org_w : ep->w);
    ch = (ep->org_h > 0 ? ep->org_h : ep->h);
    return read_mapped_video_frame(ep, frame_num, dst, dst_stride, cw, ch);
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
    int old_fn, i;
    if (!ep || !ep->is_editing)
        return 0;
    if (n < 1)
        n = 1;
    if (n > REAVIUTL_MAX_FRAME)
        n = REAVIUTL_MAX_FRAME;
    old_fn = ep->frame_n;
    editp_ensure_frame_status(ep, n);
    if (n > old_fn && !ep->has_virtual_edits && ep->vid_src_id && ep->vid_src_frame &&
        ep->aud_src_id && ep->aud_src_frame)
    {
        uint32_t def_aud = ep->audio_source ? EDITP_EXT_AUDIO_ID : 0u;
        for (i = old_fn; i < n; i++)
        {
            ep->vid_src_id[i] = 0u;
            ep->vid_src_frame[i] = (uint32_t)i;
            ep->aud_src_id[i] = def_aud;
            ep->aud_src_frame[i] = (uint32_t)i;
        }
    }
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
    if (!ep)
        return NULL;
    if (type == FRAME_STATUS_TYPE_INTER)
    {
        return ep->frame_inter;
    }
    if (type == FRAME_STATUS_TYPE_EDIT_FLAG)
    {
        return ep->frame_status;
    }
    return NULL;
}

int editp_read_audio_frame(EDITP *ep, int frame_num, short *buf)
{
    int rate, ch, fps_r, fps_s;
    long long s0, s1;
    int count;
    uint32_t aud_id;
    int aud_f;

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

    if (frame_num < 0 || frame_num >= ep->frame_n)
    {
        memset(buf, 0, (size_t)count * (size_t)ch * sizeof(short));
        return count;
    }

    aud_id = (ep->aud_src_id && frame_num < ep->frame_status_cap)
                 ? ep->aud_src_id[frame_num]
                 : (ep->audio_source ? EDITP_EXT_AUDIO_ID : 0u);
    aud_f = (ep->aud_src_frame && frame_num < ep->frame_status_cap)
                ? (int)ep->aud_src_frame[frame_num]
                : frame_num;

    if (aud_id == EDITP_EXT_AUDIO_ID && ep->audio_source)
    {
        long long src_s0 = ((long long)aud_f * rate * fps_s) / fps_r;
        vfw_input_read_audio(ep->audio_source, (int)src_s0, buf, count);
        return count;
    }
    if (aud_id < EDITP_MAX_SOURCES && ep->sources[aud_id].in_use && ep->sources[aud_id].input)
    {
        int sr = (aud_id == 0) ? fps_r : (ep->sources[aud_id].fi.video_rate > 0 ? ep->sources[aud_id].fi.video_rate : fps_r);
        int ss = (aud_id == 0) ? fps_s : (ep->sources[aud_id].fi.video_scale > 0 ? ep->sources[aud_id].fi.video_scale : fps_s);
        long long src_s0 = ((long long)aud_f * rate * ss) / sr;
        vfw_input_read_audio(ep->sources[aud_id].input, (int)src_s0, buf, count);
        return count;
    }
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

void editp_get_org_dimensions(EDITP *ep, int *org_w, int *org_h)
{
    if (org_w)
        *org_w = (ep && ep->org_w > 0) ? ep->org_w : (ep ? ep->w : 0);
    if (org_h)
        *org_h = (ep && ep->org_h > 0) ? ep->org_h : (ep ? ep->h : 0);
}

void editp_set_frame_dimensions(EDITP *ep, int w, int h)
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

void editp_set_dimensions(EDITP *ep, int w, int h)
{
    if (!ep || w <= 0 || h <= 0)
        return;
    editp_set_frame_dimensions(ep, w, h);
    ep->org_w = ep->w;
    ep->org_h = ep->h;
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
    int i;
    if (!ep)
        return;
    if (audio_rate > 0)
        ep->audio_rate = audio_rate;
    if (audio_ch > 0)
        ep->audio_ch = audio_ch;
    ep->has_audio = has_audio ? TRUE : FALSE;
    if (ep->audio_rate > 0 && ep->audio_ch > 0)
    {
        for (i = 0; i < EDITP_MAX_SOURCES; i++)
        {
            if (ep->sources[i].in_use && ep->sources[i].input)
            {
                vfw_input_set_audio_format(ep->sources[i].input, ep->audio_rate, ep->audio_ch);
            }
        }
        if (ep->audio_source)
        {
            vfw_input_set_audio_format(ep->audio_source, ep->audio_rate, ep->audio_ch);
        }
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
    uint32_t vid_id;
    int vid_f;

    if (!ep || !ep->is_editing || frame_num < 0 || frame_num >= ep->frame_n)
    {
        return FALSE;
    }
    if (ep->frame_status && (ep->frame_status[frame_num] & EDIT_FRAME_EDIT_FLAG_KEYFRAME))
    {
        return TRUE;
    }
    vid_id = (ep->vid_src_id && frame_num < ep->frame_status_cap) ? ep->vid_src_id[frame_num] : 0u;
    vid_f = (ep->vid_src_frame && frame_num < ep->frame_status_cap) ? (int)ep->vid_src_frame[frame_num] : frame_num;
    if (vid_id >= EDITP_MEM_SRC_BASE)
    {
        return TRUE;
    }
    if (vid_id < EDITP_MAX_SOURCES && ep->sources[vid_id].in_use && ep->sources[vid_id].input)
    {
        return vfw_input_is_keyframe(ep->sources[vid_id].input, vid_f);
    }
    return TRUE;
}

BOOL editp_has_virtual_edits(EDITP *ep)
{
    return (ep && ep->is_editing && ep->has_virtual_edits) ? TRUE : FALSE;
}

int editp_get_source_count(EDITP *ep)
{
    return (ep && ep->is_editing) ? ep->source_count : 0;
}

const char *editp_get_source_path(EDITP *ep, int source_file_id)
{
    if (!ep || !ep->is_editing || source_file_id < 0 || source_file_id >= EDITP_MAX_SOURCES)
        return g_empty_str;
    if (!ep->sources[source_file_id].in_use)
        return g_empty_str;
    return ep->sources[source_file_id].filepath;
}

int editp_add_source_file(EDITP *ep, const char *filepath)
{
    int i, slot = -1;
    VFWInput *vin;
    FILE_INFO fi;

    if (!ep || !ep->is_editing || !filepath || !filepath[0])
        return -1;

    for (i = 0; i < EDITP_MAX_SOURCES; i++)
    {
        if (ep->sources[i].in_use && lstrcmpiA(ep->sources[i].filepath, filepath) == 0)
        {
            return i;
        }
        if (!ep->sources[i].in_use && slot < 0)
        {
            slot = i;
        }
    }
    if (slot < 0)
        return -1;

    vin = vfw_input_open(filepath, &fi);
    if (!vin)
        return -1;
    if (ep->audio_rate > 0 && ep->audio_ch > 0)
    {
        vfw_input_set_audio_format(vin, ep->audio_rate, ep->audio_ch);
    }
    ep->sources[slot].in_use = TRUE;
    lstrcpynA(ep->sources[slot].filepath, filepath, MAX_PATH);
    ep->sources[slot].fi = fi;
    ep->sources[slot].fi.name = ep->sources[slot].filepath;
    ep->sources[slot].input = vin;
    if (slot >= ep->source_count)
        ep->source_count = slot + 1;
    return slot;
}

BOOL editp_get_source_file_info(EDITP *ep, FILE_INFO *fi, int source_file_id)
{
    if (!fi)
        return FALSE;
    memset(fi, 0, sizeof(*fi));
    if (!ep || !ep->is_editing || source_file_id < 0 || source_file_id >= EDITP_MAX_SOURCES)
    {
        fi->name = g_empty_str;
        return FALSE;
    }
    if (!ep->sources[source_file_id].in_use)
    {
        if (source_file_id == 0)
        {
            return editp_get_file_info(ep, fi);
        }
        fi->name = g_empty_str;
        return FALSE;
    }
    *fi = ep->sources[source_file_id].fi;
    fi->name = ep->sources[source_file_id].filepath;
    return TRUE;
}

BOOL editp_get_source_video_number(EDITP *ep, int n, int *source_file_id, int *source_video_number)
{
    uint32_t vid_id;
    int vid_f;

    if (!ep || !ep->is_editing || n < 0 || n >= ep->frame_n)
        return FALSE;

    vid_id = (ep->vid_src_id && n < ep->frame_status_cap) ? ep->vid_src_id[n] : 0u;
    vid_f = (ep->vid_src_frame && n < ep->frame_status_cap) ? (int)ep->vid_src_frame[n] : n;
    if (vid_id >= EDITP_MEM_SRC_BASE)
    {
        if (source_file_id)
            *source_file_id = -1;
        if (source_video_number)
            *source_video_number = 0;
        return TRUE;
    }
    if (source_file_id)
        *source_file_id = (int)vid_id;
    if (source_video_number)
        *source_video_number = vid_f;
    return TRUE;
}

BOOL editp_get_frame_mapping(EDITP *ep, int n, uint32_t *vid_id, uint32_t *vid_f,
                             uint32_t *aud_id, uint32_t *aud_f)
{
    if (!ep || !ep->is_editing || n < 0 || n >= ep->frame_n)
        return FALSE;
    if (vid_id)
        *vid_id = (ep->vid_src_id && n < ep->frame_status_cap) ? ep->vid_src_id[n] : 0u;
    if (vid_f)
        *vid_f = (ep->vid_src_frame && n < ep->frame_status_cap) ? ep->vid_src_frame[n] : (uint32_t)n;
    if (aud_id)
    {
        uint32_t aid = (ep->aud_src_id && n < ep->frame_status_cap) ? ep->aud_src_id[n] : 0u;
        *aud_id = (aid == EDITP_EXT_AUDIO_ID) ? 0u : aid;
    }
    if (aud_f)
        *aud_f = (ep->aud_src_frame && n < ep->frame_status_cap) ? ep->aud_src_frame[n] : (uint32_t)n;
    return TRUE;
}

BOOL editp_set_frame_mapping(EDITP *ep, int n, uint32_t vid_id, uint32_t vid_f,
                             uint32_t aud_id, uint32_t aud_f)
{
    if (!ep || !ep->is_editing || n < 0 || n >= ep->frame_n)
        return FALSE;
    if (!editp_ensure_frame_status(ep, n + 1))
        return FALSE;
    ep->vid_src_id[n] = vid_id;
    ep->vid_src_frame[n] = vid_f;
    ep->aud_src_id[n] = aud_id;
    ep->aud_src_frame[n] = aud_f;
    if (vid_id != 0u || vid_f != (uint32_t)n || aud_id != 0u || aud_f != (uint32_t)n)
    {
        ep->has_virtual_edits = TRUE;
    }
    return TRUE;
}

BYTE editp_get_frame_inter(EDITP *ep, int n)
{
    if (!ep || !ep->is_editing || !ep->frame_inter || n < 0 || n >= ep->frame_n || n >= ep->frame_status_cap)
        return EDIT_FRAME_INTER_NORMAL;
    return ep->frame_inter[n];
}

BOOL editp_set_frame_inter(EDITP *ep, int n, BYTE inter)
{
    if (!ep || !ep->is_editing || n < 0 || n >= ep->frame_n || inter > EDIT_FRAME_INTER_AUTO)
        return FALSE;
    if (!editp_ensure_frame_status(ep, n + 1))
        return FALSE;
    ep->frame_inter[n] = inter;
    return TRUE;
}

BYTE editp_get_frame_config(EDITP *ep, int n)
{
    if (!ep || !ep->is_editing || !ep->frame_config || n < 0 || n >= ep->frame_n || n >= ep->frame_status_cap)
        return 0;
    return ep->frame_config[n];
}

BOOL editp_set_frame_config(EDITP *ep, int n, BYTE cfg)
{
    if (!ep || !ep->is_editing || n < 0 || n >= ep->frame_n)
        return FALSE;
    if (!editp_ensure_frame_status(ep, n + 1))
        return FALSE;
    ep->frame_config[n] = cfg;
    return TRUE;
}

BOOL editp_is_frame_discontinuity(EDITP *ep, int n)
{
    if (!ep || !ep->is_editing || n <= 0 || n >= ep->frame_n || n >= ep->frame_status_cap)
        return FALSE;
    if (!ep->vid_src_id || !ep->vid_src_frame)
        return FALSE;
    if (ep->vid_src_id[n] != ep->vid_src_id[n - 1])
        return TRUE;
    if (ep->vid_src_id[n] >= EDITP_MEM_SRC_BASE)
        return FALSE;
    return ep->vid_src_frame[n] != ep->vid_src_frame[n - 1] + 1u;
}

BOOL editp_push_undo(EDITP *ep)
{
    size_t fn;
    EditpUndoState u;

    if (!ep || !ep->is_editing || ep->frame_n <= 0)
        return FALSE;
    if (!editp_ensure_frame_status(ep, ep->frame_n))
        return FALSE;

    memset(&u, 0, sizeof(u));
    fn = (size_t)ep->frame_n;
    u.vid_src_id = (uint32_t *)malloc(fn * sizeof(uint32_t));
    u.vid_src_frame = (uint32_t *)malloc(fn * sizeof(uint32_t));
    u.aud_src_id = (uint32_t *)malloc(fn * sizeof(uint32_t));
    u.aud_src_frame = (uint32_t *)malloc(fn * sizeof(uint32_t));
    u.frame_status = (BYTE *)malloc(fn);
    u.frame_inter = (BYTE *)malloc(fn);
    u.frame_config = (BYTE *)malloc(fn);

    if (!u.vid_src_id || !u.vid_src_frame || !u.aud_src_id || !u.aud_src_frame ||
        !u.frame_status || !u.frame_inter || !u.frame_config)
    {
        editp_free_undo(&u);
        return FALSE;
    }

    memcpy(u.vid_src_id, ep->vid_src_id, fn * sizeof(uint32_t));
    memcpy(u.vid_src_frame, ep->vid_src_frame, fn * sizeof(uint32_t));
    memcpy(u.aud_src_id, ep->aud_src_id, fn * sizeof(uint32_t));
    memcpy(u.aud_src_frame, ep->aud_src_frame, fn * sizeof(uint32_t));
    memcpy(u.frame_status, ep->frame_status, fn);
    memcpy(u.frame_inter, ep->frame_inter, fn);
    memcpy(u.frame_config, ep->frame_config, fn);
    u.frame_n = ep->frame_n;
    u.frame = ep->frame;
    u.select_s = ep->select_s;
    u.select_e = ep->select_e;
    u.has_virtual_edits = ep->has_virtual_edits;
    u.valid = TRUE;

    editp_free_undo(&ep->undo);
    ep->undo = u;
    return TRUE;
}

BOOL editp_can_undo(EDITP *ep)
{
    return (ep && ep->is_editing && ep->undo.valid && ep->undo.frame_n > 0) ? TRUE : FALSE;
}

BOOL editp_undo(EDITP *ep)
{
    EditpUndoState prev_undo;
    size_t fn;

    if (!editp_can_undo(ep))
        return FALSE;

    prev_undo = ep->undo;
    memset(&ep->undo, 0, sizeof(ep->undo));

    // snapshot current state into ep->undo so Ctrl+Z toggles undo/redo cleanly
    editp_push_undo(ep);

    if (!editp_ensure_frame_status(ep, prev_undo.frame_n))
    {
        editp_free_undo(&prev_undo);
        return FALSE;
    }

    fn = (size_t)prev_undo.frame_n;
    memcpy(ep->vid_src_id, prev_undo.vid_src_id, fn * sizeof(uint32_t));
    memcpy(ep->vid_src_frame, prev_undo.vid_src_frame, fn * sizeof(uint32_t));
    memcpy(ep->aud_src_id, prev_undo.aud_src_id, fn * sizeof(uint32_t));
    memcpy(ep->aud_src_frame, prev_undo.aud_src_frame, fn * sizeof(uint32_t));
    memcpy(ep->frame_status, prev_undo.frame_status, fn);
    memcpy(ep->frame_inter, prev_undo.frame_inter, fn);
    memcpy(ep->frame_config, prev_undo.frame_config, fn);
    ep->frame_n = prev_undo.frame_n;
    ep->frame = (prev_undo.frame >= 0 && prev_undo.frame < ep->frame_n) ? prev_undo.frame : 0;
    ep->select_s = prev_undo.select_s;
    ep->select_e = prev_undo.select_e;
    ep->has_virtual_edits = prev_undo.has_virtual_edits;

    editp_free_undo(&prev_undo);
    editp_sync_abi(ep);
    exfunc_invalidate_all_caches();
    editp_load_frame(ep, ep->frame);
    return TRUE;
}

BOOL editp_delete_range(EDITP *ep, int s, int e)
{
    int del_n, tail_n;

    if (!ep || !ep->is_editing || ep->frame_n <= 1)
        return FALSE;
    if (s < 0)
        s = 0;
    if (e >= ep->frame_n)
        e = ep->frame_n - 1;
    if (s > e)
        return FALSE;

    del_n = e - s + 1;
    if (del_n >= ep->frame_n)
    {
        // keep at least 1 frame on the timeline
        e = ep->frame_n - 2;
        del_n = e - s + 1;
        if (del_n <= 0)
            return FALSE;
    }

    editp_push_undo(ep);
    tail_n = ep->frame_n - (e + 1);
    if (tail_n > 0)
    {
        size_t tsz = (size_t)tail_n;
        memmove(ep->vid_src_id + s, ep->vid_src_id + e + 1, tsz * sizeof(uint32_t));
        memmove(ep->vid_src_frame + s, ep->vid_src_frame + e + 1, tsz * sizeof(uint32_t));
        memmove(ep->aud_src_id + s, ep->aud_src_id + e + 1, tsz * sizeof(uint32_t));
        memmove(ep->aud_src_frame + s, ep->aud_src_frame + e + 1, tsz * sizeof(uint32_t));
        memmove(ep->frame_status + s, ep->frame_status + e + 1, tsz);
        memmove(ep->frame_inter + s, ep->frame_inter + e + 1, tsz);
        memmove(ep->frame_config + s, ep->frame_config + e + 1, tsz);
    }
    ep->frame_n -= del_n;
    ep->frame = (s < ep->frame_n) ? s : (ep->frame_n - 1);
    ep->select_s = 0;
    ep->select_e = ep->frame_n - 1;
    ep->has_virtual_edits = TRUE;

    editp_sync_abi(ep);
    exfunc_invalidate_all_caches();
    editp_load_frame(ep, ep->frame);
    return TRUE;
}

BOOL editp_trim_to_range(EDITP *ep, int s, int e)
{
    int keep_n;

    if (!ep || !ep->is_editing || ep->frame_n <= 0)
        return FALSE;
    if (s < 0)
        s = 0;
    if (e >= ep->frame_n)
        e = ep->frame_n - 1;
    if (s > e)
        return FALSE;
    if (s == 0 && e == ep->frame_n - 1)
        return TRUE;

    keep_n = e - s + 1;
    editp_push_undo(ep);
    if (s > 0)
    {
        size_t ksz = (size_t)keep_n;
        memmove(ep->vid_src_id, ep->vid_src_id + s, ksz * sizeof(uint32_t));
        memmove(ep->vid_src_frame, ep->vid_src_frame + s, ksz * sizeof(uint32_t));
        memmove(ep->aud_src_id, ep->aud_src_id + s, ksz * sizeof(uint32_t));
        memmove(ep->aud_src_frame, ep->aud_src_frame + s, ksz * sizeof(uint32_t));
        memmove(ep->frame_status, ep->frame_status + s, ksz);
        memmove(ep->frame_inter, ep->frame_inter + s, ksz);
        memmove(ep->frame_config, ep->frame_config + s, ksz);
        ep->has_virtual_edits = TRUE;
    }
    ep->frame_n = keep_n;
    ep->frame = 0;
    ep->select_s = 0;
    ep->select_e = keep_n - 1;

    editp_sync_abi(ep);
    exfunc_invalidate_all_caches();
    editp_load_frame(ep, 0);
    return TRUE;
}

BOOL editp_copy_video(EDITP *ep, int dst, int src)
{
    if (!ep || !ep->is_editing || dst < 0 || dst >= ep->frame_n || src < 0 || src >= ep->frame_n)
        return FALSE;
    if (!editp_ensure_frame_status(ep, ep->frame_n))
        return FALSE;
    ep->vid_src_id[dst] = ep->vid_src_id[src];
    ep->vid_src_frame[dst] = ep->vid_src_frame[src];
    ep->frame_inter[dst] = ep->frame_inter[src];
    ep->has_virtual_edits = TRUE;
    exfunc_invalidate_all_caches();
    return TRUE;
}

BOOL editp_copy_audio(EDITP *ep, int dst, int src)
{
    if (!ep || !ep->is_editing || dst < 0 || dst >= ep->frame_n || src < 0 || src >= ep->frame_n)
        return FALSE;
    if (!editp_ensure_frame_status(ep, ep->frame_n))
        return FALSE;
    ep->aud_src_id[dst] = ep->aud_src_id[src];
    ep->aud_src_frame[dst] = ep->aud_src_frame[src];
    ep->has_virtual_edits = TRUE;
    return TRUE;
}

BOOL editp_copy_frame(EDITP *ep, int dst, int src)
{
    if (!editp_copy_video(ep, dst, src) || !editp_copy_audio(ep, dst, src))
        return FALSE;
    ep->frame_status[dst] = ep->frame_status[src];
    ep->frame_config[dst] = ep->frame_config[src];
    return TRUE;
}

static int editp_store_mem_frame(EDITP *ep, const PIXEL_YC *src_yc, int src_stride, int w, int h)
{
    PIXEL_YC *buf;
    int slot, y;

    if (!ep || !src_yc || src_stride <= 0 || w <= 0 || h <= 0)
        return -1;
    if (ep->mem_frame_count >= EDITP_MAX_MEM_FRAMES)
    {
        // recycle last slot if 256 pasted still-frames exceeded
        slot = EDITP_MAX_MEM_FRAMES - 1;
        free(ep->mem_frames[slot].ycp);
        ep->mem_frames[slot].ycp = NULL;
    }
    else
    {
        slot = ep->mem_frame_count++;
    }

    buf = (PIXEL_YC *)malloc((size_t)w * (size_t)h * sizeof(PIXEL_YC));
    if (!buf)
        return -1;
    for (y = 0; y < h; y++)
    {
        memcpy(buf + (size_t)y * (size_t)w,
               src_yc + (size_t)y * (size_t)src_stride,
               (size_t)w * sizeof(PIXEL_YC));
    }
    ep->mem_frames[slot].ycp = buf;
    ep->mem_frames[slot].w = w;
    ep->mem_frames[slot].h = h;
    return slot;
}

BOOL editp_copy_frame_image(EDITP *ep, const PIXEL_YC *src_yc, int src_stride, int w, int h)
{
    PIXEL_YC *buf;
    int y;
    if (!ep || !src_yc || src_stride <= 0 || w <= 0 || h <= 0)
        return FALSE;
    buf = (PIXEL_YC *)malloc((size_t)w * (size_t)h * sizeof(PIXEL_YC));
    if (!buf)
        return FALSE;
    for (y = 0; y < h; y++)
    {
        memcpy(buf + (size_t)y * (size_t)w,
               src_yc + (size_t)y * (size_t)src_stride,
               (size_t)w * sizeof(PIXEL_YC));
    }
    free(ep->copied_yc);
    ep->copied_yc = buf;
    ep->copied_w = w;
    ep->copied_h = h;
    return TRUE;
}

BOOL editp_has_copied_frame_image(EDITP *ep)
{
    return (ep && ep->copied_yc && ep->copied_w > 0 && ep->copied_h > 0) ? TRUE : FALSE;
}

BOOL editp_paste_frame_image(EDITP *ep, int s, int e)
{
    int slot, f;

    if (!ep || !ep->is_editing || !editp_has_copied_frame_image(ep))
        return FALSE;
    if (s < 0)
        s = 0;
    if (e >= ep->frame_n)
        e = ep->frame_n - 1;
    if (s > e)
        return FALSE;
    if (!editp_ensure_frame_status(ep, ep->frame_n))
        return FALSE;

    slot = editp_store_mem_frame(ep, ep->copied_yc, ep->copied_w, ep->copied_w, ep->copied_h);
    if (slot < 0)
        return FALSE;

    editp_push_undo(ep);
    for (f = s; f <= e; f++)
    {
        ep->vid_src_id[f] = EDITP_MEM_SRC_BASE + (uint32_t)slot;
        ep->vid_src_frame[f] = 0u;
        ep->frame_status[f] |= EDIT_FRAME_EDIT_FLAG_KEYFRAME;
    }
    ep->has_virtual_edits = TRUE;
    exfunc_invalidate_all_caches();
    editp_load_frame(ep, ep->frame);
    return TRUE;
}

BOOL editp_paste_clipboard_dib(HWND hwnd, EDITP *ep, int dst_frame, BOOL record_undo)
{
    HANDLE hClip;
    const BITMAPINFOHEADER *bih;
    int proj_w, proj_h, src_w, src_h, dib_stride, slot = -1;
    DWORD clr_used, extra_bytes;
    const unsigned char *src_bits;
    HDC mem_dc = NULL;
    BITMAPINFO dst_bmi;
    void *dst_bits = NULL;
    HBITMAP hbmp = NULL, old_bmp = NULL;
    PIXEL_YC *yc_buf = NULL;
    BOOL ok = FALSE;

    if (!ep || !ep->is_editing || dst_frame < 0 || dst_frame >= ep->frame_n)
        return FALSE;
    if (!IsClipboardFormatAvailable(CF_DIB))
        return FALSE;
    if (!OpenClipboard(hwnd))
        return FALSE;

    hClip = GetClipboardData(CF_DIB);
    if (!hClip)
    {
        CloseClipboard();
        return FALSE;
    }
    bih = (const BITMAPINFOHEADER *)GlobalLock(hClip);
    if (!bih || bih->biSize < sizeof(BITMAPINFOHEADER) || bih->biWidth <= 0 || bih->biHeight == 0)
    {
        if (bih)
            GlobalUnlock(hClip);
        CloseClipboard();
        return FALSE;
    }

    src_w = (int)bih->biWidth;
    src_h = (bih->biHeight < 0) ? -(int)bih->biHeight : (int)bih->biHeight;
    proj_w = (ep->org_w > 0) ? ep->org_w : ep->w;
    proj_h = (ep->org_h > 0) ? ep->org_h : ep->h;
    if (proj_w <= 0 || proj_h <= 0)
    {
        GlobalUnlock(hClip);
        CloseClipboard();
        return FALSE;
    }

    clr_used = bih->biClrUsed;
    if (clr_used == 0 && bih->biBitCount > 0 && bih->biBitCount <= 8)
    {
        clr_used = 1u << bih->biBitCount;
    }
    extra_bytes = (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER))
                      ? 12u
                      : 0u;
    src_bits = (const unsigned char *)bih + bih->biSize + clr_used * 4u + extra_bytes;

    mem_dc = CreateCompatibleDC(NULL);
    if (mem_dc)
    {
        dib_stride = (proj_w * 3 + 3) & ~3;
        ZeroMemory(&dst_bmi, sizeof(dst_bmi));
        dst_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        dst_bmi.bmiHeader.biWidth = proj_w;
        dst_bmi.bmiHeader.biHeight = proj_h; // bottom-up BGR24
        dst_bmi.bmiHeader.biPlanes = 1;
        dst_bmi.bmiHeader.biBitCount = 24;
        dst_bmi.bmiHeader.biCompression = BI_RGB;

        hbmp = CreateDIBSection(mem_dc, &dst_bmi, DIB_RGB_COLORS, &dst_bits, NULL, 0);
        if (hbmp && dst_bits)
        {
            memset(dst_bits, 0, (size_t)dib_stride * (size_t)proj_h);
            old_bmp = (HBITMAP)SelectObject(mem_dc, hbmp);
            StretchDIBits(mem_dc, 0, 0, src_w, src_h, 0, 0, src_w, src_h,
                          src_bits, (const BITMAPINFO *)bih, DIB_RGB_COLORS, SRCCOPY);
            GdiFlush();

            yc_buf = (PIXEL_YC *)calloc((size_t)proj_w * (size_t)proj_h, sizeof(PIXEL_YC));
            if (yc_buf)
            {
                rgb2yc_dib24(yc_buf, proj_w, (const unsigned char *)dst_bits,
                             dib_stride, proj_w, proj_h, TRUE);
                slot = editp_store_mem_frame(ep, yc_buf, proj_w, proj_w, proj_h);
                free(yc_buf);
            }
            SelectObject(mem_dc, old_bmp);
        }
        if (hbmp)
            DeleteObject(hbmp);
        DeleteDC(mem_dc);
    }

    GlobalUnlock(hClip);
    CloseClipboard();

    if (slot >= 0 && editp_ensure_frame_status(ep, ep->frame_n))
    {
        if (record_undo)
            editp_push_undo(ep);
        ep->vid_src_id[dst_frame] = EDITP_MEM_SRC_BASE + (uint32_t)slot;
        ep->vid_src_frame[dst_frame] = 0u;
        ep->frame_status[dst_frame] |= EDIT_FRAME_EDIT_FLAG_KEYFRAME;
        ep->has_virtual_edits = TRUE;
        exfunc_invalidate_all_caches();
        if (ep->frame == dst_frame)
            editp_load_frame(ep, dst_frame);
        ok = TRUE;
    }
    return ok;
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
