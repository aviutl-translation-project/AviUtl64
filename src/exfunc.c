#include <windows.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "exfunc.h"
#include "profile_cfg.h"
#include "yc48_convert.h"
#include "vfw_input.h"
#include "input_plugin.h"
#include "debug_console.h"

static EDITP *g_current_editp = NULL;
static char g_ini_path[MAX_PATH] = "";
static ExfuncFilterLookupFn g_filter_lookup = NULL;
static ExfuncAddMenuItemFn g_add_menu_handler = NULL;
static ExfuncHostNotifyFn g_on_state_changed = NULL;
static ExfuncEditOpenFn g_on_edit_open = NULL;
static ExfuncEditCloseFn g_on_edit_close = NULL;
static ExfuncRenderUpToFn g_on_render_up_to = NULL;
static ExfuncRenderAudioUpToFn g_on_render_audio_up_to = NULL;
static ExfuncEditOutputFn g_on_edit_output = NULL;

// scratch buffers lazily allocated for EXFUNC frame/pixel queries
static PIXEL_YC *g_filtering_yc_buf = NULL;
static size_t g_filtering_yc_cap = 0;
static unsigned char *g_disp_bgr_buf = NULL;
static size_t g_disp_bgr_cap = 0;

// persistent multithreaded worker pool for exec_multi_thread_func (0x00434b80)
#define MT_MAX_THREADS 64

typedef struct
{
    HANDLE start_ev;
    HANDLE done_ev;
    HANDLE thread;
    int thread_id;
} MtWorker;

static BOOL g_mt_init = FALSE;
static volatile LONG g_mt_busy = 0;
static volatile BOOL g_mt_quit = FALSE;
static int g_mt_thread_num = 1;
static int g_mt_cfg_threads = 0;
static BOOL g_mt_cpu_sep = FALSE;
static DWORD_PTR g_mt_core0_mask = 0;
static MULTI_THREAD_FUNC g_mt_func = NULL;
static void *g_mt_param1 = NULL;
static void *g_mt_param2 = NULL;
static MtWorker g_mt_workers[MT_MAX_THREADS];

// cached [system] settings for EXFUNC dialogs & LRU cache budget
static BOOL g_sys_cfg_loaded = FALSE;
static int g_cfg_overwritecheck = 0;
static size_t g_cache_budget_bytes = 32u * 1024u * 1024u;
static size_t g_cache_used_bytes = 0;
static uint64_t g_cache_lru_clock = 1;
static int g_filtering_cache_depth = 0;

#define MAX_FILTER_CACHES 128
#define MAX_SOURCE_CACHE_SLOTS 64

typedef struct FilterCacheSlot
{
    int frame_plus1; // 0 = empty, frame + 1 when valid (0x00432070)
    int orig_w;
    int orig_h;
    uint64_t lru_seq;
    void *raw_alloc;
    PIXEL_YC *ret_ptr;
    size_t alloc_bytes;
} FilterCacheSlot;

typedef struct FilterCacheEntry
{
    FILTER *fp;
    int w;
    int h;
    int d;
    int flag;
    FilterCacheSlot *slots;
    int slot_cap;
} FilterCacheEntry;

typedef struct SourceCacheSlot
{
    int frame_plus1;
    uint64_t lru_seq;
    PIXEL_YC *ycp;
    size_t alloc_bytes;
} SourceCacheSlot;

static FilterCacheEntry g_filter_caches[MAX_FILTER_CACHES];
static int g_filter_cache_count = 0;
static SourceCacheSlot g_source_cache[MAX_SOURCE_CACHE_SLOTS];

static const char *ensure_ini_path(void);

static void mt_shutdown_pool(void)
{
    int i;
    if (g_mt_init && g_mt_thread_num > 1)
    {
        g_mt_quit = TRUE;
        for (i = 1; i < g_mt_thread_num; i++)
        {
            if (g_mt_workers[i].thread)
            {
                SetEvent(g_mt_workers[i].start_ev);
                WaitForSingleObject(g_mt_workers[i].thread, 2000);
                CloseHandle(g_mt_workers[i].thread);
                CloseHandle(g_mt_workers[i].start_ev);
                CloseHandle(g_mt_workers[i].done_ev);
                memset(&g_mt_workers[i], 0, sizeof(MtWorker));
            }
        }
    }
    g_mt_init = FALSE;
    g_mt_thread_num = 1;
    g_mt_core0_mask = 0;
}

static void ensure_sys_cfg_loaded(void)
{
    if (!g_sys_cfg_loaded)
    {
        exfunc_reload_system_settings();
    }
}

static void free_filter_cache_slot_buf(FilterCacheSlot *s)
{
    if (!s)
        return;
    if (s->raw_alloc)
    {
        free(s->raw_alloc);
        s->raw_alloc = NULL;
        s->ret_ptr = NULL;
        if (g_cache_used_bytes >= s->alloc_bytes)
            g_cache_used_bytes -= s->alloc_bytes;
        else
            g_cache_used_bytes = 0;
        s->alloc_bytes = 0;
    }
    s->frame_plus1 = 0;
    s->lru_seq = 0;
}

static void free_filter_cache_entry(FilterCacheEntry *ce)
{
    int i;
    if (!ce)
        return;
    if (ce->slots)
    {
        for (i = 0; i < ce->slot_cap; i++)
        {
            free_filter_cache_slot_buf(&ce->slots[i]);
        }
        free(ce->slots);
        ce->slots = NULL;
    }
    ce->slot_cap = 0;
}

static void free_source_cache_slot_buf(SourceCacheSlot *s)
{
    if (!s)
        return;
    if (s->ycp)
    {
        free(s->ycp);
        s->ycp = NULL;
        if (g_cache_used_bytes >= s->alloc_bytes)
            g_cache_used_bytes -= s->alloc_bytes;
        else
            g_cache_used_bytes = 0;
        s->alloc_bytes = 0;
    }
    s->frame_plus1 = 0;
    s->lru_seq = 0;
}

// evict oldest allocated cache slot across all filtering + source caches to stay within sharecache MB
static BOOL cache_evict_one_oldest(const void *protect_ptr)
{
    uint64_t best_seq = ~(uint64_t)0;
    FilterCacheSlot *best_fs = NULL;
    SourceCacheSlot *best_ss = NULL;
    int i, j;

    for (i = 0; i < g_filter_cache_count; i++)
    {
        FilterCacheEntry *ce = &g_filter_caches[i];
        if (!ce->slots)
            continue;
        for (j = 0; j < ce->slot_cap; j++)
        {
            FilterCacheSlot *s = &ce->slots[j];
            if (s->raw_alloc && s->ret_ptr != protect_ptr && s->lru_seq < best_seq)
            {
                best_seq = s->lru_seq;
                best_fs = s;
                best_ss = NULL;
            }
        }
    }
    for (i = 0; i < MAX_SOURCE_CACHE_SLOTS; i++)
    {
        SourceCacheSlot *s = &g_source_cache[i];
        if (s->ycp && s->ycp != protect_ptr && s->lru_seq < best_seq)
        {
            best_seq = s->lru_seq;
            best_ss = s;
            best_fs = NULL;
        }
    }

    if (best_fs)
    {
        free_filter_cache_slot_buf(best_fs);
        return TRUE;
    }
    if (best_ss)
    {
        free_source_cache_slot_buf(best_ss);
        return TRUE;
    }
    return FALSE;
}

static void cache_enforce_budget(size_t incoming_bytes, const void *protect_ptr)
{
    ensure_sys_cfg_loaded();
    while (g_cache_used_bytes + incoming_bytes > g_cache_budget_bytes)
    {
        if (!cache_evict_one_oldest(protect_ptr))
            break;
    }
}

static DWORD WINAPI mt_worker_proc(LPVOID arg)
{
    MtWorker *w = (MtWorker *)arg;
    for (;;)
    {
        WaitForSingleObject(w->start_ev, INFINITE);
        if (g_mt_quit)
        {
            SetEvent(w->done_ev);
            break;
        }
        if (g_mt_func)
        {
            g_mt_func(w->thread_id, g_mt_thread_num, g_mt_param1, g_mt_param2);
        }
        SetEvent(w->done_ev);
    }
    return 0;
}

