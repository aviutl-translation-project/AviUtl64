#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <msacm.h>
#include "vfw_input.h"
#include "yc48_convert.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WAVE_FORMAT_IEEE_FLOAT
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#endif
#ifndef WAVE_FORMAT_MPEGLAYER3
#define WAVE_FORMAT_MPEGLAYER3 0x0055
#endif
#ifndef WAVE_FORMAT_EXTENSIBLE
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#endif
#ifndef MPEGLAYER3_WFX_EXTRA_BYTES
#define MPEGLAYER3_WFX_EXTRA_BYTES 12
#define MPEGLAYER3_ID_MPEG 1
#define MPEGLAYER3_FLAG_PADDING_ISO 0x00000000
#define MPEGLAYER3_FLAG_PADDING_ON 0x00000001
#define MPEGLAYER3_FLAG_PADDING_OFF 0x00000002
#endif

typedef enum
{
    INPUT_KIND_VFW = 0,
    INPUT_KIND_EXEDIT_CANVAS = 1,
    INPUT_KIND_STILL_IMAGE = 2,
    INPUT_KIND_AUDIO_FILE = 3
} InputKind;

typedef enum
{
    AUDIO_SRC_NONE = 0,
    AUDIO_SRC_SILENT = 1,  // .exedit virtual canvas
    AUDIO_SRC_WAV_PCM = 2, // uncompressed WAV (PCM 8/16/24/32 or IEEE Float 32/64)
    AUDIO_SRC_WAV_ACM = 3, // compressed WAV chunk decoded via msacm32.dll
    AUDIO_SRC_MP3_ACM = 4, // indexed MPEG-1/2/2.5 Layer III frames via msacm32.dll
    AUDIO_SRC_VFW_PCM = 5, // uncompressed AVI audio stream via AVIStreamRead
    AUDIO_SRC_VFW_ACM = 6  // compressed AVI audio stream via AVIStreamRead + ACM
} AudioSrcKind;

typedef struct
{
    uint32_t offset;
    uint16_t size;
    uint16_t pad;
} Mp3FrameEntry;

struct VFWInput
{
    InputKind kind;
    AudioSrcKind audio_kind;
    char filepath[MAX_PATH];

    PAVIFILE pavi_file;
    PAVISTREAM pavi_stream;
    PAVISTREAM pavi_audio;
    PGETFRAME pgf;

    int width;
    int height;
    int frame_count;
    int fps_num;
    int fps_den;
    int decode_bpp; // 24 or 32

    int src_audio_rate;
    int src_audio_ch; // 1 or 2 exposed to aviutl/exedit
    int src_audio_bits;
    int src_audio_samples;
    int req_audio_rate;
    int req_audio_ch;

    HANDLE hfile;
    uint64_t wav_data_offset;
    uint64_t wav_data_bytes;
    uint64_t wav_cur_byte;
    int wav_block_align;
    int wav_avg_bps;
    int wav_raw_ch;
    BOOL wav_is_float;

    Mp3FrameEntry *mp3_frames;
    int mp3_frame_count;
    int mp3_spf; // 1152 (MPEG-1) or 576 (MPEG-2/2.5)
    int mp3_next_frame;

    LONG vfw_cur_pos;
    LONG vfw_end_pos;
    int vfw_block_align;
    BOOL vfw_is_float;
    int vfw_raw_ch;

    // Win32 ACM decoder state (0x00401050)
    HACMSTREAM acm_stream;
    ACMSTREAMHEADER acm_hdr;
    BOOL acm_hdr_prepared;
    unsigned char *acm_src_buf;
    DWORD acm_src_cap;
    DWORD acm_src_len;
    unsigned char *acm_dst_buf;
    DWORD acm_dst_cap;
    int acm_out_ch;
    int acm_out_bits;
    BOOL acm_need_start;
    BOOL acm_eof_flushed;

    // rolling decoded 16-bit PCM cache (interleaved src_audio_ch channels @ src_audio_rate)
    short *pcm_cache;
    int pcm_cache_cap;
    int pcm_cache_start;
    int pcm_cache_len;
    int pcm_decode_pos;

    // reusable scratch buffer for resampling source reads
    short *resample_tmp;
    int resample_tmp_cap;

    PIXEL *rgb_buffer;         // top-down PIXEL[width * height]
    unsigned char *dib_buffer; // bottom-up BGR24 or BGRA32 for avi_file_get_video_pixelp
    size_t dib_size;
};

static BOOL g_vfw_initialized = FALSE;

static void vfw_ensure_initialized(void)
{
    if (!g_vfw_initialized)
    {
        AVIFileInit();
        g_vfw_initialized = TRUE;
    }
}

static short clamp_pcm16(int v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (short)v;
}

static BOOL file_read_at(HANDLE hf, uint64_t offset, void *buf, DWORD bytes, DWORD *out_got)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    if (out_got)
        *out_got = 0;
    if (!hf || hf == INVALID_HANDLE_VALUE)
        return FALSE;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(hf, li, NULL, FILE_BEGIN))
        return FALSE;
    if (!ReadFile(hf, buf, bytes, &got, NULL))
        return FALSE;
    if (out_got)
        *out_got = got;
    return TRUE;
}

static const char *path_ext(const char *filepath)
{
    const char *dot = NULL;
    const unsigned char *p = (const unsigned char *)filepath;
    if (!filepath)
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
            if (*p == '\\' || *p == '/')
                dot = NULL;
            else if (*p == '.')
                dot = (const char *)p;
            p++;
        }
    }
    return dot ? dot : "";
}

// pcm / float to interleaved pcm16
static short decode_one_pcm_channel(const unsigned char *p, int bits, BOOL is_float)
{
    if (is_float)
    {
        if (bits == 32)
        {
            float f = 0.0f;
            int v;
            memcpy(&f, p, sizeof(float));
            if (!(f == f))
                return 0; // nan guard
            v = (int)(f * 32767.0f);
            return clamp_pcm16(v);
        }
        if (bits == 64)
        {
            double d = 0.0;
            int v;
            memcpy(&d, p, sizeof(double));
            if (!(d == d))
                return 0;
            v = (int)(d * 32767.0);
            return clamp_pcm16(v);
        }
        return 0;
    }

    if (bits == 8)
    {
        return (short)(((int)p[0] - 128) << 8);
    }
    if (bits == 16)
    {
        int16_t s = 0;
        memcpy(&s, p, sizeof(int16_t));
        return (short)s;
    }
    if (bits == 24)
    {
        int32_t s24 = ((int32_t)p[0] << 8) | ((int32_t)p[1] << 16) | ((int32_t)(signed char)p[2] << 24);
        return (short)(s24 >> 16);
    }
    if (bits == 32)
    {
        int32_t s32 = 0;
        memcpy(&s32, p, sizeof(int32_t));
        return (short)(s32 >> 16);
    }
    return 0;
}

static void convert_raw_frames_to_pcm16(const unsigned char *raw, int frames,
                                        int raw_ch, int block_align, int bits, BOOL is_float,
                                        short *dst, int dst_ch)
{
    int i;
    int bytes_per_ch = bits / 8;
    if (bytes_per_ch < 1)
        bytes_per_ch = 2;

    for (i = 0; i < frames; i++)
    {
        const unsigned char *frame_p = raw + (size_t)i * (size_t)block_align;
        short l = decode_one_pcm_channel(frame_p, bits, is_float);
        short r = (raw_ch >= 2) ? decode_one_pcm_channel(frame_p + bytes_per_ch, bits, is_float) : l;
        if (dst_ch == 1)
        {
            dst[i] = (short)(((int)l + (int)r) / 2);
        }
        else
        {
            dst[i * 2 + 0] = l;
            dst[i * 2 + 1] = r;
        }
    }
}

// Win32 ACM (msacm32.dll) decoder engine + rolling PCM16 cache
static BOOL acm_init_decoder(VFWInput *input, WAVEFORMATEX *pwfx_src)
{
    WAVEFORMATEX dst_wfx;
    MMRESULT mmr;
    int out_ch;

    if (!input || !pwfx_src || pwfx_src->nSamplesPerSec == 0 || pwfx_src->nChannels == 0)
    {
        return FALSE;
    }

    out_ch = (pwfx_src->nChannels >= 2) ? 2 : 1;

    memset(&dst_wfx, 0, sizeof(dst_wfx));
    dst_wfx.wFormatTag = WAVE_FORMAT_PCM;
    dst_wfx.nChannels = (WORD)out_ch;
    dst_wfx.nSamplesPerSec = pwfx_src->nSamplesPerSec;
    dst_wfx.wBitsPerSample = 16;
    dst_wfx.nBlockAlign = (WORD)(dst_wfx.nChannels * 2);
    dst_wfx.nAvgBytesPerSec = dst_wfx.nSamplesPerSec * (DWORD)dst_wfx.nBlockAlign;

    mmr = acmStreamOpen(&input->acm_stream, NULL, pwfx_src, &dst_wfx,
                        NULL, 0, 0, ACM_STREAMOPENF_NONREALTIME);
    if (mmr != 0 && pwfx_src->nChannels != dst_wfx.nChannels)
    {
        dst_wfx.nChannels = pwfx_src->nChannels;
        dst_wfx.nBlockAlign = (WORD)(dst_wfx.nChannels * 2);
        dst_wfx.nAvgBytesPerSec = dst_wfx.nSamplesPerSec * (DWORD)dst_wfx.nBlockAlign;
        mmr = acmStreamOpen(&input->acm_stream, NULL, pwfx_src, &dst_wfx,
                            NULL, 0, 0, ACM_STREAMOPENF_NONREALTIME);
    }
    if (mmr != 0)
    {
        // fallback: ask ACM for its native suggested PCM format (0x004011f0)
        memset(&dst_wfx, 0, sizeof(dst_wfx));
        dst_wfx.wFormatTag = WAVE_FORMAT_PCM;
        if (acmFormatSuggest(NULL, pwfx_src, &dst_wfx, sizeof(dst_wfx),
                             ACM_FORMATSUGGESTF_WFORMATTAG) == 0 &&
            dst_wfx.nSamplesPerSec == pwfx_src->nSamplesPerSec &&
            (dst_wfx.wBitsPerSample == 16 || dst_wfx.wBitsPerSample == 8))
        {
            mmr = acmStreamOpen(&input->acm_stream, NULL, pwfx_src, &dst_wfx,
                                NULL, 0, 0, ACM_STREAMOPENF_NONREALTIME);
        }
    }
    if (mmr != 0 || !input->acm_stream)
    {
        input->acm_stream = NULL;
        return FALSE;
    }

    input->acm_out_ch = (int)dst_wfx.nChannels;
    input->acm_out_bits = (int)dst_wfx.wBitsPerSample;
    input->acm_src_cap = 65536;
    input->acm_dst_cap = 262144;
    input->acm_src_buf = (unsigned char *)calloc(1, input->acm_src_cap);
    input->acm_dst_buf = (unsigned char *)calloc(1, input->acm_dst_cap);
    if (!input->acm_src_buf || !input->acm_dst_buf)
    {
        acmStreamClose(input->acm_stream, 0);
        input->acm_stream = NULL;
        return FALSE;
    }

    // prepare header at full buffer capacities (0x00401050)
    memset(&input->acm_hdr, 0, sizeof(input->acm_hdr));
    input->acm_hdr.cbStruct = sizeof(input->acm_hdr);
    input->acm_hdr.pbSrc = input->acm_src_buf;
    input->acm_hdr.cbSrcLength = input->acm_src_cap;
    input->acm_hdr.pbDst = input->acm_dst_buf;
    input->acm_hdr.cbDstLength = input->acm_dst_cap;

    if (acmStreamPrepareHeader(input->acm_stream, &input->acm_hdr, 0) != 0)
    {
        acmStreamClose(input->acm_stream, 0);
        input->acm_stream = NULL;
        return FALSE;
    }

    input->acm_hdr_prepared = TRUE;
    input->acm_src_len = 0;
    input->acm_need_start = TRUE;
    input->acm_eof_flushed = FALSE;
    input->pcm_cache_cap = 65536;
    input->pcm_cache = (short *)calloc((size_t)input->pcm_cache_cap * 2u, sizeof(short));
    if (!input->pcm_cache)
    {
        return FALSE;
    }
    input->pcm_cache_start = 0;
    input->pcm_cache_len = 0;
    input->pcm_decode_pos = 0;
    return TRUE;
}

