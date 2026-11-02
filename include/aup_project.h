// aviutl 0.18 .aup project reader and writer

#pragma once

#ifndef AUP_PROJECT_H
#define AUP_PROJECT_H

#include <windows.h>
#include <stdint.h>
#include "editp.h"

#define AUP_MAGIC_018 "AviUtl ProjectFile version 0.18"
#define AUP_MAGIC_LEN 32u
#define AUP_FILTER_MAGIC "FilterProject 0.1"
#define AUP_FILTER_MAGIC_LEN 18u
#define AUP_EDIT_HANDLE_SIZE 0x004C09E8u
#define AUP_UNCOMPRESSED_PREFIX 0x0000020Cu

BOOL aup_load_local(const char *aup_path, EDITP *ep);
BOOL aup_save_local(const char *aup_path, EDITP *ep);
void aup_clear_preserved_chunks(void);

#endif
