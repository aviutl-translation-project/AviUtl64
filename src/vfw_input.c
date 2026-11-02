#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <msacm.h>
#include "vfw_input.h"
#include "yc48_convert.h"
#include "color_plugin.h"
#include "input_plugin.h"
#include "plugin_bridge.h"
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
    INPUT_KIND_AUDIO_FILE = 3,
    INPUT_KIND_AUI_NATIVE = 4,
    INPUT_KIND_AUI_BRIDGED = 5,
    INPUT_KIND_ODML_AVI = 6
} InputKind;

typedef enum
{
    AUDIO_SRC_NONE = 0,
    AUDIO_SRC_SILENT = 1,      // .exedit virtual canvas
    AUDIO_SRC_WAV_PCM = 2,     // uncompressed WAV (PCM 8/16/24/32 or IEEE Float 32/64)
    AUDIO_SRC_WAV_ACM = 3,     // compressed WAV chunk decoded via msacm32.dll
    AUDIO_SRC_MP3_ACM = 4,     // indexed MPEG-1/2/2.5 Layer III frames via msacm32.dll
    AUDIO_SRC_VFW_PCM = 5,     // uncompressed AVI audio stream via AVIStreamRead
    AUDIO_SRC_VFW_ACM = 6,     // compressed AVI audio stream via AVIStreamRead + ACM
    AUDIO_SRC_AUI_PCM = 7,     // uncompressed PCM/Float audio via .aui func_read_audio
    AUDIO_SRC_AUI_ACM = 8,     // compressed audio via .aui func_read_audio + ACM
    AUDIO_SRC_AUI_BRIDGED = 9, // 32-bit .aui audio via IPC bridge
    AUDIO_SRC_ODML_PCM = 10,   // uncompressed AVI/AVI2 audio chunks (64-bit OpenDML)
    AUDIO_SRC_ODML_ACM = 11    // compressed AVI/AVI2 audio chunks via msacm32.dll
} AudioSrcKind;

typedef struct
{
    uint32_t offset;
    uint16_t size;
    uint16_t pad;
} Mp3FrameEntry;

typedef struct
{
    uint64_t offset;
    uint32_t size_and_flags; // bit 31: 1 = non-keyframe, 0 = keyframe; bits 0..30: byte size
} OdmlChunkEntry;

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
    int mp3_lead_skip;

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
    int acm_valid_from;

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
    PIXEL_YC *aui_yc_tmp;

    // sequential numbered still image state (0x0042c680 / 0x0042c740)
    char seq_prefix[MAX_PATH];
    char seq_ext[64];
    int seq_digits;
    int seq_start_num;
    int cached_img_frame;

    // native / bridged .aui state
    InputPluginInstance *aui_inst;
    int aui_flags;
    DWORD aui_vid_comp;
    int aui_vid_bpp;
    BOOL aui_vid_bottom_up;
    uint32_t aui_vid_gmask;
    BITMAPINFOHEADER *aui_vid_bih;
    int aui_vid_bih_bytes;
    unsigned char *aui_vid_buf;
    size_t aui_vid_buf_cap;
    HIC aui_hic;
    BITMAPINFOHEADER aui_hic_out_bih;
    unsigned char *aui_hic_out_buf;
    int aui_last_decoded_frame;
    int aui_aud_block_align;
    int aui_aud_raw_ch;
    BOOL aui_aud_is_float;
    int aui_aud_cur_pos;
    BOOL aui_aud_eof;
    unsigned char *aui_aud_buf;
    size_t aui_aud_buf_cap;
    int bridge_aui_slot;

    // 64-bit OpenDML AVI 1.0/2.0 demuxer state (0x00407d70)
    OdmlChunkEntry *odml_vid_chunks;
    int odml_vid_count;
    OdmlChunkEntry *odml_aud_chunks;
    int odml_aud_count;
    uint64_t *odml_aud_cum_bytes;
    int odml_aud_cur_chunk;
    uint32_t odml_aud_cur_chunk_off;
    uint32_t odml_aud_scale;
    uint32_t odml_aud_rate;
    BOOL odml_aud_vbr;
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

#define PCM_CACHE_DEFAULT_CAP 65536
#define PCM_CACHE_HISTORY_KEEP 24576

static BOOL pcm_cache_append_raw(VFWInput *input, const unsigned char *raw_pcm,
                                 int frames_produced, int raw_ch, int block_align,
                                 int bits, BOOL is_float,
                                 int discard_before_sample, int retain_ref_sample)
{
    int ch = input->src_audio_ch > 0 ? input->src_audio_ch : 2;

    if (frames_produced <= 0 || block_align <= 0)
    {
        return TRUE;
    }

    // discard codec warmup preroll frames that precede discard_before_sample
    if (input->pcm_decode_pos + frames_produced <= discard_before_sample)
    {
        input->pcm_decode_pos += frames_produced;
        input->pcm_cache_start = input->pcm_decode_pos;
        input->pcm_cache_len = 0;
        return TRUE;
    }
    if (input->pcm_decode_pos < discard_before_sample)
    {
        int skip = discard_before_sample - input->pcm_decode_pos;
        raw_pcm += (size_t)skip * (size_t)block_align;
        frames_produced -= skip;
        input->pcm_decode_pos = discard_before_sample;
        input->pcm_cache_start = discard_before_sample;
        input->pcm_cache_len = 0;
    }

    // slide or grow cache while keeping PCM_CACHE_HISTORY_KEEP lookback frames
    // so resampler +2 lookahead and overlapping exedit/waveform reads never seek backward
    if (input->pcm_cache_len + frames_produced > input->pcm_cache_cap)
    {
        int retain_from = (retain_ref_sample > PCM_CACHE_HISTORY_KEEP)
                              ? (retain_ref_sample - PCM_CACHE_HISTORY_KEEP)
                              : 0;
        int can_drop = retain_from - input->pcm_cache_start;
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
            int new_cap = input->pcm_cache_cap > 0 ? (input->pcm_cache_cap * 2) : PCM_CACHE_DEFAULT_CAP;
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
        raw_ch, block_align, bits, is_float,
        input->pcm_cache + (size_t)input->pcm_cache_len * (size_t)ch, ch);

    input->pcm_cache_len += frames_produced;
    input->pcm_decode_pos += frames_produced;
    return TRUE;
}