static void mt_ensure_pool(void)
{
    SYSTEM_INFO sysinfo;
    DWORD_PTR proc_mask = 0, sys_mask = 0;
    DWORD_PTR core_masks[MT_MAX_THREADS];
    int core_count = 0;
    int n, i;

    ensure_sys_cfg_loaded();
    if (g_mt_init)
        return;
    g_mt_init = TRUE;
    g_mt_quit = FALSE;
    g_mt_core0_mask = 0;

    GetSystemInfo(&sysinfo);
    if (g_mt_cfg_threads > 0)
    {
        n = g_mt_cfg_threads;
    }
    else
    {
        n = (int)sysinfo.dwNumberOfProcessors;
    }
    if (n < 1)
        n = 1;
    if (n > MT_MAX_THREADS)
        n = MT_MAX_THREADS;

    if (g_mt_cpu_sep && GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask) && proc_mask != 0)
    {
        int b;
        for (b = 0; b < (int)(sizeof(DWORD_PTR) * 8) && core_count < MT_MAX_THREADS; b++)
        {
            DWORD_PTR bit = ((DWORD_PTR)1u) << b;
            if (proc_mask & bit)
            {
                core_masks[core_count++] = bit;
            }
        }
        if (core_count > 0)
        {
            g_mt_core0_mask = core_masks[0];
        }
    }

    g_mt_thread_num = n;
    for (i = 1; i < n; i++)
    {
        g_mt_workers[i].thread_id = i;
        g_mt_workers[i].start_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_mt_workers[i].done_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_mt_workers[i].thread = CreateThread(NULL, 0, mt_worker_proc,
                                              &g_mt_workers[i], 0, NULL);
        if (!g_mt_workers[i].thread)
        {
            if (g_mt_workers[i].start_ev)
                CloseHandle(g_mt_workers[i].start_ev);
            if (g_mt_workers[i].done_ev)
                CloseHandle(g_mt_workers[i].done_ev);
            memset(&g_mt_workers[i], 0, sizeof(MtWorker));
            g_mt_thread_num = i;
            break;
        }
        if (g_mt_cpu_sep && core_count > 0)
        {
            SetThreadAffinityMask(g_mt_workers[i].thread, core_masks[i % core_count]);
        }
    }
}

static EDITP *resolve_ep(void *editp)
{
    return editp ? (EDITP *)editp : g_current_editp;
}

static const char *ensure_ini_path(void)
{
    if (!g_ini_path[0])
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
            wsprintfA(g_ini_path, "%saviutl.ini", mod_path);
        }
        else
        {
            lstrcpyA(g_ini_path, ".\\aviutl.ini");
        }
    }
    return g_ini_path;
}

static const char *filter_ini_section(FILTER *fp)
{
    if (fp && fp->name && fp->name[0])
    {
        return fp->name;
    }
    return "AviUtl";
}

static PIXEL_YC *ensure_filtering_yc_buf(void)
{
    size_t need = (size_t)REAVIUTL_VRAM_W * (size_t)REAVIUTL_VRAM_H;
    if (!g_filtering_yc_buf || g_filtering_yc_cap < need)
    {
        free(g_filtering_yc_buf);
        g_filtering_yc_buf = (PIXEL_YC *)calloc(need, sizeof(PIXEL_YC));
        g_filtering_yc_cap = g_filtering_yc_buf ? need : 0;
    }
    return g_filtering_yc_buf;
}

static unsigned char *ensure_disp_fmt_buf(int w, int h, DWORD format)
{
    size_t need = yc48_format_frame_size(w, h, format);
    if (need == 0)
        need = 64;
    if (need > g_disp_bgr_cap || !g_disp_bgr_buf)
    {
        unsigned char *nb = (unsigned char *)realloc(g_disp_bgr_buf, need);
        if (!nb)
            return NULL;
        g_disp_bgr_buf = nb;
        g_disp_bgr_cap = need;
    }
    return g_disp_bgr_buf;
}

static unsigned char *ensure_disp_bgr_buf(int w, int h)
{
    return ensure_disp_fmt_buf(w, h, 0);
}

// +0x000
static void *__cdecl impl_get_ycp_ofs(void *editp, int n, int ofs)
{
    EDITP *ep = resolve_ep(editp);
    if (!ep || !editp_is_editing(ep))
        return NULL;
    editp_load_frame(ep, n + ofs);
    return editp_get_ycp_edit(ep);
}

// +0x004
static void *__cdecl impl_get_ycp(void *editp, int n)
{
    return impl_get_ycp_ofs(editp, n, 0);
}

// +0x008
static void *__cdecl impl_get_pixelp(void *editp, int n)
{
    EDITP *ep = resolve_ep(editp);
    int w = 0, h = 0, mw = 0, mh = 0, stride;
    unsigned char *bgr;
    if (!ep || !editp_is_editing(ep))
        return NULL;
    editp_load_frame(ep, n);
    editp_get_dimensions(ep, &w, &h, &mw, &mh);
    if (w <= 0 || h <= 0)
        return NULL;
    bgr = ensure_disp_bgr_buf(w, h);
    if (!bgr)
        return NULL;
    stride = (w * 3 + 3) & ~3;
    yc2rgb_dib24(bgr, stride, editp_get_ycp_edit(ep), mw, w, h, TRUE);
    return bgr;
}

// +0x00c
static int __cdecl impl_get_audio(void *editp, int n, void *buf)
{
    EDITP *ep = resolve_ep(editp);
    return editp_read_audio_frame(ep, n, (short *)buf);
}

// +0x010
static BOOL __cdecl impl_is_editing(void *editp)
{
    return editp_is_editing(resolve_ep(editp));
}

// +0x014
static BOOL __cdecl impl_is_saving(void *editp)
{
    return editp_is_saving(resolve_ep(editp));
}

// +0x018
static int __cdecl impl_get_frame(void *editp)
{
    return editp_get_frame(resolve_ep(editp));
}

// +0x01c
static int __cdecl impl_get_frame_n(void *editp)
{
    return editp_get_frame_n(resolve_ep(editp));
}

// +0x020
static BOOL __cdecl impl_get_frame_size(void *editp, int *w, int *h)
{
    EDITP *ep = resolve_ep(editp);
    if (!ep || !editp_is_editing(ep))
    {
        if (w)
            *w = 0;
        if (h)
            *h = 0;
        return FALSE;
    }
    editp_get_dimensions(ep, w, h, NULL, NULL);
    return TRUE;
}

// +0x024
static int __cdecl impl_set_frame(void *editp, int n)
{
    EDITP *ep = resolve_ep(editp);
    int prev = editp_get_frame(ep);
    int cur = editp_set_frame(ep, n);
    if (ep && cur != prev && g_on_state_changed)
    {
        g_on_state_changed(ep);
    }
    return prev;
}

// +0x028
static int __cdecl impl_set_frame_n(void *editp, int n)
{
    EDITP *ep = resolve_ep(editp);
    int prev = editp_get_frame_n(ep);
    int cur = editp_set_frame_n(ep, n);
    if (ep && cur != prev && g_on_state_changed)
    {
        g_on_state_changed(ep);
    }
    return prev;
}

// +0x02c
static BOOL __cdecl impl_copy_frame(void *editp, int d, int s)
{
    EDITP *ep = resolve_ep(editp);
    BOOL ok = editp_copy_frame(ep, d, s);
    if (ok && g_on_state_changed)
        g_on_state_changed(ep);
    return ok;
}

// +0x030
static BOOL __cdecl impl_copy_video(void *editp, int d, int s)
{
    EDITP *ep = resolve_ep(editp);
    BOOL ok = editp_copy_video(ep, d, s);
    if (ok && g_on_state_changed)
        g_on_state_changed(ep);
    return ok;
}

// +0x034
static BOOL __cdecl impl_copy_audio(void *editp, int d, int s)
{
    EDITP *ep = resolve_ep(editp);
    BOOL ok = editp_copy_audio(ep, d, s);
    if (ok && g_on_state_changed)
        g_on_state_changed(ep);
    return ok;
}

// +0x038
static BOOL __cdecl impl_copy_clip(HWND hwnd, void *pixelp, int w, int h)
{
    int stride;
    size_t img_sz;
    HGLOBAL hMem;
    BITMAPINFOHEADER *bih;

    if (!pixelp || w <= 0 || h <= 0)
        return FALSE;
    stride = (w * 3 + 3) & ~3;
    img_sz = (size_t)stride * (size_t)h;
    if (!OpenClipboard(hwnd))
        return FALSE;
    EmptyClipboard();
    hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + img_sz);
    if (!hMem)
    {
        CloseClipboard();
        return FALSE;
    }
    bih = (BITMAPINFOHEADER *)GlobalLock(hMem);
    memset(bih, 0, sizeof(*bih));
    bih->biSize = sizeof(*bih);
    bih->biWidth = w;
    bih->biHeight = h;
    bih->biPlanes = 1;
    bih->biBitCount = 24;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = (DWORD)img_sz;
    memcpy((unsigned char *)bih + sizeof(*bih), pixelp, img_sz);
    GlobalUnlock(hMem);
    SetClipboardData(CF_DIB, hMem);
    CloseClipboard();
    return TRUE;
}

// +0x03c
static BOOL __cdecl impl_paste_clip(HWND hwnd, void *editp, int n)
{
    EDITP *ep = resolve_ep(editp);
    BOOL ok = editp_paste_clipboard_dib(hwnd, ep, n, FALSE);
    if (ok && g_on_state_changed)
        g_on_state_changed(ep);
    return ok;
}

// +0x040
static BOOL __cdecl impl_get_frame_status(void *editp, int n, BYTE *fps)
{
    return editp_get_frame_status(resolve_ep(editp), n, fps);
}