static BOOL pcm_cache_append(VFWInput *input, const unsigned char *raw_pcm,
                             int frames_produced, int keep_from_sample)
{
    int ch = input->src_audio_ch > 0 ? input->src_audio_ch : 2;
    int block_align = input->acm_out_ch * (input->acm_out_bits / 8);

    if (frames_produced <= 0 || block_align <= 0)
    {
        return TRUE;
    }

    // discard any preroll frames that precede keep_from_sample
    if (input->pcm_decode_pos + frames_produced <= keep_from_sample)
    {
        input->pcm_decode_pos += frames_produced;
        input->pcm_cache_start = input->pcm_decode_pos;
        input->pcm_cache_len = 0;
        return TRUE;
    }
    if (input->pcm_decode_pos < keep_from_sample)
    {
        int skip = keep_from_sample - input->pcm_decode_pos;
        raw_pcm += (size_t)skip * (size_t)block_align;
        frames_produced -= skip;
        input->pcm_decode_pos = keep_from_sample;
        input->pcm_cache_start = keep_from_sample;
        input->pcm_cache_len = 0;
    }

    // slide or grow cache if needed
    if (input->pcm_cache_len + frames_produced > input->pcm_cache_cap)
    {
        int can_drop = keep_from_sample - input->pcm_cache_start;
        if (can_drop > 0)
        {
            if (can_drop > input->pcm_cache_len)
                can_drop = input->pcm_cache_len;
            if (input->pcm_cache_len > can_drop)
            {
                memmove(input->pcm_cache,
                        input->pcm_cache + (size_t)can_drop * (size_t)ch,
                        (size_t)(input->pcm_cache_len - can_drop) * (size_t)ch * sizeof(short));
            }
            input->pcm_cache_start += can_drop;
            input->pcm_cache_len -= can_drop;
        }
        if (input->pcm_cache_len + frames_produced > input->pcm_cache_cap)
        {
            int new_cap = input->pcm_cache_cap * 2;
            short *nb;
            while (new_cap < input->pcm_cache_len + frames_produced + 4096)
            {
                new_cap *= 2;
            }
            nb = (short *)realloc(input->pcm_cache, (size_t)new_cap * 2u * sizeof(short));
            if (!nb)
                return FALSE;
            input->pcm_cache = nb;
            input->pcm_cache_cap = new_cap;
        }
    }

    convert_raw_frames_to_pcm16(
        raw_pcm, frames_produced,
        input->acm_out_ch, block_align, input->acm_out_bits, FALSE,
        input->pcm_cache + (size_t)input->pcm_cache_len * (size_t)ch, ch);

    input->pcm_cache_len += frames_produced;
    input->pcm_decode_pos += frames_produced;
    return TRUE;
}

static BOOL acm_convert_step(VFWInput *input, BOOL is_eof, int keep_from_sample, int *out_produced)
{
    DWORD flags = ACM_STREAMCONVERTF_BLOCKALIGN;
    int block_align = input->acm_out_ch * (input->acm_out_bits / 8);
    int produced = 0;

    if (out_produced)
        *out_produced = 0;
    if (!input->acm_stream || block_align <= 0)
        return FALSE;
    if (input->acm_src_len == 0 && !is_eof)
        return TRUE;

    if (input->acm_need_start)
    {
        flags |= ACM_STREAMCONVERTF_START;
        input->acm_need_start = FALSE;
    }
    if (is_eof)
    {
        flags = ACM_STREAMCONVERTF_END;
    }

    input->acm_hdr.cbSrcLength = input->acm_src_len;
    input->acm_hdr.cbSrcLengthUsed = 0;
    input->acm_hdr.cbDstLength = input->acm_dst_cap;
    input->acm_hdr.cbDstLengthUsed = 0;

    if (acmStreamConvert(input->acm_stream, &input->acm_hdr, flags) != 0)
    {
        return FALSE;
    }

    if (input->acm_hdr.cbSrcLengthUsed > 0)
    {
        if (input->acm_hdr.cbSrcLengthUsed < input->acm_src_len)
        {
            memmove(input->acm_src_buf,
                    input->acm_src_buf + input->acm_hdr.cbSrcLengthUsed,
                    input->acm_src_len - input->acm_hdr.cbSrcLengthUsed);
            input->acm_src_len -= input->acm_hdr.cbSrcLengthUsed;
        }
        else
        {
            input->acm_src_len = 0;
        }
    }

    produced = (int)(input->acm_hdr.cbDstLengthUsed / (DWORD)block_align);
    if (produced > 0)
    {
        if (!pcm_cache_append(input, input->acm_dst_buf, produced, keep_from_sample))
        {
            return FALSE;
        }
    }
    if (out_produced)
        *out_produced = produced;
    return TRUE;
}

// MPEG-1 / MPEG-2 / MPEG-2.5 Layer III (.mp3) parser & frame indexer
static BOOL parse_mp3_header(const unsigned char b[4],
                             int *out_ver, int *out_sr, int *out_ch,
                             int *out_spf, int *out_frame_bytes, int *out_kbps)
{
    static const int s_kbps_v1_l3[16] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int s_kbps_v2_l3[16] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int s_srate_v1[4] = {44100, 48000, 32000, 0};
    static const int s_srate_v2[4] = {22050, 24000, 16000, 0};
    static const int s_srate_v25[4] = {11025, 12000, 8000, 0};

    uint32_t hdr = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                   ((uint32_t)b[2] << 8) | (uint32_t)b[3];
    int ver, layer, br_idx, sr_idx, pad, mode, kbps, srate, spf, fsize;

    if ((hdr & 0xFFE00000u) != 0xFFE00000u)
        return FALSE;
    ver = (int)((hdr >> 19) & 3u);
    layer = (int)((hdr >> 17) & 3u);
    br_idx = (int)((hdr >> 12) & 0xFu);
    sr_idx = (int)((hdr >> 10) & 3u);
    pad = (int)((hdr >> 9) & 1u);
    mode = (int)((hdr >> 6) & 3u);

    // require valid version (MPEG-1=3, MPEG-2=2, MPEG-2.5=0) and Layer III (1)
    if (ver == 1 || layer != 1 || br_idx == 0 || br_idx == 15 || sr_idx == 3)
    {
        return FALSE;
    }

    if (ver == 3)
    {
        kbps = s_kbps_v1_l3[br_idx];
        srate = s_srate_v1[sr_idx];
        spf = 1152;
        fsize = (144000 * kbps) / srate + pad;
    }
    else
    {
        kbps = s_kbps_v2_l3[br_idx];
        srate = (ver == 2) ? s_srate_v2[sr_idx] : s_srate_v25[sr_idx];
        spf = 576;
        fsize = (72000 * kbps) / srate + pad;
    }

    if (fsize < 24 || fsize > 4608)
        return FALSE;

    if (out_ver)
        *out_ver = ver;
    if (out_sr)
        *out_sr = srate;
    if (out_ch)
        *out_ch = (mode == 3) ? 1 : 2;
    if (out_spf)
        *out_spf = spf;
    if (out_frame_bytes)
        *out_frame_bytes = fsize;
    if (out_kbps)
        *out_kbps = kbps;
    return TRUE;
}

static BOOL is_vbr_info_frame(const unsigned char *frame, int fsize, int ver, int ch)
{
    int side_info = (ver == 3) ? ((ch == 1) ? 17 : 32) : ((ch == 1) ? 9 : 17);
    int xing_off = 4 + side_info;
    if (xing_off + 8 <= fsize)
    {
        if (memcmp(frame + xing_off, "Xing", 4) == 0 ||
            memcmp(frame + xing_off, "Info", 4) == 0)
        {
            return TRUE;
        }
    }
    if (36 + 4 <= fsize && memcmp(frame + 36, "VBRI", 4) == 0)
    {
        return TRUE;
    }
    return FALSE;
}

