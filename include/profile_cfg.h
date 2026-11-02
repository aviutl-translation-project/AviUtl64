#pragma once

#ifndef PROFILE_CFG_H
#define PROFILE_CFG_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include "editp.h"

#define PROFILE_CFG_MAGIC "AviUtl FilterConfigFile version 0.1"
#define PROFILE_CFG_HEADER_SIZE 36u

#define PROFILE_MAX_COUNT 96
#define PROFILE_CMD_RESET 8000
#define PROFILE_CMD_CREATE 8001
#define PROFILE_CMD_DELETE 8002
#define PROFILE_CMD_SELECT_BASE 8006

void profile_cfg_init(void);
void profile_cfg_shutdown(void);

int profile_cfg_get_count(void);
const char *profile_cfg_get_name(int index);
int profile_cfg_find_by_name(const char *name);
int profile_cfg_get_active(void);
BOOL profile_cfg_select(int index, BOOL save_current_first);
int profile_cfg_create_interactive(HWND owner, const char *initial_name);
BOOL profile_cfg_delete_active(EDITP *ep);
BOOL profile_cfg_reset_active(void);
void profile_cfg_save_active(void);

void profile_cfg_clear_project_slots(void);
BOOL profile_cfg_has_project_slot(int slot);
const char *profile_cfg_get_slot_name(int slot);
BOOL profile_cfg_get_slot_blob(int slot, const uint8_t **out_blob, size_t *out_size);
BOOL profile_cfg_set_project_slot(int slot, const char *name,
                                  const uint8_t *blob, size_t blob_size);
int profile_cfg_bind_selection_new_profile(HWND owner, EDITP *ep);
int profile_cfg_resolve_or_bind_name(const char *name);

int profile_cfg_get_applied_slot(void);
void profile_cfg_apply_slot(int slot);
void profile_cfg_apply_for_frame(EDITP *ep, int frame_num);
void profile_cfg_restore_active(void);

#endif