// +0x044
static BOOL __cdecl impl_set_frame_status(void *editp, int n, BYTE fps)
{
    return editp_set_frame_status(resolve_ep(editp), n, fps);
}

// +0x048
static BOOL __cdecl impl_is_saveframe(void *editp, int n)
{
    BYTE st = 0;
    if (!editp_get_frame_status(resolve_ep(editp), n, &st))
        return FALSE;
    return (st & EDIT_FRAME_EDIT_FLAG_DELFRAME) == 0;
}

// +0x04c
static BOOL __cdecl impl_is_keyframe(void *editp, int n)
{
    return editp_is_source_keyframe(resolve_ep(editp), n);
}

// +0x050
static BOOL __cdecl impl_is_recompress(void *editp, int n)
{
    (void)editp;
    (void)n;
    return TRUE;
}

// +0x054
static BOOL __cdecl impl_filter_window_update(FILTER *fp)
{
    if (fp && fp->hwnd && IsWindow(fp->hwnd))
    {
        SendMessageA(fp->hwnd, WM_APP + 0x41, 0, 0);
        InvalidateRect(fp->hwnd, NULL, FALSE);
        return TRUE;
    }
    return FALSE;
}

// +0x058
static BOOL __cdecl impl_is_filter_window_disp(FILTER *fp)
{
    if (!fp)
        return FALSE;
    if (fp->hwnd && IsWindow(fp->hwnd))
    {
        return IsWindowVisible(fp->hwnd);
    }
    return (fp->flag & FILTER_FLAG_WINDOW_DISP) != 0;
}

// +0x05c
static BOOL __cdecl impl_get_file_info(void *editp, FILE_INFO *fip)
{
    return editp_get_file_info(resolve_ep(editp), fip);
}

// +0x060
static LPSTR __cdecl impl_get_config_name(void *editp, int n)
{
    static char s_cfg[260];
    EDITP *ep = resolve_ep(editp);
    const char *nm;

    if (!ep || !editp_is_editing(ep))
        return NULL;
    if (n < 0 || n >= editp_get_frame_n(ep))
        return NULL;

    nm = profile_cfg_get_slot_name((int)editp_get_frame_config(ep, n));
    lstrcpynA(s_cfg, (nm && nm[0]) ? nm : "Default", sizeof(s_cfg));
    return s_cfg;
}

// +0x064
static BOOL __cdecl impl_is_filter_active(FILTER *fp)
{
    if (!fp)
        return FALSE;
    return (fp->flag & (FILTER_FLAG_ACTIVE | FILTER_FLAG_ALWAYS_ACTIVE)) != 0;
}

// +0x068
static BOOL __cdecl impl_get_pixel_filtered(void *editp, int n, void *pixelp, int *w, int *h)
{
    EDITP *ep = resolve_ep(editp);
    int fw = 0, fh = 0, mw = 0, mh = 0;
    PIXEL_YC *ycp;

    if (!ep || !editp_is_editing(ep))
        return FALSE;

    ycp = ensure_filtering_yc_buf();
    if (!ycp)
        return FALSE;
    if (g_on_render_up_to)
    {
        g_on_render_up_to(NULL, ep, n, ycp);
    }
    else
    {
        editp_load_frame(ep, n);
        editp_get_dimensions(ep, &fw, &fh, &mw, &mh);
        if (fh > 0 && mw > 0)
        {
            memcpy(ycp, editp_get_ycp_edit(ep),
                   (size_t)mw * (size_t)fh * sizeof(PIXEL_YC));
        }
    }
    editp_get_dimensions(ep, &fw, &fh, &mw, &mh);
    if (w)
        *w = fw;
    if (h)
        *h = fh;
    if (!pixelp)
        return TRUE;
    return yc48_convert_to_format(pixelp, ycp, mw, fw, fh, 0);
}

// +0x06c
static int __cdecl impl_get_audio_filtered(void *editp, int n, void *buf)
{
    EDITP *ep = resolve_ep(editp);
    if (g_on_render_audio_up_to && buf)
    {
        return g_on_render_audio_up_to(NULL, ep, n, (short *)buf);
    }
    return editp_read_audio_frame(ep, n, (short *)buf);
}

// +0x070
static BOOL __cdecl impl_get_select_frame(void *editp, int *s, int *e)
{
    return editp_get_select_frame(resolve_ep(editp), s, e);
}

// +0x074
static BOOL __cdecl impl_set_select_frame(void *editp, int s, int e)
{
    dbg_exfunc("set_select_frame(editp=0x%p, s=%d, e=%d)", editp, s, e);
    return editp_set_select_frame(resolve_ep(editp), s, e);
}

// +0x078
static BOOL __cdecl impl_rgb2yc(PIXEL_YC *ycp, PIXEL *pixelp, int w)
{
    return rgb2yc(ycp, pixelp, w);
}

// +0x07c
static BOOL __cdecl impl_yc2rgb(PIXEL *pixelp, PIXEL_YC *ycp, int w)
{
    return yc2rgb(pixelp, ycp, w);
}

// helper: convert '|' separators to '\0' if caller didn't pass double-NUL filter
static const char *prepare_dlg_filter(const char *filter, char *buf, size_t buf_sz)
{
    size_t i;
    if (!filter || !filter[0])
    {
        memcpy(buf, "All Files (*.*)\0*.*\0\0", 21);
        return buf;
    }
    if (strchr(filter, '|'))
    {
        strncpy(buf, filter, buf_sz - 2);
        buf[buf_sz - 2] = '\0';
        buf[buf_sz - 1] = '\0';
        for (i = 0; buf[i]; i++)
        {
            if (buf[i] == '|')
                buf[i] = '\0';
        }
        return buf;
    }
    return filter;
}

// +0x080
static BOOL __cdecl impl_dlg_get_load_name(LPSTR name, LPSTR filter, LPSTR def)
{
    OPENFILENAMEA ofn;
    char flt_buf[512];
    if (!name)
        return FALSE;
    if (def && def[0])
        lstrcpynA(name, def, MAX_PATH);
    else
        name[0] = '\0';

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_current_editp ? editp_get_main_hwnd(g_current_editp) : GetActiveWindow();
    ofn.lpstrFilter = prepare_dlg_filter(filter, flt_buf, sizeof(flt_buf));
    ofn.lpstrFile = name;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn);
}

// +0x084
static BOOL __cdecl impl_dlg_get_save_name(LPSTR name, LPSTR filter, LPSTR def)
{
    OPENFILENAMEA ofn;
    char flt_buf[512];
    if (!name)
        return FALSE;
    ensure_sys_cfg_loaded();
    if (def && def[0])
        lstrcpynA(name, def, MAX_PATH);
    else
        name[0] = '\0';

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_current_editp ? editp_get_main_hwnd(g_current_editp) : GetActiveWindow();
    ofn.lpstrFilter = prepare_dlg_filter(filter, flt_buf, sizeof(flt_buf));
    ofn.lpstrFile = name;
    ofn.nMaxFile = MAX_PATH;
    // 0x00412020: overwritecheck != 0 means "Do NOT confirm overwrite"
    ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR |
                ((g_cfg_overwritecheck == 0) ? OFN_OVERWRITEPROMPT : 0);
    return GetSaveFileNameA(&ofn);
}

// +0x088
static int __cdecl impl_ini_load_int(FILTER *fp, LPSTR key, int n)
{
    int val;
    if (!key)
        return n;
    val = (int)GetPrivateProfileIntA(filter_ini_section(fp), key, n, ensure_ini_path());
    // guard window geometry keys against negative/corrupted INI values that would cause exedit's timeline layer count (0x100a3fbc) to go negative
    if (lstrcmpA(key, "w") == 0)
    {
        if (val < 128 || val > 16384)
            return n;
    }
    else if (lstrcmpA(key, "h") == 0)
    {
        if (val < 96 || val > 16384)
            return n;
    }
    else if (lstrcmpA(key, "config_x") == 0 || lstrcmpA(key, "config_y") == 0)
    {
        if (val < -16384 || val > 16384)
            return n;
    }
    dbg_exfunc("ini_load_int([%s] %s, def=%d) -> %d", filter_ini_section(fp), key, n, val);
    return val;
}

// +0x08c
static int __cdecl impl_ini_save_int(FILTER *fp, LPSTR key, int n)
{
    char val[32];
    if (!key)
        return n;
    if (lstrcmpA(key, "w") == 0 && (n < 128 || n > 16384))
    {
        return n;
    }
    if (lstrcmpA(key, "h") == 0 && (n < 96 || n > 16384))
    {
        return n;
    }
    if ((lstrcmpA(key, "config_x") == 0 || lstrcmpA(key, "config_y") == 0) &&
        (n < -16384 || n > 16384))
    {
        return n;
    }
    wsprintfA(val, "%d", n);
    WritePrivateProfileStringA(filter_ini_section(fp), key, val, ensure_ini_path());
    return n;
}