static BOOL index_mp3_stream(VFWInput *input, HANDLE hf, uint64_t start_off, uint64_t end_off,
                             BOOL strict_probe)
{
    unsigned char *win;
    const DWORD WIN_SZ = 65536;
    uint64_t pos = start_off;
    Mp3FrameEntry *frames = NULL;
    int count = 0, cap = 0;
    int stream_ver = -1, stream_sr = 0, stream_ch = 0, stream_spf = 0;
    uint64_t total_frame_bytes = 0;
    BOOL checked_first_vbr = FALSE;
    MPEGLAYER3WAVEFORMAT mp3wfx;

    if (end_off <= start_off + 24)
        return FALSE;

    win = (unsigned char *)malloc(WIN_SZ);
    if (!win)
        return FALSE;

    // skip leading ID3v2 tags (supports stacked ID3v2 headers)
    while (pos + 10 <= end_off)
    {
        DWORD got = 0;
        if (!file_read_at(hf, pos, win, 10, &got) || got < 10)
            break;
        if (win[0] == 'I' && win[1] == 'D' && win[2] == '3' &&
            win[3] < 0xFF && win[4] < 0xFF &&
            ((win[6] | win[7] | win[8] | win[9]) & 0x80) == 0)
        {
            uint32_t tag_sz = ((uint32_t)win[6] << 21) | ((uint32_t)win[7] << 14) |
                              ((uint32_t)win[8] << 7) | (uint32_t)win[9];
            pos += 10u + (uint64_t)tag_sz + ((win[5] & 0x10) ? 10u : 0u);
        }
        else
        {
            break;
        }
    }

    // strip trailing 128-byte ID3v1 tag if present
    if (end_off >= pos + 128)
    {
        unsigned char tail[3] = {0};
        DWORD got = 0;
        if (file_read_at(hf, end_off - 128, tail, 3, &got) && got == 3 &&
            tail[0] == 'T' && tail[1] == 'A' && tail[2] == 'G')
        {
            end_off -= 128;
        }
    }

    // scan & index MPEG Layer III frames using a 64 KB sliding window
    {
        uint64_t win_file_off = pos;
        DWORD win_valid = 0;
        uint64_t search_limit = strict_probe ? (pos + 4096) : (pos + 131072);
        if (search_limit > end_off)
            search_limit = end_off;

        if (!file_read_at(hf, win_file_off, win, WIN_SZ, &win_valid) || win_valid < 4)
        {
            free(win);
            return FALSE;
        }

        while (pos + 4 <= end_off)
        {
            int ver = 0, sr = 0, ch = 0, spf = 0, fsize = 0, kbps = 0;
            size_t rel;

            // refill sliding buffer when fewer than 8 KB remain
            if (pos < win_file_off || pos + 4620 > win_file_off + win_valid)
            {
                DWORD to_read = WIN_SZ;
                if ((uint64_t)to_read > end_off - pos)
                {
                    to_read = (DWORD)(end_off - pos);
                }
                win_file_off = pos;
                if (!file_read_at(hf, win_file_off, win, to_read, &win_valid) || win_valid < 4)
                {
                    break;
                }
            }

            rel = (size_t)(pos - win_file_off);
            if (rel + 4 > win_valid)
                break;

            // stop at APEv2 footer/tag if encountered
            if (rel + 8 <= win_valid && memcmp(win + rel, "APETAGEX", 8) == 0)
            {
                break;
            }

            if (parse_mp3_header(win + rel, &ver, &sr, &ch, &spf, &fsize, &kbps) &&
                pos + (uint64_t)fsize <= end_off &&
                (stream_ver < 0 || (ver == stream_ver && sr == stream_sr)))
            {

                // verify next header at pos + fsize before locking onto stream
                BOOL sync_ok = (stream_ver >= 0);
                if (!sync_ok)
                {
                    uint64_t next_pos = pos + (uint64_t)fsize;
                    if (next_pos == end_off && !strict_probe)
                    {
                        sync_ok = TRUE;
                    }
                    else if (next_pos + 4 <= end_off && rel + (size_t)fsize + 4 <= win_valid)
                    {
                        int nv = 0, nsr = 0;
                        if (parse_mp3_header(win + rel + fsize, &nv, &nsr, NULL, NULL, NULL, NULL) &&
                            nv == ver && nsr == sr)
                        {
                            sync_ok = TRUE;
                        }
                    }
                }

                if (sync_ok)
                {
                    if (stream_ver < 0)
                    {
                        stream_ver = ver;
                        stream_sr = sr;
                        stream_ch = ch;
                        stream_spf = spf;
                    }

                    // skip leading Xing / Info / VBRI metadata-only frame
                    if (!checked_first_vbr)
                    {
                        checked_first_vbr = TRUE;
                        if (rel + (size_t)fsize <= win_valid &&
                            is_vbr_info_frame(win + rel, fsize, ver, ch))
                        {
                            pos += (uint64_t)fsize;
                            continue;
                        }
                    }

                    if (pos <= 0xFFFFFFFFull)
                    {
                        if (count >= cap)
                        {
                            int ncap = cap ? (cap * 2) : 4096;
                            Mp3FrameEntry *nf = (Mp3FrameEntry *)realloc(
                                frames, (size_t)ncap * sizeof(Mp3FrameEntry));
                            if (!nf)
                            {
                                free(frames);
                                free(win);
                                return FALSE;
                            }
                            frames = nf;
                            cap = ncap;
                        }
                        frames[count].offset = (uint32_t)pos;
                        frames[count].size = (uint16_t)fsize;
                        frames[count].pad = 0;
                        count++;
                        total_frame_bytes += (uint64_t)fsize;
                    }
                    pos += (uint64_t)fsize;
                    continue;
                }
            }

            // sync lost or not yet found
            if (stream_ver < 0 && pos >= search_limit)
            {
                break;
            }
            pos++;
        }
    }

    free(win);

    if (count <= 0 || stream_sr <= 0 || stream_spf <= 0)
    {
        free(frames);
        return FALSE;
    }

    input->audio_kind = AUDIO_SRC_MP3_ACM;
    input->mp3_frames = frames;
    input->mp3_frame_count = count;
    input->mp3_spf = stream_spf;
    input->mp3_next_frame = 0;
    input->src_audio_rate = stream_sr;
    input->src_audio_ch = stream_ch;
    input->src_audio_bits = 16;
    input->src_audio_samples = count * stream_spf;
    input->req_audio_rate = stream_sr;
    input->req_audio_ch = stream_ch;

    memset(&mp3wfx, 0, sizeof(mp3wfx));
    mp3wfx.wfx.wFormatTag = WAVE_FORMAT_MPEGLAYER3;
    mp3wfx.wfx.nChannels = (WORD)stream_ch;
    mp3wfx.wfx.nSamplesPerSec = (DWORD)stream_sr;
    mp3wfx.wfx.nAvgBytesPerSec = (DWORD)((total_frame_bytes * (uint64_t)stream_sr) /
                                         (uint64_t)input->src_audio_samples);
    if (mp3wfx.wfx.nAvgBytesPerSec == 0)
    {
        mp3wfx.wfx.nAvgBytesPerSec = 16000;
    }
    mp3wfx.wfx.nBlockAlign = 1;
    mp3wfx.wfx.wBitsPerSample = 0;
    mp3wfx.wfx.cbSize = MPEGLAYER3_WFX_EXTRA_BYTES;
    mp3wfx.wID = MPEGLAYER3_ID_MPEG;
    mp3wfx.fdwFlags = MPEGLAYER3_FLAG_PADDING_OFF;
    mp3wfx.nBlockSize = frames[0].size;
    mp3wfx.nFramesPerBlock = 1;
    mp3wfx.nCodecDelay = 0;

    if (!acm_init_decoder(input, &mp3wfx.wfx))
    {
        free(input->mp3_frames);
        input->mp3_frames = NULL;
        input->mp3_frame_count = 0;
        return FALSE;
    }
    return TRUE;
}