static BOOL pcm_cache_append(VFWInput *input, const unsigned char *raw_pcm,
                             int frames_produced, int keep_from_sample)
{
    int block_align = input->acm_out_ch * (input->acm_out_bits / 8);
    int discard_before = (input->acm_valid_from < keep_from_sample)
                             ? input->acm_valid_from
                             : keep_from_sample;
    return pcm_cache_append_raw(input, raw_pcm, frames_produced,
                                input->acm_out_ch, block_align,
                                input->acm_out_bits, FALSE,
                                discard_before, keep_from_sample);
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

static BOOL parse_vbr_info_frame(const unsigned char *frame, int fsize, int ver, int ch,
                                 int *out_enc_delay, int *out_enc_padding, BOOL *out_has_delay)
{
    int side_info = (ver == 3) ? ((ch == 1) ? 17 : 32) : ((ch == 1) ? 9 : 17);
    int xing_off = 4 + side_info;

    if (out_enc_delay)
        *out_enc_delay = 0;
    if (out_enc_padding)
        *out_enc_padding = 0;
    if (out_has_delay)
        *out_has_delay = FALSE;

    if (xing_off + 8 <= fsize &&
        (memcmp(frame + xing_off, "Xing", 4) == 0 ||
         memcmp(frame + xing_off, "Info", 4) == 0))
    {
        uint32_t flags = ((uint32_t)frame[xing_off + 4] << 24) |
                         ((uint32_t)frame[xing_off + 5] << 16) |
                         ((uint32_t)frame[xing_off + 6] << 8) |
                         (uint32_t)frame[xing_off + 7];
        int p = xing_off + 8;
        if (flags & 0x01u)
            p += 4;
        if (flags & 0x02u)
            p += 4;
        if (flags & 0x04u)
            p += 100;
        if (flags & 0x08u)
            p += 4;
        if (p + 24 <= fsize &&
            (memcmp(frame + p, "LAME", 4) == 0 ||
             memcmp(frame + p, "Lavf", 4) == 0 ||
             memcmp(frame + p, "Lavc", 4) == 0 ||
             memcmp(frame + p, "GOGO", 4) == 0))
        {
            int d = ((int)frame[p + 21] << 4) | ((int)frame[p + 22] >> 4);
            int pad = (((int)frame[p + 22] & 0x0F) << 8) | (int)frame[p + 23];
            if (d >= 0 && d <= 2880 && pad >= 0 && pad <= 8192)
            {
                if (out_enc_delay)
                    *out_enc_delay = d;
                if (out_enc_padding)
                    *out_enc_padding = pad;
                if (out_has_delay)
                    *out_has_delay = TRUE;
            }
        }
        return TRUE;
    }
    if (36 + 26 <= fsize && memcmp(frame + 36, "VBRI", 4) == 0)
    {
        int d = ((int)frame[42] << 8) | (int)frame[43];
        if (d > 0 && d <= 2880)
        {
            if (out_enc_delay)
                *out_enc_delay = d;
            if (out_has_delay)
                *out_has_delay = TRUE;
        }
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
    uint16_t max_frame_size = 0;
    int enc_delay = 0, enc_padding = 0;
    BOOL has_vbr_delay = FALSE;
    BOOL checked_first_vbr = FALSE;
    BOOL in_sync = FALSE;
    int64_t raw_samples, usable_samples;
    int lead_skip = 0;
    WORD avg_block_size;
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

            // stop at APEv2 or Lyrics3 tag if encountered
            if ((rel + 8 <= win_valid && memcmp(win + rel, "APETAGEX", 8) == 0) ||
                (rel + 11 <= win_valid && memcmp(win + rel, "LYRICSBEGIN", 11) == 0))
            {
                break;
            }

            if (parse_mp3_header(win + rel, &ver, &sr, &ch, &spf, &fsize, &kbps) &&
                pos + (uint64_t)fsize <= end_off &&
                (stream_ver < 0 || (ver == stream_ver && sr == stream_sr)))
            {
                // verify next header at pos + fsize before locking (or re-locking after lost sync)
                BOOL sync_ok = in_sync;
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
                    in_sync = TRUE;
                    if (stream_ver < 0)
                    {
                        stream_ver = ver;
                        stream_sr = sr;
                        stream_ch = ch;
                        stream_spf = spf;
                    }

                    // skip leading Xing / Info / VBRI metadata-only frame and capture LAME delay/padding
                    if (!checked_first_vbr)
                    {
                        checked_first_vbr = TRUE;
                        if (rel + (size_t)fsize <= win_valid &&
                            parse_vbr_info_frame(win + rel, fsize, ver, ch,
                                                 &enc_delay, &enc_padding, &has_vbr_delay))
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
                        if ((uint16_t)fsize > max_frame_size)
                            max_frame_size = (uint16_t)fsize;
                        count++;
                        total_frame_bytes += (uint64_t)fsize;
                    }
                    pos += (uint64_t)fsize;
                    continue;
                }
            }

            // sync lost or not yet found
            in_sync = FALSE;
            if (stream_ver < 0 && pos >= search_limit)
            {
                break;
            }
            // if trailing metadata/padding is hit near EOF after valid frames, stop scanning past 64 KB
            if (stream_ver >= 0 && count > 0 &&
                pos > (uint64_t)frames[count - 1].offset + (uint64_t)frames[count - 1].size + 65536u)
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

    // MPEG Layer III synthesis filterbank adds 529 samples of decoder delay on top of encoder delay
    raw_samples = (int64_t)count * (int64_t)stream_spf;
    usable_samples = raw_samples;
    if (has_vbr_delay)
    {
        int cand_skip = enc_delay + 529;
        int64_t trimmed = raw_samples - (int64_t)enc_delay - (int64_t)enc_padding;
        if (trimmed > 0 && raw_samples > (int64_t)cand_skip)
        {
            lead_skip = cand_skip;
            usable_samples = trimmed;
            if ((int64_t)lead_skip + usable_samples > raw_samples)
            {
                usable_samples = raw_samples - (int64_t)lead_skip;
            }
        }
    }
    else if (raw_samples > (int64_t)stream_spf * 2)
    {
        lead_skip = 529;
        usable_samples = raw_samples - 529;
    }
    if (usable_samples > 0x7FFFFFFFLL)
        usable_samples = 0x7FFFFFFFLL;
    if (usable_samples < 1)
        usable_samples = 1;

    input->audio_kind = AUDIO_SRC_MP3_ACM;
    input->mp3_frames = frames;
    input->mp3_frame_count = count;
    input->mp3_spf = stream_spf;
    input->mp3_next_frame = 0;
    input->mp3_lead_skip = lead_skip;
    input->src_audio_rate = stream_sr;
    input->src_audio_ch = stream_ch;
    input->src_audio_bits = 16;
    input->src_audio_samples = (int)usable_samples;
    input->req_audio_rate = stream_sr;
    input->req_audio_ch = stream_ch;

    avg_block_size = (WORD)((total_frame_bytes + (uint64_t)count - 1u) / (uint64_t)count);
    if (avg_block_size < 24)
        avg_block_size = (max_frame_size >= 24) ? max_frame_size : frames[0].size;

    memset(&mp3wfx, 0, sizeof(mp3wfx));
    mp3wfx.wfx.wFormatTag = WAVE_FORMAT_MPEGLAYER3;
    mp3wfx.wfx.nChannels = (WORD)stream_ch;
    mp3wfx.wfx.nSamplesPerSec = (DWORD)stream_sr;
    mp3wfx.wfx.nAvgBytesPerSec = (DWORD)((total_frame_bytes * (uint64_t)stream_sr) /
                                         (uint64_t)raw_samples);
    if (mp3wfx.wfx.nAvgBytesPerSec == 0)
    {
        mp3wfx.wfx.nAvgBytesPerSec = 16000;
    }
    mp3wfx.wfx.nBlockAlign = 1;
    mp3wfx.wfx.wBitsPerSample = 0;
    mp3wfx.wfx.cbSize = MPEGLAYER3_WFX_EXTRA_BYTES;
    mp3wfx.wID = MPEGLAYER3_ID_MPEG;
    mp3wfx.fdwFlags = MPEGLAYER3_FLAG_PADDING_OFF;
    mp3wfx.nBlockSize = avg_block_size;
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

static BOOL try_open_wav_or_mp3_file(VFWInput *input, const char *filepath, BOOL wav_only)
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

    if (wav_only)
    {
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

static BOOL load_still_image_into_buffers(VFWInput *input, const char *filepath, BOOL initial_alloc)
{
    WCHAR wpath[MAX_PATH];
    void *bmp = NULL;
    UINT w = 0, h = 0;
    struct
    {
        INT X, Y, Width, Height;
    } rect;
    GdipBitmapData bd;
    UINT y, x, copy_w, copy_h;
    int dst_stride;

    if (!ensure_gdiplus() || !filepath || !filepath[0])
    {
        return FALSE;
    }
    if (MultiByteToWideChar(CP_ACP, 0, filepath, -1, wpath, MAX_PATH) <= 0 &&
        MultiByteToWideChar(932, 0, filepath, -1, wpath, MAX_PATH) <= 0)
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

    if (initial_alloc)
    {
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
    }
    else
    {
        dst_stride = input->width * 4;
        if (!input->dib_buffer || !input->rgb_buffer)
        {
            pGdipBitmapUnlockBits(bmp, &bd);
            pGdipDisposeImage(bmp);
            return FALSE;
        }
        if ((int)w != input->width || (int)h != input->height)
        {
            memset(input->dib_buffer, 0, input->dib_size);
            memset(input->rgb_buffer, 0, (size_t)input->width * (size_t)input->height * sizeof(PIXEL));
        }
    }

    copy_w = ((int)w < input->width) ? w : (UINT)input->width;
    copy_h = ((int)h < input->height) ? h : (UINT)input->height;

    for (y = 0; y < copy_h; y++)
    {
        const unsigned char *src_row = (const unsigned char *)bd.Scan0 + (ptrdiff_t)y * bd.Stride;
        // bottom-up BGRA32 for avi_file_get_video_pixelp
        unsigned char *dib_row = input->dib_buffer + (size_t)((UINT)input->height - 1u - y) * (size_t)dst_stride;
        PIXEL *rgb_row = input->rgb_buffer + (size_t)y * (size_t)input->width;
        memcpy(dib_row, src_row, (size_t)copy_w * 4u);
        for (x = 0; x < copy_w; x++)
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

// 0x0042c680 / 0x0042c740: detect sequential numbered still images (e.g. img_0001.png .. img_0100.png)
static void detect_still_image_sequence(VFWInput *input, const char *filepath)
{
    const char *base = filepath;
    const char *dot = NULL;
    const char *ext;
    const char *digit_run = NULL;
    const unsigned char *p;
    int digits = 0;
    int start_num = 0;
    size_t prefix_len;
    int count;

    input->seq_prefix[0] = '\0';
    input->seq_ext[0] = '\0';
    input->seq_digits = 0;
    input->seq_start_num = 0;
    input->cached_img_frame = 0;

    if (!filepath || !filepath[0])
        return;

    for (p = (const unsigned char *)filepath; *p; )
    {
        if (IsDBCSLeadByte(*p) && p[1])
        {
            p += 2;
            continue;
        }
        if (*p == '\\' || *p == '/')
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
    ext = dot ? dot : (filepath + strlen(filepath));

    for (p = (const unsigned char *)base; (const char *)p < ext; )
    {
        if (IsDBCSLeadByte(*p) && p[1])
        {
            digit_run = NULL;
            p += 2;
            continue;
        }
        if (*p >= '0' && *p <= '9')
        {
            if (!digit_run)
                digit_run = (const char *)p;
        }
        else
        {
            digit_run = NULL;
        }
        p++;
    }

    if (!digit_run)
        return;

    digits = (int)(ext - digit_run);
    if (digits < 1 || digits > 9)
        return;

    prefix_len = (size_t)(digit_run - filepath);
    if (prefix_len >= sizeof(input->seq_prefix) || strlen(ext) >= sizeof(input->seq_ext))
        return;

    for (count = 0; count < digits; count++)
    {
        start_num = start_num * 10 + (digit_run[count] - '0');
    }

    memcpy(input->seq_prefix, filepath, prefix_len);
    input->seq_prefix[prefix_len] = '\0';
    lstrcpynA(input->seq_ext, ext, (int)sizeof(input->seq_ext));
    input->seq_digits = digits;
    input->seq_start_num = start_num;

    for (count = 1; count < 0x7fffff && start_num <= 0x7fffffff - count; count++)
    {
        char cand[MAX_PATH * 2];
        DWORD attr;
        int n = _snprintf(cand, sizeof(cand), "%s%0*d%s",
                          input->seq_prefix, input->seq_digits,
                          start_num + count, input->seq_ext);
        cand[sizeof(cand) - 1] = '\0';
        if (n <= 0 || n >= MAX_PATH)
            break;
        attr = GetFileAttributesA(cand);
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
            break;
    }

    input->frame_count = count;
}

static BOOL ensure_still_image_frame(VFWInput *input, int frame_num)
{
    int f;
    char cand[MAX_PATH * 2];
    int n;

    if (!input || input->kind != INPUT_KIND_STILL_IMAGE)
        return FALSE;
    if (input->frame_count <= 1 || input->seq_digits <= 0)
        return TRUE;

    f = frame_num;
    if (f < 0)
        f = 0;
    if (f >= input->frame_count)
        f = input->frame_count - 1;
    if (f == input->cached_img_frame)
        return TRUE;

    n = _snprintf(cand, sizeof(cand), "%s%0*d%s",
                  input->seq_prefix, input->seq_digits,
                  input->seq_start_num + f, input->seq_ext);
    cand[sizeof(cand) - 1] = '\0';
    if (n <= 0 || n >= MAX_PATH)
        return FALSE;
    if (!load_still_image_into_buffers(input, cand, FALSE))
        return FALSE;
    input->cached_img_frame = f;
    return TRUE;
}

static BOOL try_open_still_image(VFWInput *input, const char *filepath)
{
    if (!load_still_image_into_buffers(input, filepath, TRUE))
    {
        return FALSE;
    }
    detect_still_image_sequence(input, filepath);
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
            int64_t scaled = ((int64_t)input->src_audio_samples * input->req_audio_rate) /
                             input->src_audio_rate;
            if (scaled > 0x7FFFFFFFLL)
                scaled = 0x7FFFFFFFLL;
            fi->audio_n = (int)scaled;
        }
        else if (fi->frame_n > 0 && fi->video_rate > 0)
        {
            int64_t scaled = ((int64_t)fi->frame_n * fi->audio_rate * fi->video_scale) /
                             fi->video_rate;
            if (scaled > 0x7FFFFFFFLL)
                scaled = 0x7FFFFFFFLL;
            fi->audio_n = (int)scaled;
        }
    }
    return TRUE;
}

static VFWInput *alloc_vfw_input(const char *filepath)
{
    VFWInput *input = (VFWInput *)calloc(1, sizeof(VFWInput));
    if (!input)
    {
        return NULL;
    }
    input->hfile = INVALID_HANDLE_VALUE;
    input->aui_last_decoded_frame = -1;
    input->bridge_aui_slot = -1;
    strncpy(input->filepath, filepath, MAX_PATH - 1);
    input->filepath[MAX_PATH - 1] = '\0';
    return input;
}

#pragma pack(push, 1)
typedef struct
{
    uint32_t fccType;
    uint32_t fccHandler;
    uint32_t dwFlags;
    uint16_t wPriority;
    uint16_t wLanguage;
    uint32_t dwInitialFrames;
    uint32_t dwScale;
    uint32_t dwRate;
    uint32_t dwStart;
    uint32_t dwLength;
    uint32_t dwSuggestedBufferSize;
    uint32_t dwQuality;
    uint32_t dwSampleSize;
    int16_t rcLeft;
    int16_t rcTop;
    int16_t rcRight;
    int16_t rcBottom;
} AviRiffStreamHeader;

typedef struct
{
    uint16_t wLongsPerEntry;
    uint8_t bIndexSubType;
    uint8_t bIndexType;
    uint32_t nEntriesInUse;
    uint32_t dwChunkId;
    uint32_t dwReserved[3];
} AviSuperIndexHdr;

typedef struct
{
    uint64_t qwOffset;
    uint32_t dwSize;
    uint32_t dwDuration;
} AviSuperIndexRec;

typedef struct
{
    uint16_t wLongsPerEntry;
    uint8_t bIndexSubType;
    uint8_t bIndexType;
    uint32_t nEntriesInUse;
    uint32_t dwChunkId;
    uint64_t qwBaseOffset;
    uint32_t dwReserved3;
} AviStdIndexHdr;

typedef struct
{
    uint32_t dwOffset;
    uint32_t dwSize;
} AviStdIndexRec;

typedef struct
{
    uint32_t ckid;
    uint32_t dwFlags;
    uint32_t dwChunkOffset;
    uint32_t dwChunkLength;
} AviIdx1Rec;
#pragma pack(pop)

static BOOL avi_parse_ckid(uint32_t ckid, int *out_stream_idx, BOOL *out_is_vid, BOOL *out_is_aud)
{
    unsigned char c0 = (unsigned char)(ckid & 0xFFu);
    unsigned char c1 = (unsigned char)((ckid >> 8) & 0xFFu);
    unsigned char c2 = (unsigned char)((ckid >> 16) & 0xFFu);
    unsigned char c3 = (unsigned char)((ckid >> 24) & 0xFFu);

    if (c0 < '0' || c0 > '9' || c1 < '0' || c1 > '9')
        return FALSE;
    *out_stream_idx = (int)(c0 - '0') * 10 + (int)(c1 - '0');
    *out_is_vid = ((c2 == 'd' || c2 == 'D') &&
                   (c3 == 'b' || c3 == 'B' || c3 == 'c' || c3 == 'C'));
    *out_is_aud = ((c2 == 'w' || c2 == 'W') &&
                   (c3 == 'b' || c3 == 'B'));
    return (*out_is_vid || *out_is_aud);
}

static BOOL odml_append_chunk(OdmlChunkEntry **arr, int *cnt, int *cap,
                              uint64_t offset, uint32_t size_and_flags)
{
    if (*cnt >= *cap)
    {
        int ncap = (*cap == 0) ? 4096 : (*cap * 2);
        OdmlChunkEntry *nb = (OdmlChunkEntry *)realloc(*arr, (size_t)ncap * sizeof(OdmlChunkEntry));
        if (!nb)
            return FALSE;
        *arr = nb;
        *cap = ncap;
    }
    (*arr)[*cnt].offset = offset;
    (*arr)[*cnt].size_and_flags = size_and_flags;
    (*cnt)++;
    return TRUE;
}

static BOOL odml_read_std_index_at(HANDLE hf, uint64_t ix_hdr_or_body_pos, BOOL has_chunk_hdr,
                                   BOOL is_audio, OdmlChunkEntry **arr, int *cnt, int *cap)
{
    AviStdIndexHdr shdr;
    uint64_t body_pos = ix_hdr_or_body_pos;
    DWORD got = 0;
    AviStdIndexRec *recs;
    size_t bytes_need;
    uint32_t i;

    if (has_chunk_hdr)
        body_pos += 8ULL;

    if (!file_read_at(hf, body_pos, &shdr, sizeof(shdr), &got) || got != sizeof(shdr))
        return FALSE;
    if (shdr.wLongsPerEntry != 2 || shdr.bIndexType != 0x01 || shdr.bIndexSubType != 0)
        return FALSE;
    if (shdr.nEntriesInUse == 0)
        return TRUE;
    if (shdr.nEntriesInUse > 16777216u)
        return FALSE;

    bytes_need = (size_t)shdr.nEntriesInUse * sizeof(AviStdIndexRec);
    recs = (AviStdIndexRec *)malloc(bytes_need);
    if (!recs)
        return FALSE;
    if (!file_read_at(hf, body_pos + sizeof(shdr), recs, (DWORD)bytes_need, &got) ||
        got != (DWORD)bytes_need)
    {
        free(recs);
        return FALSE;
    }

    for (i = 0; i < shdr.nEntriesInUse; i++)
    {
        uint64_t payload_off = shdr.qwBaseOffset + (uint64_t)recs[i].dwOffset;
        uint32_t sf = is_audio ? (recs[i].dwSize & 0x7FFFFFFFu) : recs[i].dwSize;
        if (!odml_append_chunk(arr, cnt, cap, payload_off, sf))
        {
            free(recs);
            return FALSE;
        }
    }
    free(recs);
    return TRUE;
}

static BOOL odml_parse_indx_chunk(HANDLE hf, uint64_t indx_body_pos, uint32_t indx_cb,
                                  BOOL is_audio, OdmlChunkEntry **arr, int *cnt, int *cap)
{
    AviSuperIndexHdr hdr;
    DWORD got = 0;
    uint32_t i;

    if (indx_cb < sizeof(hdr))
        return FALSE;
    if (!file_read_at(hf, indx_body_pos, &hdr, sizeof(hdr), &got) || got != sizeof(hdr))
        return FALSE;

    // direct AVISTDINDEX stored inside 'indx'
    if (hdr.bIndexType == 0x01 && hdr.wLongsPerEntry == 2)
    {
        return odml_read_std_index_at(hf, indx_body_pos, FALSE, is_audio, arr, cnt, cap);
    }

    // AVISUPERINDEX (index of 'ixNN' standard index chunks across RIFF AVI + AVIX segments)
    if (hdr.bIndexType != 0x00 || hdr.wLongsPerEntry != 4 || hdr.nEntriesInUse == 0)
        return FALSE;
    if ((uint64_t)sizeof(hdr) + (uint64_t)hdr.nEntriesInUse * sizeof(AviSuperIndexRec) > indx_cb ||
        hdr.nEntriesInUse > 65536u)
    {
        return FALSE;
    }

    for (i = 0; i < hdr.nEntriesInUse; i++)
    {
        AviSuperIndexRec srec;
        if (!file_read_at(hf, indx_body_pos + sizeof(hdr) + (uint64_t)i * sizeof(srec),
                          &srec, sizeof(srec), &got) ||
            got != sizeof(srec) || srec.qwOffset == 0)
        {
            break;
        }
        if (!odml_read_std_index_at(hf, srec.qwOffset, TRUE, is_audio, arr, cnt, cap))
            break;
    }
    return (*cnt > 0);
}

static void reset_odml_attempt(VFWInput *input)
{
    if (input->aui_hic)
    {
        ICDecompressEnd(input->aui_hic);
        ICClose(input->aui_hic);
        input->aui_hic = NULL;
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
    free(input->aui_hic_out_buf);
    input->aui_hic_out_buf = NULL;
    free(input->aui_vid_bih);
    input->aui_vid_bih = NULL;
    free(input->aui_vid_buf);
    input->aui_vid_buf = NULL;
    free(input->acm_src_buf);
    input->acm_src_buf = NULL;
    free(input->acm_dst_buf);
    input->acm_dst_buf = NULL;
    free(input->pcm_cache);
    input->pcm_cache = NULL;
    input->pcm_cache_cap = 0;
    input->pcm_cache_len = 0;
    free(input->odml_vid_chunks);
    input->odml_vid_chunks = NULL;
    input->odml_vid_count = 0;
    free(input->odml_aud_chunks);
    input->odml_aud_chunks = NULL;
    input->odml_aud_count = 0;
    free(input->odml_aud_cum_bytes);
    input->odml_aud_cum_bytes = NULL;
    input->audio_kind = AUDIO_SRC_NONE;
    input->width = 0;
    input->height = 0;
    input->frame_count = 0;
    input->src_audio_rate = 0;
    input->src_audio_ch = 0;
    input->src_audio_samples = 0;
}

// 0x00407d70: built-in 64-bit OpenDML AVI 1.0 / AVI 2.0 ('AVIX' >2 GB) demuxer
static BOOL try_open_avi_opendml(VFWInput *input, const char *filepath, BOOL want_video, BOOL want_audio)
{
    HANDLE hf;
    LARGE_INTEGER fsz_li;
    uint64_t file_size, riff0_end, pos;
    unsigned char top_hdr[12];
    DWORD got = 0;
    uint32_t riff0_sz = 0;
    uint64_t hdrl_body = 0, movi0_pos = 0, idx1_body = 0;
    uint32_t hdrl_sz = 0, movi0_sz = 0, idx1_sz = 0;

    int vid_stream_idx = -1, aud_stream_idx = -1;
    AviRiffStreamHeader vstrh, astrh;
    BITMAPINFOHEADER *vstrf = NULL;
    uint32_t vstrf_bytes = 0;
    WAVEFORMATEX *astrf = NULL;
    uint32_t astrf_bytes = 0;
    uint64_t vindx_body = 0, aindx_body = 0;
    uint32_t vindx_sz = 0, aindx_sz = 0;

    int vid_cap = 0, aud_cap = 0;
    BOOL video_ok = FALSE, audio_ok = FALSE;

    ZeroMemory(&vstrh, sizeof(vstrh));
    ZeroMemory(&astrh, sizeof(astrh));

    hf = CreateFileA(filepath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
        return FALSE;

    if (!GetFileSizeEx(hf, &fsz_li) || fsz_li.QuadPart < 32 ||
        !file_read_at(hf, 0, top_hdr, 12, &got) || got != 12 ||
        memcmp(top_hdr, "RIFF", 4) != 0 || memcmp(top_hdr + 8, "AVI ", 4) != 0)
    {
        CloseHandle(hf);
        return FALSE;
    }

    file_size = (uint64_t)fsz_li.QuadPart;
    memcpy(&riff0_sz, top_hdr + 4, 4);
    riff0_end = 8ULL + (uint64_t)riff0_sz;
    if (riff0_end > file_size || riff0_sz == 0)
        riff0_end = file_size;

    // scan top-level chunks in Segment 0 ('RIFF AVI ')
    pos = 12ULL;
    while (pos + 8ULL <= riff0_end)
    {
        unsigned char ch[12];
        uint32_t cb = 0;
        if (!file_read_at(hf, pos, ch, 8, &got) || got != 8)
            break;
        memcpy(&cb, ch + 4, 4);
        if (memcmp(ch, "LIST", 4) == 0 && pos + 12ULL <= riff0_end)
        {
            if (file_read_at(hf, pos + 8ULL, ch + 8, 4, &got) && got == 4)
            {
                if (memcmp(ch + 8, "hdrl", 4) == 0 && cb >= 4u && hdrl_body == 0)
                {
                    hdrl_body = pos + 12ULL;
                    hdrl_sz = cb - 4u;
                }
                else if (memcmp(ch + 8, "movi", 4) == 0 && cb >= 4u && movi0_pos == 0)
                {
                    movi0_pos = pos;
                    movi0_sz = cb;
                }
            }
        }
        else if (memcmp(ch, "idx1", 4) == 0 && idx1_body == 0)
        {
            idx1_body = pos + 8ULL;
            idx1_sz = cb;
        }
        pos += 8ULL + (uint64_t)cb + (uint64_t)(cb & 1u);
    }

    if (hdrl_body == 0 || hdrl_sz < 12u)
    {
        CloseHandle(hf);
        return FALSE;
    }

    // parse stream headers inside LIST 'hdrl'
    {
        uint64_t h_end = hdrl_body + (uint64_t)hdrl_sz;
        uint64_t hp = hdrl_body;
        int s_idx = 0;
        if (h_end > riff0_end)
            h_end = riff0_end;

        while (hp + 8ULL <= h_end)
        {
            unsigned char ch[12];
            uint32_t cb = 0;
            if (!file_read_at(hf, hp, ch, 8, &got) || got != 8)
                break;
            memcpy(&cb, ch + 4, 4);

            if (memcmp(ch, "LIST", 4) == 0 && cb >= 4u && hp + 12ULL <= h_end &&
                file_read_at(hf, hp + 8ULL, ch + 8, 4, &got) && got == 4 &&
                memcmp(ch + 8, "strl", 4) == 0)
            {
                uint64_t sp = hp + 12ULL;
                uint64_t s_end = hp + 8ULL + (uint64_t)cb;
                AviRiffStreamHeader cur_strh;
                BOOL have_strh = FALSE;
                uint64_t cur_strf_pos = 0, cur_indx_pos = 0;
                uint32_t cur_strf_cb = 0, cur_indx_cb = 0;

                if (s_end > h_end)
                    s_end = h_end;
                ZeroMemory(&cur_strh, sizeof(cur_strh));

                while (sp + 8ULL <= s_end)
                {
                    unsigned char sch[8];
                    uint32_t scb = 0;
                    if (!file_read_at(hf, sp, sch, 8, &got) || got != 8)
                        break;
                    memcpy(&scb, sch + 4, 4);
                    if (memcmp(sch, "strh", 4) == 0 && scb >= sizeof(AviRiffStreamHeader) && !have_strh)
                    {
                        if (file_read_at(hf, sp + 8ULL, &cur_strh, sizeof(cur_strh), &got) &&
                            got == sizeof(cur_strh))
                        {
                            have_strh = TRUE;
                        }
                    }
                    else if (memcmp(sch, "strf", 4) == 0 && scb > 0 && cur_strf_pos == 0)
                    {
                        cur_strf_pos = sp + 8ULL;
                        cur_strf_cb = scb;
                    }
                    else if (memcmp(sch, "indx", 4) == 0 && scb >= sizeof(AviSuperIndexHdr) && cur_indx_pos == 0)
                    {
                        cur_indx_pos = sp + 8ULL;
                        cur_indx_cb = scb;
                    }
                    sp += 8ULL + (uint64_t)scb + (uint64_t)(scb & 1u);
                }

                if (have_strh)
                {
                    if (cur_strh.fccType == streamtypeVIDEO && vid_stream_idx < 0 &&
                        cur_strf_pos != 0 && cur_strf_cb >= sizeof(BITMAPINFOHEADER) && cur_strf_cb <= 65536u)
                    {
                        vstrf = (BITMAPINFOHEADER *)calloc(1, (size_t)cur_strf_cb + 16u);
                        if (vstrf && file_read_at(hf, cur_strf_pos, vstrf, cur_strf_cb, &got) && got == cur_strf_cb)
                        {
                            vid_stream_idx = s_idx;
                            vstrh = cur_strh;
                            vstrf_bytes = cur_strf_cb;
                            vindx_body = cur_indx_pos;
                            vindx_sz = cur_indx_cb;
                        }
                        else
                        {
                            free(vstrf);
                            vstrf = NULL;
                        }
                    }
                    else if (cur_strh.fccType == streamtypeAUDIO && aud_stream_idx < 0 &&
                             cur_strf_pos != 0 && cur_strf_cb >= 14u && cur_strf_cb <= 65536u)
                    {
                        size_t alloc_wfx = cur_strf_cb < sizeof(WAVEFORMATEX) ? sizeof(WAVEFORMATEX) : (size_t)cur_strf_cb;
                        astrf = (WAVEFORMATEX *)calloc(1, alloc_wfx + 16u);
                        if (astrf && file_read_at(hf, cur_strf_pos, astrf, cur_strf_cb, &got) && got == cur_strf_cb)
                        {
                            aud_stream_idx = s_idx;
                            astrh = cur_strh;
                            astrf_bytes = (uint32_t)alloc_wfx;
                            aindx_body = cur_indx_pos;
                            aindx_sz = cur_indx_cb;
                        }
                        else
                        {
                            free(astrf);
                            astrf = NULL;
                        }
                    }
                    s_idx++;
                }
            }
            hp += 8ULL + (uint64_t)cb + (uint64_t)(cb & 1u);
        }
    }

    input->hfile = hf;

    // parse OpenDML super-indices ('indx' -> 'ix00'/'ix01') first
    if (want_video && vid_stream_idx >= 0 && vindx_body != 0)
    {
        odml_parse_indx_chunk(hf, vindx_body, vindx_sz, FALSE,
                              &input->odml_vid_chunks, &input->odml_vid_count, &vid_cap);
    }
    if (want_audio && aud_stream_idx >= 0 && aindx_body != 0)
    {
        odml_parse_indx_chunk(hf, aindx_body, aindx_sz, TRUE,
                              &input->odml_aud_chunks, &input->odml_aud_count, &aud_cap);
    }

    // fallback to legacy 'idx1' if OpenDML index was absent for requested stream(s)
    if (((want_video && vid_stream_idx >= 0 && input->odml_vid_count == 0) ||
         (want_audio && aud_stream_idx >= 0 && input->odml_aud_count == 0)) &&
        idx1_body != 0 && idx1_sz >= sizeof(AviIdx1Rec) && movi0_pos != 0)
    {
        BOOL need_vid = (want_video && vid_stream_idx >= 0 && input->odml_vid_count == 0);
        BOOL need_aud = (want_audio && aud_stream_idx >= 0 && input->odml_aud_count == 0);
        uint32_t n_idx1 = idx1_sz / (uint32_t)sizeof(AviIdx1Rec);
        if (n_idx1 <= 16777216u)
        {
            size_t idx1_bytes = (size_t)n_idx1 * sizeof(AviIdx1Rec);
            AviIdx1Rec *recs = (AviIdx1Rec *)malloc(idx1_bytes);
            if (recs && file_read_at(hf, idx1_body, recs, (DWORD)idx1_bytes, &got) && got == (DWORD)idx1_bytes)
            {
                uint64_t movi_payload_start = movi0_pos + 12ULL;
                uint64_t idx1_base = 0;
                BOOL base_set = FALSE;
                uint32_t i;

                for (i = 0; i < n_idx1; i++)
                {
                    int sidx = -1;
                    BOOL is_v = FALSE, is_a = FALSE;
                    if (!avi_parse_ckid(recs[i].ckid, &sidx, &is_v, &is_a))
                        continue;
                    if (!base_set)
                    {
                        idx1_base = ((uint64_t)recs[i].dwChunkOffset < movi_payload_start)
                                        ? (movi0_pos + 16ULL) // relative to 'movi' + skip 8-byte chunk header
                                        : 8ULL;               // relative to file start + skip 8-byte chunk header
                        base_set = TRUE;
                    }
                    if (need_vid && is_v && sidx == vid_stream_idx)
                    {
                        uint32_t sf = (recs[i].dwChunkLength & 0x7FFFFFFFu) |
                                      ((recs[i].dwFlags & AVIIF_KEYFRAME) ? 0u : 0x80000000u);
                        if (!odml_append_chunk(&input->odml_vid_chunks, &input->odml_vid_count, &vid_cap,
                                               idx1_base + (uint64_t)recs[i].dwChunkOffset, sf))
                        {
                            break;
                        }
                    }
                    else if (need_aud && is_a && sidx == aud_stream_idx)
                    {
                        if (!odml_append_chunk(&input->odml_aud_chunks, &input->odml_aud_count, &aud_cap,
                                               idx1_base + (uint64_t)recs[i].dwChunkOffset,
                                               recs[i].dwChunkLength & 0x7FFFFFFFu))
                        {
                            break;
                        }
                    }
                }
            }
            free(recs);
        }
    }

    // fallback linear scan of LIST 'movi' if neither 'indx' nor 'idx1' indexed the stream(s)
    if (((want_video && vid_stream_idx >= 0 && input->odml_vid_count == 0) ||
         (want_audio && aud_stream_idx >= 0 && input->odml_aud_count == 0)) &&
        movi0_pos != 0 && movi0_sz > 4u)
    {
        BOOL need_vid = (want_video && vid_stream_idx >= 0 && input->odml_vid_count == 0);
        BOOL need_aud = (want_audio && aud_stream_idx >= 0 && input->odml_aud_count == 0);
        uint64_t mp = movi0_pos + 12ULL;
        uint64_t m_end = movi0_pos + 8ULL + (uint64_t)movi0_sz;
        if (m_end > file_size)
            m_end = file_size;
        while (mp + 8ULL <= m_end)
        {
            unsigned char ch[12];
            uint32_t ckid = 0, cb = 0;
            int sidx = -1;
            BOOL is_v = FALSE, is_a = FALSE;
            if (!file_read_at(hf, mp, ch, 8, &got) || got != 8)
                break;
            memcpy(&ckid, ch, 4);
            memcpy(&cb, ch + 4, 4);
            if (memcmp(ch, "LIST", 4) == 0 && cb >= 4u && mp + 12ULL <= m_end &&
                file_read_at(hf, mp + 8ULL, ch + 8, 4, &got) && got == 4 &&
                memcmp(ch + 8, "rec ", 4) == 0)
            {
                mp += 12ULL; // step inside 'rec ' list
                continue;
            }
            if (avi_parse_ckid(ckid, &sidx, &is_v, &is_a))
            {
                if (need_vid && is_v && sidx == vid_stream_idx)
                {
                    BOOL is_db = (((ckid >> 24) & 0xFFu) == 'b' || ((ckid >> 24) & 0xFFu) == 'B');
                    uint32_t sf = (cb & 0x7FFFFFFFu) | ((is_db || input->odml_vid_count == 0) ? 0u : 0x80000000u);
                    odml_append_chunk(&input->odml_vid_chunks, &input->odml_vid_count, &vid_cap, mp + 8ULL, sf);
                }
                else if (need_aud && is_a && sidx == aud_stream_idx)
                {
                    odml_append_chunk(&input->odml_aud_chunks, &input->odml_aud_count, &aud_cap,
                                      mp + 8ULL, cb & 0x7FFFFFFFu);
                }
            }
            mp += 8ULL + (uint64_t)cb + (uint64_t)(cb & 1u);
        }
    }

    // initialize video decoder if requested
    if (want_video && vid_stream_idx >= 0 && vstrf && input->odml_vid_count > 0)
    {
        int w = (int)vstrf->biWidth;
        int raw_h = (int)vstrf->biHeight;
        int h = (raw_h < 0) ? -raw_h : raw_h;
        DWORD comp = vstrf->biCompression;
        int bpp = vstrf->biBitCount ? (int)vstrf->biBitCount : 24;

        if (comp == mmioFOURCC('D', 'I', 'B', ' '))
        {
            comp = BI_RGB;
            vstrf->biCompression = BI_RGB;
        }

        if (w > 0 && h > 0 && (comp != BI_RGB || bpp >= 16))
        {
            uint32_t max_vchunk = 0;
            size_t raw_cap;
            int i;

            for (i = 0; i < input->odml_vid_count; i++)
            {
                uint32_t csz = input->odml_vid_chunks[i].size_and_flags & 0x7FFFFFFFu;
                if (csz > max_vchunk)
                    max_vchunk = csz;
                // uncompressed non-empty frames are always intra keyframes
                if (csz > 0 && (comp == BI_RGB || comp == BI_BITFIELDS ||
                                comp == 0x38344359u || yc48_normalize_format(comp) == 0x32595559u))
                {
                    input->odml_vid_chunks[i].size_and_flags &= 0x7FFFFFFFu;
                }
            }

            raw_cap = (size_t)w * (size_t)h * 4u + 4096u;
            if ((size_t)vstrf->biSizeImage + 4096u > raw_cap)
                raw_cap = (size_t)vstrf->biSizeImage + 4096u;
            if ((size_t)max_vchunk + 4096u > raw_cap)
                raw_cap = (size_t)max_vchunk + 4096u;

            input->aui_vid_bih = vstrf;
            vstrf = NULL;
            input->aui_vid_bih_bytes = (int)vstrf_bytes;
            input->aui_vid_buf = (unsigned char *)calloc(1, raw_cap);
            input->aui_vid_buf_cap = raw_cap;
            input->aui_vid_comp = comp;
            input->aui_vid_bpp = bpp;
            input->aui_vid_bottom_up = (raw_h >= 0);
            input->aui_vid_gmask = 0x03E0u;
            if (comp == BI_BITFIELDS && vstrf_bytes >= sizeof(BITMAPINFOHEADER) + 12u)
            {
                memcpy(&input->aui_vid_gmask,
                       ((const unsigned char *)input->aui_vid_bih) + sizeof(BITMAPINFOHEADER) + 4, 4);
            }

            if (input->aui_vid_buf)
            {
                BOOL can_decode = FALSE;
                if (comp == BI_RGB || comp == BI_BITFIELDS ||
                    comp == 0x38344359u || yc48_normalize_format(comp) == 0x32595559u)
                {
                    can_decode = TRUE;
                }
                else
                {
                    memset(&input->aui_hic_out_bih, 0, sizeof(input->aui_hic_out_bih));
                    input->aui_hic_out_bih.biSize = sizeof(BITMAPINFOHEADER);
                    input->aui_hic_out_bih.biWidth = w;
                    input->aui_hic_out_bih.biHeight = h;
                    input->aui_hic_out_bih.biPlanes = 1;
                    input->aui_hic_out_bih.biBitCount = 24;
                    input->aui_hic_out_bih.biCompression = BI_RGB;
                    input->aui_hic_out_bih.biSizeImage = (DWORD)(((w * 3 + 3) & ~3) * h);

                    if (vstrh.fccHandler != 0)
                    {
                        input->aui_hic = ICLocate(ICTYPE_VIDEO, vstrh.fccHandler,
                                                  input->aui_vid_bih,
                                                  &input->aui_hic_out_bih,
                                                  ICMODE_DECOMPRESS);
                    }
                    if (!input->aui_hic)
                    {
                        input->aui_hic = ICLocate(ICTYPE_VIDEO, 0,
                                                  input->aui_vid_bih,
                                                  &input->aui_hic_out_bih,
                                                  ICMODE_DECOMPRESS);
                    }
                    if (input->aui_hic)
                    {
                        if (ICDecompressBegin(input->aui_hic,
                                              input->aui_vid_bih,
                                              &input->aui_hic_out_bih) == ICERR_OK)
                        {
                            input->aui_hic_out_buf = (unsigned char *)calloc(
                                1, (size_t)input->aui_hic_out_bih.biSizeImage + 64u);
                        }
                        if (input->aui_hic_out_buf)
                        {
                            can_decode = TRUE;
                        }
                        else
                        {
                            ICClose(input->aui_hic);
                            input->aui_hic = NULL;
                        }
                    }
                    else if (color_plugin_get_ext_count() > 0)
                    {
                        PIXEL_YC test_yc[4];
                        can_decode = color_plugin_pixel2yc(test_yc, 2, input->aui_vid_buf,
                                                           comp, 2, 2, input->aui_vid_bottom_up);
                    }
                }

                if (can_decode)
                {
                    input->width = w;
                    input->height = h;
                    input->frame_count = input->odml_vid_count;
                    input->fps_num = (vstrh.dwRate > 0) ? (int)vstrh.dwRate : 30;
                    input->fps_den = (vstrh.dwScale > 0) ? (int)vstrh.dwScale : 1;
                    input->decode_bpp = (bpp == 32 && !input->aui_hic) ? 32 : 24;
                    input->aui_last_decoded_frame = -1;
                    video_ok = TRUE;
                }
            }
        }
    }

    // if file contains a video stream and caller asked for video, do not downgrade to audio-only
    if (want_video && vid_stream_idx >= 0 && !video_ok)
    {
        free(vstrf);
        free(astrf);
        reset_odml_attempt(input);
        return FALSE;
    }

    // initialize audio stream if requested
    if (want_audio && aud_stream_idx >= 0 && astrf && input->odml_aud_count > 0 &&
        astrf->nSamplesPerSec > 0 && astrf->nChannels > 0)
    {
        input->odml_aud_cum_bytes = (uint64_t *)malloc(((size_t)input->odml_aud_count + 1u) * sizeof(uint64_t));
        if (input->odml_aud_cum_bytes)
        {
            int i;
            uint64_t total_aud_bytes;
            WORD tag = astrf->wFormatTag;
            WORD bits = astrf->wBitsPerSample;

            input->odml_aud_cum_bytes[0] = 0;
            for (i = 0; i < input->odml_aud_count; i++)
            {
                input->odml_aud_cum_bytes[i + 1] =
                    input->odml_aud_cum_bytes[i] + (uint64_t)(input->odml_aud_chunks[i].size_and_flags & 0x7FFFFFFFu);
            }
            total_aud_bytes = input->odml_aud_cum_bytes[input->odml_aud_count];

            if (tag == WAVE_FORMAT_EXTENSIBLE && astrf_bytes >= 40u)
            {
                static const unsigned char s_guid_tail[12] = {
                    0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
                const unsigned char *sub = ((const unsigned char *)astrf) + 24;
                uint32_t sub_tag = 0;
                memcpy(&sub_tag, sub, 4);
                if (memcmp(sub + 4, s_guid_tail, 12) == 0 &&
                    (sub_tag == WAVE_FORMAT_PCM || sub_tag == WAVE_FORMAT_IEEE_FLOAT))
                {
                    tag = (WORD)sub_tag;
                }
            }

            input->src_audio_rate = (int)astrf->nSamplesPerSec;
            input->src_audio_ch = (astrf->nChannels >= 2) ? 2 : 1;
            input->req_audio_rate = input->src_audio_rate;
            input->req_audio_ch = input->src_audio_ch;

            if (total_aud_bytes > 0 &&
                ((tag == WAVE_FORMAT_PCM && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
                 (tag == WAVE_FORMAT_IEEE_FLOAT && (bits == 32 || bits == 64))))
            {
                int blk = (int)astrf->nBlockAlign;
                if (blk <= 0)
                    blk = (int)astrf->nChannels * (bits / 8);
                if (blk > 0)
                {
                    uint64_t nsmp = total_aud_bytes / (uint64_t)blk;
                    if (nsmp > 0x7FFFFFFFULL)
                        nsmp = 0x7FFFFFFFULL;
                    if (nsmp > 0)
                    {
                        input->audio_kind = AUDIO_SRC_ODML_PCM;
                        input->src_audio_bits = (int)bits;
                        input->src_audio_samples = (int)nsmp;
                        input->wav_block_align = blk;
                        input->wav_raw_ch = (int)astrf->nChannels;
                        input->wav_is_float = (tag == WAVE_FORMAT_IEEE_FLOAT);
                        audio_ok = TRUE;
                    }
                }
            }
            else if (total_aud_bytes > 0 && acm_init_decoder(input, astrf))
            {
                int64_t est = 0;
                input->audio_kind = AUDIO_SRC_ODML_ACM;
                input->src_audio_bits = 16;
                input->wav_block_align = astrf->nBlockAlign > 0 ? (int)astrf->nBlockAlign : 1;
                input->wav_avg_bps = astrf->nAvgBytesPerSec > 0 ? (int)astrf->nAvgBytesPerSec : 0;
                input->odml_aud_scale = astrh.dwScale > 0 ? astrh.dwScale : 1u;
                input->odml_aud_rate = astrh.dwRate > 0 ? astrh.dwRate : (uint32_t)input->src_audio_rate;
                input->odml_aud_vbr = (astrh.dwSampleSize == 0);

                if (video_ok && input->frame_count > 0 && input->fps_num > 0)
                {
                    est = ((int64_t)input->frame_count * input->src_audio_rate * input->fps_den) / input->fps_num;
                }
                else if (input->odml_aud_vbr && input->odml_aud_rate > 0)
                {
                    est = ((int64_t)input->odml_aud_count * (int64_t)input->odml_aud_scale * input->src_audio_rate) /
                          (int64_t)input->odml_aud_rate;
                }
                else if (input->wav_avg_bps > 0)
                {
                    est = (int64_t)((total_aud_bytes * (uint64_t)input->src_audio_rate) / (uint64_t)input->wav_avg_bps);
                }
                if (est > 0x7FFFFFFFLL)
                    est = 0x7FFFFFFFLL;
                input->src_audio_samples = (int)est;
                audio_ok = (input->src_audio_samples > 0);
            }
        }
    }

    free(vstrf);
    free(astrf);

    if (!video_ok && !audio_ok)
    {
        reset_odml_attempt(input);
        return FALSE;
    }

    input->kind = video_ok ? INPUT_KIND_ODML_AVI : INPUT_KIND_AUDIO_FILE;
    return TRUE;
}

static BOOL try_open_avi_vfw(VFWInput *input, const char *filepath, BOOL want_video, BOOL want_audio)
{
    AVISTREAMINFOA asi;
    BITMAPINFOHEADER req_bih;
    HRESULT hr;
    BOOL has_vids = FALSE;
    BOOL video_ok = FALSE;
    BOOL audio_ok = FALSE;

    hr = AVIFileOpenA(&input->pavi_file, filepath, OF_READ | OF_SHARE_DENY_WRITE, NULL);
    if (FAILED(hr) || !input->pavi_file)
    {
        input->pavi_file = NULL;
        return FALSE;
    }

    if (want_video &&
        SUCCEEDED(AVIFileGetStream(input->pavi_file, &input->pavi_stream, streamtypeVIDEO, 0)) &&
        input->pavi_stream &&
        SUCCEEDED(AVIStreamInfoA(input->pavi_stream, &asi, sizeof(asi))))
    {
        has_vids = TRUE;
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

    if (video_ok || (audio_ok && !(want_video && has_vids)))
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
        return TRUE;
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
    return FALSE;
}

static BOOL is_pure_audio_ext(const char *filepath)
{
    const char *ext = path_ext(filepath);
    return (_stricmp(ext, ".mp3") == 0 ||
            _stricmp(ext, ".mp2") == 0 ||
            _stricmp(ext, ".mpa") == 0 ||
            _stricmp(ext, ".wav") == 0 ||
            _stricmp(ext, ".flac") == 0 ||
            _stricmp(ext, ".ogg") == 0 ||
            _stricmp(ext, ".aac") == 0 ||
            _stricmp(ext, ".m4a") == 0 ||
            _stricmp(ext, ".opus") == 0 ||
            _stricmp(ext, ".wma") == 0);
}

static BOOL try_open_native_aui(VFWInput *input, int aui_idx, const char *filepath,
                                BOOL want_video, BOOL want_audio)
{
    INPUT_INFO iip;
    InputPluginInstance *inst;
    BOOL video_ok = FALSE;
    BOOL audio_ok = FALSE;

    memset(&iip, 0, sizeof(iip));
    inst = input_plugin_open_native(aui_idx, filepath, &iip);
    if (!inst)
    {
        return FALSE;
    }

    // ignore 1-frame embedded ID3v2/Vorbis/MP4 cover-art pictures on pure audio files
    if (want_video && want_audio && is_pure_audio_ext(filepath) &&
        (iip.flag & INPUT_INFO_FLAG_AUDIO) && iip.audio_n > 0 && iip.n <= 1)
    {
        want_video = FALSE;
    }

    if (want_video && (iip.flag & INPUT_INFO_FLAG_VIDEO) && iip.format)
    {
        const BITMAPINFOHEADER *pfmt = (const BITMAPINFOHEADER *)iip.format;
        int w = (int)pfmt->biWidth;
        int raw_h = (int)pfmt->biHeight;
        int h = (raw_h < 0) ? -raw_h : raw_h;
        if (w > 0 && h > 0)
        {
            int bih_sz = iip.format_size;
            size_t raw_cap;
            if (bih_sz < (int)sizeof(BITMAPINFOHEADER))
                bih_sz = (int)sizeof(BITMAPINFOHEADER);
            if ((int)pfmt->biSize > bih_sz && pfmt->biSize <= 65536u)
                bih_sz = (int)pfmt->biSize;

            input->aui_vid_bih = (BITMAPINFOHEADER *)calloc(1, (size_t)bih_sz + 16u);
            raw_cap = (size_t)w * (size_t)h * 8u + 4096u;
            if ((size_t)pfmt->biSizeImage + 4096u > raw_cap)
                raw_cap = (size_t)pfmt->biSizeImage + 4096u;
            input->aui_vid_buf = (unsigned char *)calloc(1, raw_cap);

            if (input->aui_vid_bih && input->aui_vid_buf)
            {
                DWORD comp = pfmt->biCompression;
                int bpp = pfmt->biBitCount ? (int)pfmt->biBitCount : 24;
                if (iip.format_size > 0)
                    memcpy(input->aui_vid_bih, pfmt, (size_t)iip.format_size);
                else
                    memcpy(input->aui_vid_bih, pfmt, sizeof(BITMAPINFOHEADER));

                input->aui_vid_bih_bytes = bih_sz;
                input->aui_vid_buf_cap = raw_cap;
                input->aui_vid_comp = comp;
                input->aui_vid_bpp = bpp;
                input->aui_vid_bottom_up = (raw_h >= 0);
                input->aui_vid_gmask = 0x03E0u;
                if (comp == BI_BITFIELDS && bih_sz >= (int)(sizeof(BITMAPINFOHEADER) + 12))
                {
                    memcpy(&input->aui_vid_gmask,
                           ((const unsigned char *)input->aui_vid_bih) + sizeof(BITMAPINFOHEADER) + 4,
                           4);
                }

                input->width = w;
                input->height = h;
                input->frame_count = (iip.n > 0) ? iip.n : 1;
                input->fps_num = (iip.rate > 0) ? iip.rate : 30;
                input->fps_den = (iip.scale > 0) ? iip.scale : 1;
                input->decode_bpp = (bpp == 32) ? 32 : 24;

                // try opening a VCM decompressor if FourCC is neither RGB/YC48 nor YUY2
                if (comp != BI_RGB && comp != BI_BITFIELDS &&
                    comp != 0x38344359u && comp != 0x32595559u)
                {
                    memset(&input->aui_hic_out_bih, 0, sizeof(input->aui_hic_out_bih));
                    input->aui_hic_out_bih.biSize = sizeof(BITMAPINFOHEADER);
                    input->aui_hic_out_bih.biWidth = w;
                    input->aui_hic_out_bih.biHeight = h;
                    input->aui_hic_out_bih.biPlanes = 1;
                    input->aui_hic_out_bih.biBitCount = 24;
                    input->aui_hic_out_bih.biCompression = BI_RGB;
                    input->aui_hic_out_bih.biSizeImage = (DWORD)(((w * 3 + 3) & ~3) * h);

                    input->aui_hic = ICLocate(ICTYPE_VIDEO, 0,
                                              input->aui_vid_bih,
                                              &input->aui_hic_out_bih,
                                              ICMODE_DECOMPRESS);
                    if (input->aui_hic)
                    {
                        if (ICDecompressBegin(input->aui_hic,
                                              input->aui_vid_bih,
                                              &input->aui_hic_out_bih) == ICERR_OK)
                        {
                            input->aui_hic_out_buf = (unsigned char *)calloc(
                                1, (size_t)input->aui_hic_out_bih.biSizeImage + 64u);
                        }
                        if (!input->aui_hic_out_buf)
                        {
                            ICClose(input->aui_hic);
                            input->aui_hic = NULL;
                        }
                    }
                }

                video_ok = TRUE;
            }
        }
    }

    if (want_audio && (iip.flag & INPUT_INFO_FLAG_AUDIO) &&
        iip.audio_format && iip.audio_n > 0 &&
        ((const WAVEFORMATEX *)iip.audio_format)->nSamplesPerSec > 0 &&
        ((const WAVEFORMATEX *)iip.audio_format)->nChannels > 0)
    {
        WAVEFORMATEX *pwfx = (WAVEFORMATEX *)iip.audio_format;
        WORD tag = pwfx->wFormatTag;
        WORD bits = pwfx->wBitsPerSample;

        if (tag == WAVE_FORMAT_EXTENSIBLE && iip.audio_format_size >= 40)
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

        input->src_audio_rate = (int)pwfx->nSamplesPerSec;
        input->src_audio_ch = (pwfx->nChannels >= 2) ? 2 : 1;
        input->req_audio_rate = input->src_audio_rate;
        input->req_audio_ch = input->src_audio_ch;

        if ((tag == WAVE_FORMAT_PCM && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
            (tag == WAVE_FORMAT_IEEE_FLOAT && (bits == 32 || bits == 64)))
        {
            int blk = (int)pwfx->nBlockAlign;
            if (blk <= 0)
                blk = (int)pwfx->nChannels * (bits / 8);
            if (blk > 0)
            {
                input->pcm_cache_cap = PCM_CACHE_DEFAULT_CAP;
                input->pcm_cache = (short *)calloc((size_t)input->pcm_cache_cap * 2u, sizeof(short));
                input->pcm_cache_start = 0;
                input->pcm_cache_len = 0;
                input->pcm_decode_pos = 0;
                input->audio_kind = AUDIO_SRC_AUI_PCM;
                input->src_audio_bits = (int)bits;
                input->src_audio_samples = iip.audio_n;
                input->aui_aud_raw_ch = (int)pwfx->nChannels;
                input->aui_aud_is_float = (tag == WAVE_FORMAT_IEEE_FLOAT);
                input->aui_aud_block_align = blk;
                input->aui_aud_cur_pos = 0;
                input->aui_aud_eof = FALSE;
                audio_ok = TRUE;
            }
        }
        else if (acm_init_decoder(input, pwfx))
        {
            input->audio_kind = AUDIO_SRC_AUI_ACM;
            input->src_audio_bits = 16;
            input->aui_aud_block_align = (pwfx->nBlockAlign > 0) ? (int)pwfx->nBlockAlign : 1;
            input->aui_aud_cur_pos = 0;
            if (iip.n > 0 && iip.rate > 0 && iip.scale > 0)
            {
                int64_t est = ((int64_t)iip.n * input->src_audio_rate * iip.scale) / iip.rate;
                if (est > 0x7FFFFFFFLL)
                    est = 0x7FFFFFFFLL;
                input->src_audio_samples = (int)est;
            }
            else
            {
                input->src_audio_samples = iip.audio_n;
            }
            audio_ok = (input->src_audio_samples > 0);
        }
    }

    if (!video_ok && !audio_ok)
    {
        if (input->aui_hic)
        {
            ICDecompressEnd(input->aui_hic);
            ICClose(input->aui_hic);
            input->aui_hic = NULL;
        }
        free(input->aui_hic_out_buf);
        input->aui_hic_out_buf = NULL;
        free(input->aui_vid_bih);
        input->aui_vid_bih = NULL;
        free(input->aui_vid_buf);
        input->aui_vid_buf = NULL;
        input_plugin_close_native(inst);
        return FALSE;
    }

    input->aui_inst = inst;
    input->aui_flags = iip.flag;
    input->aui_last_decoded_frame = -1;
    input->kind = video_ok ? INPUT_KIND_AUI_NATIVE : INPUT_KIND_AUDIO_FILE;
    if (!video_ok)
    {
        free(input->aui_vid_bih);
        input->aui_vid_bih = NULL;
        free(input->aui_vid_buf);
        input->aui_vid_buf = NULL;
        input->width = 0;
        input->height = 0;
        input->frame_count = 0;
    }
    return TRUE;
}

static BOOL try_open_bridged_aui(VFWInput *input, int bridge_aui_idx, const char *filepath,
                                 int open_flag, BOOL want_video, BOOL want_audio)
{
    BridgeAuiOpenInfo binfo;
    int slot;
    BOOL video_ok = FALSE;
    BOOL audio_ok = FALSE;

    memset(&binfo, 0, sizeof(binfo));
    slot = plugin_bridge_aui_open(bridge_aui_idx, filepath, open_flag, &binfo);
    if (slot < 0)
    {
        return FALSE;
    }

    if (want_video && want_audio && is_pure_audio_ext(filepath) &&
        binfo.has_audio && binfo.audio_n > 0 && binfo.frame_n <= 1)
    {
        want_video = FALSE;
    }

    if (want_video && binfo.has_video && binfo.w > 0 && binfo.h > 0)
    {
        input->width = binfo.w;
        input->height = binfo.h;
        input->frame_count = (binfo.frame_n > 0) ? binfo.frame_n : 1;
        input->fps_num = (binfo.video_rate > 0) ? binfo.video_rate : 30;
        input->fps_den = (binfo.video_scale > 0) ? binfo.video_scale : 1;
        input->decode_bpp = binfo.decode_bpp ? binfo.decode_bpp : 24;
        video_ok = TRUE;
    }

    if (want_audio && binfo.has_audio &&
        binfo.audio_rate > 0 && binfo.audio_ch > 0 && binfo.audio_n > 0)
    {
        input->audio_kind = AUDIO_SRC_AUI_BRIDGED;
        input->src_audio_rate = binfo.audio_rate;
        input->src_audio_ch = (binfo.audio_ch >= 2) ? 2 : 1;
        input->src_audio_bits = 16;
        input->src_audio_samples = binfo.audio_n;
        input->req_audio_rate = input->src_audio_rate;
        input->req_audio_ch = input->src_audio_ch;
        audio_ok = TRUE;
    }

    if (!video_ok && !audio_ok)
    {
        plugin_bridge_aui_close(slot);
        return FALSE;
    }

    input->bridge_aui_slot = slot;
    input->kind = video_ok ? INPUT_KIND_AUI_BRIDGED : INPUT_KIND_AUDIO_FILE;
    return TRUE;
}

VFWInput *vfw_input_open_aui_index(int aui_idx, const char *filepath, FILE_INFO *fi, int open_flag)
{
    VFWInput *input;
    InputPluginSummary sum;
    BOOL opened = FALSE;
    BOOL want_video = (open_flag & AVI_FILE_OPEN_FLAG_AUDIO_ONLY) == 0;
    BOOL want_audio = (open_flag & AVI_FILE_OPEN_FLAG_VIDEO_ONLY) == 0;

    if (!filepath || !filepath[0] || !input_plugin_get_summary(aui_idx, &sum))
    {
        return NULL;
    }
    vfw_ensure_initialized();
    input = alloc_vfw_input(filepath);
    if (!input)
    {
        return NULL;
    }
    if (sum.bridged_x86 && sum.bridge_aui_idx >= 0)
    {
        opened = try_open_bridged_aui(input, sum.bridge_aui_idx, filepath,
                                      open_flag, want_video, want_audio);
    }
    else
    {
        opened = try_open_native_aui(input, aui_idx, filepath, want_video, want_audio);
    }
    if (!opened)
    {
        vfw_input_close(input);
        return NULL;
    }
    if (fi)
    {
        vfw_input_get_file_info(input, fi);
    }
    return input;
}

VFWInput *vfw_input_open_ex(const char *filepath, FILE_INFO *fi, int open_flag)
{
    VFWInput *input;
    int cw, ch, crate, cscale, chz;
    int order[INPUT_PLUGIN_MAX];
    int order_n, k;
    const char *ext;
    BOOL want_video = (open_flag & AVI_FILE_OPEN_FLAG_AUDIO_ONLY) == 0;
    BOOL want_audio = (open_flag & AVI_FILE_OPEN_FLAG_VIDEO_ONLY) == 0;

    if (!filepath || !filepath[0])
    {
        return NULL;
    }

    vfw_ensure_initialized();

    input = alloc_vfw_input(filepath);
    if (!input)
    {
        return NULL;
    }

    ext = path_ext(filepath);
    order_n = input_plugin_get_priority_order(order, INPUT_PLUGIN_MAX, TRUE);

    // 0x004262b0: probe input plugins in ascending priority order
    for (k = 0; k < order_n; k++)
    {
        InputPluginSummary sum;
        BOOL opened = FALSE;
        if (!input_plugin_get_summary(order[k], &sum))
        {
            continue;
        }

        switch (sum.builtin_kind)
        {
        case BUILTIN_INPUT_AVI:
            if (_stricmp(ext, ".avi") == 0)
            {
                opened = try_open_avi_opendml(input, filepath, want_video, want_audio);
            }
            break;

        case BUILTIN_INPUT_VFW:
            if (_stricmp(ext, ".avi") == 0 || _stricmp(ext, ".avs") == 0)
            {
                opened = try_open_avi_vfw(input, filepath, want_video, want_audio);
            }
            break;

        case BUILTIN_INPUT_AVIS:
            if (_stricmp(ext, ".avs") == 0)
            {
                opened = try_open_avi_vfw(input, filepath, want_video, want_audio);
            }
            break;

        case BUILTIN_INPUT_WAV:
            if (_stricmp(ext, ".wav") == 0)
            {
                int wav_w = 0, wav_h = 0;
                input_plugin_get_wav_video_size(&wav_w, &wav_h);
                if ((want_audio || (want_video && wav_w > 0 && wav_h > 0)) &&
                    try_open_wav_or_mp3_file(input, filepath, TRUE))
                {
                    if (!want_audio)
                    {
                        input->audio_kind = AUDIO_SRC_NONE;
                    }
                    if (want_video && wav_w > 0 && wav_h > 0)
                    {
                        int dib_stride = (wav_w * 3 + 3) & ~3;
                        int sr = input->src_audio_rate > 0 ? input->src_audio_rate : 44100;
                        int fc = (int)(((int64_t)input->src_audio_samples * 30 + sr - 1) / sr);
                        if (fc < 1)
                            fc = 1;
                        input->kind = INPUT_KIND_EXEDIT_CANVAS;
                        input->width = wav_w;
                        input->height = wav_h;
                        input->frame_count = fc;
                        input->fps_num = 30;
                        input->fps_den = 1;
                        input->decode_bpp = 24;
                        input->rgb_buffer = (PIXEL *)calloc((size_t)wav_w * (size_t)wav_h, sizeof(PIXEL));
                        input->dib_size = (size_t)dib_stride * (size_t)wav_h;
                        input->dib_buffer = (unsigned char *)calloc(1, input->dib_size);
                    }
                    opened = TRUE;
                }
            }
            break;

        case BUILTIN_INPUT_BMP:
            if (want_video && _stricmp(ext, ".bmp") == 0)
            {
                opened = try_open_still_image(input, filepath);
            }
            break;

        case BUILTIN_INPUT_JPG_PNG:
            if (want_video &&
                (_stricmp(ext, ".jpg") == 0 || _stricmp(ext, ".jpeg") == 0 ||
                 _stricmp(ext, ".png") == 0))
            {
                opened = try_open_still_image(input, filepath);
            }
            break;

        default:
            if (sum.bridged_x86 && sum.bridge_aui_idx >= 0)
            {
                opened = try_open_bridged_aui(input, sum.bridge_aui_idx, filepath,
                                              open_flag, want_video, want_audio);
            }
            else if (!(sum.flag & INPUT_PLUGIN_FLAG_BUILTIN))
            {
                opened = try_open_native_aui(input, order[k], filepath,
                                             want_video, want_audio);
            }
            break;
        }

        if (opened)
        {
            if (fi)
                vfw_input_get_file_info(input, fi);
            return input;
        }
    }

    // fallback virtual exedit blank project canvas if exedit.aui is not installed
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

    // fallback MP3 / WAV audio reader (.mp3, .mp2, .mpa, header-probed WAV/MP3)
    if (want_audio && try_open_wav_or_mp3_file(input, filepath, FALSE))
    {
        if (fi)
            vfw_input_get_file_info(input, fi);
        return input;
    }

    // fallback OpenDML AVI / VFW container probe & GDI+ still image (.gif, .tif, etc.)
    if (try_open_avi_opendml(input, filepath, want_video, want_audio) ||
        try_open_avi_vfw(input, filepath, want_video, want_audio))
    {
        if (fi)
            vfw_input_get_file_info(input, fi);
        return input;
    }
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
    if (input->bridge_aui_slot >= 0)
    {
        plugin_bridge_aui_close(input->bridge_aui_slot);
        input->bridge_aui_slot = -1;
    }
    if (input->aui_hic)
    {
        ICDecompressEnd(input->aui_hic);
        ICClose(input->aui_hic);
        input->aui_hic = NULL;
    }
    if (input->aui_inst)
    {
        input_plugin_close_native(input->aui_inst);
        input->aui_inst = NULL;
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
    free(input->aui_hic_out_buf);
    free(input->aui_vid_bih);
    free(input->aui_vid_buf);
    free(input->aui_aud_buf);
    free(input->aui_yc_tmp);
    free(input->mp3_frames);
    free(input->odml_vid_chunks);
    free(input->odml_aud_chunks);
    free(input->odml_aud_cum_bytes);
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

static BOOL convert_aui_vid_buf_to_yc48(VFWInput *input, PIXEL_YC *ycp, int yc_stride)
{
    int w = input->width;
    int h = input->height;
    int x, y;

    if (input->aui_hic)
    {
        int stride = (w * 3 + 3) & ~3;
        return rgb2yc_dib24(ycp, yc_stride, input->aui_hic_out_buf, stride, w, h, TRUE);
    }

    // tightly-packed YC48 ('YC48' = 0x38344359): w * 6 bytes per row
    if (input->aui_vid_comp == 0x38344359u)
    {
        for (y = 0; y < h; y++)
        {
            const PIXEL_YC *src_row =
                (const PIXEL_YC *)(input->aui_vid_buf + (size_t)y * (size_t)w * sizeof(PIXEL_YC));
            memcpy(ycp + (size_t)y * (size_t)yc_stride,
                   src_row,
                   (size_t)w * sizeof(PIXEL_YC));
        }
        return TRUE;
    }

    if (input->aui_vid_comp == BI_RGB || input->aui_vid_comp == BI_BITFIELDS)
    {
        int bpp = input->aui_vid_bpp;
        BOOL bottom_up = input->aui_vid_bottom_up;
        if (bpp == 24)
        {
            int stride = (w * 3 + 3) & ~3;
            return rgb2yc_dib24(ycp, yc_stride, input->aui_vid_buf, stride, w, h, bottom_up);
        }
        if (!input->rgb_buffer)
        {
            input->rgb_buffer = (PIXEL *)calloc((size_t)w * (size_t)h, sizeof(PIXEL));
            if (!input->rgb_buffer)
                return FALSE;
        }
        if (bpp == 32)
        {
            int stride = w * 4;
            for (y = 0; y < h; y++)
            {
                int src_y = bottom_up ? (h - 1 - y) : y;
                const unsigned char *src = input->aui_vid_buf + (size_t)src_y * (size_t)stride;
                PIXEL *row = input->rgb_buffer + (size_t)y * (size_t)w;
                for (x = 0; x < w; x++)
                {
                    row[x].b = src[x * 4 + 0];
                    row[x].g = src[x * 4 + 1];
                    row[x].r = src[x * 4 + 2];
                }
                rgb2yc(ycp + (size_t)y * (size_t)yc_stride, row, w);
            }
            return TRUE;
        }
        if (bpp == 16)
        {
            int stride = (w * 2 + 3) & ~3;
            BOOL rgb565 = (input->aui_vid_comp == BI_BITFIELDS && input->aui_vid_gmask == 0x07E0u);
            for (y = 0; y < h; y++)
            {
                int src_y = bottom_up ? (h - 1 - y) : y;
                const uint16_t *src = (const uint16_t *)(input->aui_vid_buf + (size_t)src_y * (size_t)stride);
                PIXEL *row = input->rgb_buffer + (size_t)y * (size_t)w;
                for (x = 0; x < w; x++)
                {
                    uint16_t px = src[x];
                    if (rgb565)
                    {
                        int r5 = (px >> 11) & 31;
                        int g6 = (px >> 5) & 63;
                        int b5 = px & 31;
                        row[x].r = (unsigned char)((r5 << 3) | (r5 >> 2));
                        row[x].g = (unsigned char)((g6 << 2) | (g6 >> 4));
                        row[x].b = (unsigned char)((b5 << 3) | (b5 >> 2));
                    }
                    else
                    {
                        int r5 = (px >> 10) & 31;
                        int g5 = (px >> 5) & 31;
                        int b5 = px & 31;
                        row[x].r = (unsigned char)((r5 << 3) | (r5 >> 2));
                        row[x].g = (unsigned char)((g5 << 3) | (g5 >> 2));
                        row[x].b = (unsigned char)((b5 << 3) | (b5 >> 2));
                    }
                }
                rgb2yc(ycp + (size_t)y * (size_t)yc_stride, row, w);
            }
            return TRUE;
        }
    }

    return color_plugin_pixel2yc(ycp, yc_stride,
                                 input->aui_vid_buf,
                                 input->aui_vid_comp,
                                 w, h,
                                 input->aui_vid_bottom_up);
}

static BOOL decode_native_aui_frame(VFWInput *input, int frame_num, PIXEL_YC *ycp, int yc_stride)
{
    int w = input->width;
    int h = input->height;
    int got = 0;

    if (!input->aui_inst || !input->aui_vid_buf || w <= 0 || h <= 0)
    {
        return FALSE;
    }

    if (frame_num < 0)
        frame_num = 0;
    if (frame_num >= input->frame_count)
        frame_num = input->frame_count - 1;

    if (!(input->aui_flags & INPUT_INFO_FLAG_VIDEO_RANDOM_ACCESS))
    {
        if (frame_num != input->aui_last_decoded_frame)
        {
            if (input->aui_last_decoded_frame < 0 ||
                frame_num < input->aui_last_decoded_frame ||
                frame_num > input->aui_last_decoded_frame + 1)
            {
                int kf = frame_num;
                int start_f, p;
                while (kf > 0 && !input_plugin_is_keyframe_native(input->aui_inst, kf))
                {
                    kf--;
                }
                start_f = kf;
                if (input->aui_last_decoded_frame >= 0 &&
                    input->aui_last_decoded_frame < frame_num &&
                    kf <= input->aui_last_decoded_frame)
                {
                    start_f = input->aui_last_decoded_frame + 1;
                }
                for (p = start_f; p < frame_num; p++)
                {
                    int pgot = input_plugin_read_video_native(input->aui_inst, p, input->aui_vid_buf);
                    if (input->aui_hic && pgot > 0)
                    {
                        input->aui_vid_bih->biSizeImage = (DWORD)pgot;
                        ICDecompress(input->aui_hic, ICDECOMPRESS_PREROLL,
                                     input->aui_vid_bih, input->aui_vid_buf,
                                     &input->aui_hic_out_bih, input->aui_hic_out_buf);
                    }
                }
            }
            got = input_plugin_read_video_native(input->aui_inst, frame_num, input->aui_vid_buf);
            if (input->aui_hic && got > 0)
            {
                DWORD ic_fl = input_plugin_is_keyframe_native(input->aui_inst, frame_num)
                                  ? 0
                                  : ICDECOMPRESS_NOTKEYFRAME;
                input->aui_vid_bih->biSizeImage = (DWORD)got;
                if (ICDecompress(input->aui_hic, ic_fl,
                                 input->aui_vid_bih, input->aui_vid_buf,
                                 &input->aui_hic_out_bih, input->aui_hic_out_buf) != ICERR_OK)
                {
                    return FALSE;
                }
            }
            input->aui_last_decoded_frame = frame_num;
        }
    }
    else
    {
        got = input_plugin_read_video_native(input->aui_inst, frame_num, input->aui_vid_buf);
        if (input->aui_hic)
        {
            DWORD ic_fl;
            if (got <= 0)
                return FALSE;
            ic_fl = input_plugin_is_keyframe_native(input->aui_inst, frame_num)
                        ? 0
                        : ICDECOMPRESS_NOTKEYFRAME;
            input->aui_vid_bih->biSizeImage = (DWORD)got;
            if (ICDecompress(input->aui_hic, ic_fl,
                             input->aui_vid_bih, input->aui_vid_buf,
                             &input->aui_hic_out_bih, input->aui_hic_out_buf) != ICERR_OK)
            {
                return FALSE;
            }
        }
        input->aui_last_decoded_frame = frame_num;
    }

    return convert_aui_vid_buf_to_yc48(input, ycp, yc_stride);
}

static BOOL decode_odml_avi_frame(VFWInput *input, int frame_num, PIXEL_YC *ycp, int yc_stride)
{
    int actual_f;
    uint32_t sz;
    DWORD got = 0;

    if (!input->odml_vid_chunks || input->odml_vid_count <= 0 || !input->aui_vid_buf)
        return FALSE;

    if (frame_num < 0)
        frame_num = 0;
    if (frame_num >= input->odml_vid_count)
        frame_num = input->odml_vid_count - 1;

    // step back across 0-byte drop chunks ([copy] / NULLFRAME) to nearest coded frame
    actual_f = frame_num;
    while (actual_f > 0 && (input->odml_vid_chunks[actual_f].size_and_flags & 0x7FFFFFFFu) == 0)
        actual_f--;

    sz = input->odml_vid_chunks[actual_f].size_and_flags & 0x7FFFFFFFu;
    if (sz == 0)
    {
        int y;
        for (y = 0; y < input->height; y++)
            memset(ycp + (size_t)y * (size_t)yc_stride, 0, (size_t)input->width * sizeof(PIXEL_YC));
        return TRUE;
    }

    if (actual_f != input->aui_last_decoded_frame)
    {
        if (input->aui_hic)
        {
            if (input->aui_last_decoded_frame < 0 ||
                actual_f < input->aui_last_decoded_frame ||
                actual_f > input->aui_last_decoded_frame + 1)
            {
                int kf = actual_f;
                int start_f, p;
                while (kf > 0 &&
                       ((input->odml_vid_chunks[kf].size_and_flags & 0x80000000u) != 0 ||
                        (input->odml_vid_chunks[kf].size_and_flags & 0x7FFFFFFFu) == 0))
                {
                    kf--;
                }
                start_f = kf;
                if (input->aui_last_decoded_frame >= 0 &&
                    input->aui_last_decoded_frame < actual_f &&
                    kf <= input->aui_last_decoded_frame)
                {
                    start_f = input->aui_last_decoded_frame + 1;
                }
                for (p = start_f; p < actual_f; p++)
                {
                    uint32_t psz = input->odml_vid_chunks[p].size_and_flags & 0x7FFFFFFFu;
                    if (psz > 0 && psz <= input->aui_vid_buf_cap &&
                        file_read_at(input->hfile, input->odml_vid_chunks[p].offset,
                                     input->aui_vid_buf, psz, &got) &&
                        got == psz)
                    {
                        DWORD pfl = ICDECOMPRESS_PREROLL |
                                    ((input->odml_vid_chunks[p].size_and_flags & 0x80000000u)
                                         ? ICDECOMPRESS_NOTKEYFRAME
                                         : 0);
                        input->aui_vid_bih->biSizeImage = psz;
                        ICDecompress(input->aui_hic, pfl,
                                     input->aui_vid_bih, input->aui_vid_buf,
                                     &input->aui_hic_out_bih, input->aui_hic_out_buf);
                    }
                }
            }

            if (sz > input->aui_vid_buf_cap ||
                !file_read_at(input->hfile, input->odml_vid_chunks[actual_f].offset,
                              input->aui_vid_buf, sz, &got) ||
                got != sz)
            {
                return FALSE;
            }
            {
                DWORD ic_fl = (input->odml_vid_chunks[actual_f].size_and_flags & 0x80000000u)
                                  ? ICDECOMPRESS_NOTKEYFRAME
                                  : 0;
                input->aui_vid_bih->biSizeImage = sz;
                if (ICDecompress(input->aui_hic, ic_fl,
                                 input->aui_vid_bih, input->aui_vid_buf,
                                 &input->aui_hic_out_bih, input->aui_hic_out_buf) != ICERR_OK)
                {
                    return FALSE;
                }
            }
        }
        else
        {
            if (sz > input->aui_vid_buf_cap)
                sz = (uint32_t)input->aui_vid_buf_cap;
            memset(input->aui_vid_buf, 0, input->aui_vid_buf_cap);
            if (!file_read_at(input->hfile, input->odml_vid_chunks[actual_f].offset,
                              input->aui_vid_buf, sz, &got) ||
                got == 0)
            {
                return FALSE;
            }
        }
        input->aui_last_decoded_frame = actual_f;
    }

    return convert_aui_vid_buf_to_yc48(input, ycp, yc_stride);
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

    if (input->kind == INPUT_KIND_ODML_AVI)
    {
        return decode_odml_avi_frame(input, frame_num, ycp, yc_stride);
    }

    if (input->kind == INPUT_KIND_AUI_NATIVE)
    {
        return decode_native_aui_frame(input, frame_num, ycp, yc_stride);
    }

    if (input->kind == INPUT_KIND_AUI_BRIDGED)
    {
        return plugin_bridge_aui_read_video(input->bridge_aui_slot, frame_num,
                                            ycp, yc_stride, input->width, input->height);
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
        if (!ensure_still_image_frame(input, frame_num))
            return FALSE;
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
        LPBITMAPINFOHEADER pbmi;
        const unsigned char *dib = NULL;
        int bpp = 24, stride = 0;
        BOOL bottom_up = TRUE;
        int f = frame_num;
        if (f < 0)
            f = 0;
        if (f >= input->frame_count)
            f = input->frame_count - 1;
        pbmi = (LPBITMAPINFOHEADER)AVIStreamGetFrame(input->pgf,
                                                     AVIStreamStart(input->pavi_stream) + f);
        if (pbmi && pbmi->biCompression != BI_RGB && pbmi->biCompression != BI_BITFIELDS)
        {
            const unsigned char *fdata = (const unsigned char *)pbmi + pbmi->biSize +
                                         pbmi->biClrUsed * sizeof(RGBQUAD);
            return color_plugin_pixel2yc(ycp, yc_stride, fdata, pbmi->biCompression,
                                         input->width, input->height, pbmi->biHeight >= 0);
        }
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
        int64_t scaled = ((int64_t)input->src_audio_samples * audio_rate) / input->src_audio_rate;
        if (scaled > 0x7FFFFFFFLL)
            scaled = 0x7FFFFFFFLL;
        return (int)scaled;
    }
    if (input->frame_count > 0 && input->fps_num > 0)
    {
        int64_t scaled = ((int64_t)input->frame_count * audio_rate * input->fps_den) / input->fps_num;
        if (scaled > 0x7FFFFFFFLL)
            scaled = 0x7FFFFFFFLL;
        return (int)scaled;
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
    if (src_count > input->src_audio_samples - src_start)
    {
        src_count = input->src_audio_samples - src_start;
    }
    if (src_count <= 0)
        return 0;

    if (input->audio_kind == AUDIO_SRC_AUI_PCM && input->aui_inst)
    {
        int target_end = (src_count > 0x7FFFFFFF - src_start) ? 0x7FFFFFFF : (src_start + src_count);
        int blk = input->aui_aud_block_align;

        if (blk <= 0)
            return 0;

        // fast cache hit: prevents backward 1-2 sample seeks during resampling & overlapping reads
        if (input->pcm_cache &&
            src_start >= input->pcm_cache_start &&
            target_end <= input->pcm_cache_start + input->pcm_cache_len)
        {
            int off = src_start - input->pcm_cache_start;
            memcpy(dst,
                   input->pcm_cache + (size_t)off * (size_t)src_ch,
                   (size_t)src_count * (size_t)src_ch * sizeof(short));
            return src_count;
        }

        if (!input->pcm_cache || input->pcm_cache_cap <= 0)
        {
            size_t raw_bytes = (size_t)src_count * (size_t)blk;
            unsigned char *raw = (unsigned char *)malloc(raw_bytes);
            int got;
            if (!raw)
                return 0;
            got = input_plugin_read_audio_native(input->aui_inst, src_start, src_count, raw);
            if (got > 0)
            {
                if (got > src_count)
                    got = src_count;
                convert_raw_frames_to_pcm16(raw, got,
                                            input->aui_aud_raw_ch, blk,
                                            input->src_audio_bits, input->aui_aud_is_float,
                                            dst, src_ch);
            }
            free(raw);
            return (got > 0) ? got : 0;
        }

        // seek .aui cursor only on backward miss past retained history or >1s forward jump;
        // small forward gaps (e.g. 1.5x/2x speed or dropped frames) advance sequentially
        {
            int forward_limit = (input->src_audio_rate > 16384) ? input->src_audio_rate : 16384;
            BOOL need_seek = FALSE;
            if (input->pcm_cache_len == 0)
            {
                if (src_start < input->aui_aud_cur_pos ||
                    src_start > input->aui_aud_cur_pos + forward_limit)
                {
                    need_seek = TRUE;
                }
            }
            else if (src_start < input->pcm_cache_start ||
                     src_start > input->aui_aud_cur_pos + forward_limit)
            {
                need_seek = TRUE;
            }

            if (need_seek)
            {
                int seek_pos = (src_start > 8192) ? (src_start - 4096) : 0;
                input->aui_aud_cur_pos = seek_pos;
                input->aui_aud_eof = FALSE;
                input->pcm_decode_pos = seek_pos;
                input->pcm_cache_start = seek_pos;
                input->pcm_cache_len = 0;
            }
        }

        while (!input->aui_aud_eof &&
               input->pcm_cache_start + input->pcm_cache_len < target_end &&
               input->aui_aud_cur_pos < input->src_audio_samples)
        {
            int need_now = target_end - input->aui_aud_cur_pos;
            int chunk_smp = 4096;
            int rem_smp = input->src_audio_samples - input->aui_aud_cur_pos;
            size_t need_bytes;
            int got;

            if (need_now > chunk_smp)
            {
                chunk_smp = (need_now < 16384) ? need_now : 16384;
            }
            if (chunk_smp > rem_smp)
            {
                chunk_smp = rem_smp;
            }
            if (chunk_smp <= 0)
            {
                break;
            }

            need_bytes = (size_t)chunk_smp * (size_t)blk;
            if (!input->aui_aud_buf || input->aui_aud_buf_cap < need_bytes)
            {
                size_t ncap = (need_bytes < 65536u) ? 65536u : need_bytes;
                unsigned char *nb = (unsigned char *)realloc(input->aui_aud_buf, ncap);
                if (!nb)
                    break;
                input->aui_aud_buf = nb;
                input->aui_aud_buf_cap = ncap;
            }

            got = input_plugin_read_audio_native(input->aui_inst,
                                                 input->aui_aud_cur_pos,
                                                 chunk_smp,
                                                 input->aui_aud_buf);
            if (got <= 0)
            {
                input->aui_aud_eof = TRUE;
                break;
            }
            if (got > chunk_smp)
            {
                got = chunk_smp;
            }
            input->aui_aud_cur_pos += got;

            if (!pcm_cache_append_raw(input, input->aui_aud_buf, got,
                                      input->aui_aud_raw_ch, blk,
                                      input->src_audio_bits, input->aui_aud_is_float,
                                      0, src_start))
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

    if (input->audio_kind == AUDIO_SRC_AUI_ACM && input->aui_inst)
    {
        int target_end = (src_count > 0x7FFFFFFF - src_start) ? 0x7FFFFFFF : (src_start + src_count);
        int stall_guard = 0;
        int ba = input->aui_aud_block_align > 0 ? input->aui_aud_block_align : 1;

        if (input->pcm_cache &&
            src_start >= input->pcm_cache_start &&
            target_end <= input->pcm_cache_start + input->pcm_cache_len)
        {
            int off = src_start - input->pcm_cache_start;
            memcpy(dst,
                   input->pcm_cache + (size_t)off * (size_t)src_ch,
                   (size_t)src_count * (size_t)src_ch * sizeof(short));
            return src_count;
        }

        if (input->pcm_cache_len == 0 ||
            src_start < input->pcm_cache_start ||
            src_start > input->pcm_decode_pos + input->src_audio_rate * 2)
        {
            int preroll_smp = (src_start > 8192) ? (src_start - 8192) : 0;
            input->aui_aud_cur_pos = preroll_smp;
            input->acm_src_len = 0;
            input->acm_need_start = TRUE;
            input->acm_eof_flushed = FALSE;
            input->acm_valid_from = (preroll_smp > 0) ? (src_start - 2048) : 0;
            input->pcm_decode_pos = preroll_smp;
            input->pcm_cache_start = preroll_smp;
            input->pcm_cache_len = 0;
        }

        while (input->pcm_cache_start + input->pcm_cache_len < target_end)
        {
            int produced = 0;
            if (input->aui_aud_cur_pos < input->src_audio_samples &&
                input->acm_src_len + (DWORD)ba <= input->acm_src_cap)
            {
                DWORD space = input->acm_src_cap - input->acm_src_len;
                int req_smp = (int)(space / (DWORD)ba);
                int rem_smp = input->src_audio_samples - input->aui_aud_cur_pos;
                int got_smp;
                if (req_smp > 4096)
                    req_smp = 4096;
                if (req_smp > rem_smp)
                    req_smp = rem_smp;
                got_smp = (req_smp > 0)
                              ? input_plugin_read_audio_native(input->aui_inst,
                                                               input->aui_aud_cur_pos,
                                                               req_smp,
                                                               input->acm_src_buf + input->acm_src_len)
                              : 0;
                if (got_smp > 0)
                {
                    if (got_smp > req_smp)
                        got_smp = req_smp;
                    input->acm_src_len += (DWORD)got_smp * (DWORD)ba;
                    input->aui_aud_cur_pos += got_smp;
                }
                else
                {
                    input->aui_aud_cur_pos = input->src_audio_samples;
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
        int raw_start = (src_start > 0x7FFFFFFF - input->mp3_lead_skip)
                            ? 0x7FFFFFFF
                            : (src_start + input->mp3_lead_skip);
        int target_end = (src_count > 0x7FFFFFFF - raw_start)
                             ? 0x7FFFFFFF
                             : (raw_start + src_count);
        int stall_guard = 0;

        if (input->pcm_cache &&
            raw_start >= input->pcm_cache_start &&
            target_end <= input->pcm_cache_start + input->pcm_cache_len)
        {
            int off = raw_start - input->pcm_cache_start;
            memcpy(dst,
                   input->pcm_cache + (size_t)off * (size_t)src_ch,
                   (size_t)src_count * (size_t)src_ch * sizeof(short));
            return src_count;
        }

        // seek + 10-frame VBR-safe bit-reservoir preroll if target is outside sequential window
        if (input->pcm_cache_len == 0 ||
            raw_start < input->pcm_cache_start ||
            raw_start > input->pcm_decode_pos + input->mp3_spf * 36)
        {
            int target_f = raw_start / input->mp3_spf;
            int preroll_f = target_f - 10;
            int valid_f;
            if (preroll_f < 0)
                preroll_f = 0;
            if (preroll_f >= input->mp3_frame_count)
            {
                preroll_f = input->mp3_frame_count - 1;
            }
            valid_f = (preroll_f > 0 && target_f - 2 > preroll_f) ? (target_f - 2) : preroll_f;
            input->mp3_next_frame = preroll_f;
            input->acm_src_len = 0;
            input->acm_need_start = TRUE;
            input->acm_eof_flushed = FALSE;
            input->acm_valid_from = valid_f * input->mp3_spf;
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
                if (!acm_convert_step(input, FALSE, raw_start, &produced))
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
                acm_convert_step(input, TRUE, raw_start, &produced);
                break;
            }
            else
            {
                break;
            }
        }

        if (raw_start >= input->pcm_cache_start &&
            raw_start < input->pcm_cache_start + input->pcm_cache_len)
        {
            int off = raw_start - input->pcm_cache_start;
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
        int target_end = (src_count > 0x7FFFFFFF - src_start) ? 0x7FFFFFFF : (src_start + src_count);
        int stall_guard = 0;

        if (input->pcm_cache &&
            src_start >= input->pcm_cache_start &&
            target_end <= input->pcm_cache_start + input->pcm_cache_len)
        {
            int off = src_start - input->pcm_cache_start;
            memcpy(dst,
                   input->pcm_cache + (size_t)off * (size_t)src_ch,
                   (size_t)src_count * (size_t)src_ch * sizeof(short));
            return src_count;
        }

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
            input->acm_valid_from = (input->wav_cur_byte > 0) ? src_start : 0;
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
        int target_end = (src_count > 0x7FFFFFFF - src_start) ? 0x7FFFFFFF : (src_start + src_count);
        int stall_guard = 0;

        if (input->pcm_cache &&
            src_start >= input->pcm_cache_start &&
            target_end <= input->pcm_cache_start + input->pcm_cache_len)
        {
            int off = src_start - input->pcm_cache_start;
            memcpy(dst,
                   input->pcm_cache + (size_t)off * (size_t)src_ch,
                   (size_t)src_count * (size_t)src_ch * sizeof(short));
            return src_count;
        }

        if (input->pcm_cache_len == 0 ||
            src_start < input->pcm_cache_start ||
            src_start > input->pcm_decode_pos + input->src_audio_rate * 2)
        {
            int preroll_smp = (src_start > 8192) ? (src_start - 8192) : 0;
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
            input->acm_valid_from = (preroll_smp > 0) ? (src_start - 2048) : 0;
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

    if (input->audio_kind == AUDIO_SRC_ODML_PCM && input->odml_aud_chunks && input->odml_aud_cum_bytes)
    {
        size_t raw_bytes = (size_t)src_count * (size_t)input->wav_block_align;
        unsigned char *raw = (unsigned char *)calloc(1, raw_bytes);
        uint64_t byte_pos = (uint64_t)src_start * (uint64_t)input->wav_block_align;
        uint64_t total_b = input->odml_aud_cum_bytes[input->odml_aud_count];
        size_t done = 0;
        int lo = 0, hi = input->odml_aud_count - 1, ci = 0;

        if (!raw)
            return 0;

        while (lo <= hi)
        {
            int mid = lo + (hi - lo) / 2;
            if (input->odml_aud_cum_bytes[mid + 1] <= byte_pos)
                lo = mid + 1;
            else if (input->odml_aud_cum_bytes[mid] > byte_pos)
                hi = mid - 1;
            else
            {
                ci = mid;
                break;
            }
        }
        if (lo > hi)
            ci = lo;

        while (done < raw_bytes && ci < input->odml_aud_count && byte_pos < total_b)
        {
            uint64_t c_start = input->odml_aud_cum_bytes[ci];
            uint32_t c_len = input->odml_aud_chunks[ci].size_and_flags & 0x7FFFFFFFu;
            if (c_len > 0 && byte_pos >= c_start && byte_pos < c_start + c_len)
            {
                uint32_t in_off = (uint32_t)(byte_pos - c_start);
                uint32_t avail = c_len - in_off;
                DWORD want = (raw_bytes - done < (size_t)avail) ? (DWORD)(raw_bytes - done) : avail;
                DWORD got = 0;
                if (!file_read_at(input->hfile,
                                  input->odml_aud_chunks[ci].offset + (uint64_t)in_off,
                                  raw + done, want, &got) ||
                    got == 0)
                {
                    break;
                }
                done += (size_t)got;
                byte_pos += (uint64_t)got;
                if (got < avail)
                    break;
            }
            ci++;
        }

        {
            int frames_got = (int)(done / (size_t)input->wav_block_align);
            if (frames_got > src_count)
                frames_got = src_count;
            if (frames_got > 0)
            {
                convert_raw_frames_to_pcm16(raw, frames_got,
                                            input->wav_raw_ch, input->wav_block_align,
                                            input->src_audio_bits, input->wav_is_float,
                                            dst, src_ch);
            }
            free(raw);
            return frames_got;
        }
    }

    if (input->audio_kind == AUDIO_SRC_ODML_ACM && input->odml_aud_chunks && input->odml_aud_cum_bytes)
    {
        int target_end = (src_count > 0x7FFFFFFF - src_start) ? 0x7FFFFFFF : (src_start + src_count);
        int stall_guard = 0;

        if (input->pcm_cache &&
            src_start >= input->pcm_cache_start &&
            target_end <= input->pcm_cache_start + input->pcm_cache_len)
        {
            int off = src_start - input->pcm_cache_start;
            memcpy(dst,
                   input->pcm_cache + (size_t)off * (size_t)src_ch,
                   (size_t)src_count * (size_t)src_ch * sizeof(short));
            return src_count;
        }

        if (input->pcm_cache_len == 0 ||
            src_start < input->pcm_cache_start ||
            src_start > input->pcm_decode_pos + input->src_audio_rate * 2)
        {
            int preroll_smp = (src_start > 8192) ? (src_start - 8192) : 0;
            int ci = 0;
            if (input->odml_aud_vbr && input->odml_aud_scale > 0 && input->odml_aud_rate > 0)
            {
                int64_t est_c = ((int64_t)preroll_smp * (int64_t)input->odml_aud_rate) /
                                ((int64_t)input->src_audio_rate * (int64_t)input->odml_aud_scale);
                if (est_c < 0)
                    est_c = 0;
                if (est_c >= input->odml_aud_count)
                    est_c = input->odml_aud_count - 1;
                ci = (int)est_c;
                input->pcm_decode_pos = (int)(((int64_t)ci * (int64_t)input->odml_aud_scale * input->src_audio_rate) /
                                              (int64_t)input->odml_aud_rate);
            }
            else if (input->wav_avg_bps > 0)
            {
                uint64_t approx_b = ((uint64_t)preroll_smp * (uint64_t)input->wav_avg_bps) /
                                    (uint64_t)input->src_audio_rate;
                int lo = 0, hi = input->odml_aud_count - 1;
                while (lo <= hi)
                {
                    int mid = lo + (hi - lo) / 2;
                    if (input->odml_aud_cum_bytes[mid + 1] <= approx_b)
                        lo = mid + 1;
                    else if (input->odml_aud_cum_bytes[mid] > approx_b)
                        hi = mid - 1;
                    else
                    {
                        ci = mid;
                        break;
                    }
                }
                if (lo > hi && lo < input->odml_aud_count)
                    ci = lo;
                if (ci > 0)
                    ci--;
                input->pcm_decode_pos = (int)((input->odml_aud_cum_bytes[ci] * (uint64_t)input->src_audio_rate) /
                                              (uint64_t)input->wav_avg_bps);
            }
            else
            {
                input->pcm_decode_pos = 0;
            }
            input->odml_aud_cur_chunk = ci;
            input->odml_aud_cur_chunk_off = 0;
            input->acm_src_len = 0;
            input->acm_need_start = TRUE;
            input->acm_eof_flushed = FALSE;
            input->acm_valid_from = (ci > 0 && src_start > 2048) ? (src_start - 2048) : 0;
            input->pcm_cache_start = input->pcm_decode_pos;
            input->pcm_cache_len = 0;
        }

        while (input->pcm_cache_start + input->pcm_cache_len < target_end)
        {
            int produced = 0;
            while (input->odml_aud_cur_chunk < input->odml_aud_count &&
                   input->acm_src_len + 4096 <= input->acm_src_cap)
            {
                const OdmlChunkEntry *ce = &input->odml_aud_chunks[input->odml_aud_cur_chunk];
                uint32_t clen = ce->size_and_flags & 0x7FFFFFFFu;
                uint32_t rem = (input->odml_aud_cur_chunk_off < clen)
                                   ? (clen - input->odml_aud_cur_chunk_off)
                                   : 0;
                DWORD space = input->acm_src_cap - input->acm_src_len;
                DWORD take = rem;
                DWORD got = 0;

                if (take == 0)
                {
                    input->odml_aud_cur_chunk++;
                    input->odml_aud_cur_chunk_off = 0;
                    continue;
                }
                if (take > space)
                {
                    int ba = input->wav_block_align > 0 ? input->wav_block_align : 1;
                    take = (space / (DWORD)ba) * (DWORD)ba;
                    if (take == 0)
                        break;
                }
                if (!file_read_at(input->hfile, ce->offset + input->odml_aud_cur_chunk_off,
                                  input->acm_src_buf + input->acm_src_len, take, &got) ||
                    got == 0)
                {
                    input->odml_aud_cur_chunk = input->odml_aud_count;
                    break;
                }
                input->acm_src_len += got;
                input->odml_aud_cur_chunk_off += got;
                if (input->odml_aud_cur_chunk_off >= clen)
                {
                    input->odml_aud_cur_chunk++;
                    input->odml_aud_cur_chunk_off = 0;
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
    {
        int64_t scaled = ((int64_t)input->src_audio_samples * out_rate) / src_rate;
        if (scaled > 0x7FFFFFFFLL)
            scaled = 0x7FFFFFFFLL;
        total_out = (int)scaled;
    }

    if (sample_num >= total_out)
    {
        return 0;
    }
    out_valid = samples;
    {
        int64_t avail_after = (int64_t)total_out - (int64_t)sample_num;
        if ((int64_t)out_valid > avail_after)
        {
            out_valid = (avail_after > 0) ? (int)avail_after : 0;
        }
    }
    if (out_valid <= 0)
    {
        return 0;
    }

    if (input->audio_kind == AUDIO_SRC_AUI_BRIDGED)
    {
        return plugin_bridge_aui_read_audio(input->bridge_aui_slot, sample_num,
                                            dst, out_valid, out_rate, req_ch);
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
    if (input->kind == INPUT_KIND_ODML_AVI ||
        input->kind == INPUT_KIND_AUI_NATIVE ||
        input->kind == INPUT_KIND_AUI_BRIDGED)
    {
        int w = input->width;
        int h = input->height;
        int dib_stride;
        size_t dib_need;
        if (w <= 0 || h <= 0)
            return NULL;
        dib_stride = (w * 3 + 3) & ~3;
        dib_need = (size_t)dib_stride * (size_t)h;
        if (!input->aui_yc_tmp)
        {
            input->aui_yc_tmp = (PIXEL_YC *)malloc((size_t)w * (size_t)h * sizeof(PIXEL_YC));
            if (!input->aui_yc_tmp)
                return NULL;
        }
        if (!input->dib_buffer || input->dib_size < dib_need)
        {
            unsigned char *nb = (unsigned char *)realloc(input->dib_buffer, dib_need);
            if (!nb)
                return NULL;
            input->dib_buffer = nb;
            input->dib_size = dib_need;
        }
        if (!vfw_input_read_frame_stride(input, frame_num, input->aui_yc_tmp, w))
            return NULL;
        yc2rgb_dib24(input->dib_buffer, dib_stride, input->aui_yc_tmp, w, w, h, TRUE);
        return input->dib_buffer;
    }
    if (input->kind == INPUT_KIND_EXEDIT_CANVAS)
    {
        return input->dib_buffer;
    }
    if (input->kind == INPUT_KIND_STILL_IMAGE)
    {
        if (!ensure_still_image_frame(input, frame_num))
            return NULL;
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

    if (!input)
    {
        return NULL;
    }
    if (input->kind == INPUT_KIND_ODML_AVI ||
        input->kind == INPUT_KIND_AUI_NATIVE ||
        input->kind == INPUT_KIND_AUI_BRIDGED)
    {
        int w = input->width;
        int h = input->height;
        if (w <= 0 || h <= 0)
            return NULL;
        if (!input->aui_yc_tmp)
        {
            input->aui_yc_tmp = (PIXEL_YC *)malloc((size_t)w * (size_t)h * sizeof(PIXEL_YC));
            if (!input->aui_yc_tmp)
                return NULL;
        }
        if (!input->rgb_buffer)
        {
            input->rgb_buffer = (PIXEL *)malloc((size_t)w * (size_t)h * sizeof(PIXEL));
            if (!input->rgb_buffer)
                return NULL;
        }
        if (!vfw_input_read_frame_stride(input, frame_num, input->aui_yc_tmp, w))
            return NULL;
        for (y = 0; y < h; y++)
        {
            yc2rgb(input->rgb_buffer + (size_t)y * (size_t)w,
                   input->aui_yc_tmp + (size_t)y * (size_t)w,
                   w);
        }
        return input->rgb_buffer;
    }
    if (!input->rgb_buffer)
    {
        return NULL;
    }
    if (input->kind == INPUT_KIND_EXEDIT_CANVAS)
    {
        return input->rgb_buffer;
    }
    if (input->kind == INPUT_KIND_STILL_IMAGE)
    {
        if (!ensure_still_image_frame(input, frame_num))
            return NULL;
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
    if (input->kind == INPUT_KIND_ODML_AVI)
    {
        if (frame_num >= input->odml_vid_count)
            return FALSE;
        return ((input->odml_vid_chunks[frame_num].size_and_flags & 0x80000000u) == 0 &&
                (input->odml_vid_chunks[frame_num].size_and_flags & 0x7FFFFFFFu) > 0);
    }
    if (input->kind == INPUT_KIND_AUI_NATIVE)
    {
        return input_plugin_is_keyframe_native(input->aui_inst, frame_num);
    }
    if (input->kind == INPUT_KIND_AUI_BRIDGED)
    {
        return plugin_bridge_aui_is_keyframe(input->bridge_aui_slot, frame_num);
    }
    if (input->kind == INPUT_KIND_VFW && input->pavi_stream)
    {
        LONG kf = AVIStreamFindSample(input->pavi_stream, frame_num, FIND_KEY | FIND_PREV);
        return (kf == (LONG)frame_num);
    }
    return TRUE;
}