// +0x090
static BOOL __cdecl impl_ini_load_str(FILTER *fp, LPSTR key, LPSTR str, LPSTR def)
{
    if (!str)
        return FALSE;
    if (!key)
    {
        lstrcpyA(str, def ? def : "");
        return FALSE;
    }
    GetPrivateProfileStringA(filter_ini_section(fp), key, def ? def : "",
                             str, 1024, ensure_ini_path());
    return TRUE;
}

// +0x094
static BOOL __cdecl impl_ini_save_str(FILTER *fp, LPSTR key, LPSTR str)
{
    if (!key || !str)
        return FALSE;
    return WritePrivateProfileStringA(filter_ini_section(fp), key, str, ensure_ini_path());
}

// +0x098
static BOOL __cdecl impl_get_source_file_info(void *editp, FILE_INFO *fip, int source_file_id)
{
    return editp_get_source_file_info(resolve_ep(editp), fip, source_file_id);
}

// +0x09c
static BOOL __cdecl impl_get_source_video_number(void *editp, int n,
                                                 int *source_file_id, int *source_video_number)
{
    return editp_get_source_video_number(resolve_ep(editp), n, source_file_id, source_video_number);
}

// +0x0a0 (editp can be NULL during func_init)
static BOOL __cdecl impl_get_sys_info(void *editp, SYS_INFO *sip)
{
    if (!sip)
        return FALSE;
    editp_get_sys_info(resolve_ep(editp), sip);
    dbg_exfunc("get_sys_info(editp=0x%p) -> build=%d flag=0x%x filter_n=%d",
               editp, sip->build, (unsigned)sip->flag, sip->filter_n);
    return TRUE;
}

// +0x0a4
static FILTER *__cdecl impl_get_filterp(int filter_id)
{
    if (g_filter_lookup)
    {
        return g_filter_lookup(filter_id);
    }
    return NULL;
}

// +0x0a8
static void *__cdecl impl_get_ycp_filtering(FILTER *fp, void *editp, int n, void *reserve)
{
    EDITP *ep = resolve_ep(editp);
    PIXEL_YC *buf;
    int w = 0, h = 0, mw = 0, mh = 0;

    (void)reserve;
    if (!ep || !editp_is_editing(ep))
        return NULL;
    buf = ensure_filtering_yc_buf();
    if (!buf)
        return NULL;

    if (g_on_render_up_to)
    {
        g_on_render_up_to(fp, ep, n, buf);
        return buf;
    }

    editp_load_frame(ep, n);
    editp_get_dimensions(ep, &w, &h, &mw, &mh);
    if (h > 0 && mw > 0)
    {
        memcpy(buf, editp_get_ycp_edit(ep),
               (size_t)mw * (size_t)h * sizeof(PIXEL_YC));
    }
    return buf;
}

// +0x0ac
static int __cdecl impl_get_audio_filtering(FILTER *fp, void *editp, int n, void *buf)
{
    EDITP *ep = resolve_ep(editp);
    if (g_on_render_audio_up_to && buf)
    {
        return g_on_render_audio_up_to(fp, ep, n, (short *)buf);
    }
    return editp_read_audio_frame(ep, n, (short *)buf);
}

static FilterCacheEntry *find_or_create_filter_cache(FILTER *fp, BOOL create_if_missing)
{
    int i;
    if (!fp)
        return NULL;
    for (i = 0; i < g_filter_cache_count; i++)
    {
        if (g_filter_caches[i].fp == fp)
            return &g_filter_caches[i];
    }
    if (!create_if_missing || g_filter_cache_count >= MAX_FILTER_CACHES)
        return NULL;
    memset(&g_filter_caches[g_filter_cache_count], 0, sizeof(FilterCacheEntry));
    g_filter_caches[g_filter_cache_count].fp = fp;
    return &g_filter_caches[g_filter_cache_count++];
}

static PIXEL_YC *query_filtering_cache_internal(FILTER *fp, void *editp, int n,
                                                int *out_w, int *out_h)
{
    EDITP *ep = resolve_ep(editp);
    FilterCacheEntry *ce;
    FilterCacheSlot *slot = NULL;
    int fn, i, best_idx = 0;
    uint64_t oldest_seq = ~(uint64_t)0;
    int fw = 0, fh = 0, mw = 0, mh = 0;
    int copy_w, copy_h, y;
    size_t slot_yc_count, alloc_bytes;
    PIXEL_YC *scratch = NULL;
    BOOL own_scratch = FALSE;

    if (!ep || !editp_is_editing(ep))
        return NULL;

    fn = editp_get_frame_n(ep);
    if (fn <= 0)
        return NULL;
    if (n < 0)
        n = 0;
    if (n >= fn)
        n = fn - 1;

    ce = find_or_create_filter_cache(fp, FALSE);
    if (!ce || ce->d <= 0 || ce->w <= 0 || ce->h <= 0 || g_filtering_cache_depth >= 8)
    {
        editp_get_dimensions(ep, out_w, out_h, NULL, NULL);
        return (PIXEL_YC *)impl_get_ycp_filtering(fp, editp, n, NULL);
    }

    if (!ce->slots || ce->slot_cap != ce->d)
    {
        free_filter_cache_entry(ce);
        ce->slots = (FilterCacheSlot *)calloc((size_t)ce->d, sizeof(FilterCacheSlot));
        if (!ce->slots)
        {
            editp_get_dimensions(ep, out_w, out_h, NULL, NULL);
            return (PIXEL_YC *)impl_get_ycp_filtering(fp, editp, n, NULL);
        }
        ce->slot_cap = ce->d;
    }

    // check lru cache hit (frame_plus1 == n + 1)
    for (i = 0; i < ce->slot_cap; i++)
    {
        if (ce->slots[i].frame_plus1 == n + 1 && ce->slots[i].ret_ptr)
        {
            ce->slots[i].lru_seq = ++g_cache_lru_clock;
            if (out_w)
                *out_w = ce->slots[i].orig_w;
            if (out_h)
                *out_h = ce->slots[i].orig_h;
            return ce->slots[i].ret_ptr;
        }
    }

    // cache miss: render upstream pipeline into scratch buffer before selecting slot
    editp_get_dimensions(ep, &fw, &fh, &mw, &mh);
    if (g_filtering_cache_depth == 0)
    {
        scratch = ensure_filtering_yc_buf();
    }
    if (!scratch || mw > REAVIUTL_VRAM_W || mh > REAVIUTL_VRAM_H)
    {
        int smw = (mw > REAVIUTL_VRAM_W) ? mw : REAVIUTL_VRAM_W;
        int smh = (mh > REAVIUTL_VRAM_H) ? mh : REAVIUTL_VRAM_H;
        scratch = (PIXEL_YC *)calloc((size_t)smw * (size_t)smh, sizeof(PIXEL_YC));
        own_scratch = TRUE;
    }
    if (!scratch)
        return NULL;

    g_filtering_cache_depth++;
    if (g_on_render_up_to)
    {
        g_on_render_up_to(fp, ep, n, scratch);
        editp_get_dimensions(ep, &fw, &fh, &mw, &mh);
    }
    else
    {
        editp_read_source_frame(ep, n, scratch, mw > 0 ? mw : REAVIUTL_VRAM_W);
    }
    g_filtering_cache_depth--;

    // re-check slot table in case set_ycp_filtering_cache_size ran during upstream render
    if (!ce->slots || ce->slot_cap <= 0)
    {
        if (out_w)
            *out_w = fw;
        if (out_h)
            *out_h = fh;
        if (own_scratch)
        {
            PIXEL_YC *fb = ensure_filtering_yc_buf();
            if (fb && mw <= REAVIUTL_VRAM_W && mh <= REAVIUTL_VRAM_H)
                memcpy(fb, scratch, (size_t)mw * (size_t)mh * sizeof(PIXEL_YC));
            free(scratch);
            return fb;
        }
        return scratch;
    }

    // pick empty slot or oldest LRU slot in this filter's ring (0x00432070)
    for (i = 0; i < ce->slot_cap; i++)
    {
        if (ce->slots[i].frame_plus1 == 0 || !ce->slots[i].raw_alloc)
        {
            best_idx = i;
            break;
        }
        if (ce->slots[i].lru_seq < oldest_seq)
        {
            oldest_seq = ce->slots[i].lru_seq;
            best_idx = i;
        }
    }
    slot = &ce->slots[best_idx];

    slot_yc_count = (size_t)ce->w * (size_t)ce->h + 32u;
    alloc_bytes = slot_yc_count * sizeof(PIXEL_YC) + 64u;
    if (!slot->raw_alloc || slot->alloc_bytes != alloc_bytes)
    {
        free_filter_cache_slot_buf(slot);
        cache_enforce_budget(alloc_bytes, NULL);
        slot->raw_alloc = calloc(1, alloc_bytes);
        if (!slot->raw_alloc)
        {
            if (own_scratch)
                free(scratch);
            return NULL;
        }
        slot->alloc_bytes = alloc_bytes;
        g_cache_used_bytes += alloc_bytes;

        if ((ce->flag & 1) != 0)
        {
            size_t border_ofs = (size_t)(4 * ce->w + 4) * sizeof(PIXEL_YC);
            uintptr_t aligned = ((uintptr_t)slot->raw_alloc + border_ofs + 15u) & ~(uintptr_t)15u;
            slot->ret_ptr = (PIXEL_YC *)aligned;
        }
        else
        {
            uintptr_t aligned = ((uintptr_t)slot->raw_alloc + 15u) & ~(uintptr_t)15u;
            slot->ret_ptr = (PIXEL_YC *)aligned;
        }
    }
    else if ((ce->flag & 1) != 0)
    {
        // keep 4-pixel border zeroed on slot reuse
        memset(slot->raw_alloc, 0, slot->alloc_bytes);
    }

    // 0x00432070 lines 56..73: copy into cache_w-strided slot (clamping to w-8, h-8 when flag & 1)
    copy_w = fw;
    copy_h = fh;
    if ((ce->flag & 1) != 0)
    {
        int max_cw = ce->w - 8;
        int max_ch = ce->h - 8;
        if (copy_w > max_cw)
            copy_w = max_cw;
        if (copy_h > max_ch)
            copy_h = max_ch;
    }
    else
    {
        if (copy_w > ce->w)
            copy_w = ce->w;
        if (copy_h > ce->h)
            copy_h = ce->h;
    }
    if (copy_w > 0 && copy_h > 0 && mw > 0)
    {
        for (y = 0; y < copy_h; y++)
        {
            memcpy(slot->ret_ptr + (size_t)y * (size_t)ce->w,
                   scratch + (size_t)y * (size_t)mw,
                   (size_t)copy_w * sizeof(PIXEL_YC));
        }
    }

    if (own_scratch)
        free(scratch);

    slot->frame_plus1 = n + 1;
    slot->orig_w = fw;
    slot->orig_h = fh;
    slot->lru_seq = ++g_cache_lru_clock;

    if (out_w)
        *out_w = fw;
    if (out_h)
        *out_h = fh;
    return slot->ret_ptr;
}

