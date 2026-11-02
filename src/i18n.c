#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "i18n.h"

static ReLang g_lang = LANG_JA;

static const wchar_t *STRINGS[STR__COUNT][2] = {
    {L"reaviutl - AviUtl 1.10  |  File menu -> Open",
     L"reaviutl - AviUtl 1.10  |  \x30d5\x30a1\x30a4\x30eb\x30e1\x30cb\x30e5\x30fc\x2192\x958b\x304f"},
    {L"[ preview pane - no frame loaded ]",
     L"[ \x30d7\x30ec\x30d3\x30e5\x30fc - \x30d5\x30ec\x30fc\x30e0\x672a\x8aad\x307f\x8fbc\x307f ]"},
    {L"(load: pending engine)",
     L"(\x8aad\x307f\x8fbc\x307f: \x30a8\x30f3\x30b8\x30f3\x672a\x5b9f\x88c5)"},
    {L"English", L"English"},
    {L"Japanese", L"\x65e5\x672c\x8a9e"},
    {L"Language", L"\x8a00\x8a9e"},
};

ReLang i18n_get_lang(void)
{
    return g_lang;
}

void i18n_set_lang(ReLang lang)
{
    if (lang == LANG_EN || lang == LANG_JA)
    {
        g_lang = lang;
    }
}

const wchar_t *TW(ReStrId id)
{
    if ((unsigned)id >= STR__COUNT)
    {
        return L"";
    }
    return STRINGS[id][g_lang];
}

const char *T(ReStrId id)
{
    static char bufs[4][512];
    static int idx = 0;
    char *b = bufs[idx = (idx + 1) & 3];
    const wchar_t *w = TW(id);

    WideCharToMultiByte(CP_ACP, 0, w, -1, b, sizeof(bufs[0]), NULL, NULL);
    return b;
}
