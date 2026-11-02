#pragma once

#ifndef I18N_H
#define I18N_H

typedef enum ReLang
{
    LANG_EN = 0,
    LANG_JA = 1
} ReLang;

typedef enum ReStrId
{
    STR_STATUS_READY = 0,
    STR_PREVIEW_EMPTY,
    STR_LOAD_PENDING,
    STR_MENU_LANG_EN,
    STR_MENU_LANG_JA,
    STR_MENU_LANGUAGE,
    STR__COUNT
} ReStrId;

ReLang i18n_get_lang(void);
void i18n_set_lang(ReLang lang);

const char *T(ReStrId id);
const wchar_t *TW(ReStrId id);

#endif