// +0x0b0 (0x00421250)
static BOOL __cdecl impl_set_ycp_filtering_cache_size(FILTER *fp, int w, int h, int d, int flag)
{
    FilterCacheEntry *ce;
    if (!fp)
        return FALSE;
    ce = find_or_create_filter_cache(fp, TRUE);
    if (!ce)
        return FALSE;
    if (ce->w != w || ce->h != h || ce->d != d || ce->flag != flag)
    {
        if (g_filtering_cache_depth > 0 && ce->slots)
        {
            return FALSE;
        }
        free_filter_cache_entry(ce);
        ce->w = w;
        ce->h = h;
        ce->d = d;
        ce->flag = flag;
    }
    return TRUE;
}

// +0x0b4 (0x00421330)
static void *__cdecl impl_get_ycp_filtering_cache(FILTER *fp, void *editp, int n)
{
    return query_filtering_cache_internal(fp, editp, n, NULL, NULL);
}

// +0x0b8 (0x00420450): LRU cache for raw unfiltered source frames (VRAM stride)
static void *__cdecl impl_get_ycp_source_cache(void *editp, int n, int ofs)
{
    EDITP *ep = resolve_ep(editp);
    int fn, target, i, best_idx = 0;
    uint64_t oldest_seq = ~(uint64_t)0;
    SourceCacheSlot *slot;
    size_t frame_bytes;

    if (!ep || !editp_is_editing(ep))
        return NULL;
    fn = editp_get_frame_n(ep);
    if (fn <= 0)
        return NULL;

    target = n + ofs;
    if (target < 0)
        target = 0;
    if (target >= fn)
        target = fn - 1;

    for (i = 0; i < MAX_SOURCE_CACHE_SLOTS; i++)
    {
        if (g_source_cache[i].frame_plus1 == target + 1 && g_source_cache[i].ycp)
        {
            g_source_cache[i].lru_seq = ++g_cache_lru_clock;
            return g_source_cache[i].ycp;
        }
    }

    for (i = 0; i < MAX_SOURCE_CACHE_SLOTS; i++)
    {
        if (g_source_cache[i].frame_plus1 == 0 || !g_source_cache[i].ycp)
        {
            best_idx = i;
            break;
        }
        if (g_source_cache[i].lru_seq < oldest_seq)
        {
            oldest_seq = g_source_cache[i].lru_seq;
            best_idx = i;
        }
    }
    slot = &g_source_cache[best_idx];
    frame_bytes = (size_t)REAVIUTL_VRAM_W * (size_t)REAVIUTL_VRAM_H * sizeof(PIXEL_YC);

    if (!slot->ycp || slot->alloc_bytes < frame_bytes)
    {
        if (slot->ycp)
            free_source_cache_slot_buf(slot);
        cache_enforce_budget(frame_bytes, NULL);
        slot->ycp = (PIXEL_YC *)calloc((size_t)REAVIUTL_VRAM_W * (size_t)REAVIUTL_VRAM_H,
                                       sizeof(PIXEL_YC));
        if (!slot->ycp)
        {
            return impl_get_ycp_ofs(editp, n, ofs);
        }
        slot->alloc_bytes = frame_bytes;
        g_cache_used_bytes += frame_bytes;
    }

    editp_read_source_frame(ep, target, slot->ycp, REAVIUTL_VRAM_W);
    slot->frame_plus1 = target + 1;
    slot->lru_seq = ++g_cache_lru_clock;
    return slot->ycp;
}

// +0x0bc
static void *__cdecl impl_get_disp_pixelp(void *editp, DWORD format)
{
    EDITP *ep = resolve_ep(editp);
    int w = 0, h = 0, mw = 0, mh = 0;
    unsigned char *out;
    if (!ep || !editp_is_editing(ep))
        return NULL;
    editp_get_dimensions(ep, &w, &h, &mw, &mh);
    if (w <= 0 || h <= 0)
        return NULL;
    out = ensure_disp_fmt_buf(w, h, format);
    if (!out)
        return NULL;
    if (!yc48_convert_to_format(out, editp_get_ycp_edit(ep), mw, w, h, format))
        return NULL;
    return out;
}

// +0x0c0
static BOOL __cdecl impl_get_pixel_source(void *editp, int n, void *pixelp, DWORD format)
{
    EDITP *ep = resolve_ep(editp);
    int w = 0, h = 0, mw = 0, mh = 0;
    if (!ep || !editp_is_editing(ep) || !pixelp)
        return FALSE;
    editp_load_frame(ep, n);
    editp_get_dimensions(ep, &w, &h, &mw, &mh);
    return yc48_convert_to_format(pixelp, editp_get_ycp_edit(ep), mw, w, h, format);
}

// +0x0c4
static BOOL __cdecl impl_get_pixel_filtered_ex(void *editp, int n, void *pixelp,
                                               int *w, int *h, DWORD format)
{
    EDITP *ep = resolve_ep(editp);
    int fw = 0, fh = 0, mw = 0, mh = 0;
    PIXEL_YC *ycp;

    if (!ep || !editp_is_editing(ep))
        return FALSE;

    ycp = ensure_filtering_yc_buf();
    if (!ycp)
        return FALSE;
    if (g_on_render_up_to)
    {
        g_on_render_up_to(NULL, ep, n, ycp);
    }
    else
    {
        editp_load_frame(ep, n);
        editp_get_dimensions(ep, &fw, &fh, &mw, &mh);
        if (fh > 0 && mw > 0)
        {
            memcpy(ycp, editp_get_ycp_edit(ep),
                   (size_t)mw * (size_t)fh * sizeof(PIXEL_YC));
        }
    }
    editp_get_dimensions(ep, &fw, &fh, &mw, &mh);
    if (w)
        *w = fw;
    if (h)
        *h = fh;
    if (!pixelp)
        return TRUE;
    return yc48_convert_to_format(pixelp, ycp, mw, fw, fh, format);
}

// +0x0c8 (0x00421680)
static PIXEL_YC *__cdecl impl_get_ycp_filtering_cache_ex(FILTER *fp, void *editp, int n,
                                                         int *w, int *h)
{
    return query_filtering_cache_internal(fp, editp, n, w, h);
}

