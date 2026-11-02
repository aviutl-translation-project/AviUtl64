#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <msacm.h>
#include <vfw.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video_export.h"
#include "plugin_manager.h"
#include "plugin_bridge.h"
#include "yc48_convert.h"
#include "exfunc.h"
#include "i18n.h"
#include "aviutl_dlg.h"
#include "debug_console.h"

// local .auo registry entry (in-process)
typedef struct
{
    char path[MAX_PATH];
    char name[128];
    char filefilter[512]; // double-NUL terminated
    char information[256];
    int flag;
    BOOL has_config;
    HMODULE module;
    OUTPUT_PLUGIN_TABLE *opt;
    OUTPUT_PLUGIN_TABLE runtime_opt;
    BOOL initialized;
    BOOL faulted;
} LocalAuoEntry;

static LocalAuoEntry g_local_auos[AUO_MAX_PLUGINS];
static int g_local_auo_count = 0;
static int g_last_auo_idx = 0;
static HWND g_host_hwnd = NULL;
static VideoExportPreviewUpdateFn g_on_preview_update = NULL;
static VideoExportProgressFn g_on_progress = NULL;

// optional pointers into BridgeSharedMem when running inside reaviutl_bridge32.exe
static volatile LONG *g_shm_abort = NULL;
static volatile LONG *g_shm_now = NULL;
static volatile LONG *g_shm_total = NULL;

// VFW video compression & ACM audio compression state (IDs 5127 & 5128)
static COMPVARS g_vcodec;
static BOOL g_vcodec_valid = FALSE;
static WAVEFORMATEX *g_acodec_wfx = NULL;
static DWORD g_acodec_max_sz = 0;
static BOOL g_acodec_valid = FALSE;

// active encode session state for OUTPUT_INFO callbacks & Esc abort
typedef struct
{
    BOOL active;
    volatile LONG abort_req;
    HWND owner;
    EDITP *ep;
    int select_s;
    int select_e;
    int *tl_frames; // [0 .. out_n-1] non-deleted timeline frame indices
    int out_n;
    int *aud_sample_ofs; // [0 .. out_n] cumulative sample offsets
    int total_audio_samples;
    int audio_rate;
    int audio_ch;
    unsigned char *vid_buf; // persistent frame buffer returned by func_get_video*
    size_t vid_buf_cap;
    short *aud_out_buf; // persistent audio buffer returned by func_get_audio
    size_t aud_out_shorts_cap;
    int cached_aud_k;
    short *cached_aud_pcm;
    int cached_aud_shorts_cap;
    DWORD last_ui_tick;
} ActiveEncodeCtx;

static ActiveEncodeCtx g_enc;

static void sjis_to_wide_auo(const char *src, wchar_t *dst, int dst_cch)
{
    if (!dst || dst_cch <= 0)
        return;
    dst[0] = L'\0';
    if (!src || !src[0])
        return;
    if (MultiByteToWideChar(932, 0, src, -1, dst, dst_cch) > 0)
        return;
    MultiByteToWideChar(CP_ACP, 0, src, -1, dst, dst_cch);
}

static void copy_double_nul_filter(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!dst || cap < 4)
        return;
    memset(dst, 0, cap);
    if (!src || !src[0])
    {
        memcpy(dst, "All Files (*.*)\0*.*\0\0", 21 < cap ? 21 : cap - 2);
        dst[cap - 2] = '\0';
        dst[cap - 1] = '\0';
        return;
    }
    // copy pairs of NUL-terminated strings until double-NUL (0x0042ac80)
    while (src[i] != '\0' && i + 2 < cap)
    {
        while (src[i] != '\0' && i + 2 < cap)
        {
            dst[i] = src[i];
            i++;
        }
        dst[i++] = '\0';
        if (src[i] == '\0')
        {
            break;
        }
    }
    dst[i] = '\0';
}

static void extract_default_ext(const char *double_nul_flt, char *ext_out, size_t ext_cap)
{
    const char *pat;
    const char *dot;
    size_t k = 0;

    if (!ext_out || ext_cap == 0)
        return;
    ext_out[0] = '\0';
    if (!double_nul_flt || !double_nul_flt[0])
    {
        lstrcpynA(ext_out, "avi", (int)ext_cap);
        return;
    }
    pat = double_nul_flt + strlen(double_nul_flt) + 1;
    dot = strrchr(pat, '.');
    if (dot && dot[1] && dot[1] != '*')
    {
        dot++;
        while (dot[k] && dot[k] != ';' && dot[k] != ' ' && k + 1 < ext_cap)
        {
            ext_out[k] = dot[k];
            k++;
        }
        ext_out[k] = '\0';
    }
    if (!ext_out[0])
    {
        lstrcpynA(ext_out, "avi", (int)ext_cap);
    }
}

static const char *path_basename_dbcs(const char *path)
{
    const char *base;
    const unsigned char *p;
    if (!path)
        return "";
    base = path;
    p = (const unsigned char *)path;
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
            }
            p++;
        }
    }
    return base;
}

static void get_export_ini_path(char *out_path, size_t out_sz)
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

static DWORD get_export_save_ofn_flags(void)
{
    char ini[MAX_PATH];
    DWORD flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    get_export_ini_path(ini, sizeof(ini));
    if (GetPrivateProfileIntA("system", "overwritecheck", 0, ini) == 0)
    {
        flags |= OFN_OVERWRITEPROMPT;
    }
    return flags;
}

