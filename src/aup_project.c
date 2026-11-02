#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aup_project.h"
#include "plugin_manager.h"
#include "debug_console.h"

#define AUP_IO_BUF_SIZE 65536u
#define AUP_MAX_PRESERVED 64
#define AUP_MAX_CHUNK_BYTES (256u * 1024u * 1024u)

#define FOURCC_YC48 0x38344359u // 'YC48'
#define FOURCC_VIDS 0x73646976u // 'vids'
#define FOURCC_AUDS 0x73647561u // 'auds'

typedef struct
{
    char name[256];
    uint8_t *data;
    uint32_t size;
} PreservedFilterChunk;

static PreservedFilterChunk g_preserved[AUP_MAX_PRESERVED];
static int g_preserved_count = 0;

void aup_clear_preserved_chunks(void)
{
    int i;
    for (i = 0; i < g_preserved_count; i++)
    {
        free(g_preserved[i].data);
        g_preserved[i].data = NULL;
        g_preserved[i].size = 0;
        g_preserved[i].name[0] = '\0';
    }
    g_preserved_count = 0;
}

static void preserve_unmatched_chunk(const char *name, const uint8_t *data, uint32_t size)
{
    PreservedFilterChunk *slot;
    if (!name || !name[0] || g_preserved_count >= AUP_MAX_PRESERVED)
        return;

    slot = &g_preserved[g_preserved_count];
    slot->data = (uint8_t *)malloc(size > 0 ? (size_t)size : 1u);
    if (!slot->data)
        return;
    if (size > 0 && data)
    {
        memcpy(slot->data, data, (size_t)size);
    }
    lstrcpynA(slot->name, name, sizeof(slot->name));
    slot->size = size;
    g_preserved_count++;
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t rd_i32(const uint8_t *p)
{
    return (int32_t)rd_u32(p);
}

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr_i32(uint8_t *p, int32_t v)
{
    wr_u32(p, (uint32_t)v);
}

// 0x0042c1b0, rle stream reader

typedef struct
{
    HANDLE hFile;
    uint8_t *buf;
    size_t pos;
    size_t len;
    BOOL eof;
    BOOL err;
} AupReader;

static BOOL ar_init(AupReader *r, HANDLE hFile)
{
    ZeroMemory(r, sizeof(*r));
    r->hFile = hFile;
    r->buf = (uint8_t *)malloc(AUP_IO_BUF_SIZE);
    return r->buf != NULL;
}

static void ar_free(AupReader *r)
{
    if (r && r->buf)
    {
        free(r->buf);
        r->buf = NULL;
    }
}

// read up to `want` bytes; returns actual bytes read (0 on clean EOF)
static size_t ar_read_some(AupReader *r, void *dst, size_t want)
{
    uint8_t *out = (uint8_t *)dst;
    size_t total = 0;

    if (r->err)
        return 0;

    while (total < want)
    {
        size_t avail = r->len - r->pos;
        if (avail > 0)
        {
            size_t take = want - total;
            if (take > avail)
                take = avail;
            memcpy(out + total, r->buf + r->pos, take);
            r->pos += take;
            total += take;
        }
        else
        {
            DWORD got = 0;
            if (r->eof)
                break;
            if (!ReadFile(r->hFile, r->buf, AUP_IO_BUF_SIZE, &got, NULL))
            {
                r->err = TRUE;
                break;
            }
            r->pos = 0;
            r->len = (size_t)got;
            if (got == 0)
            {
                r->eof = TRUE;
                break;
            }
        }
    }
    return total;
}

static BOOL ar_read_exact(AupReader *r, void *dst, size_t want)
{
    return ar_read_some(r, dst, want) == want && !r->err;
}

static BOOL ar_read_u32(AupReader *r, uint32_t *out_val)
{
    uint8_t b[4];
    if (!ar_read_exact(r, b, 4))
        return FALSE;
    *out_val = rd_u32(b);
    return TRUE;
}

static BOOL ar_skip(AupReader *r, size_t bytes)
{
    while (bytes > 0)
    {
        size_t avail = r->len - r->pos;
        if (avail > 0)
        {
            size_t take = (bytes < avail) ? bytes : avail;
            r->pos += take;
            bytes -= take;
        }
        else
        {
            DWORD got = 0;
            if (r->eof)
                return FALSE;
            if (!ReadFile(r->hFile, r->buf, AUP_IO_BUF_SIZE, &got, NULL) || got == 0)
            {
                r->err = TRUE;
                return FALSE;
            }
            r->pos = 0;
            r->len = (size_t)got;
        }
    }
    return TRUE;
}

// 0x0042c1b0, aviutl .aup RLE decoder
static BOOL ar_rle_read(AupReader *r, uint8_t *dst, size_t uncompressed_len)
{
    size_t rem = uncompressed_len;
    while (rem > 0)
    {
        uint8_t hdr = 0;
        uint32_t count;

        if (!ar_read_exact(r, &hdr, 1))
            return FALSE;
        count = (uint32_t)(hdr & 0x7Fu);
        if (count == 0)
        {
            uint8_t ext[3];
            if (!ar_read_exact(r, ext, 3))
                return FALSE;
            count = (uint32_t)ext[0] |
                    ((uint32_t)ext[1] << 8) |
                    ((uint32_t)ext[2] << 16);
        }
        if (count == 0 || (size_t)count > rem)
        {
            return FALSE;
        }
        if (hdr & 0x80u)
        {
            uint8_t val = 0;
            if (!ar_read_exact(r, &val, 1))
                return FALSE;
            memset(dst, val, (size_t)count);
        }
        else
        {
            if (!ar_read_exact(r, dst, (size_t)count))
                return FALSE;
        }
        dst += count;
        rem -= (size_t)count;
    }
    return TRUE;
}

// 0x0042c270, rle stream writer

typedef struct
{
    HANDLE hFile;
    uint8_t *buf;
    size_t pos;
    BOOL err;
} AupWriter;

static BOOL aw_init(AupWriter *w, HANDLE hFile)
{
    ZeroMemory(w, sizeof(*w));
    w->hFile = hFile;
    w->buf = (uint8_t *)malloc(AUP_IO_BUF_SIZE);
    return w->buf != NULL;
}

static BOOL aw_flush(AupWriter *w)
{
    size_t off = 0;
    if (w->err)
        return FALSE;
    while (off < w->pos)
    {
        DWORD written = 0;
        DWORD chunk = (DWORD)(w->pos - off);
        if (!WriteFile(w->hFile, w->buf + off, chunk, &written, NULL) || written == 0)
        {
            w->err = TRUE;
            return FALSE;
        }
        off += (size_t)written;
    }
    w->pos = 0;
    return TRUE;
}

static void aw_free(AupWriter *w)
{
    if (w && w->buf)
    {
        free(w->buf);
        w->buf = NULL;
    }
}

static BOOL aw_write(AupWriter *w, const void *src, size_t len)
{
    const uint8_t *p = (const uint8_t *)src;
    if (w->err)
        return FALSE;

    while (len > 0)
    {
        size_t space = AUP_IO_BUF_SIZE - w->pos;
        size_t take;
        if (space == 0)
        {
            if (!aw_flush(w))
                return FALSE;
            space = AUP_IO_BUF_SIZE;
        }
        take = (len < space) ? len : space;
        memcpy(w->buf + w->pos, p, take);
        w->pos += take;
        p += take;
        len -= take;
    }
    return TRUE;
}

static BOOL aw_write_u32(AupWriter *w, uint32_t v)
{
    uint8_t b[4];
    wr_u32(b, v);
    return aw_write(w, b, 4);
}

// 0x0042c270, aviutl .aup RLE encoder
static BOOL aw_rle_write(AupWriter *w, const uint8_t *src, size_t len)
{
    size_t rem = len;
    while (rem > 0)
    {
        uint8_t b0 = src[0];
        if (rem >= 4 && b0 == src[1] && b0 == src[2] && b0 == src[3])
        {
            uint32_t run = 0;
            while (rem > 0 && src[0] == b0 && run < 0x7FFFFFu)
            {
                run++;
                src++;
                rem--;
            }
            if (run < 0x80u)
            {
                uint8_t hdr = (uint8_t)(0x80u | run);
                if (!aw_write(w, &hdr, 1))
                    return FALSE;
            }
            else
            {
                uint8_t hdr[4];
                hdr[0] = 0x80u;
                hdr[1] = (uint8_t)(run & 0xFFu);
                hdr[2] = (uint8_t)((run >> 8) & 0xFFu);
                hdr[3] = (uint8_t)((run >> 16) & 0xFFu);
                if (!aw_write(w, hdr, 4))
                    return FALSE;
            }
            if (!aw_write(w, &b0, 1))
                return FALSE;
        }
        else
        {
            const uint8_t *lit_start = src;
            uint32_t lit = 0;
            while (rem > 0 && lit < 0x7FFFFFu)
            {
                if (rem >= 4 && src[0] == src[1] && src[0] == src[2] && src[0] == src[3])
                {
                    break;
                }
                lit++;
                src++;
                rem--;
            }
            if (lit < 0x80u)
            {
                uint8_t hdr = (uint8_t)lit;
                if (!aw_write(w, &hdr, 1))
                    return FALSE;
            }
            else
            {
                uint8_t hdr[4];
                hdr[0] = 0x00u;
                hdr[1] = (uint8_t)(lit & 0xFFu);
                hdr[2] = (uint8_t)((lit >> 8) & 0xFFu);
                hdr[3] = (uint8_t)((lit >> 16) & 0xFFu);
                if (!aw_write(w, hdr, 4))
                    return FALSE;
            }
            if (!aw_write(w, lit_start, (size_t)lit))
                return FALSE;
        }
    }
    return TRUE;
}

// media and virtual canvas loader for .aup

static const char *path_filename_part(const char *path)
{
    const char *base = path;
    const unsigned char *p = (const unsigned char *)path;
    if (!path)
        return "";
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

static BOOL try_open_candidate_media(EDITP *ep, const char *aup_abs, const char *candidate)
{
    size_t len;
    char sibling_path[MAX_PATH];
    const char *slash;
    const char *fname;

    if (!candidate || !candidate[0])
        return FALSE;

    len = strlen(candidate);
    if (len >= 8 && _stricmp(candidate + len - 7, ".exedit") == 0)
    {
        return editp_open_video(ep, candidate);
    }

    if (editp_open_video(ep, candidate))
    {
        return TRUE;
    }

    // try looking for the media file in the same directory as the .aup file
    slash = strrchr(aup_abs, '\\');
    if (!slash)
        slash = strrchr(aup_abs, '/');
    fname = path_filename_part(candidate);
    if (slash && fname[0])
    {
        int dir_len = (int)(slash - aup_abs);
        if (dir_len > 0 && dir_len + 1 + (int)strlen(fname) < MAX_PATH)
        {
            memcpy(sibling_path, aup_abs, (size_t)dir_len);
            sibling_path[dir_len] = '\\';
            lstrcpynA(sibling_path + dir_len + 1, fname, MAX_PATH - dir_len - 1);
            if (editp_open_video(ep, sibling_path))
            {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static BOOL open_aup_media_or_canvas(EDITP *ep, const char *aup_abs,
                                     const char *edit_name, const char *slot0_name,
                                     int w, int h, int rate, int scale, int audio_rate)
{
    char synth_spec[128];

    if (try_open_candidate_media(ep, aup_abs, edit_name))
    {
        return TRUE;
    }
    if (slot0_name && slot0_name[0] && lstrcmpiA(slot0_name, edit_name) != 0)
    {
        if (try_open_candidate_media(ep, aup_abs, slot0_name))
        {
            return TRUE;
        }
    }

    // synthesize a matching *.exedit virtual canvas from the .aup's EDIT_HANDLE specs
    if (w <= 0)
        w = 1280;
    if (h <= 0)
        h = 720;
    if (rate <= 0)
        rate = 30;
    if (scale <= 0)
        scale = 1;
    if (audio_rate <= 0)
        audio_rate = 44100;

    if (scale == 1)
    {
        wsprintfA(synth_spec, "%dx%d_%dfps_%dHz.exedit", w, h, rate, audio_rate);
    }
    else
    {
        wsprintfA(synth_spec, "%dx%d_%d-%dfps_%dHz.exedit", w, h, rate, scale, audio_rate);
    }
    return editp_open_video(ep, synth_spec);
}

// 0x00423560, load .aup project

BOOL aup_load_local(const char *aup_path, EDITP *ep)
{
    char abs_aup[MAX_PATH];
    HANDLE hFile = INVALID_HANDLE_VALUE;
    AupReader rd;
    uint8_t *eh = NULL;
    uint8_t *tbl_tmp = NULL;
    uint8_t *status_buf = NULL;
    uint8_t *chunk_buf = NULL;
    size_t chunk_cap = 0;
    char magic1[AUP_MAGIC_LEN + 1];
    char magic2[AUP_MAGIC_LEN + 1];
    uint32_t eh_size = 0;
    uint32_t tbl_frames = 0;
    char edit_name[260];
    char output_name[260];
    char slot0_name[260];
    int w, h, frame_n, select_s, select_e, frame;
    int video_rate, video_scale, audio_rate, audio_ch;
    BOOL has_audio;
    int i;
    BOOL ok = FALSE;

    if (!aup_path || !aup_path[0] || !ep)
        return FALSE;
    if (GetFullPathNameA(aup_path, MAX_PATH, abs_aup, NULL) == 0)
    {
        lstrcpynA(abs_aup, aup_path, MAX_PATH);
    }

    hFile = CreateFileA(abs_aup, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    if (!ar_init(&rd, hFile))
    {
        CloseHandle(hFile);
        return FALSE;
    }

    // verify Magic #1 ("AviUtl ProjectFile version 0.18") and EDIT_HANDLE size
    ZeroMemory(magic1, sizeof(magic1));
    if (!ar_read_exact(&rd, magic1, AUP_MAGIC_LEN) ||
        lstrcmpA(magic1, AUP_MAGIC_018) != 0 ||
        !ar_read_u32(&rd, &eh_size) ||
        eh_size != AUP_EDIT_HANDLE_SIZE)
    {
        goto cleanup;
    }

    // read 0x20C uncompressed prefix + RLE-decode remaining 0x4C07DC bytes of EDIT_HANDLE
    eh = (uint8_t *)calloc(1, (size_t)eh_size);
    if (!eh)
        goto cleanup;

    if (!ar_read_exact(&rd, eh, AUP_UNCOMPRESSED_PREFIX) ||
        !ar_rle_read(&rd, eh + AUP_UNCOMPRESSED_PREFIX,
                     (size_t)(eh_size - AUP_UNCOMPRESSED_PREFIX)))
    {
        goto cleanup;
    }

    memcpy(edit_name, eh + 0x000004u, 260);
    edit_name[259] = '\0';
    memcpy(output_name, eh + 0x000108u, 260);
    output_name[259] = '\0';
    memcpy(slot0_name, eh + 0x026E98u, 260);
    slot0_name[259] = '\0';

    w = rd_i32(eh + 0x000310u);
    h = rd_i32(eh + 0x000314u);
    frame_n = rd_i32(eh + 0x000318u);
    select_s = rd_i32(eh + 0x00031Cu);
    select_e = rd_i32(eh + 0x000320u);
    frame = rd_i32(eh + 0x000330u);
    has_audio = (rd_u32(eh + 0x0003C0u) & 4u) != 0;
    audio_ch = (int)rd_u16(eh + 0x0003FAu);
    audio_rate = (int)rd_u32(eh + 0x0003FCu);
    video_scale = rd_i32(eh + 0x000468u);
    video_rate = rd_i32(eh + 0x00046Cu);

    if (video_scale <= 0)
        video_scale = rd_i32(eh + 0x000418u);
    if (video_rate <= 0)
        video_rate = rd_i32(eh + 0x000414u);
    if (video_scale <= 0)
        video_scale = 1;
    if (video_rate <= 0)
        video_rate = 30;
    if (audio_ch <= 0)
        audio_ch = 2;
    if (audio_rate <= 0)
        audio_rate = 44100;

    // read per-frame table count + 10 RLE-compressed per-frame arrays (0x00423560)
    if (!ar_read_u32(&rd, &tbl_frames) || tbl_frames > (uint32_t)(REAVIUTL_MAX_FRAME * 4))
    {
        goto cleanup;
    }

    if (tbl_frames > 0)
    {
        tbl_tmp = (uint8_t *)malloc((size_t)tbl_frames * 4u);
        status_buf = (uint8_t *)calloc((size_t)tbl_frames, 1u);
        if (!tbl_tmp || !status_buf)
            goto cleanup;

        for (i = 0; i < 4; i++)
        {
            if (!ar_rle_read(&rd, tbl_tmp, (size_t)tbl_frames * 4u))
                goto cleanup;
        }
        // byte table 0 is frame_status (KEYFRAME / MARKFRAME / DELFRAME / NULLFRAME)
        if (!ar_rle_read(&rd, status_buf, (size_t)tbl_frames))
            goto cleanup;
        for (i = 1; i < 6; i++)
        {
            if (!ar_rle_read(&rd, tbl_tmp, (size_t)tbl_frames))
                goto cleanup;
        }
    }

    // skip embedded profile blobs (96 slots @ +0x20D18) & HGLOBAL blobs (256 slots @ +0x4BBD98)
    for (i = 0; i < 96; i++)
    {
        if (eh[0x020D18u + (size_t)i * 0x104u] != 0)
        {
            uint32_t blob_sz = 0;
            if (!ar_read_u32(&rd, &blob_sz))
                goto cleanup;
            if (blob_sz > 0 && !ar_skip(&rd, (size_t)blob_sz))
                goto cleanup;
        }
    }
    for (i = 0; i < 256; i++)
    {
        if (rd_u32(eh + 0x4BBD98u + (size_t)i * 4u) != 0)
        {
            uint32_t blob_sz = 0;
            if (!ar_read_u32(&rd, &blob_sz))
                goto cleanup;
            if (blob_sz > 0 && !ar_skip(&rd, (size_t)blob_sz))
                goto cleanup;
        }
    }

    // verify Magic #2 ("AviUtl ProjectFile version 0.18")
    ZeroMemory(magic2, sizeof(magic2));
    if (!ar_read_exact(&rd, magic2, AUP_MAGIC_LEN) ||
        lstrcmpA(magic2, AUP_MAGIC_018) != 0)
    {
        goto cleanup;
    }

    // apply loaded header and tables to editp
    aup_clear_preserved_chunks();

    if (editp_is_editing(ep))
    {
        plugin_manager_notify_file_close(ep);
        editp_close_video(ep);
    }

    if (!open_aup_media_or_canvas(ep, abs_aup, edit_name, slot0_name,
                                  w, h, video_rate, video_scale, audio_rate))
    {
        goto cleanup;
    }

    if (w > 0 && h > 0)
    {
        editp_set_dimensions(ep, w, h);
    }
    editp_set_fps(ep, video_rate, video_scale);
    editp_set_audio_format(ep, audio_rate, audio_ch, has_audio);
    if (frame_n > 0)
    {
        editp_set_frame_n(ep, frame_n);
    }
    if (status_buf && frame_n > 0)
    {
        int lim = ((uint32_t)frame_n < tbl_frames) ? frame_n : (int)tbl_frames;
        for (i = 0; i < lim; i++)
        {
            if (status_buf[i] != 0)
            {
                editp_set_frame_status(ep, i, status_buf[i]);
            }
        }
    }
    editp_set_select_frame(ep, select_s, select_e);
    editp_set_frame(ep, frame);
    editp_set_project_path(ep, abs_aup);
    editp_set_output_path(ep, output_name);
    editp_load_frame(ep, editp_get_frame(ep));

    // send WM_FILTER_FILE_OPEN (0x0465) before func_project_load
    plugin_manager_notify_file_open(ep);

    // read "FilterProject 0.1" chunks until EOF and dispatch func_project_load (0x00431c20)
    for (;;)
    {
        char fp_magic[AUP_FILTER_MAGIC_LEN + 1];
        char flt_name[512];
        uint32_t name_len = 0;
        uint32_t data_size = 0;
        size_t got_hdr;
        int fcount, fi;
        BOOL matched = FALSE;

        ZeroMemory(fp_magic, sizeof(fp_magic));
        got_hdr = ar_read_some(&rd, fp_magic, AUP_FILTER_MAGIC_LEN);
        if (got_hdr == 0)
        {
            break; // clean EOF
        }
        if (got_hdr != AUP_FILTER_MAGIC_LEN ||
            lstrcmpA(fp_magic, AUP_FILTER_MAGIC) != 0)
        {
            break;
        }

        if (!ar_read_u32(&rd, &name_len) || name_len == 0 || name_len >= sizeof(flt_name))
        {
            goto cleanup;
        }
        if (!ar_read_exact(&rd, flt_name, (size_t)name_len))
        {
            goto cleanup;
        }
        flt_name[name_len - 1] = '\0';

        if (!ar_read_u32(&rd, &data_size) || data_size > AUP_MAX_CHUNK_BYTES)
        {
            goto cleanup;
        }
        if ((size_t)data_size > chunk_cap || !chunk_buf)
        {
            size_t new_cap = (data_size > 0x100000u) ? (size_t)data_size : 0x100000u;
            uint8_t *nb = (uint8_t *)realloc(chunk_buf, new_cap);
            if (!nb)
                goto cleanup;
            chunk_buf = nb;
            chunk_cap = new_cap;
        }
        if (data_size > 0)
        {
            memset(chunk_buf, 0, (size_t)data_size);
            if (!ar_rle_read(&rd, chunk_buf, (size_t)data_size))
            {
                goto cleanup;
            }
        }

        fcount = plugin_manager_get_count();
        for (fi = 0; fi < fcount; fi++)
        {
            FILTER *fp = plugin_manager_get_filter(fi);
            if (fp && fp->name && lstrcmpA(fp->name, flt_name) == 0)
            {
                matched = TRUE;
                if (fp->func_project_load)
                {
                    BOOL rc_load = fp->func_project_load(fp, ep, chunk_buf, (int)data_size);
                    dbg_aup("Loaded FilterProject chunk '%s' (%u bytes) -> %s",
                            flt_name, (unsigned)data_size, rc_load ? "OK" : "FALSE");
                }
                break;
            }
        }
        if (!matched)
        {
            dbg_warn("Unmatched FilterProject chunk '%s' (%u bytes) -> preserved for re-save",
                     flt_name, (unsigned)data_size);
            preserve_unmatched_chunk(flt_name, chunk_buf, data_size);
        }
    }

    // re-apply saved selection & playhead cursor (clamped to final frame_n) and notify update
    if (select_s >= 0 && select_e >= select_s)
    {
        editp_set_select_frame(ep, select_s, select_e);
    }
    editp_set_frame(ep, frame);
    plugin_manager_notify_update(ep);
    dbg_aup("Opened project '%s' (%dx%d, frames=%d, fps=%d/%d, audio=%dHz/%dch)",
            abs_aup, editp_get_frame_n(ep) > 0 ? w : 0, h,
            editp_get_frame_n(ep), video_rate, video_scale, audio_rate, audio_ch);
    ok = TRUE;

cleanup:
    free(chunk_buf);
    free(status_buf);
    free(tbl_tmp);
    free(eh);
    ar_free(&rd);
    CloseHandle(hFile);
    return ok;
}

// 0x00424160, save .aup project

static void populate_aviutl_edit_handle(uint8_t *eh, EDITP *ep, const char *abs_aup)
{
    int w = 0, h = 0, max_w = 0, max_h = 0;
    int fps_rate = 30, fps_scale = 1;
    int frame_n, frame, select_s = 0, select_e = 0;
    int audio_rate, audio_ch;
    BOOL has_audio;
    uint32_t rgb_stride, rgb_img_sz, yc_img_sz, avg_bps;
    uint16_t block_align;
    uint8_t *src_blk;

    editp_get_dimensions(ep, &w, &h, &max_w, &max_h);
    editp_get_fps(ep, &fps_rate, &fps_scale);
    frame_n = editp_get_frame_n(ep);
    frame = editp_get_frame(ep);
    editp_get_select_frame(ep, &select_s, &select_e);
    audio_rate = editp_get_audio_rate(ep);
    audio_ch = editp_get_audio_ch(ep);
    has_audio = editp_has_audio(ep);

    if (w <= 0)
        w = 640;
    if (h <= 0)
        h = 480;
    if (frame_n <= 0)
        frame_n = 1;
    if (fps_rate <= 0)
        fps_rate = 30;
    if (fps_scale <= 0)
        fps_scale = 1;
    if (audio_rate <= 0)
        audio_rate = 44100;
    if (audio_ch <= 0)
        audio_ch = 2;
    if (select_s < 0)
        select_s = 0;
    if (select_e < select_s || select_e >= frame_n)
        select_e = frame_n - 1;

    rgb_stride = (uint32_t)((w * 3 + 3) & ~3);
    rgb_img_sz = rgb_stride * (uint32_t)h;
    yc_img_sz = (uint32_t)w * (uint32_t)h * 6u;
    block_align = (uint16_t)(audio_ch * 2);
    avg_bps = (uint32_t)audio_rate * (uint32_t)block_align;

    // prefix (+0x000 .. +0x20B) + project_name (+0x20C)
    wr_u32(eh + 0x000000u, 1u);
    lstrcpynA((char *)(eh + 0x000004u), editp_get_edit_path(ep), 260);
    lstrcpynA((char *)(eh + 0x000108u), editp_get_output_path(ep), 260);
    lstrcpynA((char *)(eh + 0x00020Cu), abs_aup, 260);

    // core geometry, timeline range, & RGB24 display BITMAPINFOHEADER (+0x310 .. +0x35F)
    wr_i32(eh + 0x000310u, w);
    wr_i32(eh + 0x000314u, h);
    wr_i32(eh + 0x000318u, frame_n);
    wr_i32(eh + 0x00031Cu, select_s);
    wr_i32(eh + 0x000320u, select_e);
    wr_i32(eh + 0x000328u, w);
    wr_i32(eh + 0x00032Cu, h);
    wr_i32(eh + 0x000330u, frame);

    wr_u32(eh + 0x000338u, 40u);
    wr_i32(eh + 0x00033Cu, w);
    wr_i32(eh + 0x000340u, h);
    wr_u16(eh + 0x000344u, 1u);
    wr_u16(eh + 0x000346u, 24u);
    wr_u32(eh + 0x000348u, 0u);
    wr_u32(eh + 0x00034Cu, rgb_img_sz);

    wr_i32(eh + 0x0003B0u, w);
    wr_i32(eh + 0x0003B4u, h);

    // active SOURCE_FILE_INFO block (+0x3C0 .. +0xC777, size 0xC3B8)
    src_blk = eh + 0x0003C0u;
    wr_u32(src_blk + 0x0000u, has_audio ? 0x07u : 0x03u);
    wr_i32(src_blk + 0x0004u, w);
    wr_i32(src_blk + 0x0008u, h);
    wr_i32(src_blk + 0x000Cu, frame_n);

    // YC48 BITMAPINFOHEADER @ src_blk + 0x0010 (+0x3D0)
    wr_u32(src_blk + 0x0010u, 40u);
    wr_i32(src_blk + 0x0014u, w);
    wr_i32(src_blk + 0x0018u, h);
    wr_u16(src_blk + 0x001Cu, 1u);
    wr_u16(src_blk + 0x001Eu, 48u);
    wr_u32(src_blk + 0x0020u, FOURCC_YC48);
    wr_u32(src_blk + 0x0024u, yc_img_sz);

    // PCM16 WAVEFORMATEX @ src_blk + 0x0038 (+0x3F8)
    wr_u16(src_blk + 0x0038u, 1u);
    wr_u16(src_blk + 0x003Au, (uint16_t)audio_ch);
    wr_u32(src_blk + 0x003Cu, (uint32_t)audio_rate);
    wr_u32(src_blk + 0x0040u, avg_bps);
    wr_u16(src_blk + 0x0044u, block_align);
    wr_u16(src_blk + 0x0046u, 16u);

    // AVIFILEINFOA & AVISTREAMINFOA fields
    wr_u32(src_blk + 0x0050u, 3u);
    wr_i32(src_blk + 0x0054u, fps_rate);
    wr_i32(src_blk + 0x0058u, fps_scale);
    wr_i32(src_blk + 0x005Cu, 1);

    wr_u32(src_blk + 0x0094u, FOURCC_VIDS);
    wr_u32(src_blk + 0x0098u, FOURCC_YC48);
    wr_i32(src_blk + 0x00A8u, fps_scale); // +0x468: video_scale
    wr_i32(src_blk + 0x00ACu, fps_rate);  // +0x46C: video_rate
    wr_i32(src_blk + 0x00B4u, frame_n);

    wr_u32(src_blk + 0x0120u, FOURCC_AUDS);
    wr_i32(src_blk + 0x0134u, (int32_t)block_align);
    wr_u32(src_blk + 0x0138u, avg_bps);
    wr_u32(src_blk + 0x0140u, 0x7FFFFFFFu);
    wr_i32(src_blk + 0x0150u, (int32_t)block_align);

    // embedded format copies inside SOURCE_FILE_INFO
    wr_u32(src_blk + 0x01ACu, 40u);
    memcpy(src_blk + 0x01B0u, src_blk + 0x0010u, 40u);
    wr_u32(src_blk + 0x21ACu, 40u);
    memcpy(src_blk + 0x21B0u, src_blk + 0x0038u, 18u);
    wr_i32(src_blk + 0xC398u, -1);
    wr_i32(src_blk + 0xC39Cu, 1204);

    // mirror source slot[0] at +0x26E98 (filepath[260] + 0xC3B8 SOURCE_FILE_INFO)
    lstrcpynA((char *)(eh + 0x026E98u), editp_get_edit_path(ep), 260);
    memcpy(eh + 0x026E98u + 0x0104u, src_blk, 0xC3B8u);
    wr_i32(eh + 0x026E98u + 0x0104u + 0xC398u, 0);
}

BOOL aup_save_local(const char *aup_path, EDITP *ep)
{
    char abs_aup[MAX_PATH];
    HANDLE hFile = INVALID_HANDLE_VALUE;
    AupWriter wr;
    uint8_t *eh = NULL;
    uint8_t *u32_zeros = NULL;
    uint8_t *u32_seq = NULL;
    uint8_t *u8_status = NULL;
    uint8_t *u8_key = NULL;
    uint8_t *u8_zeros = NULL;
    uint8_t *flt_buf = NULL;
    size_t flt_cap = 0;
    char magic_buf[AUP_MAGIC_LEN];
    char fp_magic[AUP_FILTER_MAGIC_LEN];
    int frame_n, i, fcount;
    BOOL ok = FALSE;

    if (!aup_path || !aup_path[0] || !ep || !editp_is_editing(ep))
    {
        return FALSE;
    }
    if (GetFullPathNameA(aup_path, MAX_PATH, abs_aup, NULL) == 0)
    {
        lstrcpynA(abs_aup, aup_path, MAX_PATH);
    }

    editp_set_project_path(ep, abs_aup);
    frame_n = editp_get_frame_n(ep);
    if (frame_n < 1)
        frame_n = 1;

    eh = (uint8_t *)calloc(1, (size_t)AUP_EDIT_HANDLE_SIZE);
    u32_zeros = (uint8_t *)calloc((size_t)frame_n, 4u);
    u32_seq = (uint8_t *)malloc((size_t)frame_n * 4u);
    u8_status = (uint8_t *)calloc((size_t)frame_n, 1u);
    u8_key = (uint8_t *)malloc((size_t)frame_n);
    u8_zeros = (uint8_t *)calloc((size_t)frame_n, 1u);
    if (!eh || !u32_zeros || !u32_seq || !u8_status || !u8_key || !u8_zeros)
    {
        goto cleanup_mem;
    }

    populate_aviutl_edit_handle(eh, ep, abs_aup);

    for (i = 0; i < frame_n; i++)
    {
        BYTE st = 0;
        wr_u32(u32_seq + (size_t)i * 4u, (uint32_t)i);
        if (editp_get_frame_status(ep, i, &st))
        {
            u8_status[i] = st;
        }
        u8_key[i] = editp_is_source_keyframe(ep, i) ? 2u : 0u;
    }

    ZeroMemory(magic_buf, sizeof(magic_buf));
    memcpy(magic_buf, AUP_MAGIC_018, strlen(AUP_MAGIC_018));

    ZeroMemory(fp_magic, sizeof(fp_magic));
    memcpy(fp_magic, AUP_FILTER_MAGIC, strlen(AUP_FILTER_MAGIC));

    hFile = CreateFileA(abs_aup, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        goto cleanup_mem;
    }
    if (!aw_init(&wr, hFile))
    {
        CloseHandle(hFile);
        goto cleanup_mem;
    }

    // magic #1 + EDIT_HANDLE size + 0x20C raw prefix + RLE-compressed remainder
    if (!aw_write(&wr, magic_buf, AUP_MAGIC_LEN) ||
        !aw_write_u32(&wr, AUP_EDIT_HANDLE_SIZE) ||
        !aw_write(&wr, eh, AUP_UNCOMPRESSED_PREFIX) ||
        !aw_rle_write(&wr, eh + AUP_UNCOMPRESSED_PREFIX,
                      (size_t)(AUP_EDIT_HANDLE_SIZE - AUP_UNCOMPRESSED_PREFIX)))
    {
        goto cleanup_file;
    }

    // per-frame table count + 10 RLE-compressed per-frame arrays (0x00424160)
    if (!aw_write_u32(&wr, (uint32_t)frame_n) ||
        !aw_rle_write(&wr, u32_zeros, (size_t)frame_n * 4u) ||
        !aw_rle_write(&wr, u32_seq, (size_t)frame_n * 4u) ||
        !aw_rle_write(&wr, u32_zeros, (size_t)frame_n * 4u) ||
        !aw_rle_write(&wr, u32_zeros, (size_t)frame_n * 4u) ||
        !aw_rle_write(&wr, u8_status, (size_t)frame_n) ||
        !aw_rle_write(&wr, u8_zeros, (size_t)frame_n) ||
        !aw_rle_write(&wr, u8_key, (size_t)frame_n) ||
        !aw_rle_write(&wr, u8_zeros, (size_t)frame_n) ||
        !aw_rle_write(&wr, u8_zeros, (size_t)frame_n) ||
        !aw_rle_write(&wr, u8_zeros, (size_t)frame_n))
    {
        goto cleanup_file;
    }

    // magic #2 ("AviUtl ProjectFile version 0.18")
    if (!aw_write(&wr, magic_buf, AUP_MAGIC_LEN))
    {
        goto cleanup_file;
    }

    // save "FilterProject 0.1" chunks from loaded plugins (0x004319c0)
    fcount = plugin_manager_get_count();
    for (i = 0; i < fcount; i++)
    {
        FILTER *fp = plugin_manager_get_filter(i);
        int sz = 0;
        uint32_t name_len;

        if (!fp || !fp->func_project_save || !fp->name || !fp->name[0])
        {
            continue;
        }
        if (!fp->func_project_save(fp, ep, NULL, &sz) || sz <= 0)
        {
            continue;
        }
        if ((size_t)sz > flt_cap || !flt_buf)
        {
            size_t new_cap = ((size_t)sz > 0x100000u) ? (size_t)sz : 0x100000u;
            uint8_t *nb = (uint8_t *)realloc(flt_buf, new_cap);
            if (!nb)
                goto cleanup_file;
            flt_buf = nb;
            flt_cap = new_cap;
        }
        memset(flt_buf, 0, (size_t)sz);
        if (!fp->func_project_save(fp, ep, flt_buf, &sz) || sz <= 0)
        {
            continue;
        }

        name_len = (uint32_t)strlen(fp->name) + 1u;
        if (!aw_write(&wr, fp_magic, AUP_FILTER_MAGIC_LEN) ||
            !aw_write_u32(&wr, name_len) ||
            !aw_write(&wr, fp->name, (size_t)name_len) ||
            !aw_write_u32(&wr, (uint32_t)sz) ||
            !aw_rle_write(&wr, flt_buf, (size_t)sz))
        {
            goto cleanup_file;
        }
        dbg_aup("Saved FilterProject chunk '%s' (%d bytes)", fp->name, sz);
    }

    // re-emit any preserved chunks from unloaded third-party plugins
    for (i = 0; i < g_preserved_count; i++)
    {
        const PreservedFilterChunk *pc = &g_preserved[i];
        uint32_t name_len;
        if (!pc->name[0] || !pc->data || pc->size == 0)
            continue;
        name_len = (uint32_t)strlen(pc->name) + 1u;
        if (!aw_write(&wr, fp_magic, AUP_FILTER_MAGIC_LEN) ||
            !aw_write_u32(&wr, name_len) ||
            !aw_write(&wr, pc->name, (size_t)name_len) ||
            !aw_write_u32(&wr, pc->size) ||
            !aw_rle_write(&wr, pc->data, (size_t)pc->size))
        {
            goto cleanup_file;
        }
    }

    if (!aw_flush(&wr))
    {
        goto cleanup_file;
    }
    dbg_aup("Saved project '%s' (frames=%d)", abs_aup, frame_n);
    ok = TRUE;

cleanup_file:
    aw_free(&wr);
    CloseHandle(hFile);
    if (!ok)
    {
        DeleteFileA(abs_aup);
    }

cleanup_mem:
    free(flt_buf);
    free(u8_zeros);
    free(u8_key);
    free(u8_status);
    free(u32_seq);
    free(u32_zeros);
    free(eh);
    return ok;
}