// +0x0cc
static BOOL __cdecl impl_exec_multi_thread_func(MULTI_THREAD_FUNC func, void *param1, void *param2)
{
    int i;
    DWORD_PTR prev_aff = 0;
    HANDLE done_handles[MT_MAX_THREADS];

    if (!func)
        return FALSE;

    mt_ensure_pool();

    // re-entrant / nested call guard: run synchronously on calling thread
    if (g_mt_thread_num <= 1 || InterlockedCompareExchange(&g_mt_busy, 1, 0) != 0)
    {
        func(0, 1, param1, param2);
        return TRUE;
    }

    g_mt_func = func;
    g_mt_param1 = param1;
    g_mt_param2 = param2;

    for (i = 1; i < g_mt_thread_num; i++)
    {
        done_handles[i - 1] = g_mt_workers[i].done_ev;
        SetEvent(g_mt_workers[i].start_ev);
    }

    // thread 0 runs right on the caller thread (pinned to core 0 if threadcpuseparate)
    if (g_mt_cpu_sep && g_mt_core0_mask != 0)
    {
        prev_aff = SetThreadAffinityMask(GetCurrentThread(), g_mt_core0_mask);
    }
    func(0, g_mt_thread_num, param1, param2);
    if (prev_aff != 0)
    {
        SetThreadAffinityMask(GetCurrentThread(), prev_aff);
    }

    if (g_mt_thread_num > 1)
    {
        WaitForMultipleObjects((DWORD)(g_mt_thread_num - 1), done_handles, TRUE, INFINITE);
    }

    g_mt_func = NULL;
    InterlockedExchange(&g_mt_busy, 0);
    return TRUE;
}

// +0x0d0
static PIXEL_YC *__cdecl impl_create_yc(void)
{
    return (PIXEL_YC *)calloc((size_t)REAVIUTL_VRAM_W * (size_t)REAVIUTL_VRAM_H,
                              sizeof(PIXEL_YC));
}

// +0x0d4
static void __cdecl impl_delete_yc(PIXEL_YC *ycp)
{
    free(ycp);
}

// +0x0d8
static BOOL __cdecl impl_load_image(PIXEL_YC *ycp, LPSTR file, int *w, int *h, int flag)
{
    FILE_INFO fi;
    VFWInput *in;
    BOOL ok = TRUE;

    (void)flag;
    if (!file || !file[0])
        return FALSE;
    in = vfw_input_open(file, &fi);
    if (!in)
        return FALSE;
    if (w)
        *w = fi.w;
    if (h)
        *h = fi.h;
    if (ycp)
    {
        ok = vfw_input_read_frame_stride(in, 0, ycp, REAVIUTL_VRAM_W);
    }
    vfw_input_close(in);
    return ok;
}

// +0x0dc, bilinear yc48 resize
static void __cdecl impl_resize_yc(PIXEL_YC *ycp, int w, int h,
                                   PIXEL_YC *src, int sx, int sy, int sw, int sh)
{
    const int stride = REAVIUTL_VRAM_W;
    PIXEL_YC *temp_src = NULL;
    const PIXEL_YC *sbuf;
    int dx, dy;

    if (!ycp || w <= 0 || h <= 0 || sw <= 0 || sh <= 0)
        return;
    if (w > REAVIUTL_VRAM_W)
        w = REAVIUTL_VRAM_W;
    if (h > REAVIUTL_VRAM_H)
        h = REAVIUTL_VRAM_H;

    if (!src || src == ycp)
    {
        int y;
        temp_src = (PIXEL_YC *)malloc((size_t)sw * (size_t)sh * sizeof(PIXEL_YC));
        if (!temp_src)
            return;
        for (y = 0; y < sh; y++)
        {
            int ry = sy + y;
            if (ry < 0)
                ry = 0;
            if (ry >= REAVIUTL_VRAM_H)
                ry = REAVIUTL_VRAM_H - 1;
            memcpy(temp_src + (size_t)y * (size_t)sw,
                   ycp + (size_t)ry * (size_t)stride + sx,
                   (size_t)sw * sizeof(PIXEL_YC));
        }
        sbuf = temp_src;
    }
    else
    {
        sbuf = src;
    }

    for (dy = 0; dy < h; dy++)
    {
        int fy_fp = ((dy * 2 + 1) * sh * 256) / (h * 2) - 128;
        int y0, y1, wy;
        PIXEL_YC *dst_row = ycp + (size_t)dy * (size_t)stride;

        if (fy_fp < 0)
            fy_fp = 0;
        y0 = fy_fp >> 8;
        wy = fy_fp & 255;
        y1 = y0 + 1;
        if (y0 >= sh)
            y0 = sh - 1;
        if (y1 >= sh)
            y1 = sh - 1;

        for (dx = 0; dx < w; dx++)
        {
            int fx_fp = ((dx * 2 + 1) * sw * 256) / (w * 2) - 128;
            int x0, x1, wx;
            const PIXEL_YC *p00, *p10, *p01, *p11;

            if (fx_fp < 0)
                fx_fp = 0;
            x0 = fx_fp >> 8;
            wx = fx_fp & 255;
            x1 = x0 + 1;
            if (x0 >= sw)
                x0 = sw - 1;
            if (x1 >= sw)
                x1 = sw - 1;

            if (temp_src)
            {
                p00 = sbuf + (size_t)y0 * (size_t)sw + x0;
                p10 = sbuf + (size_t)y0 * (size_t)sw + x1;
                p01 = sbuf + (size_t)y1 * (size_t)sw + x0;
                p11 = sbuf + (size_t)y1 * (size_t)sw + x1;
            }
            else
            {
                p00 = sbuf + (size_t)(sy + y0) * (size_t)stride + (sx + x0);
                p10 = sbuf + (size_t)(sy + y0) * (size_t)stride + (sx + x1);
                p01 = sbuf + (size_t)(sy + y1) * (size_t)stride + (sx + x0);
                p11 = sbuf + (size_t)(sy + y1) * (size_t)stride + (sx + x1);
            }

            {
                int w00 = (256 - wx) * (256 - wy);
                int w10 = wx * (256 - wy);
                int w01 = (256 - wx) * wy;
                int w11 = wx * wy;
                dst_row[dx].y = (short)((p00->y * w00 + p10->y * w10 + p01->y * w01 + p11->y * w11 + 32768) >> 16);
                dst_row[dx].cb = (short)((p00->cb * w00 + p10->cb * w10 + p01->cb * w01 + p11->cb * w11 + 32768) >> 16);
                dst_row[dx].cr = (short)((p00->cr * w00 + p10->cr * w10 + p01->cr * w01 + p11->cr * w11 + 32768) >> 16);
            }
        }
    }

    free(temp_src);
}

// +0x0e0
static void __cdecl impl_copy_yc(PIXEL_YC *ycp, int x, int y,
                                 PIXEL_YC *src, int sx, int sy, int sw, int sh, int tr)
{
    const int stride = REAVIUTL_VRAM_W;
    int row, col, alpha;

    if (!ycp || !src || sw <= 0 || sh <= 0)
        return;
    if (tr < 0)
        tr = 0;
    if (tr > 4096)
        tr = 4096;
    alpha = 4096 - tr;
    if (alpha <= 0)
        return;

    for (row = 0; row < sh; row++)
    {
        int dy = y + row;
        int ry = sy + row;
        if (dy < 0 || dy >= REAVIUTL_VRAM_H || ry < 0 || ry >= REAVIUTL_VRAM_H)
            continue;
        for (col = 0; col < sw; col++)
        {
            int dx = x + col;
            int rx = sx + col;
            PIXEL_YC *d, *s;
            if (dx < 0 || dx >= REAVIUTL_VRAM_W || rx < 0 || rx >= REAVIUTL_VRAM_W)
                continue;
            d = ycp + (size_t)dy * (size_t)stride + dx;
            s = src + (size_t)ry * (size_t)stride + rx;
            if (alpha >= 4096)
            {
                *d = *s;
            }
            else
            {
                d->y = (short)((s->y * alpha + d->y * (4096 - alpha) + 2048) >> 12);
                d->cb = (short)((s->cb * alpha + d->cb * (4096 - alpha) + 2048) >> 12);
                d->cr = (short)((s->cr * alpha + d->cr * (4096 - alpha) + 2048) >> 12);
            }
        }
    }
}