static BOOL try_open_wav_or_mp3_file(VFWInput *input, const char *filepath)
{
    HANDLE hf;
    LARGE_INTEGER fsz;
    unsigned char hdr[16] = {0};
    DWORD got = 0;
    const char *ext = path_ext(filepath);
    BOOL is_mp3_ext = (_stricmp(ext, ".mp3") == 0 || _stricmp(ext, ".mp2") == 0 ||
                       _stricmp(ext, ".mpa") == 0);

    hf = CreateFileA(filepath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    if (!GetFileSizeEx(hf, &fsz) || fsz.QuadPart < 28)
    {
        CloseHandle(hf);
        return FALSE;
    }
    if (!file_read_at(hf, 0, hdr, sizeof(hdr), &got) || got < 12)
    {
        CloseHandle(hf);
        return FALSE;
    }

    // RIFF WAVE container (PCM, IEEE Float, EXTENSIBLE, RIFF-MP3, ADPCM, etc.)
    if (memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0)
    {
        uint64_t pos = 12;
        uint64_t file_end = (uint64_t)fsz.QuadPart;
        WAVEFORMATEX *pwfx = NULL;
        DWORD cb_fmt = 0;
        uint64_t data_off = 0, data_sz = 0;
        BOOL have_data = FALSE;

        while (pos + 8 <= file_end)
        {
            unsigned char chdr[8];
            uint32_t csize = 0;
            if (!file_read_at(hf, pos, chdr, 8, &got) || got < 8)
                break;
            memcpy(&csize, chdr + 4, 4);
            pos += 8;

            if (memcmp(chdr, "fmt ", 4) == 0 && csize >= 16 && csize <= 65536 && !pwfx)
            {
                pwfx = (WAVEFORMATEX *)calloc(1, (size_t)csize + sizeof(WAVEFORMATEX));
                if (pwfx && file_read_at(hf, pos, pwfx, csize, &got) && got >= 16)
                {
                    cb_fmt = got;
                    if (cb_fmt < 18)
                    {
                        pwfx->cbSize = 0;
                    }
                    else if ((DWORD)pwfx->cbSize > cb_fmt - 18)
                    {
                        pwfx->cbSize = (WORD)(cb_fmt - 18);
                    }
                }
                else
                {
                    free(pwfx);
                    pwfx = NULL;
                }
            }
            else if (memcmp(chdr, "data", 4) == 0 && !have_data)
            {
                data_off = pos;
                data_sz = csize;
                if (data_off + data_sz > file_end)
                {
                    data_sz = file_end - data_off;
                }
                have_data = TRUE;
                if (pwfx)
                    break;
            }
            pos += ((uint64_t)csize + 1ull) & ~1ull;
        }

        if (pwfx && have_data && data_sz > 0 &&
            pwfx->nSamplesPerSec > 0 && pwfx->nChannels > 0)
        {
            WORD tag = pwfx->wFormatTag;
            WORD bits = pwfx->wBitsPerSample;

            // unwrap WAVE_FORMAT_EXTENSIBLE (0xFFFE) PCM / IEEE_FLOAT SubFormat GUID
            if (tag == WAVE_FORMAT_EXTENSIBLE && cb_fmt >= 40)
            {
                static const unsigned char s_guid_tail[12] = {
                    0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
                const unsigned char *sub = ((const unsigned char *)pwfx) + 24;
                uint32_t sub_tag = 0;
                memcpy(&sub_tag, sub, 4);
                if (memcmp(sub + 4, s_guid_tail, 12) == 0 &&
                    (sub_tag == WAVE_FORMAT_PCM || sub_tag == WAVE_FORMAT_IEEE_FLOAT))
                {
                    tag = (WORD)sub_tag;
                }
            }

            // direct uncompressed PCM / IEEE Float WAV
            if ((tag == WAVE_FORMAT_PCM && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
                (tag == WAVE_FORMAT_IEEE_FLOAT && (bits == 32 || bits == 64)))
            {
                int blk = (int)pwfx->nBlockAlign;
                if (blk <= 0)
                    blk = (int)pwfx->nChannels * (bits / 8);
                if (blk > 0)
                {
                    uint64_t nsmp = data_sz / (uint64_t)blk;
                    if (nsmp > 0x7FFFFFFFull)
                        nsmp = 0x7FFFFFFFull;
                    input->kind = INPUT_KIND_AUDIO_FILE;
                    input->audio_kind = AUDIO_SRC_WAV_PCM;
                    input->hfile = hf;
                    input->wav_data_offset = data_off;
                    input->wav_data_bytes = data_sz;
                    input->wav_block_align = blk;
                    input->wav_raw_ch = (int)pwfx->nChannels;
                    input->wav_is_float = (tag == WAVE_FORMAT_IEEE_FLOAT);
                    input->src_audio_rate = (int)pwfx->nSamplesPerSec;
                    input->src_audio_ch = (pwfx->nChannels >= 2) ? 2 : 1;
                    input->src_audio_bits = (int)bits;
                    input->src_audio_samples = (int)nsmp;
                    input->req_audio_rate = input->src_audio_rate;
                    input->req_audio_ch = input->src_audio_ch;
                    free(pwfx);
                    return TRUE;
                }
            }

            // riff-wrapped mp3 (0x0055)
            if (tag == WAVE_FORMAT_MPEGLAYER3)
            {
                if (index_mp3_stream(input, hf, data_off, data_off + data_sz, FALSE))
                {
                    input->kind = INPUT_KIND_AUDIO_FILE;
                    input->hfile = hf;
                    free(pwfx);
                    return TRUE;
                }
            }

            // any other ACM-supported compressed WAV (ADPCM, GSM, A-law, mu-law, etc.)
            input->src_audio_rate = (int)pwfx->nSamplesPerSec;
            input->src_audio_ch = (pwfx->nChannels >= 2) ? 2 : 1;
            input->src_audio_bits = 16;
            input->req_audio_rate = input->src_audio_rate;
            input->req_audio_ch = input->src_audio_ch;
            if (acm_init_decoder(input, pwfx))
            {
                DWORD est_dst_bytes = 0;
                int out_blk = input->acm_out_ch * (input->acm_out_bits / 8);
                input->kind = INPUT_KIND_AUDIO_FILE;
                input->audio_kind = AUDIO_SRC_WAV_ACM;
                input->hfile = hf;
                input->wav_data_offset = data_off;
                input->wav_data_bytes = data_sz;
                input->wav_cur_byte = 0;
                input->wav_block_align = (pwfx->nBlockAlign > 0) ? (int)pwfx->nBlockAlign : 1024;
                input->wav_avg_bps = (pwfx->nAvgBytesPerSec > 0) ? (int)pwfx->nAvgBytesPerSec : 16000;
                if (data_sz <= 0xFFFFFFFFull &&
                    acmStreamSize(input->acm_stream, (DWORD)data_sz, &est_dst_bytes,
                                  ACM_STREAMSIZEF_SOURCE) == 0 &&
                    out_blk > 0)
                {
                    input->src_audio_samples = (int)(est_dst_bytes / (DWORD)out_blk);
                }
                else
                {
                    input->src_audio_samples = (int)((data_sz * (uint64_t)input->src_audio_rate) /
                                                     (uint64_t)input->wav_avg_bps);
                }
                free(pwfx);
                return TRUE;
            }
        }
        free(pwfx);
        CloseHandle(hf);
        return FALSE;
    }

    // quick reject obvious still-image / container magic before probing MP3
    if ((hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G') ||
        (hdr[0] == 0xFF && hdr[1] == 0xD8 && hdr[2] == 0xFF) ||
        (hdr[0] == 'B' && hdr[1] == 'M') ||
        (memcmp(hdr, "GIF8", 4) == 0) ||
        (memcmp(hdr, "RIFF", 4) == 0))
    {
        CloseHandle(hf);
        return FALSE;
    }

    // raw / ID3-tagged MP3 file
    if (is_mp3_ext ||
        (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') ||
        ((hdr[0] == 0xFF) && ((hdr[1] & 0xE0) == 0xE0)))
    {
        if (index_mp3_stream(input, hf, 0, (uint64_t)fsz.QuadPart, !is_mp3_ext))
        {
            input->kind = INPUT_KIND_AUDIO_FILE;
            input->hfile = hf;
            return TRUE;
        }
    }

    CloseHandle(hf);
    return FALSE;
}

// minimal dynamic GDI+ Flat API loader for .png / .jpg / .bmp still images
typedef struct
{
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdipStartupInput;

typedef struct
{
    UINT Width;
    UINT Height;
    INT Stride;
    INT PixelFormat;
    VOID *Scan0;
    UINT_PTR Reserved;
} GdipBitmapData;

typedef int(WINAPI *PFN_GdiplusStartup)(ULONG_PTR *, const GdipStartupInput *, void *);
typedef int(WINAPI *PFN_GdipCreateBitmapFromFile)(const WCHAR *, void **);
typedef int(WINAPI *PFN_GdipGetImageWidth)(void *, UINT *);
typedef int(WINAPI *PFN_GdipGetImageHeight)(void *, UINT *);
typedef int(WINAPI *PFN_GdipBitmapLockBits)(void *, const void *, UINT, INT, GdipBitmapData *);
typedef int(WINAPI *PFN_GdipBitmapUnlockBits)(void *, GdipBitmapData *);
typedef int(WINAPI *PFN_GdipDisposeImage)(void *);

static BOOL g_gdip_tried = FALSE;
static BOOL g_gdip_ok = FALSE;
static PFN_GdipCreateBitmapFromFile pGdipCreateBitmapFromFile = NULL;
static PFN_GdipGetImageWidth pGdipGetImageWidth = NULL;
static PFN_GdipGetImageHeight pGdipGetImageHeight = NULL;
static PFN_GdipBitmapLockBits pGdipBitmapLockBits = NULL;
static PFN_GdipBitmapUnlockBits pGdipBitmapUnlockBits = NULL;
static PFN_GdipDisposeImage pGdipDisposeImage = NULL;

static BOOL ensure_gdiplus(void)
{
    HMODULE mod;
    PFN_GdiplusStartup pStartup;
    GdipStartupInput si;
    ULONG_PTR token = 0;

    if (g_gdip_tried)
    {
        return g_gdip_ok;
    }
    g_gdip_tried = TRUE;

    mod = LoadLibraryA("gdiplus.dll");
    if (!mod)
    {
        return FALSE;
    }
    pStartup = (PFN_GdiplusStartup)GetProcAddress(mod, "GdiplusStartup");
    pGdipCreateBitmapFromFile = (PFN_GdipCreateBitmapFromFile)GetProcAddress(mod, "GdipCreateBitmapFromFile");
    pGdipGetImageWidth = (PFN_GdipGetImageWidth)GetProcAddress(mod, "GdipGetImageWidth");
    pGdipGetImageHeight = (PFN_GdipGetImageHeight)GetProcAddress(mod, "GdipGetImageHeight");
    pGdipBitmapLockBits = (PFN_GdipBitmapLockBits)GetProcAddress(mod, "GdipBitmapLockBits");
    pGdipBitmapUnlockBits = (PFN_GdipBitmapUnlockBits)GetProcAddress(mod, "GdipBitmapUnlockBits");
    pGdipDisposeImage = (PFN_GdipDisposeImage)GetProcAddress(mod, "GdipDisposeImage");

    if (!pStartup || !pGdipCreateBitmapFromFile || !pGdipGetImageWidth ||
        !pGdipGetImageHeight || !pGdipBitmapLockBits || !pGdipBitmapUnlockBits ||
        !pGdipDisposeImage)
    {
        return FALSE;
    }

    memset(&si, 0, sizeof(si));
    si.GdiplusVersion = 1;
    if (pStartup(&token, &si, NULL) != 0)
    {
        return FALSE;
    }
    g_gdip_ok = TRUE;
    return TRUE;
}

static BOOL parse_exedit_canvas_spec(const char *filepath,
                                     int *w, int *h,
                                     int *rate, int *scale, int *hz)
{
    const char *base;
    size_t len;
    int pw = 0, ph = 0, pr = 30, ps = 1, pa = 44100;

    if (!filepath)
        return FALSE;
    len = strlen(filepath);
    if (len < 8 || _stricmp(filepath + len - 7, ".exedit") != 0)
    {
        return FALSE;
    }

    base = strrchr(filepath, '\\');
    if (!base)
        base = strrchr(filepath, '/');
    base = base ? (base + 1) : filepath;

    if (sscanf(base, "%dx%d_%d-%dfps_%dHz", &pw, &ph, &pr, &ps, &pa) == 5)
    {
        // exedit fractional fps: e.g. 1920x1080_30000-1001fps_44100Hz.exedit (0x1003ac30)
    }
    else if (sscanf(base, "%dx%d_%d/%dfps_%dHz", &pw, &ph, &pr, &ps, &pa) == 5)
    {
        // slash fractional fps: e.g. 1920x1080_30000/1001fps_44100Hz.exedit
    }
    else if (sscanf(base, "%dx%d_%dfps_%dHz", &pw, &ph, &pr, &pa) == 4)
    {
        ps = 1;
    }
    else
    {
        pw = 1280;
        ph = 720;
        pr = 30;
        ps = 1;
        pa = 44100;
    }

    if (pw < 32)
        pw = 32;
    if (ph < 32)
        ph = 32;
    if (pr <= 0)
        pr = 30;
    if (ps <= 0)
        ps = 1;
    if (pa <= 0)
        pa = 44100;

    *w = pw;
    *h = ph;
    *rate = pr;
    *scale = ps;
    *hz = pa;
    return TRUE;
}

static BOOL try_open_still_image(VFWInput *input, const char *filepath)
{
    WCHAR wpath[MAX_PATH];
    void *bmp = NULL;
    UINT w = 0, h = 0;
    struct
    {
        INT X, Y, Width, Height;
    } rect;
    GdipBitmapData bd;
    UINT y, x;
    int dst_stride;

    if (!ensure_gdiplus())
    {
        return FALSE;
    }
    if (MultiByteToWideChar(CP_ACP, 0, filepath, -1, wpath, MAX_PATH) <= 0)
    {
        return FALSE;
    }
    if (pGdipCreateBitmapFromFile(wpath, &bmp) != 0 || !bmp)
    {
        return FALSE;
    }
    if (pGdipGetImageWidth(bmp, &w) != 0 || pGdipGetImageHeight(bmp, &h) != 0 ||
        w == 0 || h == 0)
    {
        pGdipDisposeImage(bmp);
        return FALSE;
    }

    rect.X = 0;
    rect.Y = 0;
    rect.Width = (INT)w;
    rect.Height = (INT)h;
    memset(&bd, 0, sizeof(bd));

    // PixelFormat32bppARGB = 0x26200A, ImageLockModeRead = 1
    if (pGdipBitmapLockBits(bmp, &rect, 1, 0x26200A, &bd) != 0 || !bd.Scan0)
    {
        pGdipDisposeImage(bmp);
        return FALSE;
    }

    input->kind = INPUT_KIND_STILL_IMAGE;
    input->width = (int)w;
    input->height = (int)h;
    input->frame_count = 1;
    input->fps_num = 30;
    input->fps_den = 1;
    input->decode_bpp = 32;

    dst_stride = (int)w * 4;
    input->dib_size = (size_t)dst_stride * (size_t)h;
    input->dib_buffer = (unsigned char *)calloc(1, input->dib_size);
    input->rgb_buffer = (PIXEL *)calloc((size_t)w * (size_t)h, sizeof(PIXEL));
    if (!input->dib_buffer || !input->rgb_buffer)
    {
        pGdipBitmapUnlockBits(bmp, &bd);
        pGdipDisposeImage(bmp);
        return FALSE;
    }

    for (y = 0; y < h; y++)
    {
        const unsigned char *src_row = (const unsigned char *)bd.Scan0 + (ptrdiff_t)y * bd.Stride;
        // bottom-up BGRA32 for avi_file_get_video_pixelp
        unsigned char *dib_row = input->dib_buffer + (size_t)(h - 1 - y) * (size_t)dst_stride;
        PIXEL *rgb_row = input->rgb_buffer + (size_t)y * (size_t)w;
        memcpy(dib_row, src_row, (size_t)dst_stride);
        for (x = 0; x < w; x++)
        {
            rgb_row[x].b = src_row[x * 4 + 0];
            rgb_row[x].g = src_row[x * 4 + 1];
            rgb_row[x].r = src_row[x * 4 + 2];
        }
    }

    pGdipBitmapUnlockBits(bmp, &bd);
    pGdipDisposeImage(bmp);
    return TRUE;
}

static BOOL init_vfw_audio_stream(VFWInput *input, PAVISTREAM pavi_audio)
{
    LONG cbFmt = 0;
    WAVEFORMATEX *pwfx;
    WORD tag, bits;

    if (!pavi_audio)
        return FALSE;
    if (FAILED(AVIStreamReadFormat(pavi_audio, 0, NULL, &cbFmt)) || cbFmt <= 0)
    {
        return FALSE;
    }
    pwfx = (WAVEFORMATEX *)calloc(1, (size_t)cbFmt + sizeof(WAVEFORMATEX));
    if (!pwfx)
        return FALSE;
    if (FAILED(AVIStreamReadFormat(pavi_audio, 0, pwfx, &cbFmt)) ||
        pwfx->nSamplesPerSec == 0 || pwfx->nChannels == 0)
    {
        free(pwfx);
        return FALSE;
    }

    tag = pwfx->wFormatTag;
    bits = pwfx->wBitsPerSample;
    input->src_audio_rate = (int)pwfx->nSamplesPerSec;
    input->src_audio_ch = (pwfx->nChannels >= 2) ? 2 : 1;
    input->req_audio_rate = input->src_audio_rate;
    input->req_audio_ch = input->src_audio_ch;

    if ((tag == WAVE_FORMAT_PCM && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
        (tag == WAVE_FORMAT_IEEE_FLOAT && (bits == 32 || bits == 64)))
    {
        input->audio_kind = AUDIO_SRC_VFW_PCM;
        input->src_audio_bits = (int)bits;
        input->src_audio_samples = (int)AVIStreamLength(pavi_audio);
        input->vfw_raw_ch = (int)pwfx->nChannels;
        input->vfw_is_float = (tag == WAVE_FORMAT_IEEE_FLOAT);
        input->vfw_block_align = (pwfx->nBlockAlign > 0)
                                     ? (int)pwfx->nBlockAlign
                                     : ((int)pwfx->nChannels * (bits / 8));
        free(pwfx);
        return (input->src_audio_samples > 0);
    }

    // compressed AVI audio stream (e.g. MP3-in-AVI 0x0055, ADPCM, etc.) via ACM
    if (acm_init_decoder(input, pwfx))
    {
        LONG start_s = AVIStreamStart(pavi_audio);
        LONG len_s = AVIStreamLength(pavi_audio);
        LONG dur_ms = AVIStreamSampleToTime(pavi_audio, start_s + len_s);
        input->audio_kind = AUDIO_SRC_VFW_ACM;
        input->src_audio_bits = 16;
        input->vfw_cur_pos = start_s;
        input->vfw_end_pos = start_s + len_s;
        input->vfw_block_align = (pwfx->nBlockAlign > 0) ? (int)pwfx->nBlockAlign : 1;
        if (dur_ms > 0)
        {
            input->src_audio_samples = (int)(((int64_t)dur_ms * input->src_audio_rate) / 1000);
        }
        else
        {
            input->src_audio_samples = (int)len_s;
        }
        free(pwfx);
        return (input->src_audio_samples > 0);
    }

    free(pwfx);
    return FALSE;
}

BOOL vfw_input_get_file_info(VFWInput *input, FILE_INFO *fi)
{
    if (!input || !fi)
    {
        return FALSE;
    }
    memset(fi, 0, sizeof(FILE_INFO));
    fi->name = input->filepath;

    // audio-only files MUST report FILE_INFO_FLAG_AUDIO without FILE_INFO_FLAG_VIDEO so exedit's func_WndProc (RVA 0x3bfe5) and 0x10004ea0 classify them as audio
    if (input->kind != INPUT_KIND_AUDIO_FILE && input->width > 0 && input->height > 0)
    {
        fi->flag = FILE_INFO_FLAG_VIDEO;
        fi->w = input->width;
        fi->h = input->height;
        fi->video_rate = input->fps_num > 0 ? input->fps_num : 30;
        fi->video_scale = input->fps_den > 0 ? input->fps_den : 1;
        fi->frame_n = input->frame_count;
        fi->video_decode_format = 0;
        fi->video_decode_bit = input->decode_bpp ? input->decode_bpp : 24;
    }

    if (input->audio_kind != AUDIO_SRC_NONE &&
        input->req_audio_rate > 0 && input->req_audio_ch > 0)
    {
        fi->flag |= FILE_INFO_FLAG_AUDIO;
        fi->audio_rate = input->req_audio_rate;
        fi->audio_ch = input->req_audio_ch;
        if (input->src_audio_samples > 0 && input->src_audio_rate > 0)
        {
            fi->audio_n = (int)(((int64_t)input->src_audio_samples * input->req_audio_rate) /
                                input->src_audio_rate);
        }
        else if (fi->frame_n > 0 && fi->video_rate > 0)
        {
            fi->audio_n = (int)(((int64_t)fi->frame_n * fi->audio_rate * fi->video_scale) /
                                fi->video_rate);
        }
    }
    return TRUE;
}

VFWInput *vfw_input_open_ex(const char *filepath, FILE_INFO *fi, int open_flag)
{
    VFWInput *input;
    AVISTREAMINFOA asi;
    BITMAPINFOHEADER req_bih;
    HRESULT hr;
    int cw, ch, crate, cscale, chz;
    BOOL want_video = (open_flag & AVI_FILE_OPEN_FLAG_AUDIO_ONLY) == 0;
    BOOL want_audio = (open_flag & AVI_FILE_OPEN_FLAG_VIDEO_ONLY) == 0;

    if (!filepath || !filepath[0])
    {
        return NULL;
    }

    vfw_ensure_initialized();

    input = (VFWInput *)calloc(1, sizeof(VFWInput));
    if (!input)
    {
        return NULL;
    }
    input->hfile = INVALID_HANDLE_VALUE;
    strncpy(input->filepath, filepath, MAX_PATH - 1);
    input->filepath[MAX_PATH - 1] = '\0';

    // virtual exedit blank project canvas ("1280x720_30fps_44100Hz.exedit")
    if (parse_exedit_canvas_spec(filepath, &cw, &ch, &crate, &cscale, &chz))
    {
        int dib_stride = (cw * 3 + 3) & ~3;
        input->kind = INPUT_KIND_EXEDIT_CANVAS;
        input->audio_kind = AUDIO_SRC_SILENT;
        input->width = cw;
        input->height = ch;
        input->frame_count = 64;
        input->fps_num = crate;
        input->fps_den = cscale;
        input->src_audio_rate = chz;
        input->src_audio_ch = 2;
        input->src_audio_bits = 16;
        input->req_audio_rate = chz;
        input->req_audio_ch = 2;
        input->src_audio_samples = (int)(((int64_t)input->frame_count * chz * cscale) / crate);
        input->decode_bpp = 24;
        input->rgb_buffer = (PIXEL *)calloc((size_t)cw * (size_t)ch, sizeof(PIXEL));
        input->dib_size = (size_t)dib_stride * (size_t)ch;
        input->dib_buffer = (unsigned char *)calloc(1, input->dib_size);
        if (!input->rgb_buffer || !input->dib_buffer)
        {
            vfw_input_close(input);
            return NULL;
        }
        if (fi)
            vfw_input_get_file_info(input, fi);
        return input;
    }

    // native WAV / MP3 audio reader (handles .mp3, .wav, RIFF-MP3, Float WAV, ADPCM WAV)
    if (want_audio && try_open_wav_or_mp3_file(input, filepath))
    {
        if (fi)
            vfw_input_get_file_info(input, fi);
        return input;
    }

    // AVIFile (VFW) video/audio container
    hr = AVIFileOpenA(&input->pavi_file, filepath, OF_READ | OF_SHARE_DENY_WRITE, NULL);
    if (SUCCEEDED(hr) && input->pavi_file)
    {
        BOOL video_ok = FALSE;
        BOOL audio_ok = FALSE;

        if (want_video &&
            SUCCEEDED(AVIFileGetStream(input->pavi_file, &input->pavi_stream, streamtypeVIDEO, 0)) &&
            input->pavi_stream &&
            SUCCEEDED(AVIStreamInfoA(input->pavi_stream, &asi, sizeof(asi))))
        {
            input->width = asi.rcFrame.right - asi.rcFrame.left;
            input->height = asi.rcFrame.bottom - asi.rcFrame.top;
            input->frame_count = (int)AVIStreamLength(input->pavi_stream);
            input->fps_num = (int)(asi.dwRate ? asi.dwRate : 30);
            input->fps_den = (int)(asi.dwScale ? asi.dwScale : 1);

            memset(&req_bih, 0, sizeof(req_bih));
            req_bih.biSize = sizeof(req_bih);
            req_bih.biWidth = input->width;
            req_bih.biHeight = input->height;
            req_bih.biPlanes = 1;
            req_bih.biBitCount = 24;
            req_bih.biCompression = BI_RGB;

            input->pgf = AVIStreamGetFrameOpen(input->pavi_stream, &req_bih);
            if (!input->pgf)
            {
                input->pgf = AVIStreamGetFrameOpen(input->pavi_stream, NULL);
            }
            if (input->pgf && input->width > 0 && input->height > 0)
            {
                LPBITMAPINFOHEADER pbmi0 =
                    (LPBITMAPINFOHEADER)AVIStreamGetFrame(input->pgf, 0);
                input->decode_bpp = (pbmi0 && pbmi0->biBitCount == 32) ? 32 : 24;
                input->rgb_buffer = (PIXEL *)calloc(
                    (size_t)input->width * (size_t)input->height, sizeof(PIXEL));
                if (input->rgb_buffer)
                {
                    video_ok = TRUE;
                }
            }
        }

        if (want_audio &&
            SUCCEEDED(AVIFileGetStream(input->pavi_file, &input->pavi_audio, streamtypeAUDIO, 0)) &&
            input->pavi_audio)
        {
            if (init_vfw_audio_stream(input, input->pavi_audio))
            {
                audio_ok = TRUE;
            }
            else
            {
                AVIStreamRelease(input->pavi_audio);
                input->pavi_audio = NULL;
            }
        }

        if (video_ok || audio_ok)
        {
            input->kind = video_ok ? INPUT_KIND_VFW : INPUT_KIND_AUDIO_FILE;
            if (!video_ok)
            {
                if (input->pgf)
                {
                    AVIStreamGetFrameClose(input->pgf);
                    input->pgf = NULL;
                }
                if (input->pavi_stream)
                {
                    AVIStreamRelease(input->pavi_stream);
                    input->pavi_stream = NULL;
                }
                input->width = 0;
                input->height = 0;
                input->frame_count = 0;
            }
            if (fi)
                vfw_input_get_file_info(input, fi);
            return input;
        }

        if (input->pgf)
        {
            AVIStreamGetFrameClose(input->pgf);
            input->pgf = NULL;
        }
        if (input->pavi_audio)
        {
            AVIStreamRelease(input->pavi_audio);
            input->pavi_audio = NULL;
        }
        if (input->pavi_stream)
        {
            AVIStreamRelease(input->pavi_stream);
            input->pavi_stream = NULL;
        }
        AVIFileRelease(input->pavi_file);
        input->pavi_file = NULL;
    }

    // still image fallback (.png / .jpg / .bmp / .gif via GDI+)
    if (want_video && try_open_still_image(input, filepath))
    {
        if (fi)
            vfw_input_get_file_info(input, fi);
        return input;
    }

    vfw_input_close(input);
    return NULL;
}

VFWInput *vfw_input_open(const char *filepath, FILE_INFO *fi)
{
    return vfw_input_open_ex(filepath, fi, 0);
}

void vfw_input_close(VFWInput *input)
{
    if (!input)
    {
        return;
    }
    if (input->acm_stream)
    {
        if (input->acm_hdr_prepared)
        {
            input->acm_hdr.cbSrcLength = input->acm_src_cap;
            input->acm_hdr.cbDstLength = input->acm_dst_cap;
            acmStreamUnprepareHeader(input->acm_stream, &input->acm_hdr, 0);
            input->acm_hdr_prepared = FALSE;
        }
        acmStreamClose(input->acm_stream, 0);
        input->acm_stream = NULL;
    }
    if (input->hfile != NULL && input->hfile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(input->hfile);
        input->hfile = INVALID_HANDLE_VALUE;
    }
    free(input->mp3_frames);
    free(input->acm_src_buf);
    free(input->acm_dst_buf);
    free(input->pcm_cache);
    free(input->resample_tmp);
    free(input->rgb_buffer);
    free(input->dib_buffer);
    if (input->pgf)
    {
        AVIStreamGetFrameClose(input->pgf);
    }
    if (input->pavi_audio)
    {
        AVIStreamRelease(input->pavi_audio);
    }
    if (input->pavi_stream)
    {
        AVIStreamRelease(input->pavi_stream);
    }
    if (input->pavi_file)
    {
        AVIFileRelease(input->pavi_file);
    }
    free(input);
}

static BOOL decode_vfw_frame_to_buffers(VFWInput *input, int frame_num,
                                        const unsigned char **out_dib,
                                        int *out_bpp, int *out_stride, BOOL *out_bottom_up)
{
    LPBITMAPINFOHEADER pbmi;
    const unsigned char *frame_data;
    int bpp, stride;
    BOOL bottom_up;

    if (frame_num < 0)
        frame_num = 0;
    if (frame_num >= input->frame_count)
        frame_num = input->frame_count - 1;

    pbmi = (LPBITMAPINFOHEADER)AVIStreamGetFrame(input->pgf,
                                                 AVIStreamStart(input->pavi_stream) + frame_num);
    if (!pbmi)
    {
        return FALSE;
    }

    frame_data = (const unsigned char *)pbmi + pbmi->biSize;
    if (pbmi->biClrUsed > 0)
    {
        frame_data += pbmi->biClrUsed * sizeof(RGBQUAD);
    }

    bpp = pbmi->biBitCount ? pbmi->biBitCount : 24;
    bottom_up = (pbmi->biHeight >= 0);
    stride = ((input->width * (bpp / 8)) + 3) & ~3;

    *out_dib = frame_data;
    *out_bpp = bpp;
    *out_stride = stride;
    *out_bottom_up = bottom_up;
    return TRUE;
}

BOOL vfw_input_read_frame_stride(VFWInput *input, int frame_num, PIXEL_YC *ycp, int yc_stride)
{
    int x, y;

    if (!input || !ycp || input->width <= 0 || input->height <= 0)
    {
        return FALSE;
    }
    if (yc_stride <= 0)
    {
        yc_stride = input->width;
    }

    if (input->kind == INPUT_KIND_EXEDIT_CANVAS)
    {
        for (y = 0; y < input->height; y++)
        {
            memset(ycp + (size_t)y * (size_t)yc_stride, 0,
                   (size_t)input->width * sizeof(PIXEL_YC));
        }
        return TRUE;
    }

    if (input->kind == INPUT_KIND_STILL_IMAGE)
    {
        for (y = 0; y < input->height; y++)
        {
            rgb2yc(ycp + (size_t)y * (size_t)yc_stride,
                   input->rgb_buffer + (size_t)y * (size_t)input->width,
                   input->width);
        }
        return TRUE;
    }

    if (input->kind == INPUT_KIND_VFW && input->pgf && input->pavi_stream)
    {
        const unsigned char *dib = NULL;
        int bpp = 24, stride = 0;
        BOOL bottom_up = TRUE;
        if (!decode_vfw_frame_to_buffers(input, frame_num, &dib, &bpp, &stride, &bottom_up))
        {
            return FALSE;
        }
        if (bpp == 24)
        {
            return rgb2yc_dib24(ycp, yc_stride, dib, stride,
                                input->width, input->height, bottom_up);
        }
        if (bpp == 32 && input->rgb_buffer)
        {
            for (y = 0; y < input->height; y++)
            {
                int src_y = bottom_up ? (input->height - 1 - y) : y;
                const unsigned char *src = dib + (size_t)src_y * (size_t)stride;
                PIXEL *row = input->rgb_buffer + (size_t)y * (size_t)input->width;
                for (x = 0; x < input->width; x++)
                {
                    row[x].b = src[x * 4 + 0];
                    row[x].g = src[x * 4 + 1];
                    row[x].r = src[x * 4 + 2];
                }
                rgb2yc(ycp + (size_t)y * (size_t)yc_stride, row, input->width);
            }
            return TRUE;
        }
    }

    return FALSE;
}

BOOL vfw_input_read_frame(VFWInput *input, int frame_num, PIXEL_YC *ycp)
{
    return vfw_input_read_frame_stride(input, frame_num, ycp, input ? input->width : 0);
}

int vfw_input_set_audio_format(VFWInput *input, int audio_rate, int audio_ch)
{
    if (!input || audio_rate <= 0 || audio_ch <= 0)
    {
        return 0;
    }
    input->req_audio_rate = audio_rate;
    input->req_audio_ch = audio_ch;
    if (input->src_audio_samples > 0 && input->src_audio_rate > 0)
    {
        return (int)(((int64_t)input->src_audio_samples * audio_rate) / input->src_audio_rate);
    }
    if (input->frame_count > 0 && input->fps_num > 0)
    {
        return (int)(((int64_t)input->frame_count * audio_rate * input->fps_den) / input->fps_num);
    }
    return 0;
}

// read source-rate interleaved PCM16 [src_start .. src_start + src_count)
static int read_source_pcm16(VFWInput *input, int src_start, short *dst, int src_count)
{
    int src_ch = input->src_audio_ch > 0 ? input->src_audio_ch : 2;

    if (src_count <= 0)
        return 0;
    memset(dst, 0, (size_t)src_count * (size_t)src_ch * sizeof(short));

    if (src_start >= input->src_audio_samples)
    {
        return 0;
    }
    if (src_start < 0)
    {
        int lead_zero = -src_start;
        if (lead_zero >= src_count)
            return src_count;
        dst += (size_t)lead_zero * (size_t)src_ch;
        src_count -= lead_zero;
        src_start = 0;
    }
    if (src_start + src_count > input->src_audio_samples)
    {
        src_count = input->src_audio_samples - src_start;
    }
    if (src_count <= 0)
        return 0;

    if (input->audio_kind == AUDIO_SRC_WAV_PCM)
    {
        size_t raw_bytes = (size_t)src_count * (size_t)input->wav_block_align;
        unsigned char *raw = (unsigned char *)malloc(raw_bytes);
        DWORD got = 0;
        int frames_got;
        if (!raw)
            return 0;
        if (!file_read_at(input->hfile,
                          input->wav_data_offset + (uint64_t)src_start * (uint64_t)input->wav_block_align,
                          raw, (DWORD)raw_bytes, &got) ||
            got == 0)
        {
            free(raw);
            return 0;
        }
        frames_got = (int)(got / (DWORD)input->wav_block_align);
        if (frames_got > src_count)
            frames_got = src_count;
        convert_raw_frames_to_pcm16(raw, frames_got,
                                    input->wav_raw_ch, input->wav_block_align,
                                    input->src_audio_bits, input->wav_is_float,
                                    dst, src_ch);
        free(raw);
        return frames_got;
    }

    if (input->audio_kind == AUDIO_SRC_VFW_PCM && input->pavi_audio)
    {
        LONG bytes_read = 0, samples_read = 0;
        size_t raw_bytes = (size_t)src_count * (size_t)input->vfw_block_align;
        unsigned char *raw = (unsigned char *)malloc(raw_bytes);
        if (!raw)
            return 0;
        if (FAILED(AVIStreamRead(input->pavi_audio,
                                 AVIStreamStart(input->pavi_audio) + src_start,
                                 src_count, raw, (LONG)raw_bytes,
                                 &bytes_read, &samples_read)) ||
            samples_read <= 0)
        {
            free(raw);
            return 0;
        }
        if (samples_read > src_count)
            samples_read = src_count;
        convert_raw_frames_to_pcm16(raw, (int)samples_read,
                                    input->vfw_raw_ch, input->vfw_block_align,
                                    input->src_audio_bits, input->vfw_is_float,
                                    dst, src_ch);
        free(raw);
        return (int)samples_read;
    }

    if (input->audio_kind == AUDIO_SRC_MP3_ACM)
    {
        int target_end = src_start + src_count;
        int stall_guard = 0;

        // seek + 4-frame bit-reservoir preroll if target is outside sequential window
        if (input->pcm_cache_len == 0 ||
            src_start < input->pcm_cache_start ||
            src_start > input->pcm_decode_pos + input->mp3_spf * 16)
        {
            int target_f = src_start / input->mp3_spf;
            int preroll_f = target_f - 4;
            if (preroll_f < 0)
                preroll_f = 0;
            if (preroll_f >= input->mp3_frame_count)
            {
                preroll_f = input->mp3_frame_count - 1;
            }
            input->mp3_next_frame = preroll_f;
            input->acm_src_len = 0;
            input->acm_need_start = TRUE;
            input->acm_eof_flushed = FALSE;
            input->pcm_decode_pos = preroll_f * input->mp3_spf;
            input->pcm_cache_start = input->pcm_decode_pos;
            input->pcm_cache_len = 0;
        }

        while (input->pcm_cache_start + input->pcm_cache_len < target_end)
        {
            int produced = 0;
            int fed = 0;

            // feed up to 8 MP3 frames per ACM conversion step
            while (fed < 8 && input->mp3_next_frame < input->mp3_frame_count)
            {
                const Mp3FrameEntry *fe = &input->mp3_frames[input->mp3_next_frame];
                DWORD got = 0;
                if (input->acm_src_len + (DWORD)fe->size > input->acm_src_cap)
                {
                    break;
                }
                if (!file_read_at(input->hfile, fe->offset,
                                  input->acm_src_buf + input->acm_src_len,
                                  fe->size, &got) ||
                    got != fe->size)
                {
                    input->mp3_next_frame = input->mp3_frame_count;
                    break;
                }
                input->acm_src_len += got;
                input->mp3_next_frame++;
                fed++;
            }

            if (input->acm_src_len > 0)
            {
                if (!acm_convert_step(input, FALSE, src_start, &produced))
                    break;
                if (produced == 0)
                {
                    if (++stall_guard > 32)
                        break;
                }
                else
                {
                    stall_guard = 0;
                }
            }
            else if (input->mp3_next_frame >= input->mp3_frame_count && !input->acm_eof_flushed)
            {
                input->acm_eof_flushed = TRUE;
                acm_convert_step(input, TRUE, src_start, &produced);
                break;
            }
            else
            {
                break;
            }
        }

        if (src_start >= input->pcm_cache_start &&
            src_start < input->pcm_cache_start + input->pcm_cache_len)
        {
            int off = src_start - input->pcm_cache_start;
            int avail = input->pcm_cache_len - off;
            if (avail > src_count)
                avail = src_count;
            memcpy(dst,
                   input->pcm_cache + (size_t)off * (size_t)src_ch,
                   (size_t)avail * (size_t)src_ch * sizeof(short));
            return src_count;
        }
        return src_count;
    }

    if (input->audio_kind == AUDIO_SRC_WAV_ACM)
    {
        int target_end = src_start + src_count;
        int stall_guard = 0;

        if (input->pcm_cache_len == 0 ||
            src_start < input->pcm_cache_start ||
            src_start > input->pcm_decode_pos + input->src_audio_rate * 2)
        {
            uint64_t approx_byte = ((uint64_t)src_start * (uint64_t)input->wav_avg_bps) /
                                   (uint64_t)input->src_audio_rate;
            uint64_t blk_idx = approx_byte / (uint64_t)input->wav_block_align;
            if (blk_idx > 1)
                blk_idx -= 1;
            else
                blk_idx = 0;
            input->wav_cur_byte = blk_idx * (uint64_t)input->wav_block_align;
            if (input->wav_cur_byte >= input->wav_data_bytes)
                input->wav_cur_byte = 0;
            input->acm_src_len = 0;
            input->acm_need_start = TRUE;
            input->acm_eof_flushed = FALSE;
            input->pcm_decode_pos = (int)((input->wav_cur_byte * (uint64_t)input->src_audio_rate) /
                                          (uint64_t)input->wav_avg_bps);
            input->pcm_cache_start = input->pcm_decode_pos;
            input->pcm_cache_len = 0;
        }

        while (input->pcm_cache_start + input->pcm_cache_len < target_end)
        {
            int produced = 0;
            if (input->wav_cur_byte < input->wav_data_bytes)
            {
                DWORD space = input->acm_src_cap - input->acm_src_len;
                DWORD chunk = (space / (DWORD)input->wav_block_align) * (DWORD)input->wav_block_align;
                uint64_t rem = input->wav_data_bytes - input->wav_cur_byte;
                DWORD got = 0;
                if ((uint64_t)chunk > rem)
                    chunk = (DWORD)rem;
                if (chunk > 0 &&
                    file_read_at(input->hfile, input->wav_data_offset + input->wav_cur_byte,
                                 input->acm_src_buf + input->acm_src_len, chunk, &got) &&
                    got > 0)
                {
                    input->acm_src_len += got;
                    input->wav_cur_byte += got;
                }
                else
                {
                    input->wav_cur_byte = input->wav_data_bytes;
                }
            }

            if (input->acm_src_len > 0)
            {
                if (!acm_convert_step(input, FALSE, src_start, &produced))
                    break;
                if (produced == 0 && ++stall_guard > 32)
                    break;
            }
            else if (!input->acm_eof_flushed)
            {
                input->acm_eof_flushed = TRUE;
                acm_convert_step(input, TRUE, src_start, &produced);
                break;
            }
            else
            {
                break;
            }
        }

        if (src_start >= input->pcm_cache_start &&
            src_start < input->pcm_cache_start + input->pcm_cache_len)
        {
            int off = src_start - input->pcm_cache_start;
            int avail = input->pcm_cache_len - off;
            if (avail > src_count)
                avail = src_count;
            memcpy(dst,
                   input->pcm_cache + (size_t)off * (size_t)src_ch,
                   (size_t)avail * (size_t)src_ch * sizeof(short));
        }
        return src_count;
    }

    if (input->audio_kind == AUDIO_SRC_VFW_ACM && input->pavi_audio)
    {
        int target_end = src_start + src_count;
        int stall_guard = 0;

        if (input->pcm_cache_len == 0 ||
            src_start < input->pcm_cache_start ||
            src_start > input->pcm_decode_pos + input->src_audio_rate * 2)
        {
            int preroll_smp = (src_start > 4096) ? (src_start - 4096) : 0;
            LONG ms = (LONG)(((int64_t)preroll_smp * 1000) / input->src_audio_rate);
            LONG pos = AVIStreamTimeToSample(input->pavi_audio, ms);
            LONG kf = AVIStreamFindSample(input->pavi_audio, pos, FIND_ANY | FIND_PREV);
            if (kf >= AVIStreamStart(input->pavi_audio))
                pos = kf;
            if (pos < AVIStreamStart(input->pavi_audio))
                pos = AVIStreamStart(input->pavi_audio);
            input->vfw_cur_pos = pos;
            input->acm_src_len = 0;
            input->acm_need_start = TRUE;
            input->acm_eof_flushed = FALSE;
            ms = AVIStreamSampleToTime(input->pavi_audio, pos);
            input->pcm_decode_pos = (ms > 0) ? (int)(((int64_t)ms * input->src_audio_rate) / 1000) : 0;
            input->pcm_cache_start = input->pcm_decode_pos;
            input->pcm_cache_len = 0;
        }

        while (input->pcm_cache_start + input->pcm_cache_len < target_end)
        {
            int produced = 0;
            if (input->vfw_cur_pos < input->vfw_end_pos &&
                input->acm_src_len + 8192 <= input->acm_src_cap)
            {
                LONG bytes_got = 0, smp_got = 0;
                DWORD space = input->acm_src_cap - input->acm_src_len;
                if (SUCCEEDED(AVIStreamRead(input->pavi_audio, input->vfw_cur_pos,
                                            AVISTREAMREAD_CONVENIENT,
                                            input->acm_src_buf + input->acm_src_len,
                                            (LONG)space, &bytes_got, &smp_got)) &&
                    bytes_got > 0 && smp_got > 0)
                {
                    input->acm_src_len += (DWORD)bytes_got;
                    input->vfw_cur_pos += smp_got;
                }
                else
                {
                    input->vfw_cur_pos = input->vfw_end_pos;
                }
            }

            if (input->acm_src_len > 0)
            {
                if (!acm_convert_step(input, FALSE, src_start, &produced))
                    break;
                if (produced == 0 && ++stall_guard > 32)
                    break;
            }
            else if (!input->acm_eof_flushed)
            {
                input->acm_eof_flushed = TRUE;
                acm_convert_step(input, TRUE, src_start, &produced);
                break;
            }
            else
            {
                break;
            }
        }

        if (src_start >= input->pcm_cache_start &&
            src_start < input->pcm_cache_start + input->pcm_cache_len)
        {
            int off = src_start - input->pcm_cache_start;
            int avail = input->pcm_cache_len - off;
            if (avail > src_count)
                avail = src_count;
            memcpy(dst,
                   input->pcm_cache + (size_t)off * (size_t)src_ch,
                   (size_t)avail * (size_t)src_ch * sizeof(short));
        }
        return src_count;
    }

    return 0;
}

int vfw_input_read_audio(VFWInput *input, int sample_num, void *buf, int samples)
{
    int req_ch, src_ch, out_rate, src_rate, total_out, out_valid, i;
    short *dst;

    if (!input || !buf || samples <= 0)
    {
        return 0;
    }

    req_ch = input->req_audio_ch > 0 ? input->req_audio_ch : 2;
    dst = (short *)buf;
    memset(dst, 0, (size_t)samples * (size_t)req_ch * sizeof(short));

    if (input->audio_kind == AUDIO_SRC_SILENT)
    {
        return samples;
    }
    if (input->audio_kind == AUDIO_SRC_NONE ||
        input->src_audio_rate <= 0 || input->src_audio_ch <= 0 ||
        input->src_audio_samples <= 0)
    {
        return 0;
    }

    src_rate = input->src_audio_rate;
    out_rate = input->req_audio_rate > 0 ? input->req_audio_rate : src_rate;
    src_ch = input->src_audio_ch;
    total_out = (int)(((int64_t)input->src_audio_samples * out_rate) / src_rate);

    if (sample_num >= total_out)
    {
        return 0;
    }
    out_valid = samples;
    if (sample_num + out_valid > total_out)
    {
        out_valid = total_out - sample_num;
    }

    // fast path when sample rate and channel count already match
    if (out_rate == src_rate && req_ch == src_ch)
    {
        return read_source_pcm16(input, sample_num, dst, out_valid);
    }

    // general path: phase-continuous 64-bit linear interpolation + mono/stereo conversion
    {
        int64_t first_num = (int64_t)sample_num * (int64_t)src_rate;
        int64_t last_num = (int64_t)(sample_num + out_valid - 1) * (int64_t)src_rate;
        int src_first = (first_num >= 0) ? (int)(first_num / out_rate)
                                         : (int)((first_num - out_rate + 1) / out_rate);
        int src_last = (last_num >= 0) ? (int)(last_num / out_rate)
                                       : (int)((last_num - out_rate + 1) / out_rate);
        int src_need = (src_last - src_first) + 2;

        if (src_need < 2)
            src_need = 2;
        if (src_need > input->resample_tmp_cap || !input->resample_tmp)
        {
            int ncap = src_need + 1024;
            short *nt = (short *)realloc(input->resample_tmp,
                                         (size_t)ncap * 2u * sizeof(short));
            if (!nt)
                return 0;
            input->resample_tmp = nt;
            input->resample_tmp_cap = ncap;
        }

        read_source_pcm16(input, src_first, input->resample_tmp, src_need);

        for (i = 0; i < out_valid; i++)
        {
            int out_idx = sample_num + i;
            short l, r;
            if (out_idx < 0)
                continue;

            if (out_rate == src_rate)
            {
                int k = out_idx - src_first;
                if (src_ch == 1)
                {
                    l = r = input->resample_tmp[k];
                }
                else
                {
                    l = input->resample_tmp[k * 2 + 0];
                    r = input->resample_tmp[k * 2 + 1];
                }
            }
            else
            {
                int64_t num = (int64_t)out_idx * (int64_t)src_rate;
                int sidx = (int)(num / out_rate) - src_first;
                int rem = (int)(num % out_rate);
                int k0 = (sidx < 0) ? 0 : ((sidx >= src_need) ? (src_need - 1) : sidx);
                int k1 = (k0 + 1 < src_need) ? (k0 + 1) : k0;

                if (src_ch == 1)
                {
                    int a = input->resample_tmp[k0];
                    int b = input->resample_tmp[k1];
                    l = r = clamp_pcm16(a + (int)(((int64_t)(b - a) * rem) / out_rate));
                }
                else
                {
                    int al = input->resample_tmp[k0 * 2 + 0];
                    int bl = input->resample_tmp[k1 * 2 + 0];
                    int ar = input->resample_tmp[k0 * 2 + 1];
                    int br = input->resample_tmp[k1 * 2 + 1];
                    l = clamp_pcm16(al + (int)(((int64_t)(bl - al) * rem) / out_rate));
                    r = clamp_pcm16(ar + (int)(((int64_t)(br - ar) * rem) / out_rate));
                }
            }

            if (req_ch == 1)
            {
                dst[i] = (short)(((int)l + (int)r) / 2);
            }
            else
            {
                dst[i * 2 + 0] = l;
                dst[i * 2 + 1] = r;
            }
        }
    }
    return out_valid;
}

void *vfw_input_get_dib_pixelp(VFWInput *input, int frame_num)
{
    const unsigned char *dib = NULL;
    int bpp = 24, stride = 0;
    BOOL bottom_up = TRUE;

    if (!input)
    {
        return NULL;
    }
    if (input->kind == INPUT_KIND_EXEDIT_CANVAS || input->kind == INPUT_KIND_STILL_IMAGE)
    {
        return input->dib_buffer;
    }
    if (input->kind == INPUT_KIND_VFW && input->pgf && input->pavi_stream)
    {
        if (!decode_vfw_frame_to_buffers(input, frame_num, &dib, &bpp, &stride, &bottom_up))
        {
            return NULL;
        }
        return (void *)dib;
    }
    return NULL;
}

PIXEL *vfw_input_get_pixel_buffer(VFWInput *input, int frame_num)
{
    const unsigned char *dib = NULL;
    int bpp = 24, stride = 0, x, y;
    BOOL bottom_up = TRUE;

    if (!input || !input->rgb_buffer)
    {
        return NULL;
    }
    if (input->kind == INPUT_KIND_EXEDIT_CANVAS || input->kind == INPUT_KIND_STILL_IMAGE)
    {
        return input->rgb_buffer;
    }
    if (input->kind != INPUT_KIND_VFW || !input->pgf || !input->pavi_stream)
    {
        return NULL;
    }
    if (!decode_vfw_frame_to_buffers(input, frame_num, &dib, &bpp, &stride, &bottom_up))
    {
        return NULL;
    }
    for (y = 0; y < input->height; y++)
    {
        int src_y = bottom_up ? (input->height - 1 - y) : y;
        const unsigned char *src = dib + (size_t)src_y * (size_t)stride;
        PIXEL *row = input->rgb_buffer + (size_t)y * (size_t)input->width;
        if (bpp == 32)
        {
            for (x = 0; x < input->width; x++)
            {
                row[x].b = src[x * 4 + 0];
                row[x].g = src[x * 4 + 1];
                row[x].r = src[x * 4 + 2];
            }
        }
        else
        {
            for (x = 0; x < input->width; x++)
            {
                row[x].b = src[x * 3 + 0];
                row[x].g = src[x * 3 + 1];
                row[x].r = src[x * 3 + 2];
            }
        }
    }
    return input->rgb_buffer;
}

BOOL vfw_input_is_keyframe(VFWInput *input, int frame_num)
{
    if (!input || frame_num < 0)
    {
        return FALSE;
    }
    if (input->kind == INPUT_KIND_VFW && input->pavi_stream)
    {
        LONG kf = AVIStreamFindSample(input->pavi_stream, frame_num, FIND_KEY | FIND_PREV);
        return (kf == (LONG)frame_num);
    }
    return TRUE;
}
