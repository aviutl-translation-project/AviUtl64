#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aup_project.h"
#include "plugin_manager.h"
#include "profile_cfg.h"
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
    PreservedFilterChunk *slot = NULL;
    int i;
    if (!name || !name[0])
        return;

    for (i = 0; i < g_preserved_count; i++)
    {
        if (lstrcmpA(g_preserved[i].name, name) == 0)
        {
            slot = &g_preserved[i];
            free(slot->data);
            slot->data = NULL;
            slot->size = 0;
            break;
        }
    }
    if (!slot)
    {
        if (g_preserved_count >= AUP_MAX_PRESERVED)
            return;
        slot = &g_preserved[g_preserved_count++];
    }

    slot->data = (uint8_t *)malloc(size > 0 ? (size_t)size : 1u);
    if (!slot->data)
    {
        slot->name[0] = '\0';
        slot->size = 0;
        return;
    }
    if (size > 0 && data)
    {
        memcpy(slot->data, data, (size_t)size);
    }
    lstrcpynA(slot->name, name, sizeof(slot->name));
    slot->size = size;
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

static int try_add_candidate_source(EDITP *ep, const char *aup_abs, const char *candidate)
{
    int id;
    char sibling_path[MAX_PATH];
    const char *slash;
    const char *fname;

    if (!candidate || !candidate[0])
        return -1;

    id = editp_add_source_file(ep, candidate);
    if (id >= 0)
        return id;

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
            id = editp_add_source_file(ep, sibling_path);
            if (id >= 0)
                return id;
        }
    }
    return -1;
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

static void get_aup_ini_path(char *out_path, size_t out_cap)
{
    char mod[MAX_PATH];
    char *slash;
    if (!out_path || out_cap == 0)
        return;
    if (GetModuleFileNameA(NULL, mod, MAX_PATH) > 0)
    {
        slash = strrchr(mod, '\\');
        if (slash)
            *(slash + 1) = '\0';
        else
            mod[0] = '\0';
        _snprintf(out_path, out_cap, "%saviutl.ini", mod);
        out_path[out_cap - 1] = '\0';
    }
    else
    {
        lstrcpynA(out_path, ".\\aviutl.ini", (int)out_cap);
    }
}