// +0x0e4, gdi text to yc48
static void __cdecl impl_draw_text(PIXEL_YC *ycp, int x, int y, LPSTR text,
                                   int r, int g, int b, int tr, HFONT hfont, int *w, int *h)
{
    HDC hdc;
    HFONT use_font, old_font;
    RECT rc;
    int tw = 0, th = 0;

    if (!text || !text[0])
    {
        if (w)
            *w = 0;
        if (h)
            *h = 0;
        return;
    }

    hdc = CreateCompatibleDC(NULL);
    if (!hdc)
    {
        if (w)
            *w = 0;
        if (h)
            *h = 0;
        return;
    }

    use_font = hfont ? hfont : editp_get_ui_font();
    old_font = (HFONT)SelectObject(hdc, use_font);

    memset(&rc, 0, sizeof(rc));
    DrawTextA(hdc, text, -1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_CALCRECT);
    tw = rc.right - rc.left;
    th = rc.bottom - rc.top;

    if (w)
        *w = tw;
    if (h)
        *h = th;

    if (ycp && tw > 0 && th > 0)
    {
        BITMAPINFO bmi;
        void *bits = NULL;
        HBITMAP hbmp, old_bmp;
        int opacity;

        if (tr < 0)
            tr = 0;
        if (tr > 4096)
            tr = 4096;
        opacity = 4096 - tr;

        if (opacity > 0)
        {
            memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = tw;
            bmi.bmiHeader.biHeight = -th; // top-down 32bpp
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (hbmp && bits)
            {
                PIXEL fg_rgb;
                PIXEL_YC fg_yc;
                int py, px;

                memset(bits, 0, (size_t)tw * (size_t)th * 4);
                old_bmp = (HBITMAP)SelectObject(hdc, hbmp);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 255));
                rc.left = 0;
                rc.top = 0;
                rc.right = tw;
                rc.bottom = th;
                DrawTextA(hdc, text, -1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX);
                GdiFlush();

                fg_rgb.r = (unsigned char)(r < 0 ? 0 : (r > 255 ? 255 : r));
                fg_rgb.g = (unsigned char)(g < 0 ? 0 : (g > 255 ? 255 : g));
                fg_rgb.b = (unsigned char)(b < 0 ? 0 : (b > 255 ? 255 : b));
                rgb2yc(&fg_yc, &fg_rgb, 1);

                for (py = 0; py < th; py++)
                {
                    int dy = y + py;
                    const unsigned char *srow;
                    PIXEL_YC *drow;
                    if (dy < 0 || dy >= REAVIUTL_VRAM_H)
                        continue;
                    srow = (const unsigned char *)bits + (size_t)py * (size_t)tw * 4;
                    drow = ycp + (size_t)dy * (size_t)REAVIUTL_VRAM_W;
                    for (px = 0; px < tw; px++)
                    {
                        int dx = x + px;
                        int cov;
                        if (dx < 0 || dx >= REAVIUTL_VRAM_W)
                            continue;
                        cov = (srow[px * 4 + 0] + srow[px * 4 + 1] + srow[px * 4 + 2]) / 3;
                        if (cov > 0)
                        {
                            int a = (cov * opacity) / 255;
                            drow[dx].y = (short)((fg_yc.y * a + drow[dx].y * (4096 - a) + 2048) >> 12);
                            drow[dx].cb = (short)((fg_yc.cb * a + drow[dx].cb * (4096 - a) + 2048) >> 12);
                            drow[dx].cr = (short)((fg_yc.cr * a + drow[dx].cr * (4096 - a) + 2048) >> 12);
                        }
                    }
                }

                SelectObject(hdc, old_bmp);
                DeleteObject(hbmp);
            }
        }
    }

    SelectObject(hdc, old_font);
    DeleteDC(hdc);
}

// +0x0e8
static AVI_FILE_HANDLE __cdecl impl_avi_file_open(LPSTR file, FILE_INFO *fip, int flag)
{
    AVI_FILE_HANDLE h = (AVI_FILE_HANDLE)vfw_input_open_ex(file, fip, flag);
    if (!h)
    {
        dbg_warn("EXFUNC avi_file_open('%s', flag=0x%x) failed", file ? file : "(null)", (unsigned)flag);
    }
    else
    {
        dbg_exfunc("avi_file_open('%s', flag=0x%x) -> 0x%p", file ? file : "(null)", (unsigned)flag, (void *)h);
    }
    return h;
}

// +0x0ec
static void __cdecl impl_avi_file_close(AVI_FILE_HANDLE afh)
{
    dbg_exfunc("avi_file_close(0x%p)", (void *)afh);
    vfw_input_close((VFWInput *)afh);
}

// +0x0f0, vram-strided yc48 read
static BOOL __cdecl impl_avi_file_read_video(AVI_FILE_HANDLE afh, PIXEL_YC *ycp, int n)
{
    return vfw_input_read_frame_stride((VFWInput *)afh, n, ycp, REAVIUTL_VRAM_W);
}

// +0x0f4
static int __cdecl impl_avi_file_read_audio(AVI_FILE_HANDLE afh, void *buf, int n)
{
    FILE_INFO fi;
    int vrate, vscale, count;
    long long s0, s1;
    if (!afh || n < 0 || !vfw_input_get_file_info((VFWInput *)afh, &fi))
        return 0;
    if (fi.audio_rate <= 0)
        return 0;
    vrate = fi.video_rate;
    vscale = fi.video_scale;
    if (vrate <= 0 || vscale <= 0)
    {
        editp_get_fps(resolve_ep(NULL), &vrate, &vscale);
        if (vrate <= 0)
            vrate = 30;
        if (vscale <= 0)
            vscale = 1;
    }
    s0 = ((long long)n * fi.audio_rate * vscale) / vrate;
    s1 = ((long long)(n + 1) * fi.audio_rate * vscale) / vrate;
    count = (int)(s1 - s0);
    if (count <= 0)
        return 0;
    return vfw_input_read_audio((VFWInput *)afh, (int)s0, buf, count);
}

// +0x0f8
static void *__cdecl impl_avi_file_get_video_pixelp(AVI_FILE_HANDLE afh, int n)
{
    return vfw_input_get_dib_pixelp((VFWInput *)afh, n);
}

// +0x0fc
static LPSTR __cdecl impl_get_avi_file_filter(int type)
{
    return input_plugin_get_file_filter(type);
}

// +0x100
static int __cdecl impl_avi_file_read_audio_sample(AVI_FILE_HANDLE afh, int start, int length, void *buf)
{
    return vfw_input_read_audio((VFWInput *)afh, start, buf, length);
}

// +0x104
static int __cdecl impl_avi_file_set_audio_sample_rate(AVI_FILE_HANDLE afh, int audio_rate, int audio_ch)
{
    return vfw_input_set_audio_format((VFWInput *)afh, audio_rate, audio_ch);
}

// +0x108
static BYTE *__cdecl impl_get_frame_status_table(void *editp, int type)
{
    return editp_get_frame_status_table(resolve_ep(editp), type);
}

// +0x10c
static BOOL __cdecl impl_set_undo(void *editp)
{
    return editp_push_undo(resolve_ep(editp));
}

// +0x110
static BOOL __cdecl impl_add_menu_item(FILTER *fp, LPSTR name, HWND hwnd,
                                       int id, int def_key, int flag)
{
    dbg_exfunc("add_menu_item(fp=0x%p '%s', id=%d, key=0x%x, flag=0x%x)",
               (void *)fp, name ? name : "", id, (unsigned)def_key, (unsigned)flag);
    if (g_add_menu_handler)
    {
        return g_add_menu_handler(fp, name, hwnd, id, def_key, flag);
    }
    return TRUE;
}

// +0x114
static BOOL __cdecl impl_edit_open(void *editp, LPSTR file, int flag)
{
    EDITP *ep = resolve_ep(editp);
    char chosen[MAX_PATH];
    const char *target = file;

    dbg_info("EXFUNC edit_open('%s', flag=0x%x)", file ? file : "(null)", (unsigned)flag);
    if (!ep)
        return FALSE;

    if ((flag & EDIT_OPEN_FLAG_DIALOG) != 0)
    {
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ZeroMemory(chosen, sizeof(chosen));
        if (file && file[0])
            lstrcpynA(chosen, file, MAX_PATH);
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = editp_get_main_hwnd(ep);
        ofn.lpstrFilter = (flag & EDIT_OPEN_FLAG_PROJECT) != 0
                              ? "AviUtl ProjectFile (*.aup)\0*.aup\0All Files (*.*)\0*.*\0"
                              : "All Files (*.*)\0*.*\0";
        ofn.lpstrFile = chosen;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameA(&ofn))
            return FALSE;
        target = chosen;
    }

    if (!target || !target[0])
        return FALSE;
    if (g_on_edit_open)
    {
        return g_on_edit_open(ep, target, flag);
    }
    if ((flag & EDIT_OPEN_FLAG_AUDIO) != 0 && editp_is_editing(ep))
    {
        return editp_open_audio(ep, target);
    }
    if ((flag & EDIT_OPEN_FLAG_ADD) != 0 && editp_is_editing(ep))
    {
        return editp_append_video(ep, target);
    }
    return editp_open_video(ep, target);
}

// +0x118
static BOOL __cdecl impl_edit_close(void *editp)
{
    EDITP *ep = resolve_ep(editp);
    dbg_exfunc("edit_close(editp=0x%p)", editp);
    if (!ep)
        return FALSE;
    if (g_on_edit_close)
    {
        return g_on_edit_close(ep);
    }
    editp_close_video(ep);
    return TRUE;
}

// +0x11c
static BOOL __cdecl impl_edit_output(void *editp, LPSTR file, int flag, LPSTR type)
{
    EDITP *ep = resolve_ep(editp);
    dbg_info("EXFUNC edit_output('%s', flag=0x%x, type='%s')",
             file ? file : "(null)", (unsigned)flag, type ? type : "(avi)");
    if (!ep || !file || !file[0] || !editp_is_editing(ep))
        return FALSE;
    if (g_on_edit_output)
    {
        return g_on_edit_output(ep, file, flag, type);
    }
    return FALSE;
}

