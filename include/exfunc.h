#pragma once

#ifndef EXFUNC_H
#define EXFUNC_H

#include "filter.h"
#include "editp.h"

typedef FILTER *(*ExfuncFilterLookupFn)(int filter_id);
typedef BOOL (*ExfuncAddMenuItemFn)(FILTER *fp, LPSTR name, HWND hwnd,
                                    int id, int def_key, int flag);
typedef void (*ExfuncHostNotifyFn)(EDITP *ep);
typedef BOOL (*ExfuncEditOpenFn)(EDITP *ep, const char *file, int flag);
typedef BOOL (*ExfuncEditCloseFn)(EDITP *ep);
typedef BOOL (*ExfuncRenderUpToFn)(FILTER *stop_before_fp, EDITP *ep,
                                   int frame_num, PIXEL_YC *out_ycp);
typedef int (*ExfuncRenderAudioUpToFn)(FILTER *stop_before_fp, EDITP *ep,
                                       int frame_num, short *out_pcm);
typedef BOOL (*ExfuncEditOutputFn)(EDITP *ep, const char *file,
                                   int flag, const char *type);

EXFUNC *exfunc_get_table(void);
EXFUNC *exfunc_get_stub_table(void);

void exfunc_set_editp(EDITP *editp);
EDITP *exfunc_get_editp(void);

void exfunc_set_ini_file(const char *path);

void exfunc_set_filter_lookup(ExfuncFilterLookupFn fn);
void exfunc_set_add_menu_item_handler(ExfuncAddMenuItemFn fn);
void exfunc_set_host_callbacks(ExfuncHostNotifyFn on_state_changed,
                               ExfuncEditOpenFn on_edit_open,
                               ExfuncEditCloseFn on_edit_close,
                               ExfuncRenderUpToFn on_render_up_to);
void exfunc_set_audio_render_callback(ExfuncRenderAudioUpToFn on_render_audio_up_to);
void exfunc_set_edit_output_callback(ExfuncEditOutputFn on_edit_output);

void exfunc_shutdown(void);

#endif