BOOL aup_load_local(const char *aup_path, EDITP *ep)
{
    char abs_aup[MAX_PATH];
    HANDLE hFile = INVALID_HANDLE_VALUE;
    AupReader rd;
    uint8_t *eh = NULL;
    uint8_t *u32_0 = NULL;
    uint8_t *u32_1 = NULL;
    uint8_t *tbl_tmp = NULL;
    uint8_t *u8_0 = NULL;
    uint8_t *u8_1 = NULL;
    uint8_t *u8_2 = NULL;
    uint8_t *u8_3 = NULL;
    uint8_t *chunk_buf = NULL;
    size_t chunk_cap = 0;
    uint8_t *emb_cfg_blobs[PROFILE_MAX_COUNT];
    uint32_t emb_cfg_sizes[PROFILE_MAX_COUNT];
    char magic1[AUP_MAGIC_LEN + 1];
    char magic2[AUP_MAGIC_LEN + 1];
    uint32_t eh_size = 0;
    uint32_t tbl_frames = 0;
    char edit_name[260];
    char output_name[260];
    char slot0_name[260];
    int slot_map[EDITP_MAX_SOURCES];
    int w, h, frame_n, select_s, select_e, frame;
    int video_rate, video_scale, audio_rate, audio_ch;
    BOOL has_audio;
    int i, s;
    BOOL ok = FALSE;

    ZeroMemory(emb_cfg_blobs, sizeof(emb_cfg_blobs));
    ZeroMemory(emb_cfg_sizes, sizeof(emb_cfg_sizes));

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
    // +0x4c09c0: packed (vid_src_id<<24)|vid_src_frame
    // +0x4c09c4: packed (aud_src_id<<24)|aud_src_frame
    // +0x4c09c8, +0x4c09cc: internal cache dwords
    // +0x4c09d0: frame_inter (0..5)
    // +0x4c09d4: telecine phase (frame % 5)
    // +0x4c09d8: frame_status (KEYFRAME/MARKFRAME/DELFRAME/NULLFRAME)
    // +0x4c09dc: frame_config (profile index)
    // +0x4c09e0, +0x4c09e4: comp profile / HGLOBAL index
    if (!ar_read_u32(&rd, &tbl_frames) || tbl_frames > (uint32_t)(REAVIUTL_MAX_FRAME * 4))
    {
        goto cleanup;
    }

    if (tbl_frames > 0)
    {
        u32_0 = (uint8_t *)calloc((size_t)tbl_frames, 4u);
        u32_1 = (uint8_t *)calloc((size_t)tbl_frames, 4u);
        tbl_tmp = (uint8_t *)malloc((size_t)tbl_frames * 4u);
        u8_0 = (uint8_t *)calloc((size_t)tbl_frames, 1u);
        u8_1 = (uint8_t *)calloc((size_t)tbl_frames, 1u);
        u8_2 = (uint8_t *)calloc((size_t)tbl_frames, 1u);
        u8_3 = (uint8_t *)calloc((size_t)tbl_frames, 1u);
        if (!u32_0 || !u32_1 || !tbl_tmp || !u8_0 || !u8_1 || !u8_2 || !u8_3)
            goto cleanup;

        if (!ar_rle_read(&rd, u32_0, (size_t)tbl_frames * 4u) ||
            !ar_rle_read(&rd, u32_1, (size_t)tbl_frames * 4u) ||
            !ar_rle_read(&rd, tbl_tmp, (size_t)tbl_frames * 4u) ||
            !ar_rle_read(&rd, tbl_tmp, (size_t)tbl_frames * 4u) ||
            !ar_rle_read(&rd, u8_0, (size_t)tbl_frames) ||
            !ar_rle_read(&rd, u8_1, (size_t)tbl_frames) ||
            !ar_rle_read(&rd, u8_2, (size_t)tbl_frames) ||
            !ar_rle_read(&rd, u8_3, (size_t)tbl_frames) ||
            !ar_rle_read(&rd, tbl_tmp, (size_t)tbl_frames) ||
            !ar_rle_read(&rd, tbl_tmp, (size_t)tbl_frames))
        {
            goto cleanup;
        }
    }

    // read embedded profile blobs (96 slots @ +0x20D18) & skip HGLOBAL blobs (256 slots @ +0x4BBD98)
    for (i = 0; i < PROFILE_MAX_COUNT; i++)
    {
        if (eh[0x020D18u + (size_t)i * 0x104u] != 0)
        {
            uint32_t blob_sz = 0;
            if (!ar_read_u32(&rd, &blob_sz) || blob_sz > (64u * 1024u * 1024u))
                goto cleanup;
            if (blob_sz > 0)
            {
                emb_cfg_blobs[i] = (uint8_t *)malloc((size_t)blob_sz);
                if (!emb_cfg_blobs[i] || !ar_read_exact(&rd, emb_cfg_blobs[i], (size_t)blob_sz))
                    goto cleanup;
                emb_cfg_sizes[i] = blob_sz;
            }
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

    profile_cfg_clear_project_slots();
    if (emb_cfg_blobs[0] && emb_cfg_sizes[0] >= PROFILE_CFG_HEADER_SIZE)
    {
        char ini_path[MAX_PATH];
        get_aup_ini_path(ini_path, sizeof(ini_path));
        if (GetPrivateProfileIntA("system", "loadprojectprofile", 0, ini_path) == 0)
        {
            plugin_manager_apply_cfg_blob(emb_cfg_blobs[0], (size_t)emb_cfg_sizes[0], TRUE);
        }
    }
    for (i = 1; i < PROFILE_MAX_COUNT; i++)
    {
        if (eh[0x020D18u + (size_t)i * 0x104u] != 0 &&
            emb_cfg_blobs[i] && emb_cfg_sizes[i] >= PROFILE_CFG_HEADER_SIZE)
        {
            char pname[260];
            memcpy(pname, eh + 0x020D18u + (size_t)i * 0x104u, 260);
            pname[259] = '\0';
            profile_cfg_set_project_slot(i, pname, emb_cfg_blobs[i], (size_t)emb_cfg_sizes[i]);
        }
    }

    // open secondary multi-source slots 1..63 stored at +0x026E98 + slot * 0xC4BC
    for (s = 0; s < EDITP_MAX_SOURCES; s++)
    {
        slot_map[s] = (s == 0) ? 0 : -1;
    }
    for (s = 1; s < EDITP_MAX_SOURCES; s++)
    {
        char slot_path[260];
        memcpy(slot_path, eh + 0x026E98u + (size_t)s * 0xC4BCu, 260);
        slot_path[259] = '\0';
        if (slot_path[0])
        {
            slot_map[s] = try_add_candidate_source(ep, abs_aup, slot_path);
        }
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
    if (u32_0 && frame_n > 0)
    {
        int lim = ((uint32_t)frame_n < tbl_frames) ? frame_n : (int)tbl_frames;
        BOOL legacy_reaviutl = (tbl_frames >= 2u &&
                                rd_u32(u32_0 + 4u) == 0u &&
                                rd_u32(u32_1 + 4u) == 1u &&
                                u8_1[1] == 0u);

        for (i = 0; i < lim; i++)
        {
            if (legacy_reaviutl)
            {
                if (u8_0[i] != 0)
                    editp_set_frame_status(ep, i, u8_0[i]);
            }
            else
            {
                uint32_t pv = rd_u32(u32_0 + (size_t)i * 4u) + 0x100u;
                uint32_t pa = rd_u32(u32_1 + (size_t)i * 4u) + 0x100u;
                int v_slot = (int)(pv >> 24);
                int v_frm = (int)(pv & 0xFFFFFFu) - 0x100;
                int a_slot = (int)(pa >> 24);
                int a_frm = (int)(pa & 0xFFFFFFu) - 0x100;
                uint32_t mapped_vid_id = 0u;
                uint32_t mapped_aud_id = 0u;
                BYTE st = (BYTE)(u8_2[i] & 0x7Fu);

                if (v_slot >= 0 && v_slot < EDITP_MAX_SOURCES && slot_map[v_slot] >= 0)
                    mapped_vid_id = (uint32_t)slot_map[v_slot];
                if (a_slot >= 0 && a_slot < EDITP_MAX_SOURCES && slot_map[a_slot] >= 0)
                    mapped_aud_id = (uint32_t)slot_map[a_slot];
                if (v_frm < 0)
                    v_frm = 0;
                if (a_frm < 0)
                    a_frm = 0;

                if (mapped_vid_id != 0u || v_frm != i || mapped_aud_id != 0u || a_frm != i)
                {
                    editp_set_frame_mapping(ep, i, mapped_vid_id, (uint32_t)v_frm,
                                            mapped_aud_id, (uint32_t)a_frm);
                }
                if (st != 0)
                    editp_set_frame_status(ep, i, st);
                if (u8_0[i] != 0)
                    editp_set_frame_inter(ep, i, u8_0[i]);
                if (u8_3[i] != 0)
                    editp_set_frame_config(ep, i, u8_3[i]);
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
    for (i = 0; i < PROFILE_MAX_COUNT; i++)
    {
        free(emb_cfg_blobs[i]);
    }
    free(chunk_buf);
    free(u8_3);
    free(u8_2);
    free(u8_1);
    free(u8_0);
    free(tbl_tmp);
    free(u32_1);
    free(u32_0);
    free(eh);
    ar_free(&rd);
    CloseHandle(hFile);
    return ok;
}

// 0x00424160, save .aup project

static void fill_source_file_info_block(uint8_t *src_blk, int w, int h, int frame_n,
                                        int fps_rate, int fps_scale,
                                        int audio_rate, int audio_ch, BOOL has_audio,
                                        int slot_id)
{
    uint32_t yc_img_sz = (uint32_t)w * (uint32_t)h * 6u;
    uint16_t block_align = (uint16_t)(audio_ch * 2);
    uint32_t avg_bps = (uint32_t)audio_rate * (uint32_t)block_align;

    wr_u32(src_blk + 0x0000u, has_audio ? 0x07u : 0x03u);
    wr_i32(src_blk + 0x0004u, w);
    wr_i32(src_blk + 0x0008u, h);
    wr_i32(src_blk + 0x000Cu, frame_n);

    wr_u32(src_blk + 0x0010u, 40u);
    wr_i32(src_blk + 0x0014u, w);
    wr_i32(src_blk + 0x0018u, h);
    wr_u16(src_blk + 0x001Cu, 1u);
    wr_u16(src_blk + 0x001Eu, 48u);
    wr_u32(src_blk + 0x0020u, FOURCC_YC48);
    wr_u32(src_blk + 0x0024u, yc_img_sz);

    wr_u16(src_blk + 0x0038u, 1u);
    wr_u16(src_blk + 0x003Au, (uint16_t)audio_ch);
    wr_u32(src_blk + 0x003Cu, (uint32_t)audio_rate);
    wr_u32(src_blk + 0x0040u, avg_bps);
    wr_u16(src_blk + 0x0044u, block_align);
    wr_u16(src_blk + 0x0046u, 16u);

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

    wr_u32(src_blk + 0x01ACu, 40u);
    memcpy(src_blk + 0x01B0u, src_blk + 0x0010u, 40u);
    wr_u32(src_blk + 0x21ACu, 40u);
    memcpy(src_blk + 0x21B0u, src_blk + 0x0038u, 18u);
    wr_i32(src_blk + 0xC398u, slot_id);
    wr_i32(src_blk + 0xC39Cu, 1204);
}

static void populate_aviutl_edit_handle(uint8_t *eh, EDITP *ep, const char *abs_aup)
{
    int w = 0, h = 0, max_w = 0, max_h = 0;
    int fps_rate = 30, fps_scale = 1;
    int frame_n, frame, select_s = 0, select_e = 0;
    int audio_rate, audio_ch;
    BOOL has_audio;
    uint32_t rgb_stride, rgb_img_sz;
    int src_count, s;
    const char *src0_path;

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
    fill_source_file_info_block(eh + 0x0003C0u, w, h, frame_n,
                                fps_rate, fps_scale, audio_rate, audio_ch, has_audio, -1);

    // mirror source slots[0..N-1] at +0x26E98 + slot * 0xC4BC
    src0_path = editp_get_source_path(ep, 0);
    lstrcpynA((char *)(eh + 0x026E98u),
              (src0_path && src0_path[0]) ? src0_path : editp_get_edit_path(ep), 260);
    memcpy(eh + 0x026E98u + 0x0104u, eh + 0x0003C0u, 0xC3B8u);
    wr_i32(eh + 0x026E98u + 0x0104u + 0xC398u, 0);

    src_count = editp_get_source_count(ep);
    for (s = 1; s < src_count && s < EDITP_MAX_SOURCES; s++)
    {
        const char *spath = editp_get_source_path(ep, s);
        FILE_INFO sfi;
        uint8_t *slot_base = eh + 0x026E98u + (size_t)s * 0xC4BCu;
        if (!spath || !spath[0])
            continue;
        lstrcpynA((char *)slot_base, spath, 260);
        if (editp_get_source_file_info(ep, &sfi, s))
        {
            fill_source_file_info_block(slot_base + 0x0104u,
                                        sfi.w > 0 ? sfi.w : w,
                                        sfi.h > 0 ? sfi.h : h,
                                        sfi.frame_n > 0 ? sfi.frame_n : 1,
                                        sfi.video_rate > 0 ? sfi.video_rate : fps_rate,
                                        sfi.video_scale > 0 ? sfi.video_scale : fps_scale,
                                        sfi.audio_rate > 0 ? sfi.audio_rate : audio_rate,
                                        sfi.audio_ch > 0 ? sfi.audio_ch : audio_ch,
                                        (sfi.flag & FILE_INFO_FLAG_AUDIO) != 0,
                                        s);
        }
    }

    // embedded profile names table at +0x020D18 (96 slots * 260 bytes, 0x00424475)
    {
        const char *p0 = profile_cfg_get_slot_name(0);
        lstrcpynA((char *)(eh + 0x020D18u),
                  (p0 && p0[0]) ? p0 : "Default", 260);
        for (s = 1; s < PROFILE_MAX_COUNT; s++)
        {
            if (profile_cfg_has_project_slot(s))
            {
                const char *ps = profile_cfg_get_slot_name(s);
                if (ps && ps[0])
                {
                    lstrcpynA((char *)(eh + 0x020D18u + (size_t)s * 0x104u), ps, 260);
                }
            }
        }
    }
}

BOOL aup_save_local(const char *aup_path, EDITP *ep)
{
    char abs_aup[MAX_PATH];
    HANDLE hFile = INVALID_HANDLE_VALUE;
    AupWriter wr;
    uint8_t *eh = NULL;
    uint8_t *u32_vid = NULL;
    uint8_t *u32_aud = NULL;
    uint8_t *u32_zeros = NULL;
    uint8_t *u8_inter = NULL;
    uint8_t *u8_telecine = NULL;
    uint8_t *u8_status = NULL;
    uint8_t *u8_config = NULL;
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
    u32_vid = (uint8_t *)malloc((size_t)frame_n * 4u);
    u32_aud = (uint8_t *)malloc((size_t)frame_n * 4u);
    u32_zeros = (uint8_t *)calloc((size_t)frame_n, 4u);
    u8_inter = (uint8_t *)calloc((size_t)frame_n, 1u);
    u8_telecine = (uint8_t *)malloc((size_t)frame_n);
    u8_status = (uint8_t *)calloc((size_t)frame_n, 1u);
    u8_config = (uint8_t *)calloc((size_t)frame_n, 1u);
    u8_zeros = (uint8_t *)calloc((size_t)frame_n, 1u);
    if (!eh || !u32_vid || !u32_aud || !u32_zeros ||
        !u8_inter || !u8_telecine || !u8_status || !u8_config || !u8_zeros)
    {
        goto cleanup_mem;
    }

    populate_aviutl_edit_handle(eh, ep, abs_aup);

    for (i = 0; i < frame_n; i++)
    {
        uint32_t vid_id = 0u, vid_f = (uint32_t)i;
        uint32_t aud_id = 0u, aud_f = (uint32_t)i;
        BYTE st = 0;

        editp_get_frame_mapping(ep, i, &vid_id, &vid_f, &aud_id, &aud_f);
        if (vid_id >= EDITP_MAX_SOURCES)
            vid_id = 0u;
        if (aud_id >= EDITP_MAX_SOURCES)
            aud_id = 0u;

        wr_u32(u32_vid + (size_t)i * 4u, ((vid_id & 0xFFu) << 24) | (vid_f & 0xFFFFFFu));
        wr_u32(u32_aud + (size_t)i * 4u, ((aud_id & 0xFFu) << 24) | (aud_f & 0xFFFFFFu));

        u8_inter[i] = editp_get_frame_inter(ep, i);
        u8_telecine[i] = (uint8_t)(i % 5);
        if (editp_get_frame_status(ep, i, &st))
            u8_status[i] = st;
        u8_config[i] = editp_get_frame_config(ep, i);
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
        !aw_rle_write(&wr, u32_vid, (size_t)frame_n * 4u) ||
        !aw_rle_write(&wr, u32_aud, (size_t)frame_n * 4u) ||
        !aw_rle_write(&wr, u32_zeros, (size_t)frame_n * 4u) ||
        !aw_rle_write(&wr, u32_zeros, (size_t)frame_n * 4u) ||
        !aw_rle_write(&wr, u8_inter, (size_t)frame_n) ||
        !aw_rle_write(&wr, u8_telecine, (size_t)frame_n) ||
        !aw_rle_write(&wr, u8_status, (size_t)frame_n) ||
        !aw_rle_write(&wr, u8_config, (size_t)frame_n) ||
        !aw_rle_write(&wr, u8_zeros, (size_t)frame_n) ||
        !aw_rle_write(&wr, u8_zeros, (size_t)frame_n))
    {
        goto cleanup_file;
    }

    // embedded profile .cfg blobs for populated slots 0..95 (0x00424475)
    for (i = 0; i < PROFILE_MAX_COUNT; i++)
    {
        if (eh[0x020D18u + (size_t)i * 0x104u] != 0)
        {
            const uint8_t *pblob = NULL;
            size_t psz = 0;
            profile_cfg_get_slot_blob(i, &pblob, &psz);
            if (!aw_write_u32(&wr, (uint32_t)psz))
            {
                goto cleanup_file;
            }
            if (psz > 0 && pblob && !aw_write(&wr, pblob, psz))
            {
                goto cleanup_file;
            }
        }
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

    // re-emit any preserved chunks from unloaded third-party / 32-bit bridged plugins
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
    free(u8_config);
    free(u8_status);
    free(u8_telecine);
    free(u8_inter);
    free(u32_zeros);
    free(u32_aud);
    free(u32_vid);
    free(eh);
    return ok;
}

BOOL aup_harvest_unmatched_chunks(const char *aup_path)
{
    char abs_aup[MAX_PATH];
    HANDLE hFile = INVALID_HANDLE_VALUE;
    AupReader rd;
    uint8_t *eh = NULL;
    uint8_t *tbl_tmp = NULL;
    uint8_t *chunk_buf = NULL;
    size_t chunk_cap = 0;
    char magic1[AUP_MAGIC_LEN + 1];
    char magic2[AUP_MAGIC_LEN + 1];
    uint32_t eh_size = 0;
    uint32_t tbl_frames = 0;
    int i;
    BOOL ok = FALSE;

    if (!aup_path || !aup_path[0])
        return FALSE;
    if (GetFullPathNameA(aup_path, MAX_PATH, abs_aup, NULL) == 0)
        lstrcpynA(abs_aup, aup_path, MAX_PATH);

    hFile = CreateFileA(abs_aup, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!ar_init(&rd, hFile))
    {
        CloseHandle(hFile);
        return FALSE;
    }

    ZeroMemory(magic1, sizeof(magic1));
    if (!ar_read_exact(&rd, magic1, AUP_MAGIC_LEN) ||
        lstrcmpA(magic1, AUP_MAGIC_018) != 0 ||
        !ar_read_u32(&rd, &eh_size) ||
        eh_size != AUP_EDIT_HANDLE_SIZE)
    {
        goto cleanup;
    }

    eh = (uint8_t *)calloc(1, (size_t)eh_size);
    if (!eh)
        goto cleanup;
    if (!ar_read_exact(&rd, eh, AUP_UNCOMPRESSED_PREFIX) ||
        !ar_rle_read(&rd, eh + AUP_UNCOMPRESSED_PREFIX,
                     (size_t)(eh_size - AUP_UNCOMPRESSED_PREFIX)))
    {
        goto cleanup;
    }

    if (!ar_read_u32(&rd, &tbl_frames) || tbl_frames > (uint32_t)(REAVIUTL_MAX_FRAME * 4))
        goto cleanup;

    if (tbl_frames > 0)
    {
        tbl_tmp = (uint8_t *)malloc((size_t)tbl_frames * 4u);
        if (!tbl_tmp)
            goto cleanup;
        for (i = 0; i < 4; i++)
        {
            if (!ar_rle_read(&rd, tbl_tmp, (size_t)tbl_frames * 4u))
                goto cleanup;
        }
        for (i = 0; i < 6; i++)
        {
            if (!ar_rle_read(&rd, tbl_tmp, (size_t)tbl_frames))
                goto cleanup;
        }
    }

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

    ZeroMemory(magic2, sizeof(magic2));
    if (!ar_read_exact(&rd, magic2, AUP_MAGIC_LEN) ||
        lstrcmpA(magic2, AUP_MAGIC_018) != 0)
    {
        goto cleanup;
    }

    for (;;)
    {
        char fp_magic[AUP_FILTER_MAGIC_LEN + 1];
        char flt_name[512];
        uint32_t name_len = 0;
        uint32_t data_size = 0;
        size_t got_hdr;
        int fcount, fi;
        BOOL native_saved = FALSE;

        ZeroMemory(fp_magic, sizeof(fp_magic));
        got_hdr = ar_read_some(&rd, fp_magic, AUP_FILTER_MAGIC_LEN);
        if (got_hdr == 0)
            break;
        if (got_hdr != AUP_FILTER_MAGIC_LEN || lstrcmpA(fp_magic, AUP_FILTER_MAGIC) != 0)
            break;

        if (!ar_read_u32(&rd, &name_len) || name_len == 0 || name_len >= sizeof(flt_name))
            goto cleanup;
        if (!ar_read_exact(&rd, flt_name, (size_t)name_len))
            goto cleanup;
        flt_name[name_len - 1] = '\0';

        if (!ar_read_u32(&rd, &data_size) || data_size > AUP_MAX_CHUNK_BYTES)
            goto cleanup;
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
                goto cleanup;
        }

        fcount = plugin_manager_get_count();
        for (fi = 0; fi < fcount; fi++)
        {
            FILTER *fp = plugin_manager_get_filter(fi);
            if (fp && fp->name && fp->func_project_save && lstrcmpA(fp->name, flt_name) == 0)
            {
                native_saved = TRUE;
                break;
            }
        }
        if (!native_saved)
        {
            preserve_unmatched_chunk(flt_name, chunk_buf, data_size);
        }
    }
    ok = TRUE;

cleanup:
    free(chunk_buf);
    free(tbl_tmp);
    free(eh);
    ar_free(&rd);
    CloseHandle(hFile);
    return ok;
}