// +0x120
static BOOL __cdecl impl_set_config(void *editp, int n, LPSTR name)
{
    EDITP *ep = resolve_ep(editp);
    int idx;

    if (name && name[0])
    {
        idx = profile_cfg_find_by_name(name);
    }
    else
    {
        idx = n;
    }
    if (idx < 0 || idx >= profile_cfg_get_count())
        return FALSE;
    if (!profile_cfg_select(idx, TRUE))
        return FALSE;

    if (g_on_state_changed)
        g_on_state_changed(ep);
    return TRUE;
}

static EXFUNC g_exfunc_table = {
    impl_get_ycp_ofs,                    // +0x000
    impl_get_ycp,                        // +0x004
    impl_get_pixelp,                     // +0x008
    impl_get_audio,                      // +0x00c
    impl_is_editing,                     // +0x010
    impl_is_saving,                      // +0x014
    impl_get_frame,                      // +0x018
    impl_get_frame_n,                    // +0x01c
    impl_get_frame_size,                 // +0x020
    impl_set_frame,                      // +0x024
    impl_set_frame_n,                    // +0x028
    impl_copy_frame,                     // +0x02c
    impl_copy_video,                     // +0x030
    impl_copy_audio,                     // +0x034
    impl_copy_clip,                      // +0x038
    impl_paste_clip,                     // +0x03c
    impl_get_frame_status,               // +0x040
    impl_set_frame_status,               // +0x044
    impl_is_saveframe,                   // +0x048
    impl_is_keyframe,                    // +0x04c
    impl_is_recompress,                  // +0x050
    impl_filter_window_update,           // +0x054
    impl_is_filter_window_disp,          // +0x058
    impl_get_file_info,                  // +0x05c
    impl_get_config_name,                // +0x060
    impl_is_filter_active,               // +0x064
    impl_get_pixel_filtered,             // +0x068
    impl_get_audio_filtered,             // +0x06c
    impl_get_select_frame,               // +0x070
    impl_set_select_frame,               // +0x074
    impl_rgb2yc,                         // +0x078
    impl_yc2rgb,                         // +0x07c
    impl_dlg_get_load_name,              // +0x080
    impl_dlg_get_save_name,              // +0x084
    impl_ini_load_int,                   // +0x088
    impl_ini_save_int,                   // +0x08c
    impl_ini_load_str,                   // +0x090
    impl_ini_save_str,                   // +0x094
    impl_get_source_file_info,           // +0x098
    impl_get_source_video_number,        // +0x09c
    impl_get_sys_info,                   // +0x0a0
    impl_get_filterp,                    // +0x0a4
    impl_get_ycp_filtering,              // +0x0a8
    impl_get_audio_filtering,            // +0x0ac
    impl_set_ycp_filtering_cache_size,   // +0x0b0
    impl_get_ycp_filtering_cache,        // +0x0b4
    impl_get_ycp_source_cache,           // +0x0b8
    impl_get_disp_pixelp,                // +0x0bc
    impl_get_pixel_source,               // +0x0c0
    impl_get_pixel_filtered_ex,          // +0x0c4
    impl_get_ycp_filtering_cache_ex,     // +0x0c8
    impl_exec_multi_thread_func,         // +0x0cc
    impl_create_yc,                      // +0x0d0
    impl_delete_yc,                      // +0x0d4
    impl_load_image,                     // +0x0d8
    impl_resize_yc,                      // +0x0dc
    impl_copy_yc,                        // +0x0e0
    impl_draw_text,                      // +0x0e4
    impl_avi_file_open,                  // +0x0e8
    impl_avi_file_close,                 // +0x0ec
    impl_avi_file_read_video,            // +0x0f0
    impl_avi_file_read_audio,            // +0x0f4
    impl_avi_file_get_video_pixelp,      // +0x0f8
    impl_get_avi_file_filter,            // +0x0fc
    impl_avi_file_read_audio_sample,     // +0x100
    impl_avi_file_set_audio_sample_rate, // +0x104
    impl_get_frame_status_table,         // +0x108
    impl_set_undo,                       // +0x10c
    impl_add_menu_item,                  // +0x110
    impl_edit_open,                      // +0x114
    impl_edit_close,                     // +0x118
    impl_edit_output,                    // +0x11c
    impl_set_config,                     // +0x120
    {0, 0, 0, 0, 0, 0, 0}                // +0x124
};

EXFUNC *exfunc_get_table(void)
{
    return &g_exfunc_table;
}

void exfunc_set_editp(EDITP *editp)
{
    g_current_editp = editp;
}

EDITP *exfunc_get_editp(void)
{
    return g_current_editp;
}

void exfunc_set_ini_file(const char *path)
{
    if (path && path[0])
    {
        lstrcpynA(g_ini_path, path, MAX_PATH);
        g_sys_cfg_loaded = FALSE;
    }
}

void exfunc_set_filter_lookup(ExfuncFilterLookupFn fn)
{
    g_filter_lookup = fn;
}

void exfunc_set_add_menu_item_handler(ExfuncAddMenuItemFn fn)
{
    g_add_menu_handler = fn;
}

void exfunc_set_host_callbacks(ExfuncHostNotifyFn on_state_changed,
                               ExfuncEditOpenFn on_edit_open,
                               ExfuncEditCloseFn on_edit_close,
                               ExfuncRenderUpToFn on_render_up_to)
{
    g_on_state_changed = on_state_changed;
    g_on_edit_open = on_edit_open;
    g_on_edit_close = on_edit_close;
    g_on_render_up_to = on_render_up_to;
}

void exfunc_set_audio_render_callback(ExfuncRenderAudioUpToFn on_render_audio_up_to)
{
    g_on_render_audio_up_to = on_render_audio_up_to;
}

void exfunc_set_edit_output_callback(ExfuncEditOutputFn on_edit_output)
{
    g_on_edit_output = on_edit_output;
}

void exfunc_reload_system_settings(void)
{
    const char *ini = ensure_ini_path();
    int mt = (int)GetPrivateProfileIntA("system", "multithread", 0, ini);
    BOOL sep = GetPrivateProfileIntA("system", "threadcpuseparate", 0, ini) != 0;
    int share_mb = (int)GetPrivateProfileIntA("system", "sharecache", 256, ini);

    editp_reload_sys_flags_from_ini(ini);

    if (mt < 0)
        mt = 0;
    if (mt > MT_MAX_THREADS)
        mt = MT_MAX_THREADS;
    if (share_mb < 8)
        share_mb = 8;
    if (share_mb > (REAVIUTL_X64 ? 65536 : 2048))
        share_mb = (REAVIUTL_X64 ? 65536 : 2048);

    g_cfg_overwritecheck = (int)GetPrivateProfileIntA("system", "overwritecheck", 0, ini);
    g_cache_budget_bytes = (size_t)share_mb * 1024u * 1024u;
    g_sys_cfg_loaded = TRUE;

    cache_enforce_budget(0, NULL);

    if (mt != g_mt_cfg_threads || sep != g_mt_cpu_sep)
    {
        g_mt_cfg_threads = mt;
        g_mt_cpu_sep = sep;
        if (g_mt_init && InterlockedCompareExchange(&g_mt_busy, 0, 0) == 0)
        {
            mt_shutdown_pool();
        }
    }
}

void exfunc_invalidate_all_caches(void)
{
    int i, j;
    for (i = 0; i < g_filter_cache_count; i++)
    {
        FilterCacheEntry *ce = &g_filter_caches[i];
        if (!ce->slots)
            continue;
        for (j = 0; j < ce->slot_cap; j++)
        {
            ce->slots[j].frame_plus1 = 0;
        }
    }
    for (i = 0; i < MAX_SOURCE_CACHE_SLOTS; i++)
    {
        g_source_cache[i].frame_plus1 = 0;
    }
}

void exfunc_invalidate_filtering_caches_from(const FILTER *const *ordered_after, int count)
{
    int i, j, k;
    if (!ordered_after || count <= 0)
        return;
    for (k = 0; k < count; k++)
    {
        const FILTER *fp = ordered_after[k];
        if (!fp)
            continue;
        for (i = 0; i < g_filter_cache_count; i++)
        {
            FilterCacheEntry *ce = &g_filter_caches[i];
            if (ce->fp == fp && ce->slots)
            {
                for (j = 0; j < ce->slot_cap; j++)
                {
                    ce->slots[j].frame_plus1 = 0;
                }
            }
        }
    }
}

void exfunc_shutdown(void)
{
    int i;
    mt_shutdown_pool();
    for (i = 0; i < g_filter_cache_count; i++)
    {
        free_filter_cache_entry(&g_filter_caches[i]);
    }
    g_filter_cache_count = 0;
    for (i = 0; i < MAX_SOURCE_CACHE_SLOTS; i++)
    {
        free_source_cache_slot_buf(&g_source_cache[i]);
    }
    g_cache_used_bytes = 0;
    free(g_filtering_yc_buf);
    g_filtering_yc_buf = NULL;
    g_filtering_yc_cap = 0;
    free(g_disp_bgr_buf);
    g_disp_bgr_buf = NULL;
    g_disp_bgr_cap = 0;
}
