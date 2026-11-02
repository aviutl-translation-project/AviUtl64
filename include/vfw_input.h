#pragma once

#ifndef VFW_INPUT_H
#define VFW_INPUT_H

#include <windows.h>
#include <vfw.h>
#include "aviutl.h"
#include "filter.h"

typedef struct VFWInput VFWInput;

VFWInput *vfw_input_open(const char *filepath, FILE_INFO *fi);
VFWInput *vfw_input_open_ex(const char *filepath, FILE_INFO *fi, int open_flag);
VFWInput *vfw_input_open_aui_index(int aui_idx, const char *filepath, FILE_INFO *fi, int open_flag);
void vfw_input_close(VFWInput *input);

BOOL vfw_input_read_frame(VFWInput *input, int frame_num, PIXEL_YC *ycp);
BOOL vfw_input_read_frame_stride(VFWInput *input, int frame_num, PIXEL_YC *ycp, int yc_stride);

int vfw_input_set_audio_format(VFWInput *input, int audio_rate, int audio_ch);
int vfw_input_read_audio(VFWInput *input, int sample_num, void *buf, int samples);

PIXEL *vfw_input_get_pixel_buffer(VFWInput *input, int frame_num);
void *vfw_input_get_dib_pixelp(VFWInput *input, int frame_num);

BOOL vfw_input_get_file_info(VFWInput *input, FILE_INFO *fi);
BOOL vfw_input_is_keyframe(VFWInput *input, int frame_num);

#endif