static BOOL is_overwriting_open_edit_file(HWND owner, EDITP *ep, const char *save_path)
{
    const char *ed;
    char ed_abs[MAX_PATH], save_abs[MAX_PATH];

    if (!ep || !save_path || !save_path[0] || g_shm_now != NULL)
        return FALSE;
    ed = editp_get_edit_path(ep);
    if (!ed || !ed[0])
        return FALSE;

    if (GetFullPathNameA(ed, MAX_PATH, ed_abs, NULL) == 0)
        lstrcpynA(ed_abs, ed, MAX_PATH);
    if (GetFullPathNameA(save_path, MAX_PATH, save_abs, NULL) == 0)
        lstrcpynA(save_abs, save_path, MAX_PATH);

    if (lstrcmpiA(ed_abs, save_abs) == 0)
    {
        aviutl_msgbox_w(owner, i18n_get_wstr(1015), L"AviUtl", MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    return FALSE;
}

// 0x0041a010: flash main window and optionally play [system] finishsound on non-batch completion
static void notify_export_finished(HWND owner, int out_flags)
{
    char ini[MAX_PATH];
    char snd[MAX_PATH];

    if (g_shm_now != NULL || (out_flags & OUTPUT_INFO_FLAG_BATCH) != 0)
        return;

    if (owner && IsWindow(owner))
    {
        FlashWindow(owner, TRUE);
    }

    get_export_ini_path(ini, sizeof(ini));
    GetPrivateProfileStringA("system", "finishsound", "", snd, sizeof(snd), ini);
    if (snd[0])
    {
        PlaySoundA(snd, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }
}

// 0x00415820: expand [system] defaultsavefile (%p, %f/%a, %w, %h, %t, %b, %e, leading '*')
static void expand_default_save_path(EDITP *ep, const char *def_ext,
                                     char *out_dir, size_t dir_cap,
                                     char *out_file, size_t file_cap)
{
    char ini[MAX_PATH];
    char tpl_buf[MAX_PATH];
    char expanded[MAX_PATH * 2];
    const char *tpl;
    const char *cur_out;
    const char *ed_path;
    const char *ed_base;
    char in_dir[MAX_PATH];
    char in_stem[MAX_PATH];
    BOOL force_tpl = FALSE;
    int w = 0, h = 0, mw = 0, mh = 0, s = 0, e = 0, fn = 0, sf = 1;
    size_t pos = 0;
    const unsigned char *p;

    if (out_dir && dir_cap > 0)
        out_dir[0] = '\0';
    if (out_file && file_cap > 0)
        out_file[0] = '\0';
    if (!ep || !out_file || file_cap == 0)
        return;

    get_export_ini_path(ini, sizeof(ini));
    GetPrivateProfileStringA("system", "defaultsavefile", "%p", tpl_buf, sizeof(tpl_buf), ini);
    sf = GetPrivateProfileIntA("system", "startframe", 1, ini) ? 1 : 0;

    tpl = tpl_buf;
    if (tpl[0] == '*')
    {
        force_tpl = TRUE;
        tpl++;
    }

    cur_out = editp_get_output_path(ep);
    if (!force_tpl && cur_out && cur_out[0])
    {
        char *dot;
        const char *base_chk;
        lstrcpynA(out_file, cur_out, (int)file_cap);
        base_chk = path_basename_dbcs(out_file);
        dot = (char *)strrchr(base_chk, '.');
        if (dot && def_ext && def_ext[0])
        {
            *dot = '\0';
            if (strlen(out_file) + strlen(def_ext) + 2 < file_cap)
            {
                lstrcatA(out_file, ".");
                lstrcatA(out_file, def_ext);
            }
        }
        return;
    }

    ed_path = editp_get_edit_path(ep);
    ed_base = path_basename_dbcs(ed_path);
    {
        size_t dir_len = (size_t)(ed_base - (ed_path ? ed_path : ""));
        if (dir_len >= sizeof(in_dir))
            dir_len = sizeof(in_dir) - 1;
        if (ed_path && dir_len > 0)
            memcpy(in_dir, ed_path, dir_len);
        in_dir[dir_len] = '\0';
    }
    lstrcpynA(in_stem, ed_base[0] ? ed_base : "output", sizeof(in_stem));
    {
        char *dot = strrchr(in_stem, '.');
        if (dot)
            *dot = '\0';
    }

    editp_get_dimensions(ep, &w, &h, &mw, &mh);
    fn = editp_get_frame_n(ep);
    if (!editp_get_select_frame(ep, &s, &e) || s < 0 || e < s)
    {
        s = 0;
        e = fn > 0 ? fn - 1 : 0;
    }

    p = (const unsigned char *)tpl;
    while (*p && pos + 2 < sizeof(expanded))
    {
        if (IsDBCSLeadByte(*p))
        {
            expanded[pos++] = (char)*p++;
            if (*p && pos + 1 < sizeof(expanded))
                expanded[pos++] = (char)*p++;
            continue;
        }
        if (*p == '%')
        {
            char tok = (char)p[1];
            char num[32];
            const char *ins = NULL;

            if (tok == '\0')
            {
                expanded[pos++] = '%';
                break;
            }
            p += 2;
            if (tok == 'e' || tok == 'E')
            {
                break;
            }
            switch (tok)
            {
            case 'p':
            case 'P':
                ins = in_dir;
                break;
            case 'f':
            case 'F':
            case 'a':
            case 'A':
                ins = in_stem;
                break;
            case 'w':
            case 'W':
                wsprintfA(num, "%d", w);
                ins = num;
                break;
            case 'h':
            case 'H':
                wsprintfA(num, "%d", h);
                ins = num;
                break;
            case 't':
            case 'T':
                wsprintfA(num, "%d", s + sf);
                ins = num;
                break;
            case 'b':
            case 'B':
                wsprintfA(num, "%d", e + sf);
                ins = num;
                break;
            case '%':
                ins = "%";
                break;
            default:
                num[0] = '%';
                num[1] = tok;
                num[2] = '\0';
                ins = num;
                break;
            }
            if (ins)
            {
                size_t ilen = strlen(ins);
                if (pos + ilen >= sizeof(expanded))
                    ilen = sizeof(expanded) - 1 - pos;
                if (ilen > 0)
                {
                    memcpy(expanded + pos, ins, ilen);
                    pos += ilen;
                }
            }
        }
        else
        {
            expanded[pos++] = (char)*p++;
        }
    }
    expanded[pos] = '\0';

    {
        const char *ebase = path_basename_dbcs(expanded);
        if (!ebase[0])
        {
            if (out_dir && dir_cap > 0 && expanded[0])
            {
                lstrcpynA(out_dir, expanded, (int)dir_cap);
            }
            out_file[0] = '\0';
        }
        else
        {
            if (!strchr(ebase, '.') && def_ext && def_ext[0] &&
                strlen(expanded) + strlen(def_ext) + 2 < sizeof(expanded))
            {
                lstrcatA(expanded, ".");
                lstrcatA(expanded, def_ext);
            }
            lstrcpynA(out_file, expanded, (int)file_cap);
        }
    }
}

static BOOL safe_auo_init(LocalAuoEntry *e)
{
    BOOL ok = TRUE;
    if (!e || e->faulted)
        return FALSE;
    if (!e->runtime_opt.func_init)
        return TRUE;
    __try
    {
        ok = e->runtime_opt.func_init();
    }
    __except (debug_console_seh_filter(GetExceptionInformation(), e->name, "AUO func_init"))
    {
        e->faulted = TRUE;
        ok = FALSE;
    }
    return ok;
}

static void safe_auo_exit(LocalAuoEntry *e)
{
    if (!e || e->faulted || !e->runtime_opt.func_exit)
        return;
    __try
    {
        e->runtime_opt.func_exit();
    }
    __except (debug_console_seh_filter(GetExceptionInformation(), e->name, "AUO func_exit"))
    {
        e->faulted = TRUE;
    }
}

static BOOL safe_auo_output(LocalAuoEntry *e, OUTPUT_INFO *oip)
{
    BOOL ok = FALSE;
    if (!e || e->faulted || !e->runtime_opt.func_output)
        return FALSE;
    __try
    {
        ok = e->runtime_opt.func_output(oip);
    }
    __except (debug_console_seh_filter(GetExceptionInformation(), e->name, "AUO func_output"))
    {
        e->faulted = TRUE;
        ok = FALSE;
    }
    return ok;
}

static BOOL safe_auo_config(LocalAuoEntry *e, HWND hwnd)
{
    BOOL ok = FALSE;
    if (!e || e->faulted || !e->runtime_opt.func_config)
        return FALSE;
    __try
    {
        ok = e->runtime_opt.func_config(hwnd, (HINSTANCE)e->module);
    }
    __except (debug_console_seh_filter(GetExceptionInformation(), e->name, "AUO func_config"))
    {
        e->faulted = TRUE;
        ok = FALSE;
    }
    return ok;
}

// 0x0042b5f0, lazy-load HMODULE + GetOutputPluginTable + func_init()
static BOOL ensure_local_auo_initialized(LocalAuoEntry *e)
{
    GET_OUTPUT_PLUGIN_TABLE_FN fn;
    OUTPUT_PLUGIN_TABLE *tbl;

    if (!e || e->faulted)
        return FALSE;
    if (e->module && e->initialized)
        return TRUE;

    e->module = LoadLibraryA(e->path);
    if (!e->module)
    {
        dbg_warn("Failed to load .auo '%s' (err=%lu)", e->path, (unsigned long)GetLastError());
        return FALSE;
    }
    fn = (GET_OUTPUT_PLUGIN_TABLE_FN)(void *)GetProcAddress(e->module, "GetOutputPluginTable");
    if (!fn)
    {
        fn = (GET_OUTPUT_PLUGIN_TABLE_FN)(void *)GetProcAddress(e->module, "_GetOutputPluginTable");
    }
    if (!fn)
    {
        FreeLibrary(e->module);
        e->module = NULL;
        return FALSE;
    }
    tbl = fn();
    if (!tbl)
    {
        FreeLibrary(e->module);
        e->module = NULL;
        return FALSE;
    }
    e->opt = tbl;
    e->runtime_opt = *tbl;
    if (e->runtime_opt.func_config)
    {
        e->runtime_opt.flag |= OUTPUT_PLUGIN_TABLE_FLAG_HAS_CONFIG;
    }
    if (!safe_auo_init(e))
    {
        FreeLibrary(e->module);
        e->module = NULL;
        return FALSE;
    }
    e->initialized = TRUE;
    dbg_plugin("Initialized output plugin '%s' @ HMODULE=0x%p", e->name, (void *)e->module);
    return TRUE;
}

static void pump_encode_messages_and_progress(HWND owner, int now, int total, BOOL force_title)
{
    DWORD tick;
    MSG msg;

    if (g_shm_abort && *g_shm_abort != 0)
    {
        InterlockedExchange(&g_enc.abort_req, 1);
    }
    if (g_shm_now)
        InterlockedExchange(g_shm_now, (LONG)now);
    if (g_shm_total)
        InterlockedExchange(g_shm_total, (LONG)total);

    if (owner && IsWindow(owner))
    {
        while (PeekMessageA(&msg, owner, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE))
        {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
            {
                InterlockedExchange(&g_enc.abort_req, 1);
            }
        }
        if (PeekMessageA(&msg, owner, WM_PAINT, WM_PAINT, PM_REMOVE))
        {
            DispatchMessageA(&msg);
        }
    }

    tick = GetTickCount();
    if (force_title || (tick - g_enc.last_ui_tick) >= 100)
    {
        g_enc.last_ui_tick = tick;
        if (g_on_progress)
        {
            g_on_progress(owner, now, total);
        }
        else if (owner && IsWindow(owner) && total > 0)
        {
            char title[256];
            wchar_t wtitle[256];
            int pct10 = (int)(((long long)now * 1000LL) / (long long)total);
            if (pct10 < 0)
                pct10 = 0;
            if (pct10 > 1000)
                pct10 = 1000;
            wsprintfA(title, "[%d.%d%%] AviUtl (%d/%d)", pct10 / 10, pct10 % 10, now, total);
            sjis_to_wide_auo(title, wtitle, 256);
            SetWindowTextW(owner, wtitle);
        }
    }
}

// build non-deleted timeline frame map & sample offset table
static BOOL setup_encode_session(HWND owner, EDITP *ep, int *out_s, int *out_e)
{
    int fn, s = 0, e = 0, f, count = 0;

    if (!ep || !editp_is_editing(ep))
        return FALSE;
    fn = editp_get_frame_n(ep);
    if (fn <= 0)
        return FALSE;

    if (!editp_get_select_frame(ep, &s, &e) || s < 0 || e < s || e >= fn)
    {
        s = 0;
        e = fn - 1;
    }

    ZeroMemory(&g_enc, sizeof(g_enc));
    g_enc.tl_frames = (int *)malloc((size_t)(e - s + 1) * sizeof(int));
    if (!g_enc.tl_frames)
        return FALSE;

    for (f = s; f <= e; f++)
    {
        BYTE st = 0;
        if (editp_get_frame_status(ep, f, &st) && (st & EDIT_FRAME_EDIT_FLAG_DELFRAME))
        {
            continue;
        }
        g_enc.tl_frames[count++] = f;
    }
    if (count <= 0)
    {
        free(g_enc.tl_frames);
        g_enc.tl_frames = NULL;
        return FALSE;
    }

    g_enc.aud_sample_ofs = (int *)malloc((size_t)(count + 1) * sizeof(int));
    if (!g_enc.aud_sample_ofs)
    {
        free(g_enc.tl_frames);
        g_enc.tl_frames = NULL;
        return FALSE;
    }
    g_enc.aud_sample_ofs[0] = 0;
    for (f = 0; f < count; f++)
    {
        int smp = editp_read_audio_frame(ep, g_enc.tl_frames[f], NULL);
        if (smp < 0)
            smp = 0;
        g_enc.aud_sample_ofs[f + 1] = g_enc.aud_sample_ofs[f] + smp;
    }

    g_enc.active = TRUE;
    g_enc.abort_req = 0;
    g_enc.owner = owner;
    g_enc.ep = ep;
    g_enc.select_s = s;
    g_enc.select_e = e;
    g_enc.out_n = count;
    g_enc.total_audio_samples = g_enc.aud_sample_ofs[count];
    g_enc.audio_rate = editp_get_audio_rate(ep) > 0 ? editp_get_audio_rate(ep) : 44100;
    g_enc.audio_ch = editp_get_audio_ch(ep) > 0 ? editp_get_audio_ch(ep) : 2;
    g_enc.cached_aud_k = -1;
    g_enc.last_ui_tick = 0;

    if (out_s)
        *out_s = s;
    if (out_e)
        *out_e = e;
    return TRUE;
}

static void teardown_encode_session(void)
{
    free(g_enc.tl_frames);
    free(g_enc.aud_sample_ofs);
    free(g_enc.vid_buf);
    free(g_enc.aud_out_buf);
    free(g_enc.cached_aud_pcm);
    ZeroMemory(&g_enc, sizeof(g_enc));
    g_enc.cached_aud_k = -1;
}

// 0x0042b760, OUTPUT_INFO callbacks

static void *__cdecl auo_cb_get_video_ex(int frame, DWORD format)
{
    int tl_f, w = 0, h = 0, mw = 0, mh = 0;
    size_t need;

    if (!g_enc.active || !g_enc.ep || frame < 0 || frame >= g_enc.out_n)
    {
        return NULL;
    }
    tl_f = g_enc.tl_frames[frame];
    if (!plugin_manager_render_video_frame(g_enc.ep, tl_f))
    {
        editp_load_frame(g_enc.ep, tl_f);
    }
    editp_get_dimensions(g_enc.ep, &w, &h, &mw, &mh);
    if (w <= 0 || h <= 0)
        return NULL;

    need = yc48_format_frame_size(w, h, format);
    if (need == 0)
        return NULL;
    if (need > g_enc.vid_buf_cap || !g_enc.vid_buf)
    {
        unsigned char *nb = (unsigned char *)realloc(g_enc.vid_buf, need);
        if (!nb)
            return NULL;
        g_enc.vid_buf = nb;
        g_enc.vid_buf_cap = need;
    }
    if (!yc48_convert_to_format(g_enc.vid_buf, editp_get_ycp_edit(g_enc.ep), mw, w, h, format))
    {
        return NULL;
    }
    return g_enc.vid_buf;
}

static void *__cdecl auo_cb_get_video(int frame)
{
    return auo_cb_get_video_ex(frame, 0);
}

static void *__cdecl auo_cb_get_audio(int start, int length, int *readed)
{
    int ch, lo, hi, k, copied = 0;
    size_t need_shorts;

    if (readed)
        *readed = 0;
    if (!g_enc.active || !g_enc.ep)
        return NULL;

    ch = g_enc.audio_ch > 0 ? g_enc.audio_ch : 2;
    if (start < 0 || length <= 0 || start >= g_enc.total_audio_samples)
    {
        if (!g_enc.aud_out_buf)
        {
            g_enc.aud_out_buf = (short *)calloc(64, sizeof(short));
            g_enc.aud_out_shorts_cap = 64;
        }
        return g_enc.aud_out_buf;
    }
    if (start + length > g_enc.total_audio_samples)
    {
        length = g_enc.total_audio_samples - start;
    }

    need_shorts = (size_t)length * (size_t)ch;
    if (need_shorts > g_enc.aud_out_shorts_cap || !g_enc.aud_out_buf)
    {
        short *nb = (short *)realloc(g_enc.aud_out_buf, (need_shorts + 64) * sizeof(short));
        if (!nb)
            return NULL;
        g_enc.aud_out_buf = nb;
        g_enc.aud_out_shorts_cap = need_shorts + 64;
    }

    // binary search aud_sample_ofs[0..out_n] for first frame k with ofs[k+1] > start
    lo = 0;
    hi = g_enc.out_n - 1;
    while (lo < hi)
    {
        int mid = lo + (hi - lo) / 2;
        if (g_enc.aud_sample_ofs[mid + 1] <= start)
            lo = mid + 1;
        else
            hi = mid;
    }
    k = lo;

    while (copied < length && k < g_enc.out_n)
    {
        int f_s0 = g_enc.aud_sample_ofs[k];
        int f_s1 = g_enc.aud_sample_ofs[k + 1];
        int f_len = f_s1 - f_s0;
        if (f_len > 0)
        {
            int in_off, avail, take;
            if (g_enc.cached_aud_k != k)
            {
                int req_sh = (f_len + 128) * ch;
                int got;
                if (req_sh > g_enc.cached_aud_shorts_cap || !g_enc.cached_aud_pcm)
                {
                    short *cb = (short *)realloc(g_enc.cached_aud_pcm,
                                                 (size_t)req_sh * sizeof(short));
                    if (!cb)
                        break;
                    g_enc.cached_aud_pcm = cb;
                    g_enc.cached_aud_shorts_cap = req_sh;
                }
                memset(g_enc.cached_aud_pcm, 0, (size_t)f_len * (size_t)ch * sizeof(short));
                got = plugin_manager_render_audio_frame(
                    g_enc.ep, g_enc.tl_frames[k], g_enc.cached_aud_pcm, FALSE);
                if (got < f_len && got >= 0)
                {
                    memset(g_enc.cached_aud_pcm + (size_t)got * (size_t)ch, 0,
                           (size_t)(f_len - got) * (size_t)ch * sizeof(short));
                }
                g_enc.cached_aud_k = k;
            }
            in_off = (start + copied) - f_s0;
            avail = f_len - in_off;
            take = (length - copied < avail) ? (length - copied) : avail;
            if (take > 0)
            {
                memcpy(g_enc.aud_out_buf + (size_t)copied * (size_t)ch,
                       g_enc.cached_aud_pcm + (size_t)in_off * (size_t)ch,
                       (size_t)take * (size_t)ch * sizeof(short));
                copied += take;
            }
        }
        k++;
    }

    if (readed)
        *readed = copied;
    return g_enc.aud_out_buf;
}

static BOOL __cdecl auo_cb_is_abort(void)
{
    if (!g_enc.active)
        return TRUE;
    if (g_shm_abort && *g_shm_abort != 0)
    {
        InterlockedExchange(&g_enc.abort_req, 1);
    }
    if (g_enc.owner && IsWindow(g_enc.owner))
    {
        MSG msg;
        while (PeekMessageA(&msg, g_enc.owner, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE))
        {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
            {
                InterlockedExchange(&g_enc.abort_req, 1);
            }
        }
    }
    return g_enc.abort_req != 0;
}

static BOOL __cdecl auo_cb_rest_time_disp(int now, int total)
{
    if (!g_enc.active)
        return FALSE;
    pump_encode_messages_and_progress(g_enc.owner, now, total, FALSE);
    return TRUE;
}

static int __cdecl auo_cb_get_flag(int frame)
{
    BYTE st = 0;
    int flags = 0;
    int tl_f;

    if (!g_enc.active || !g_enc.ep || frame < 0 || frame >= g_enc.out_n)
    {
        return 0;
    }
    tl_f = g_enc.tl_frames[frame];
    if (editp_get_frame_status(g_enc.ep, tl_f, &st))
    {
        if (st & EDIT_FRAME_EDIT_FLAG_KEYFRAME)
            flags |= OUTPUT_INFO_FRAME_FLAG_KEYFRAME;
        if (st & EDIT_FRAME_EDIT_FLAG_NULLFRAME)
            flags |= OUTPUT_INFO_FRAME_FLAG_COPYFRAME;
    }
    return flags;
}

static BOOL __cdecl auo_cb_update_preview(void)
{
    if (!g_enc.active)
        return FALSE;
    if (g_on_preview_update && g_enc.owner)
    {
        g_on_preview_update(g_enc.owner);
    }
    return TRUE;
}

static BOOL cb_exfunc_edit_output(EDITP *ep, const char *file, int flag, const char *type)
{
    HWND owner = ep ? editp_get_main_hwnd(ep) : g_host_hwnd;
    if (!ep || !file || !file[0] || !editp_is_editing(ep))
        return FALSE;

    if (!type || !type[0])
    {
        if ((flag & (OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO)) == OUTPUT_INFO_FLAG_AUDIO)
        {
            return video_export_wav_file(owner, ep, file);
        }
        return video_export_avi_file(owner, ep, file,
                                     flag ? flag : (OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO));
    }
    else
    {
        int idx = video_export_find_auo_by_name(type);
        if (idx < 0)
        {
            dbg_warn("EXFUNC edit_output: output plugin '%s' not found", type);
            return FALSE;
        }
        return video_export_auo_file(owner, ep, idx, file,
                                     flag ? flag : (OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO));
    }
}

static BOOL inspect_and_register_auo(const char *full_path, const char *file_name,
                                     BOOL *saw_32bit_on_x64)
{
    HMODULE mod;
    GET_OUTPUT_PLUGIN_TABLE_FN fn;
    OUTPUT_PLUGIN_TABLE *tbl;
    LocalAuoEntry *e;
    int k;

    if (g_local_auo_count >= AUO_MAX_PLUGINS)
        return FALSE;

    // skip duplicate filenames across <exe_dir> and <exe_dir>\plugins
    for (k = 0; k < g_local_auo_count; k++)
    {
        const char *base = path_basename_dbcs(g_local_auos[k].path);
        if (lstrcmpiA(base, file_name) == 0)
        {
            return FALSE;
        }
    }

    mod = LoadLibraryA(full_path);
    if (!mod)
    {
        DWORD err = GetLastError();
        if (err == ERROR_BAD_EXE_FORMAT && saw_32bit_on_x64)
        {
            *saw_32bit_on_x64 = TRUE;
            dbg_plugin("Detected 32-bit output plugin '%s' in 64-bit host -> will bridge via reaviutl_bridge32.exe",
                       file_name);
        }
        else
        {
            dbg_warn("Failed to load output plugin '%s' (err=%lu)", full_path, (unsigned long)err);
        }
        return FALSE;
    }

    fn = (GET_OUTPUT_PLUGIN_TABLE_FN)(void *)GetProcAddress(mod, "GetOutputPluginTable");
    if (!fn)
    {
        fn = (GET_OUTPUT_PLUGIN_TABLE_FN)(void *)GetProcAddress(mod, "_GetOutputPluginTable");
    }
    if (!fn)
    {
        FreeLibrary(mod);
        return FALSE;
    }

    tbl = fn();
    if (!tbl)
    {
        FreeLibrary(mod);
        return FALSE;
    }

    e = &g_local_auos[g_local_auo_count++];
    ZeroMemory(e, sizeof(*e));
    lstrcpynA(e->path, full_path, MAX_PATH);
    lstrcpynA(e->name, (tbl->name && tbl->name[0]) ? tbl->name : file_name, sizeof(e->name));
    copy_double_nul_filter(e->filefilter, sizeof(e->filefilter), tbl->filefilter);
    lstrcpynA(e->information,
              (tbl->information && tbl->information[0]) ? tbl->information : e->name,
              sizeof(e->information));
    e->flag = tbl->flag | (tbl->func_config ? OUTPUT_PLUGIN_TABLE_FLAG_HAS_CONFIG : 0);
    e->has_config = (tbl->func_config != NULL);

    // match 0x0042ac80: release probe HMODULE now; lazy-load & func_init() on first use
    FreeLibrary(mod);
    dbg_plugin("Discovered output plugin [%d] '%s' (%s)",
               g_local_auo_count - 1, e->name, file_name);
    return TRUE;
}

static void scan_auo_dir_single(const char *dir, BOOL *saw_32bit_on_x64)
{
    WIN32_FIND_DATAA fd;
    HANDLE hFind;
    char pattern[MAX_PATH];

    if (!dir || !dir[0])
        return;
    wsprintfA(pattern, "%s\\*.auo", dir);
    hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        char full_path[MAX_PATH];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (g_local_auo_count >= AUO_MAX_PLUGINS)
            break;
        wsprintfA(full_path, "%s\\%s", dir, fd.cFileName);
        inspect_and_register_auo(full_path, fd.cFileName, saw_32bit_on_x64);
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
}

int video_export_scan_auo(const char *plugin_dir, HWND main_hwnd, EDITP *ep)
{
    char parent_dir[MAX_PATH] = {0};
    BOOL has_parent = FALSE;
    BOOL saw_32bit = FALSE;

    g_host_hwnd = main_hwnd;
    exfunc_set_edit_output_callback(cb_exfunc_edit_output);

    if (!plugin_dir || !plugin_dir[0])
    {
        return video_export_get_auo_count();
    }

    {
        const char *slash = strrchr(plugin_dir, '\\');
        if (slash && lstrcmpiA(slash + 1, "plugins") == 0 && (slash - plugin_dir) > 0)
        {
            int plen = (int)(slash - plugin_dir);
            if (plen < MAX_PATH)
            {
                memcpy(parent_dir, plugin_dir, (size_t)plen);
                parent_dir[plen] = '\0';
                has_parent = TRUE;
            }
        }
    }

    if (has_parent)
    {
        scan_auo_dir_single(parent_dir, &saw_32bit);
    }
    scan_auo_dir_single(plugin_dir, &saw_32bit);

#if REAVIUTL_X64
    if (saw_32bit || plugin_bridge_is_active())
    {
        plugin_bridge_scan_auo(plugin_dir, main_hwnd, ep);
    }
#else
    (void)ep;
    (void)saw_32bit;
#endif

    return video_export_get_auo_count();
}

BOOL video_export_load_single_auo(const char *auo_path, HWND main_hwnd, EDITP *ep)
{
    char abs_path[MAX_PATH];
    BOOL saw_32bit = FALSE;

    g_host_hwnd = main_hwnd;
    exfunc_set_edit_output_callback(cb_exfunc_edit_output);

    if (!auo_path || !auo_path[0])
        return FALSE;
    if (GetFullPathNameA(auo_path, MAX_PATH, abs_path, NULL) == 0)
    {
        lstrcpynA(abs_path, auo_path, MAX_PATH);
    }

    if (inspect_and_register_auo(abs_path, path_basename_dbcs(abs_path), &saw_32bit))
    {
        return TRUE;
    }

#if REAVIUTL_X64
    if (saw_32bit)
    {
        return plugin_bridge_load_auo(abs_path, main_hwnd, ep);
    }
#else
    (void)ep;
#endif
    return FALSE;
}

void video_export_shutdown(void)
{
    int i;
    for (i = g_local_auo_count - 1; i >= 0; i--)
    {
        LocalAuoEntry *e = &g_local_auos[i];
        if (e->initialized && e->module)
        {
            safe_auo_exit(e);
            e->initialized = FALSE;
        }
        if (e->module)
        {
            FreeLibrary(e->module);
            e->module = NULL;
        }
    }
    g_local_auo_count = 0;
    g_last_auo_idx = 0;

    if (g_vcodec_valid)
    {
        ICCompressorFree(&g_vcodec);
        ZeroMemory(&g_vcodec, sizeof(g_vcodec));
        g_vcodec_valid = FALSE;
    }
    if (g_acodec_wfx)
    {
        free(g_acodec_wfx);
        g_acodec_wfx = NULL;
        g_acodec_max_sz = 0;
        g_acodec_valid = FALSE;
    }
    teardown_encode_session();
}

void video_export_set_ui_callbacks(VideoExportPreviewUpdateFn on_preview_update,
                                   VideoExportProgressFn on_progress)
{
    g_on_preview_update = on_preview_update;
    g_on_progress = on_progress;
}

void video_export_set_bridge_telemetry(volatile LONG *abort_flag,
                                       volatile LONG *now_val,
                                       volatile LONG *total_val)
{
    g_shm_abort = abort_flag;
    g_shm_now = now_val;
    g_shm_total = total_val;
}

int video_export_get_auo_count(void)
{
    int total = g_local_auo_count + plugin_bridge_get_auo_count();
    if (total > AUO_MAX_PLUGINS)
        total = AUO_MAX_PLUGINS;
    return total;
}

BOOL video_export_get_auo_summary(int idx, AuoPluginSummary *out_summary)
{
    if (!out_summary || idx < 0)
        return FALSE;
    ZeroMemory(out_summary, sizeof(*out_summary));

    if (idx < g_local_auo_count)
    {
        const LocalAuoEntry *e = &g_local_auos[idx];
        lstrcpynA(out_summary->name, e->name, sizeof(out_summary->name));
        memcpy(out_summary->filefilter, e->filefilter, sizeof(out_summary->filefilter));
        lstrcpynA(out_summary->information, e->information, sizeof(out_summary->information));
        lstrcpynA(out_summary->path, e->path, sizeof(out_summary->path));
        out_summary->flag = e->flag;
        out_summary->has_config = e->has_config;
        out_summary->bridged_x86 = FALSE;
        return TRUE;
    }

    {
        int bidx = idx - g_local_auo_count;
        BridgeAuoInfo bi;
        if (!plugin_bridge_get_auo_info(bidx, &bi))
            return FALSE;
        lstrcpynA(out_summary->name, bi.name, sizeof(out_summary->name));
        memcpy(out_summary->filefilter, bi.filefilter, sizeof(out_summary->filefilter));
        lstrcpynA(out_summary->information, bi.information, sizeof(out_summary->information));
        lstrcpynA(out_summary->path, bi.path, sizeof(out_summary->path));
        out_summary->flag = bi.flag;
        out_summary->has_config = (bi.has_config != 0);
        out_summary->bridged_x86 = TRUE;
        return TRUE;
    }
}

const char *video_export_get_auo_name(int idx)
{
    static char s_bridged_name[128];
    if (idx < 0)
        return NULL;
    if (idx < g_local_auo_count)
    {
        return g_local_auos[idx].name;
    }
    {
        BridgeAuoInfo bi;
        if (!plugin_bridge_get_auo_info(idx - g_local_auo_count, &bi))
            return NULL;
        lstrcpynA(s_bridged_name, bi.name, sizeof(s_bridged_name));
        return s_bridged_name;
    }
}

int video_export_find_auo_by_name(const char *name)
{
    int i, n = video_export_get_auo_count();
    if (!name || !name[0])
        return -1;
    for (i = 0; i < n; i++)
    {
        AuoPluginSummary sum;
        if (video_export_get_auo_summary(i, &sum) && lstrcmpA(sum.name, name) == 0)
        {
            return i;
        }
    }
    return -1;
}

void video_export_populate_menu(HMENU bar, const wchar_t *none_label)
{
    int i, count = video_export_get_auo_count();
    if (!bar)
        return;

    if (count > 0)
    {
        for (i = 0; i < count; i++)
        {
            AuoPluginSummary sum;
            wchar_t wlabel[300];
            if (!video_export_get_auo_summary(i, &sum))
                continue;
            sjis_to_wide_auo(sum.name, wlabel, 256);
            InsertMenuW(bar, AUO_MENU_PLACEHOLDER_ID, MF_BYCOMMAND | MF_STRING,
                        (UINT_PTR)(AUO_CMD_BASE + i), wlabel);
        }
        DeleteMenu(bar, AUO_MENU_PLACEHOLDER_ID, MF_BYCOMMAND);
    }
    else
    {
        ModifyMenuW(bar, AUO_MENU_PLACEHOLDER_ID, MF_BYCOMMAND | MF_STRING | MF_GRAYED,
                    AUO_MENU_PLACEHOLDER_ID,
                    (none_label && none_label[0]) ? none_label : L"\x306a\x3057");
    }
}

BOOL video_export_choose_video_codec(HWND owner, EDITP *ep)
{
    BITMAPINFOHEADER bih;
    int w = 640, h = 480, mw = 0, mh = 0;

    if (ep && editp_is_editing(ep))
    {
        editp_get_dimensions(ep, &w, &h, &mw, &mh);
    }
    if (w <= 0)
        w = 640;
    if (h <= 0)
        h = 480;

    ZeroMemory(&bih, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = (DWORD)(((w * 3 + 3) & ~3) * h);

    if (!g_vcodec_valid)
    {
        ZeroMemory(&g_vcodec, sizeof(g_vcodec));
        g_vcodec.cbSize = sizeof(g_vcodec);
    }
    if (!ICCompressorChoose(owner, ICMF_CHOOSE_DATARATE | ICMF_CHOOSE_KEYFRAME,
                            &bih, NULL, &g_vcodec, "Video Compression"))
    {
        return FALSE;
    }
    g_vcodec_valid = TRUE;
    dbg_info("Selected VFW video compressor: fccHandler=0x%08X lQ=%ld lKey=%ld",
             (unsigned)g_vcodec.fccHandler, (long)g_vcodec.lQ, (long)g_vcodec.lKey);
    return TRUE;
}

BOOL video_export_choose_audio_codec(HWND owner, EDITP *ep)
{
    ACMFORMATCHOOSEA afc;
    DWORD max_sz = 0;
    int rate = 44100, ch = 2;

    if (ep && editp_is_editing(ep))
    {
        if (editp_get_audio_rate(ep) > 0)
            rate = editp_get_audio_rate(ep);
        if (editp_get_audio_ch(ep) > 0)
            ch = editp_get_audio_ch(ep);
    }

    if (acmMetrics(NULL, ACM_METRIC_MAX_SIZE_FORMAT, &max_sz) != MMSYSERR_NOERROR ||
        max_sz < sizeof(WAVEFORMATEX) + 64)
    {
        max_sz = sizeof(WAVEFORMATEX) + 128;
    }
    if (!g_acodec_wfx || max_sz > g_acodec_max_sz)
    {
        WAVEFORMATEX *nw = (WAVEFORMATEX *)realloc(g_acodec_wfx, max_sz);
        if (!nw)
            return FALSE;
        if (!g_acodec_wfx)
        {
            ZeroMemory(nw, max_sz);
            nw->wFormatTag = WAVE_FORMAT_PCM;
            nw->nChannels = (WORD)ch;
            nw->nSamplesPerSec = (DWORD)rate;
            nw->wBitsPerSample = 16;
            nw->nBlockAlign = (WORD)(ch * 2);
            nw->nAvgBytesPerSec = (DWORD)(rate * ch * 2);
        }
        g_acodec_wfx = nw;
        g_acodec_max_sz = max_sz;
    }

    ZeroMemory(&afc, sizeof(afc));
    afc.cbStruct = sizeof(afc);
    afc.hwndOwner = owner;
    afc.pwfx = g_acodec_wfx;
    afc.cbwfx = g_acodec_max_sz;
    afc.pszTitle = "Audio Compression";
    afc.fdwStyle = ACMFORMATCHOOSE_STYLEF_INITTOWFXSTRUCT;

    if (acmFormatChooseA(&afc) != MMSYSERR_NOERROR)
    {
        return FALSE;
    }
    g_acodec_valid = TRUE;
    dbg_info("Selected ACM audio format: tag=0x%04X %lu Hz %u ch (%lu B/s)",
             (unsigned)g_acodec_wfx->wFormatTag,
             (unsigned long)g_acodec_wfx->nSamplesPerSec,
             (unsigned)g_acodec_wfx->nChannels,
             (unsigned long)g_acodec_wfx->nAvgBytesPerSec);
    return TRUE;
}

BOOL video_export_avi_file(HWND owner, EDITP *ep, const char *save_path, int out_flags)
{
    char abs_path[MAX_PATH];
    int s = 0, e = 0, saved_frame, k;
    int w = 0, h = 0, mw = 0, mh = 0, fps_r = 30, fps_s = 1;
    int dib_stride, dib_bytes;
    BOOL include_audio, ok = FALSE;
    PAVIFILE pfile = NULL;
    PAVISTREAM pvid = NULL;
    PAVISTREAM pvid_cs = NULL;
    PAVISTREAM pvid_wr = NULL;
    PAVISTREAM paud = NULL;
    AVISTREAMINFOA vsi;
    BITMAPINFOHEADER bih;
    unsigned char *dib_buf = NULL;
    void *codec_par = NULL;
    short *pcm_buf = NULL;
    int pcm_cap = 0;
    HACMSTREAM has = NULL;
    ACMSTREAMHEADER ash;
    BOOL ash_prep = FALSE;
    unsigned char *acm_src = NULL;
    unsigned char *acm_dst = NULL;
    DWORD acm_dst_cap = 0;
    LONG aud_pos = 0;

    if (!ep || !save_path || !save_path[0] || !editp_is_editing(ep) || g_enc.active)
    {
        return FALSE;
    }
    if (GetFullPathNameA(save_path, MAX_PATH, abs_path, NULL) == 0)
    {
        lstrcpynA(abs_path, save_path, MAX_PATH);
    }
    if (is_overwriting_open_edit_file(owner, ep, abs_path))
    {
        return FALSE;
    }

    if ((out_flags & (OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO)) == 0)
    {
        out_flags |= OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO;
    }
    include_audio = ((out_flags & OUTPUT_INFO_FLAG_AUDIO) != 0) &&
                    (editp_has_audio(ep) || plugin_manager_has_active_audio_filter());

    editp_get_dimensions(ep, &w, &h, &mw, &mh);
    editp_get_fps(ep, &fps_r, &fps_s);
    if (w <= 0 || h <= 0)
        return FALSE;
    if (fps_r <= 0)
        fps_r = 30;
    if (fps_s <= 0)
        fps_s = 1;

    if (!setup_encode_session(owner, ep, &s, &e))
    {
        return FALSE;
    }

    dib_stride = (w * 3 + 3) & ~3;
    dib_bytes = dib_stride * h;
    dib_buf = (unsigned char *)malloc((size_t)dib_bytes);
    if (!dib_buf)
    {
        teardown_encode_session();
        return FALSE;
    }

    saved_frame = editp_get_frame(ep);
    editp_set_output_path(ep, abs_path);
    plugin_manager_notify_update(ep);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    plugin_manager_notify_save_start(ep, s, e,
                                     OUTPUT_INFO_FLAG_VIDEO | (include_audio ? OUTPUT_INFO_FLAG_AUDIO : 0));

    // delete existing target file first so AVIFileOpenA(OF_CREATE|OF_WRITE) starts fresh
    DeleteFileA(abs_path);
    AVIFileInit();

    if (AVIFileOpenA(&pfile, abs_path, OF_CREATE | OF_WRITE, NULL) != 0 || !pfile)
    {
        dbg_error("AVIFileOpenA failed for '%s'", abs_path);
        goto cleanup;
    }

    // create video stream ('vids')
    ZeroMemory(&bih, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = (DWORD)dib_bytes;

    ZeroMemory(&vsi, sizeof(vsi));
    vsi.fccType = streamtypeVIDEO;
    vsi.fccHandler = (g_vcodec_valid && g_vcodec.fccHandler != 0)
                         ? g_vcodec.fccHandler
                         : mmioFOURCC('D', 'I', 'B', ' ');
    vsi.dwScale = (DWORD)fps_s;
    vsi.dwRate = (DWORD)fps_r;
    vsi.dwSuggestedBufferSize = (DWORD)dib_bytes;
    vsi.dwQuality = (DWORD)(g_vcodec_valid ? g_vcodec.lQ : (DWORD)-1);
    SetRect(&vsi.rcFrame, 0, 0, w, h);

    if (AVIFileCreateStreamA(pfile, &pvid, &vsi) != 0 || !pvid)
    {
        dbg_error("AVIFileCreateStreamA(vids) failed");
        goto cleanup;
    }

    pvid_wr = pvid;
    if (g_vcodec_valid && g_vcodec.fccHandler != 0 &&
        g_vcodec.fccHandler != mmioFOURCC('D', 'I', 'B', ' '))
    {
        AVICOMPRESSOPTIONS vopts;
        ZeroMemory(&vopts, sizeof(vopts));
        vopts.fccType = streamtypeVIDEO;
        vopts.fccHandler = g_vcodec.fccHandler;
        vopts.dwKeyFrameEvery = (DWORD)(g_vcodec.lKey > 0 ? g_vcodec.lKey : 15);
        vopts.dwQuality = (DWORD)g_vcodec.lQ;
        vopts.dwBytesPerSecond = (DWORD)(g_vcodec.lDataRate > 0 ? g_vcodec.lDataRate * 1024 : 0);
        vopts.dwFlags = AVICOMPRESSF_VALID |
                        (g_vcodec.lKey > 0 ? AVICOMPRESSF_KEYFRAMES : 0) |
                        (g_vcodec.lDataRate > 0 ? AVICOMPRESSF_DATARATE : 0);
        if (g_vcodec.hic)
        {
            LRESULT st_sz = ICGetStateSize(g_vcodec.hic);
            if (st_sz > 0)
            {
                codec_par = malloc((size_t)st_sz);
                if (codec_par && ICGetState(g_vcodec.hic, codec_par, (DWORD_PTR)st_sz) == ICERR_OK)
                {
                    vopts.lpParms = codec_par;
                    vopts.cbParms = (DWORD)st_sz;
                }
            }
        }
        if (AVIMakeCompressedStream(&pvid_cs, pvid, &vopts, NULL) == AVIERR_OK && pvid_cs)
        {
            pvid_wr = pvid_cs;
        }
        else
        {
            dbg_warn("AVIMakeCompressedStream failed for fcc=0x%08X -> writing uncompressed BGR24",
                     (unsigned)g_vcodec.fccHandler);
        }
    }

    if (AVIStreamSetFormat(pvid_wr, 0, &bih, (LONG)sizeof(bih)) != 0)
    {
        dbg_error("AVIStreamSetFormat(vids) failed");
        goto cleanup;
    }

    // create audio stream ('auds') if project has audio
    if (include_audio && g_enc.total_audio_samples > 0)
    {
        AVISTREAMINFOA asi;
        WAVEFORMATEX pcm_wfx;
        int ar = g_enc.audio_rate;
        int ch = g_enc.audio_ch;

        ZeroMemory(&pcm_wfx, sizeof(pcm_wfx));
        pcm_wfx.wFormatTag = WAVE_FORMAT_PCM;
        pcm_wfx.nChannels = (WORD)ch;
        pcm_wfx.nSamplesPerSec = (DWORD)ar;
        pcm_wfx.wBitsPerSample = 16;
        pcm_wfx.nBlockAlign = (WORD)(ch * 2);
        pcm_wfx.nAvgBytesPerSec = (DWORD)(ar * ch * 2);

        ZeroMemory(&asi, sizeof(asi));
        asi.fccType = streamtypeAUDIO;
        asi.dwQuality = (DWORD)-1;

        if (g_acodec_valid && g_acodec_wfx &&
            g_acodec_wfx->wFormatTag != WAVE_FORMAT_PCM &&
            acmStreamOpen(&has, NULL, &pcm_wfx, g_acodec_wfx, NULL, 0, 0,
                          ACM_STREAMOPENF_NONREALTIME) == MMSYSERR_NOERROR &&
            has)
        {
            LONG fmt_len = (LONG)(sizeof(WAVEFORMATEX) + g_acodec_wfx->cbSize);
            DWORD max_in_bytes = (DWORD)(16384 * ch * 2);
            asi.dwScale = g_acodec_wfx->nBlockAlign > 0 ? g_acodec_wfx->nBlockAlign : 1;
            asi.dwRate = g_acodec_wfx->nAvgBytesPerSec > 0 ? g_acodec_wfx->nAvgBytesPerSec : (DWORD)(ar * ch * 2);
            asi.dwSampleSize = g_acodec_wfx->nBlockAlign;
            if (AVIFileCreateStreamA(pfile, &paud, &asi) == 0 && paud &&
                AVIStreamSetFormat(paud, 0, g_acodec_wfx, fmt_len) == 0)
            {
                acmStreamSize(has, max_in_bytes, &acm_dst_cap, ACM_STREAMSIZEF_SOURCE);
                if (acm_dst_cap < 16384)
                    acm_dst_cap = 16384;
                acm_src = (unsigned char *)malloc(max_in_bytes);
                acm_dst = (unsigned char *)malloc(acm_dst_cap);
                if (acm_src && acm_dst)
                {
                    ZeroMemory(&ash, sizeof(ash));
                    ash.cbStruct = sizeof(ash);
                    ash.pbSrc = acm_src;
                    ash.cbSrcLength = max_in_bytes;
                    ash.pbDst = acm_dst;
                    ash.cbDstLength = acm_dst_cap;
                    if (acmStreamPrepareHeader(has, &ash, 0) == MMSYSERR_NOERROR)
                    {
                        ash_prep = TRUE;
                    }
                }
            }
            if (!ash_prep)
            {
                if (paud)
                {
                    AVIStreamRelease(paud);
                    paud = NULL;
                }
                acmStreamClose(has, 0);
                has = NULL;
            }
        }

        if (!paud)
        {
            asi.dwScale = (DWORD)pcm_wfx.nBlockAlign;
            asi.dwRate = pcm_wfx.nAvgBytesPerSec;
            asi.dwSampleSize = (DWORD)pcm_wfx.nBlockAlign;
            asi.dwSuggestedBufferSize = (DWORD)(ar * ch * 2);
            if (AVIFileCreateStreamA(pfile, &paud, &asi) == 0 && paud)
            {
                if (AVIStreamSetFormat(paud, 0, &pcm_wfx, (LONG)sizeof(pcm_wfx)) != 0)
                {
                    AVIStreamRelease(paud);
                    paud = NULL;
                }
            }
        }
    }

    // frame-by-frame render + interleave loop
    for (k = 0; k < g_enc.out_n; k++)
    {
        int tl_f = g_enc.tl_frames[k];
        BYTE st = 0;

        pump_encode_messages_and_progress(owner, k, g_enc.out_n, k == 0);
        if (g_enc.abort_req)
        {
            dbg_warn("AVI export aborted by user at frame %d/%d", k, g_enc.out_n);
            break;
        }

        editp_get_frame_status(ep, tl_f, &st);
        if ((st & EDIT_FRAME_EDIT_FLAG_NULLFRAME) && k > 0)
        {
            // aviutl [copy]/NULLFRAME: emit 0-byte drop sample in AVI stream
            if (AVIStreamWrite(pvid_wr, (LONG)k, 1, NULL, 0, 0, NULL, NULL) != 0)
            {
                dbg_error("AVIStreamWrite(drop frame %d) failed", k);
                goto cleanup;
            }
        }
        else
        {
            int cw = 0, ch_h = 0, cmw = 0, cmh = 0;
            DWORD vflags = (pvid_wr == pvid || (st & EDIT_FRAME_EDIT_FLAG_KEYFRAME))
                               ? AVIIF_KEYFRAME
                               : 0;
            if (!plugin_manager_render_video_frame(ep, tl_f))
            {
                editp_load_frame(ep, tl_f);
            }
            editp_get_dimensions(ep, &cw, &ch_h, &cmw, &cmh);
            memset(dib_buf, 0, (size_t)dib_bytes);
            yc2rgb_dib24(dib_buf, dib_stride, editp_get_ycp_edit(ep),
                         cmw, cw < w ? cw : w, ch_h < h ? ch_h : h, TRUE);
            if (AVIStreamWrite(pvid_wr, (LONG)k, 1, dib_buf, (LONG)dib_bytes,
                               vflags, NULL, NULL) != 0)
            {
                dbg_error("AVIStreamWrite(video frame %d) failed", k);
                goto cleanup;
            }
        }

        if (g_on_preview_update && owner)
        {
            g_on_preview_update(owner);
        }

        if (paud)
        {
            int need_smp = g_enc.aud_sample_ofs[k + 1] - g_enc.aud_sample_ofs[k];
            int ach = g_enc.audio_ch;
            if (need_smp > 0)
            {
                int got;
                if ((need_smp + 128) * ach > pcm_cap || !pcm_buf)
                {
                    int ncap = (need_smp + 256) * ach;
                    short *nb = (short *)realloc(pcm_buf, (size_t)ncap * sizeof(short));
                    if (!nb)
                        goto cleanup;
                    pcm_buf = nb;
                    pcm_cap = ncap;
                }
                memset(pcm_buf, 0, (size_t)need_smp * (size_t)ach * sizeof(short));
                got = plugin_manager_render_audio_frame(ep, tl_f, pcm_buf, FALSE);
                if (got <= 0)
                    got = need_smp;

                if (has && ash_prep)
                {
                    DWORD in_bytes = (DWORD)(got * ach * 2);
                    if (in_bytes <= (DWORD)(16384 * ach * 2))
                    {
                        ash.cbSrcLength = in_bytes;
                        ash.cbSrcLengthUsed = 0;
                        ash.cbDstLengthUsed = 0;
                        if (acmStreamConvert(has, &ash, ACM_STREAMCONVERTF_BLOCKALIGN) == MMSYSERR_NOERROR &&
                            ash.cbDstLengthUsed > 0)
                        {
                            LONG blk_a = g_acodec_wfx->nBlockAlign > 0 ? g_acodec_wfx->nBlockAlign : 1;
                            LONG nblocks = (LONG)(ash.cbDstLengthUsed / (DWORD)blk_a);
                            if (nblocks < 1)
                                nblocks = 1;
                            AVIStreamWrite(paud, aud_pos, nblocks, acm_dst,
                                           (LONG)ash.cbDstLengthUsed, 0, NULL, NULL);
                            aud_pos += nblocks;
                        }
                    }
                }
                else
                {
                    LONG bytes = (LONG)(got * ach * 2);
                    if (AVIStreamWrite(paud, aud_pos, (LONG)got, pcm_buf, bytes,
                                       AVIIF_KEYFRAME, NULL, NULL) != 0)
                    {
                        dbg_error("AVIStreamWrite(audio frame %d) failed", k);
                        goto cleanup;
                    }
                    aud_pos += (LONG)got;
                }
            }
        }
    }

    if (paud && has && ash_prep && !g_enc.abort_req)
    {
        ash.cbSrcLength = 0;
        ash.cbSrcLengthUsed = 0;
        ash.cbDstLengthUsed = 0;
        if (acmStreamConvert(has, &ash, ACM_STREAMCONVERTF_END) == MMSYSERR_NOERROR &&
            ash.cbDstLengthUsed > 0)
        {
            LONG blk_a = g_acodec_wfx->nBlockAlign > 0 ? g_acodec_wfx->nBlockAlign : 1;
            LONG nblocks = (LONG)(ash.cbDstLengthUsed / (DWORD)blk_a);
            if (nblocks < 1)
                nblocks = 1;
            AVIStreamWrite(paud, aud_pos, nblocks, acm_dst,
                           (LONG)ash.cbDstLengthUsed, 0, NULL, NULL);
        }
    }

    pump_encode_messages_and_progress(owner, g_enc.out_n, g_enc.out_n, TRUE);
    ok = (g_enc.abort_req == 0);
    if (ok)
    {
        dbg_info("Exported AVI '%s' (%dx%d, %d frames, %d/%d fps%s)",
                 abs_path, w, h, g_enc.out_n, fps_r, fps_s,
                 paud ? ", with audio" : "");
    }

cleanup:
    if (has)
    {
        if (ash_prep)
        {
            ash.cbSrcLength = (DWORD)(16384 * g_enc.audio_ch * 2);
            ash.cbDstLength = acm_dst_cap;
            acmStreamUnprepareHeader(has, &ash, 0);
        }
        acmStreamClose(has, 0);
    }
    free(acm_src);
    free(acm_dst);
    free(pcm_buf);
    free(codec_par);
    free(dib_buf);

    if (pvid_cs)
        AVIStreamRelease(pvid_cs);
    if (pvid)
        AVIStreamRelease(pvid);
    if (paud)
        AVIStreamRelease(paud);
    if (pfile)
        AVIFileRelease(pfile);
    AVIFileExit();

    plugin_manager_notify_save_end(ep,
                                   OUTPUT_INFO_FLAG_VIDEO | (include_audio ? OUTPUT_INFO_FLAG_AUDIO : 0));
    SetThreadExecutionState(ES_CONTINUOUS);
    editp_set_frame(ep, saved_frame);
    plugin_manager_notify_update(ep);
    teardown_encode_session();
    if (ok)
    {
        notify_export_finished(owner, out_flags);
    }
    return ok;
}

BOOL video_export_avi_dialog(HWND owner, EDITP *ep)
{
    OPENFILENAMEA ofn;
    char init_dir[MAX_PATH] = {0};
    char file[MAX_PATH] = {0};
    static const char filter[] =
        "AVI Video (*.avi)\0*.avi\0"
        "All Files (*.*)\0*.*\0";

    if (!ep || !editp_is_editing(ep) || g_enc.active)
        return FALSE;
    expand_default_save_path(ep, "avi", init_dir, sizeof(init_dir), file, sizeof(file));

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = init_dir[0] ? init_dir : NULL;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrDefExt = "avi";
    ofn.Flags = get_export_save_ofn_flags();

    if (!GetSaveFileNameA(&ofn))
        return FALSE;
    if (is_overwriting_open_edit_file(owner, ep, file))
        return FALSE;
    return video_export_avi_file(owner, ep, file,
                                 OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO);
}

BOOL video_export_wav_file(HWND owner, EDITP *ep, const char *save_path)
{
    char abs_path[MAX_PATH];
    HANDLE hf;
    int s = 0, e = 0, k, rate, ch, blk, saved_frame;
    uint32_t data_bytes = 0, riff_sz, fmt_sz = 16, byte_rate, urate;
    uint16_t fmt_tag = 1, wch, wblk, wbits = 16;
    DWORD wr = 0;
    short *pcm_buf = NULL;
    int pcm_cap = 0;
    BOOL ok = FALSE;

    if (!ep || !save_path || !save_path[0] || !editp_is_editing(ep) || g_enc.active)
    {
        return FALSE;
    }
    if (GetFullPathNameA(save_path, MAX_PATH, abs_path, NULL) == 0)
    {
        lstrcpynA(abs_path, save_path, MAX_PATH);
    }
    if (is_overwriting_open_edit_file(owner, ep, abs_path))
    {
        return FALSE;
    }
    if (!setup_encode_session(owner, ep, &s, &e))
    {
        return FALSE;
    }

    hf = CreateFileA(abs_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        teardown_encode_session();
        return FALSE;
    }

    rate = g_enc.audio_rate;
    ch = g_enc.audio_ch;
    wch = (uint16_t)ch;
    blk = ch * 2;
    wblk = (uint16_t)blk;
    byte_rate = (uint32_t)(rate * blk);
    urate = (uint32_t)rate;

    {
        unsigned char hdr44[44] = {0};
        WriteFile(hf, hdr44, 44, &wr, NULL);
    }

    saved_frame = editp_get_frame(ep);
    editp_set_output_path(ep, abs_path);
    plugin_manager_notify_update(ep);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    plugin_manager_notify_save_start(ep, s, e, OUTPUT_INFO_FLAG_AUDIO);

    for (k = 0; k < g_enc.out_n; k++)
    {
        int tl_f = g_enc.tl_frames[k];
        int need_smp = g_enc.aud_sample_ofs[k + 1] - g_enc.aud_sample_ofs[k];
        int got;

        pump_encode_messages_and_progress(owner, k, g_enc.out_n, k == 0);
        if (g_enc.abort_req)
            break;
        if (need_smp <= 0)
            continue;

        if ((need_smp + 128) * ch > pcm_cap || !pcm_buf)
        {
            int ncap = (need_smp + 256) * ch;
            short *nb = (short *)realloc(pcm_buf, (size_t)ncap * sizeof(short));
            if (!nb)
                break;
            pcm_buf = nb;
            pcm_cap = ncap;
        }
        memset(pcm_buf, 0, (size_t)need_smp * (size_t)ch * sizeof(short));
        got = plugin_manager_render_audio_frame(ep, tl_f, pcm_buf, FALSE);
        if (got > 0)
        {
            DWORD chunk = (DWORD)(got * blk);
            WriteFile(hf, pcm_buf, chunk, &wr, NULL);
            data_bytes += chunk;
        }
    }

    free(pcm_buf);
    riff_sz = 36u + data_bytes;
    SetFilePointer(hf, 0, NULL, FILE_BEGIN);
    WriteFile(hf, "RIFF", 4, &wr, NULL);
    WriteFile(hf, &riff_sz, 4, &wr, NULL);
    WriteFile(hf, "WAVEfmt ", 8, &wr, NULL);
    WriteFile(hf, &fmt_sz, 4, &wr, NULL);
    WriteFile(hf, &fmt_tag, 2, &wr, NULL);
    WriteFile(hf, &wch, 2, &wr, NULL);
    WriteFile(hf, &urate, 4, &wr, NULL);
    WriteFile(hf, &byte_rate, 4, &wr, NULL);
    WriteFile(hf, &wblk, 2, &wr, NULL);
    WriteFile(hf, &wbits, 2, &wr, NULL);
    WriteFile(hf, "data", 4, &wr, NULL);
    WriteFile(hf, &data_bytes, 4, &wr, NULL);
    CloseHandle(hf);

    ok = (g_enc.abort_req == 0);
    plugin_manager_notify_save_end(ep, OUTPUT_INFO_FLAG_AUDIO);
    SetThreadExecutionState(ES_CONTINUOUS);
    editp_set_frame(ep, saved_frame);
    plugin_manager_notify_update(ep);
    teardown_encode_session();

    if (ok)
    {
        dbg_info("Exported WAV '%s' (%lu bytes, %d Hz, %d ch)",
                 abs_path, (unsigned long)data_bytes, rate, ch);
        notify_export_finished(owner, 0);
    }
    return ok;
}

BOOL video_export_wav_dialog(HWND owner, EDITP *ep)
{
    OPENFILENAMEA ofn;
    char init_dir[MAX_PATH] = {0};
    char file[MAX_PATH] = {0};
    static const char filter[] =
        "Wave Audio (*.wav)\0*.wav\0"
        "All Files (*.*)\0*.*\0";

    if (!ep || !editp_is_editing(ep) || g_enc.active)
        return FALSE;
    expand_default_save_path(ep, "wav", init_dir, sizeof(init_dir), file, sizeof(file));

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = init_dir[0] ? init_dir : NULL;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrDefExt = "wav";
    ofn.Flags = get_export_save_ofn_flags();

    if (!GetSaveFileNameA(&ofn))
        return FALSE;
    if (is_overwriting_open_edit_file(owner, ep, file))
        return FALSE;
    return video_export_wav_file(owner, ep, file);
}

static void cb_bridge_auo_progress(int now, int total, void *user)
{
    HWND owner = (HWND)user;
    if (g_on_progress)
    {
        g_on_progress(owner, now, total);
    }
    else if (owner && IsWindow(owner) && total > 0)
    {
        char title[256];
        wchar_t wtitle[256];
        int pct10 = (int)(((long long)now * 1000LL) / (long long)total);
        if (pct10 < 0)
            pct10 = 0;
        if (pct10 > 1000)
            pct10 = 1000;
        wsprintfA(title, "[%d.%d%%] AviUtl (%d/%d)", pct10 / 10, pct10 % 10, now, total);
        sjis_to_wide_auo(title, wtitle, 256);
        SetWindowTextW(owner, wtitle);
    }
}

BOOL video_export_auo_file(HWND owner, EDITP *ep, int auo_idx,
                           const char *save_path, int out_flags)
{
    char save_buf[MAX_PATH];
    int s = 0, e = 0, w = 0, h = 0, mw = 0, mh = 0, fps_r = 30, fps_s = 1;
    int saved_frame, eff_flags;
    BOOL include_audio, ok = FALSE;
    OUTPUT_INFO oip;
    LocalAuoEntry *entry;

    if (!ep || !save_path || !save_path[0] || !editp_is_editing(ep) || g_enc.active)
    {
        return FALSE;
    }
    if (auo_idx < 0 || auo_idx >= video_export_get_auo_count())
    {
        return FALSE;
    }
    g_last_auo_idx = auo_idx;

    if (GetFullPathNameA(save_path, MAX_PATH, save_buf, NULL) == 0)
    {
        lstrcpynA(save_buf, save_path, MAX_PATH);
    }
    if (is_overwriting_open_edit_file(owner, ep, save_buf))
    {
        return FALSE;
    }

    eff_flags = out_flags & (OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO | OUTPUT_INFO_FLAG_BATCH);
    if ((eff_flags & (OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO)) == 0)
    {
        eff_flags |= OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO;
    }

    // run 32-bit .auo inside bridge32 when on x64
    if (auo_idx >= g_local_auo_count)
    {
        int bidx = auo_idx - g_local_auo_count;
        editp_set_output_path(ep, save_buf);
        editp_set_saving(ep, TRUE);
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
        ok = plugin_bridge_auo_output(bidx, save_buf, eff_flags, ep, owner,
                                      cb_bridge_auo_progress, (void *)owner);
        SetThreadExecutionState(ES_CONTINUOUS);
        editp_set_saving(ep, FALSE);
        if (ok)
        {
            notify_export_finished(owner, eff_flags);
        }
        return ok;
    }

    entry = &g_local_auos[auo_idx];
    if (!ensure_local_auo_initialized(entry))
    {
        return FALSE;
    }

    include_audio = ((eff_flags & OUTPUT_INFO_FLAG_AUDIO) != 0) &&
                    (editp_has_audio(ep) || plugin_manager_has_active_audio_filter());
    if (!include_audio && (eff_flags & OUTPUT_INFO_FLAG_VIDEO) != 0)
    {
        eff_flags &= ~OUTPUT_INFO_FLAG_AUDIO;
    }

    editp_get_dimensions(ep, &w, &h, &mw, &mh);
    editp_get_fps(ep, &fps_r, &fps_s);
    if (w <= 0 || h <= 0)
        return FALSE;
    if (fps_r <= 0)
        fps_r = 30;
    if (fps_s <= 0)
        fps_s = 1;

    if (!setup_encode_session(owner, ep, &s, &e))
    {
        return FALSE;
    }

    ZeroMemory(&oip, sizeof(oip));
    oip.flag = eff_flags;
    oip.w = w;
    oip.h = h;
    oip.rate = fps_r;
    oip.scale = fps_s;
    oip.n = g_enc.out_n;
    oip.size = ((w * 3 + 3) & ~3) * h;
    if (eff_flags & OUTPUT_INFO_FLAG_AUDIO)
    {
        oip.audio_rate = g_enc.audio_rate;
        oip.audio_ch = g_enc.audio_ch;
        oip.audio_n = g_enc.total_audio_samples;
        oip.audio_size = g_enc.audio_ch * 2;
    }
    oip.savefile = save_buf;
    oip.func_get_video = auo_cb_get_video;
    oip.func_get_audio = auo_cb_get_audio;
    oip.func_is_abort = auo_cb_is_abort;
    oip.func_rest_time_disp = auo_cb_rest_time_disp;
    oip.func_get_flag = auo_cb_get_flag;
    oip.func_update_preview = auo_cb_update_preview;
    oip.func_get_video_ex = auo_cb_get_video_ex;

    saved_frame = editp_get_frame(ep);
    editp_set_output_path(ep, save_buf);
    plugin_manager_notify_update(ep);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    plugin_manager_notify_save_start(ep, s, e, eff_flags & 3);

    dbg_plugin("Starting plugin output '%s' -> '%s' (%dx%d, %d frames, flag=0x%x)",
               entry->name, save_buf, w, h, oip.n, (unsigned)oip.flag);
    ok = safe_auo_output(entry, &oip);

    plugin_manager_notify_save_end(ep, eff_flags & 3);
    SetThreadExecutionState(ES_CONTINUOUS);
    editp_set_frame(ep, saved_frame);
    plugin_manager_notify_update(ep);
    teardown_encode_session();

    dbg_plugin("Plugin output '%s' finished: %s", entry->name, ok ? "OK" : "FAILED/ABORTED");
    if (ok)
    {
        notify_export_finished(owner, eff_flags);
    }
    return ok;
}

BOOL video_export_auo_dialog(HWND owner, EDITP *ep, int auo_idx)
{
    AuoPluginSummary sum;
    OPENFILENAMEA ofn;
    char init_dir[MAX_PATH] = {0};
    char file[MAX_PATH] = {0};
    char def_ext[16] = {0};

    if (!ep || !editp_is_editing(ep) || g_enc.active)
        return FALSE;
    if (!video_export_get_auo_summary(auo_idx, &sum))
        return FALSE;

    g_last_auo_idx = auo_idx;
    extract_default_ext(sum.filefilter, def_ext, sizeof(def_ext));
    expand_default_save_path(ep, def_ext, init_dir, sizeof(init_dir), file, sizeof(file));

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = sum.filefilter;
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = init_dir[0] ? init_dir : NULL;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrDefExt = def_ext[0] ? def_ext : NULL;
    ofn.lpstrTitle = sum.name;
    ofn.Flags = get_export_save_ofn_flags();

    if (!GetSaveFileNameA(&ofn))
        return FALSE;
    if (is_overwriting_open_edit_file(owner, ep, file))
        return FALSE;
    return video_export_auo_file(owner, ep, auo_idx, file,
                                 OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO);
}

int video_export_get_last_auo_index(void)
{
    int count = video_export_get_auo_count();
    if (count <= 0)
        return 0;
    if (g_last_auo_idx < 0 || g_last_auo_idx >= count)
        return 0;
    return g_last_auo_idx;
}

void video_export_set_last_auo_index(int auo_idx)
{
    int count = video_export_get_auo_count();
    if (auo_idx >= 0 && auo_idx < count)
    {
        g_last_auo_idx = auo_idx;
    }
}

BOOL video_export_auo_config_dialog(HWND owner, int auo_idx)
{
    int count = video_export_get_auo_count();
    if (count <= 0)
    {
        return FALSE;
    }
    if (auo_idx < 0 || auo_idx >= count)
    {
        auo_idx = (g_last_auo_idx >= 0 && g_last_auo_idx < count) ? g_last_auo_idx : 0;
    }
    g_last_auo_idx = auo_idx;

    if (auo_idx >= g_local_auo_count)
    {
        return plugin_bridge_auo_config(auo_idx - g_local_auo_count, owner);
    }
    else
    {
        LocalAuoEntry *e = &g_local_auos[auo_idx];
        if (!e->has_config)
        {
            return FALSE;
        }
        if (!ensure_local_auo_initialized(e))
            return FALSE;
        return safe_auo_config(e, owner ? owner : g_host_hwnd);
    }
}

void video_export_request_abort(void)
{
    if (g_enc.active)
    {
        InterlockedExchange(&g_enc.abort_req, 1);
    }
    plugin_bridge_request_encode_abort();
}

BOOL video_export_is_encoding(void)
{
    return g_enc.active;
}
