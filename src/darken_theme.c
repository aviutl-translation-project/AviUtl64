#include <windows.h>
#include <commctrl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "darken_theme.h"
#include "debug_console.h"

#define WM_UAHDRAWMENU 0x0091
#define WM_UAHDRAWMENUITEM 0x0092

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

typedef struct UahMenu
{
    HMENU hmenu;
    HDC hdc;
    DWORD flags;
} UahMenu;

typedef union UahMenuItemMetrics
{
    struct
    {
        DWORD cx;
        DWORD cy;
    } bar[2];
    struct
    {
        DWORD cx;
        DWORD cy;
    } popup[4];
} UahMenuItemMetrics;

typedef struct UahMenuPopupMetrics
{
    DWORD rgcx[4];
    DWORD update_max_widths : 2;
} UahMenuPopupMetrics;

typedef struct UahMenuItem
{
    int position;
    UahMenuItemMetrics umim;
    UahMenuPopupMetrics umpm;
} UahMenuItem;

typedef struct UahDrawMenuItem
{
    DRAWITEMSTRUCT dis;
    UahMenu um;
    UahMenuItem umi;
} UahDrawMenuItem;

typedef HRESULT(WINAPI *SetWindowThemeFn)(HWND, LPCWSTR, LPCWSTR);
typedef BOOL(WINAPI *AllowDarkModeForWindowFn)(HWND, BOOL);
typedef int(WINAPI *SetPreferredAppModeFn)(int);
typedef void(WINAPI *FlushMenuThemesFn)(void);
typedef HRESULT(WINAPI *DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);

typedef DWORD(WINAPI *GetSysColorFn)(int);
typedef HBRUSH(WINAPI *GetSysColorBrushFn)(int);
typedef int(WINAPI *FillRectFn)(HDC, const RECT *, HBRUSH);
typedef BOOL(WINAPI *DrawEdgeFn)(HDC, LPRECT, UINT, UINT);

typedef struct NamedColorEntry
{
    char name[48];
    COLORREF fill;
    COLORREF edge;
    COLORREF text_fore;
    COLORREF text_back;
    BOOL has_fill;
    BOOL has_edge;
    BOOL has_text_fore;
    BOOL has_text_back;
} NamedColorEntry;

#define MAX_NAMED_COLORS 96
#define MAX_SKIP_ICONS 16
#define MAX_RECOLORED_ICONS 128

static BOOL g_active = FALSE;
static BOOL g_is_dark = TRUE;
static HHOOK g_cbt_hook = NULL;

static HMODULE g_uxtheme = NULL;
static HMODULE g_dwmapi = NULL;
static SetWindowThemeFn pSetWindowTheme = NULL;
static AllowDarkModeForWindowFn pAllowDarkModeForWindow = NULL;
static SetPreferredAppModeFn pSetPreferredAppMode = NULL;
static FlushMenuThemesFn pFlushMenuThemes = NULL;
static DwmSetWindowAttributeFn pDwmSetWindowAttribute = NULL;

static GetSysColorFn g_real_GetSysColor = NULL;
static GetSysColorBrushFn g_real_GetSysColorBrush = NULL;
static FillRectFn g_real_FillRect = NULL;
static DrawEdgeFn g_real_DrawEdge = NULL;

static NamedColorEntry g_named[MAX_NAMED_COLORS];
static int g_named_count = 0;

// resolved palette
static COLORREF g_col_dialog = RGB(0x22, 0x22, 0x22);
static COLORREF g_col_edit_bg = RGB(0x22, 0x22, 0x22);
static COLORREF g_col_edit_edge = RGB(0x33, 0x33, 0x33);
static COLORREF g_col_edit_fg = RGB(0xff, 0xff, 0xff);
static COLORREF g_col_sel_bg = RGB(0x55, 0x55, 0x55);
static COLORREF g_col_sel_fg = RGB(0xff, 0xff, 0xff);
static COLORREF g_col_btn_bg = RGB(0x22, 0x22, 0x22);
static COLORREF g_col_btn_hot = RGB(0x44, 0x44, 0x44);
static COLORREF g_col_btn_fg = RGB(0xcc, 0xcc, 0xcc);
static COLORREF g_col_static_fg = RGB(0xee, 0xee, 0xee);
static COLORREF g_col_disabled_fg = RGB(0x77, 0x77, 0x77);
static COLORREF g_col_track_gutter = RGB(0x11, 0x11, 0x11);
static COLORREF g_col_track_thumb = RGB(0x66, 0x66, 0x66);
static COLORREF g_col_track_hot = RGB(0x88, 0x88, 0x88);
static COLORREF g_col_menu_bg = RGB(0x33, 0x33, 0x33);
static COLORREF g_col_menu_hot = RGB(0x55, 0x55, 0x55);
static COLORREF g_col_menu_fg = RGB(0xcc, 0xcc, 0xcc);
static COLORREF g_col_menu_hot_fg = RGB(0xff, 0xff, 0xff);
static COLORREF g_col_menu_dis_fg = RGB(0x66, 0x66, 0x66);
static COLORREF g_col_edge_raised_tl = RGB(0x38, 0x38, 0x38);
static COLORREF g_col_edge_raised_br = RGB(0x18, 0x18, 0x18);
static COLORREF g_col_edge_sunken_tl = RGB(0x18, 0x18, 0x18);
static COLORREF g_col_edge_sunken_br = RGB(0x38, 0x38, 0x38);

// dwm attributes
static BOOL g_has_dwm_active_border = FALSE;
static BOOL g_has_dwm_active_caption = FALSE;
static BOOL g_has_dwm_active_text = FALSE;
static BOOL g_has_dwm_inactive_border = FALSE;
static BOOL g_has_dwm_inactive_caption = FALSE;
static BOOL g_has_dwm_inactive_text = FALSE;
static COLORREF g_dwm_active_border = 0;
static COLORREF g_dwm_active_caption = 0;
static COLORREF g_dwm_active_text = 0;
static COLORREF g_dwm_inactive_border = 0;
static COLORREF g_dwm_inactive_caption = 0;
static COLORREF g_dwm_inactive_text = 0;
static int g_dwm_dark_mode = -1;
static int g_dwm_corner_mode = -1;
static BOOL g_round_mode = FALSE;

// icon recoloring rules
static BOOL g_has_icon_recolor = FALSE;
static COLORREF g_icon_src = RGB(0, 0, 0);
static COLORREF g_icon_dst = RGB(210, 211, 213);
static char g_skip_icons[MAX_SKIP_ICONS][64];
static int g_skip_icon_count = 0;

static HICON g_owned_icons[MAX_RECOLORED_ICONS];
static int g_owned_icon_count = 0;

static HBRUSH g_br_dialog = NULL;
static HBRUSH g_br_edit = NULL;
static HBRUSH g_br_btn = NULL;
static HBRUSH g_br_menu = NULL;
static HBRUSH g_br_menu_hot = NULL;
static HBRUSH g_sys_brushes[32];

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static BOOL parse_color_str(const char *s, COLORREF *out)
{
    while (s && (*s == ' ' || *s == '\t'))
        s++;
    if (!s || !*s || !out)
        return FALSE;

    if (*s == '#')
    {
        int n = 0;
        const char *p = s + 1;
        while (hex_nibble(p[n]) >= 0)
            n++;
        if (n == 3)
        {
            int r = hex_nibble(p[0]);
            int g = hex_nibble(p[1]);
            int b = hex_nibble(p[2]);
            *out = RGB(r * 17, g * 17, b * 17);
            return TRUE;
        }
        if (n >= 6)
        {
            int r = (hex_nibble(p[0]) << 4) | hex_nibble(p[1]);
            int g = (hex_nibble(p[2]) << 4) | hex_nibble(p[3]);
            int b = (hex_nibble(p[4]) << 4) | hex_nibble(p[5]);
            *out = RGB(r, g, b);
            return TRUE;
        }
        return FALSE;
    }

    if (isdigit((unsigned char)*s))
    {
        int r = 0, g = 0, b = 0;
        if (sscanf(s, "%d , %d , %d", &r, &g, &b) == 3)
        {
            if (r < 0)
                r = 0;
            if (r > 255)
                r = 255;
            if (g < 0)
                g = 0;
            if (g > 255)
                g = 255;
            if (b < 0)
                b = 0;
            if (b > 255)
                b = 255;
            *out = RGB(r, g, b);
            return TRUE;
        }
    }
    return FALSE;
}

// read utf-16le or utf-8/acp xml file as null-terminated utf-8 text
static char *read_xml_utf8(const wchar_t *path_w)
{
    HANDLE hf;
    DWORD sz = 0, got = 0;
    unsigned char *raw;
    char *u8 = NULL;

    if (!path_w || !path_w[0])
        return NULL;
    hf = CreateFileW(path_w, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
        return NULL;

    sz = GetFileSize(hf, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0 || sz > 4u * 1024u * 1024u)
    {
        CloseHandle(hf);
        return NULL;
    }

    raw = (unsigned char *)malloc((size_t)sz + 4u);
    if (!raw)
    {
        CloseHandle(hf);
        return NULL;
    }
    if (!ReadFile(hf, raw, sz, &got, NULL) || got != sz)
    {
        free(raw);
        CloseHandle(hf);
        return NULL;
    }
    CloseHandle(hf);
    raw[sz] = 0;
    raw[sz + 1] = 0;

    if (sz >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
    {
        const wchar_t *ws = (const wchar_t *)(raw + 2);
        int wlen = (int)((sz - 2) / 2);
        int need = WideCharToMultiByte(CP_UTF8, 0, ws, wlen, NULL, 0, NULL, NULL);
        if (need > 0)
        {
            u8 = (char *)malloc((size_t)need + 1u);
            if (u8)
            {
                WideCharToMultiByte(CP_UTF8, 0, ws, wlen, u8, need, NULL, NULL);
                u8[need] = '\0';
            }
        }
        free(raw);
        return u8;
    }

    if (sz >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF)
    {
        u8 = (char *)malloc((size_t)(sz - 3) + 1u);
        if (u8)
        {
            memcpy(u8, raw + 3, (size_t)(sz - 3));
            u8[sz - 3] = '\0';
        }
        free(raw);
        return u8;
    }

    return (char *)raw;
}

// strip xml comments in-place so commented-out examples are ignored
static void strip_xml_comments(char *xml)
{
    char *p = xml;
    while (p && (p = strstr(p, "<!--")) != NULL)
    {
        char *end = strstr(p + 4, "-->");
        if (!end)
        {
            *p = '\0';
            break;
        }
        memset(p, ' ', (size_t)((end + 3) - p));
        p = end + 3;
    }
}

static BOOL xml_get_attr(const char *tag_start, const char *tag_end,
                         const char *attr_name, char *out, size_t out_cap)
{
    size_t nlen;
    const char *p;

    if (!tag_start || !tag_end || !attr_name || !out || out_cap == 0)
        return FALSE;
    out[0] = '\0';
    nlen = strlen(attr_name);
    p = tag_start;

    while (p < tag_end)
    {
        const char *hit = NULL;
        const char *q;
        for (q = p; q + nlen < tag_end; q++)
        {
            if (_strnicmp(q, attr_name, nlen) == 0 &&
                (q == tag_start || q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r' || q[-1] == '\n'))
            {
                const char *after = q + nlen;
                while (after < tag_end && (*after == ' ' || *after == '\t'))
                    after++;
                if (after < tag_end && *after == '=')
                {
                    hit = after + 1;
                    break;
                }
            }
        }
        if (!hit)
            return FALSE;
        while (hit < tag_end && (*hit == ' ' || *hit == '\t'))
            hit++;
        if (hit < tag_end && (*hit == '"' || *hit == '\''))
        {
            char quote = *hit++;
            const char *val_end = hit;
            size_t len;
            while (val_end < tag_end && *val_end != quote)
                val_end++;
            len = (size_t)(val_end - hit);
            if (len >= out_cap)
                len = out_cap - 1;
            memcpy(out, hit, len);
            out[len] = '\0';
            return TRUE;
        }
        p = hit + 1;
    }
    return FALSE;
}

static NamedColorEntry *upsert_named_color(const char *name)
{
    int i;
    for (i = 0; i < g_named_count; i++)
    {
        if (_stricmp(g_named[i].name, name) == 0)
            return &g_named[i];
    }
    if (g_named_count >= MAX_NAMED_COLORS)
        return NULL;
    ZeroMemory(&g_named[g_named_count], sizeof(NamedColorEntry));
    lstrcpynA(g_named[g_named_count].name, name, sizeof(g_named[g_named_count].name));
    return &g_named[g_named_count++];
}

static const NamedColorEntry *find_named_color(const char *name)
{
    int i;
    for (i = 0; i < g_named_count; i++)
    {
        if (_stricmp(g_named[i].name, name) == 0)
            return &g_named[i];
    }
    return NULL;
}

static void parse_skin_leaf_xml(const wchar_t *leaf_path)
{
    char *xml = read_xml_utf8(leaf_path);
    const char *p;

    if (!xml)
        return;
    strip_xml_comments(xml);

    p = xml;
    while ((p = strchr(p, '<')) != NULL)
    {
        const char *end = strchr(p, '>');
        char val[128];
        if (!end)
            break;

        if (_strnicmp(p + 1, "DrawSingleRaisedEdge", 20) == 0)
        {
            if (xml_get_attr(p, end, "topLeftColor", val, sizeof(val)))
                parse_color_str(val, &g_col_edge_raised_tl);
            if (xml_get_attr(p, end, "bottomRightColor", val, sizeof(val)))
                parse_color_str(val, &g_col_edge_raised_br);
        }
        else if (_strnicmp(p + 1, "DrawSingleSunkenEdge", 20) == 0)
        {
            if (xml_get_attr(p, end, "topLeftColor", val, sizeof(val)))
                parse_color_str(val, &g_col_edge_sunken_tl);
            if (xml_get_attr(p, end, "bottomRightColor", val, sizeof(val)))
                parse_color_str(val, &g_col_edge_sunken_br);
        }
        else if (_strnicmp(p + 1, "Dwm", 3) == 0 &&
                 (p[4] == ' ' || p[4] == '\t' || p[4] == '\r' || p[4] == '\n' || p[4] == '/'))
        {
            if (xml_get_attr(p, end, "activeBorderColor", val, sizeof(val)) &&
                parse_color_str(val, &g_dwm_active_border))
                g_has_dwm_active_border = TRUE;
            if (xml_get_attr(p, end, "activeCaptionColor", val, sizeof(val)) &&
                parse_color_str(val, &g_dwm_active_caption))
                g_has_dwm_active_caption = TRUE;
            if (xml_get_attr(p, end, "activeTextColor", val, sizeof(val)) &&
                parse_color_str(val, &g_dwm_active_text))
                g_has_dwm_active_text = TRUE;
            if (xml_get_attr(p, end, "inactiveBorderColor", val, sizeof(val)) &&
                parse_color_str(val, &g_dwm_inactive_border))
                g_has_dwm_inactive_border = TRUE;
            if (xml_get_attr(p, end, "inactiveCaptionColor", val, sizeof(val)) &&
                parse_color_str(val, &g_dwm_inactive_caption))
                g_has_dwm_inactive_caption = TRUE;
            if (xml_get_attr(p, end, "inactiveTextColor", val, sizeof(val)) &&
                parse_color_str(val, &g_dwm_inactive_text))
                g_has_dwm_inactive_text = TRUE;
            if (xml_get_attr(p, end, "darkMode", val, sizeof(val)))
                g_dwm_dark_mode = atoi(val);
            if (xml_get_attr(p, end, "cornerMode", val, sizeof(val)))
                g_dwm_corner_mode = atoi(val);
        }
        else if (_strnicmp(p + 1, "ChangeColor", 11) == 0)
        {
            COLORREF c_src, c_dst;
            if (xml_get_attr(p, end, "src", val, sizeof(val)) && parse_color_str(val, &c_src))
            {
                if (xml_get_attr(p, end, "dst", val, sizeof(val)) && parse_color_str(val, &c_dst))
                {
                    g_icon_src = c_src;
                    g_icon_dst = c_dst;
                    g_has_icon_recolor = TRUE;
                }
            }
        }
        else if (_strnicmp(p + 1, "EditIcon", 8) == 0 &&
                 (p[9] == ' ' || p[9] == '\t'))
        {
            if (xml_get_attr(p, end, "iconName", val, sizeof(val)) &&
                val[0] && g_skip_icon_count < MAX_SKIP_ICONS)
            {
                lstrcpynA(g_skip_icons[g_skip_icon_count++], val, 64);
            }
        }
        else if (_strnicmp(p + 1, "NamedColor", 10) == 0 &&
                 (p[11] == ' ' || p[11] == '\t'))
        {
            char cname[48];
            if (xml_get_attr(p, end, "name", cname, sizeof(cname)) && cname[0])
            {
                NamedColorEntry *nc = upsert_named_color(cname);
                if (nc)
                {
                    if (xml_get_attr(p, end, "fillColor", val, sizeof(val)) &&
                        parse_color_str(val, &nc->fill))
                        nc->has_fill = TRUE;
                    if (xml_get_attr(p, end, "edgeColor", val, sizeof(val)) &&
                        parse_color_str(val, &nc->edge))
                        nc->has_edge = TRUE;
                    if (xml_get_attr(p, end, "textForeColor", val, sizeof(val)) &&
                        parse_color_str(val, &nc->text_fore))
                        nc->has_text_fore = TRUE;
                    if (xml_get_attr(p, end, "textBackColor", val, sizeof(val)) &&
                        parse_color_str(val, &nc->text_back))
                        nc->has_text_back = TRUE;
                }
            }
        }
        p = end + 1;
    }

    free(xml);
}

static void resolve_palette_from_named_colors(void)
{
    const NamedColorEntry *nc;
    int lum, i;

    nc = find_named_color("dialog");
    if (nc && nc->has_fill)
        g_col_dialog = nc->fill;

    nc = find_named_color("c1_normal");
    if (nc)
    {
        if (nc->has_fill)
            g_col_edit_bg = nc->fill;
        if (nc->has_edge)
            g_col_edit_edge = nc->edge;
        if (nc->has_text_fore)
            g_col_edit_fg = nc->text_fore;
    }

    nc = find_named_color("c1_selected");
    if (nc)
    {
        if (nc->has_fill)
            g_col_sel_bg = nc->fill;
        if (nc->has_text_fore)
            g_col_sel_fg = nc->text_fore;
    }

    nc = find_named_color("c2_normal");
    if (nc)
    {
        if (nc->has_fill)
            g_col_btn_bg = nc->fill;
        if (nc->has_text_fore)
            g_col_btn_fg = nc->text_fore;
    }

    nc = find_named_color("c2_hot");
    if (nc && nc->has_fill)
        g_col_btn_hot = nc->fill;

    nc = find_named_color("c5_normal");
    if (nc)
    {
        if (nc->has_fill && !find_named_color("dialog"))
            g_col_dialog = nc->fill;
        if (nc->has_text_fore)
            g_col_static_fg = nc->text_fore;
    }

    nc = find_named_color("c5_disabled");
    if (nc && nc->has_text_fore)
        g_col_disabled_fg = nc->text_fore;

    nc = find_named_color("c3_gutter");
    if (nc && nc->has_fill)
        g_col_track_gutter = nc->fill;

    nc = find_named_color("c3_normal");
    if (nc && nc->has_fill)
        g_col_track_thumb = nc->fill;

    nc = find_named_color("c3_hot");
    if (nc && nc->has_fill)
        g_col_track_hot = nc->fill;

    nc = find_named_color("c4_normal");
    if (nc)
    {
        if (nc->has_fill)
            g_col_menu_bg = nc->fill;
        if (nc->has_text_fore)
            g_col_menu_fg = nc->text_fore;
    }

    nc = find_named_color("c4_hot");
    if (nc)
    {
        if (nc->has_fill)
            g_col_menu_hot = nc->fill;
        if (nc->has_text_fore)
            g_col_menu_hot_fg = nc->text_fore;
    }

    nc = find_named_color("c4_disabled");
    if (nc && nc->has_text_fore)
        g_col_menu_dis_fg = nc->text_fore;

    lum = (GetRValue(g_col_dialog) * 299 +
           GetGValue(g_col_dialog) * 587 +
           GetBValue(g_col_dialog) * 114) /
          1000;
    if (g_dwm_dark_mode == 0)
        g_is_dark = FALSE;
    else if (g_dwm_dark_mode > 0)
        g_is_dark = TRUE;
    else
        g_is_dark = (lum < 140);

    if (g_br_dialog)
        DeleteObject(g_br_dialog);
    if (g_br_edit)
        DeleteObject(g_br_edit);
    if (g_br_btn)
        DeleteObject(g_br_btn);
    if (g_br_menu)
        DeleteObject(g_br_menu);
    if (g_br_menu_hot)
        DeleteObject(g_br_menu_hot);

    g_br_dialog = CreateSolidBrush(g_col_dialog);
    g_br_edit = CreateSolidBrush(g_col_edit_bg);
    g_br_btn = CreateSolidBrush(g_col_btn_bg);
    g_br_menu = CreateSolidBrush(g_col_menu_bg);
    g_br_menu_hot = CreateSolidBrush(g_col_menu_hot);

    for (i = 0; i < 32; i++)
    {
        if (g_sys_brushes[i])
        {
            DeleteObject(g_sys_brushes[i]);
            g_sys_brushes[i] = NULL;
        }
    }
}

static COLORREF map_sys_color(int idx)
{
    switch (idx)
    {
    case COLOR_SCROLLBAR:
    case COLOR_BACKGROUND:
    case COLOR_APPWORKSPACE:
    case COLOR_BTNFACE:
    case COLOR_3DLIGHT:
        return g_col_dialog;
    case COLOR_WINDOW:
        return g_col_edit_bg;
    case COLOR_WINDOWFRAME:
    case COLOR_3DSHADOW:
        return g_col_edge_raised_br;
    case COLOR_3DDKSHADOW:
        return g_col_edge_sunken_tl;
    case COLOR_3DHILIGHT:
        return g_col_edge_raised_tl;
    case COLOR_MENU:
    case COLOR_MENUBAR:
        return g_col_menu_bg;
    case COLOR_MENUHILIGHT:
        return g_col_menu_hot;
    case COLOR_MENUTEXT:
        return g_col_menu_fg;
    case COLOR_WINDOWTEXT:
        return g_col_edit_fg;
    case COLOR_BTNTEXT:
    case COLOR_CAPTIONTEXT:
    case COLOR_INACTIVECAPTIONTEXT:
        return g_col_static_fg;
    case COLOR_GRAYTEXT:
        return g_col_disabled_fg;
    case COLOR_HIGHLIGHT:
    case COLOR_HOTLIGHT:
        return g_col_sel_bg;
    case COLOR_HIGHLIGHTTEXT:
        return g_col_sel_fg;
    default:
        return g_real_GetSysColor ? g_real_GetSysColor(idx) : GetSysColor(idx);
    }
}

static DWORD WINAPI hooked_GetSysColor(int idx)
{
    if (!g_active)
        return g_real_GetSysColor ? g_real_GetSysColor(idx) : 0;
    return (DWORD)map_sys_color(idx);
}

static HBRUSH WINAPI hooked_GetSysColorBrush(int idx)
{
    if (!g_active || idx < 0 || idx >= 31)
        return g_real_GetSysColorBrush ? g_real_GetSysColorBrush(idx) : NULL;
    if (!g_sys_brushes[idx])
        g_sys_brushes[idx] = CreateSolidBrush(map_sys_color(idx));
    return g_sys_brushes[idx];
}

static int WINAPI hooked_FillRect(HDC hdc, const RECT *lprc, HBRUSH hbr)
{
    if (g_active && (ULONG_PTR)hbr >= 1u && (ULONG_PTR)hbr <= 31u)
    {
        HBRUSH mapped = hooked_GetSysColorBrush((int)((ULONG_PTR)hbr - 1u));
        if (mapped)
            hbr = mapped;
    }
    return g_real_FillRect ? g_real_FillRect(hdc, lprc, hbr) : 0;
}

static BOOL WINAPI hooked_DrawEdge(HDC hdc, LPRECT qrc, UINT edge, UINT grfFlags)
{
    if (g_active && hdc && qrc)
    {
        BOOL sunken = ((edge & (BDR_SUNKENOUTER | BDR_SUNKENINNER)) != 0);
        COLORREF tl = sunken ? g_col_edge_sunken_tl : g_col_edge_raised_tl;
        COLORREF br = sunken ? g_col_edge_sunken_br : g_col_edge_raised_br;
        RECT r = *qrc;

        if (grfFlags & BF_MIDDLE)
        {
            if (g_real_FillRect)
                g_real_FillRect(hdc, &r, g_br_btn ? g_br_btn : g_br_dialog);
        }

        if (grfFlags & (BF_LEFT | BF_TOP | BF_RIGHT | BF_BOTTOM))
        {
            HPEN pen_tl = CreatePen(PS_SOLID, 1, tl);
            HPEN pen_br = CreatePen(PS_SOLID, 1, br);
            HGDIOBJ old_pen = SelectObject(hdc, pen_tl);

            if (grfFlags & BF_LEFT)
            {
                MoveToEx(hdc, r.left, r.bottom - 1, NULL);
                LineTo(hdc, r.left, r.top);
            }
            if (grfFlags & BF_TOP)
            {
                MoveToEx(hdc, r.left, r.top, NULL);
                LineTo(hdc, r.right - 1, r.top);
            }
            SelectObject(hdc, pen_br);
            if (grfFlags & BF_RIGHT)
            {
                MoveToEx(hdc, r.right - 1, r.top, NULL);
                LineTo(hdc, r.right - 1, r.bottom);
            }
            if (grfFlags & BF_BOTTOM)
            {
                MoveToEx(hdc, r.left, r.bottom - 1, NULL);
                LineTo(hdc, r.right, r.bottom - 1);
            }

            SelectObject(hdc, old_pen);
            DeleteObject(pen_tl);
            DeleteObject(pen_br);
        }

        if (grfFlags & BF_ADJUST)
        {
            if (grfFlags & BF_LEFT)
                qrc->left += 1;
            if (grfFlags & BF_TOP)
                qrc->top += 1;
            if (grfFlags & BF_RIGHT)
                qrc->right -= 1;
            if (grfFlags & BF_BOTTOM)
                qrc->bottom -= 1;
        }
        return TRUE;
    }
    return g_real_DrawEdge ? g_real_DrawEdge(hdc, qrc, edge, grfFlags) : FALSE;
}

static void patch_iat_entry(HMODULE mod, const char *dll_name, const char *func_name, void *new_fn)
{
    const BYTE *base = (const BYTE *)mod;
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS *nt;
    DWORD imp_rva, imp_sz;
    const IMAGE_IMPORT_DESCRIPTOR *imp;

    if (!mod || !dll_name || !func_name || !new_fn)
        return;

    dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;

    imp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    imp_sz = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
    if (imp_rva == 0 || imp_sz < sizeof(IMAGE_IMPORT_DESCRIPTOR))
        return;

    imp = (const IMAGE_IMPORT_DESCRIPTOR *)(base + imp_rva);
    for (; imp->Name != 0; imp++)
    {
        const char *mod_name = (const char *)(base + imp->Name);
        const IMAGE_THUNK_DATA *orig;
        IMAGE_THUNK_DATA *iat;

        if (_stricmp(mod_name, dll_name) != 0)
            continue;
        if (!imp->OriginalFirstThunk || !imp->FirstThunk)
            continue;

        orig = (const IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; orig->u1.AddressOfData != 0; orig++, iat++)
        {
            const IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old_prot = 0;
            if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal))
                continue;
            ibn = (const IMAGE_IMPORT_BY_NAME *)(base + orig->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func_name) != 0)
                continue;

            if (VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function),
                               PAGE_READWRITE, &old_prot))
            {
                iat->u1.Function = (ULONG_PTR)new_fn;
                VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function),
                               old_prot, &old_prot);
            }
            return;
        }
    }
}

void darken_theme_hook_module(HMODULE mod)
{
    if (!g_active || !mod)
        return;
    patch_iat_entry(mod, "USER32.dll", "GetSysColor", (void *)hooked_GetSysColor);
    patch_iat_entry(mod, "USER32.dll", "GetSysColorBrush", (void *)hooked_GetSysColorBrush);
    patch_iat_entry(mod, "USER32.dll", "FillRect", (void *)hooked_FillRect);
    patch_iat_entry(mod, "USER32.dll", "DrawEdge", (void *)hooked_DrawEdge);
}

static void apply_dwm_attrs(HWND hwnd, BOOL active_state)
{
    LONG style;
    if (!g_active || !hwnd || !pDwmSetWindowAttribute)
        return;
    style = GetWindowLongA(hwnd, GWL_STYLE);
    if (style & WS_CHILD)
        return;

    {
        BOOL dark = g_is_dark ? TRUE : FALSE;
        pDwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }
    if (g_dwm_corner_mode >= 0)
    {
        DWORD pref = (DWORD)g_dwm_corner_mode;
        pDwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    }
    else if (g_round_mode)
    {
        DWORD pref = 2; // DWMWCP_ROUND
        pDwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    }

    if (active_state)
    {
        if (g_has_dwm_active_border)
            pDwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &g_dwm_active_border, sizeof(COLORREF));
        if (g_has_dwm_active_caption)
            pDwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &g_dwm_active_caption, sizeof(COLORREF));
        if (g_has_dwm_active_text)
            pDwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &g_dwm_active_text, sizeof(COLORREF));
    }
    else
    {
        if (g_has_dwm_inactive_border)
            pDwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &g_dwm_inactive_border, sizeof(COLORREF));
        if (g_has_dwm_inactive_caption)
            pDwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &g_dwm_inactive_caption, sizeof(COLORREF));
        if (g_has_dwm_inactive_text)
            pDwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &g_dwm_inactive_text, sizeof(COLORREF));
    }
}

static void paint_menubar_bottom_line(HWND hwnd)
{
    MENUBARINFO mbi;
    RECT rc_win;
    HDC hdc;
    RECT rc_line;

    if (!GetMenu(hwnd))
        return;
    ZeroMemory(&mbi, sizeof(mbi));
    mbi.cbSize = sizeof(mbi);
    if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi))
        return;
    if (!GetWindowRect(hwnd, &rc_win))
        return;

    rc_line.left = mbi.rcBar.left - rc_win.left;
    rc_line.right = mbi.rcBar.right - rc_win.left;
    rc_line.top = mbi.rcBar.bottom - rc_win.top;
    rc_line.bottom = rc_line.top + 2;

    hdc = GetWindowDC(hwnd);
    if (hdc)
    {
        if (g_real_FillRect)
            g_real_FillRect(hdc, &rc_line, g_br_menu);
        ReleaseDC(hwnd, hdc);
    }
}

HICON darken_theme_recolor_icon(HICON icon, const char *icon_name)
{
    ICONINFO ii;
    BITMAP bm;
    BITMAPINFO bmi;
    HDC hdc;
    DWORD *px = NULL;
    DWORD *mask_px = NULL;
    int w, h, i, total;
    HBITMAP hbm_new_col = NULL;
    void *dib_bits = NULL;
    HICON out_icon = icon;
    BYTE sr, sg, sb, dr, dg, db;

    if (!g_active || !g_has_icon_recolor || !icon)
        return icon;

    if (icon_name && icon_name[0])
    {
        for (i = 0; i < g_skip_icon_count; i++)
        {
            if (_stricmp(g_skip_icons[i], icon_name) == 0)
                return icon;
        }
    }

    for (i = 0; i < g_owned_icon_count; i++)
    {
        if (g_owned_icons[i] == icon)
            return icon;
    }

    ZeroMemory(&ii, sizeof(ii));
    if (!GetIconInfo(icon, &ii))
        return icon;

    ZeroMemory(&bm, sizeof(bm));
    if (ii.hbmColor)
    {
        if (!GetObject(ii.hbmColor, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
            goto done;
        w = bm.bmWidth;
        h = bm.bmHeight;
    }
    else if (ii.hbmMask)
    {
        if (!GetObject(ii.hbmMask, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight < 2)
            goto done;
        w = bm.bmWidth;
        h = bm.bmHeight / 2;
    }
    else
    {
        goto done;
    }

    total = w * h;
    px = (DWORD *)calloc((size_t)total, sizeof(DWORD));
    mask_px = (DWORD *)calloc((size_t)total, sizeof(DWORD));
    if (!px || !mask_px)
        goto done;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hdc = CreateCompatibleDC(NULL);
    if (!hdc)
        goto done;

    if (ii.hbmColor)
    {
        GetDIBits(hdc, ii.hbmColor, 0, (UINT)h, px, &bmi, DIB_RGB_COLORS);
        if (ii.hbmMask)
            GetDIBits(hdc, ii.hbmMask, 0, (UINT)h, mask_px, &bmi, DIB_RGB_COLORS);
    }
    else
    {
        // monochrome icon: top half AND mask, bottom half XOR mask
        HBITMAP tmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dib_bits, NULL, 0);
        if (tmp && dib_bits)
        {
            HGDIOBJ old = SelectObject(hdc, tmp);
            RECT r = {0, 0, w, h};
            HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
            if (g_real_FillRect)
                g_real_FillRect(hdc, &r, magenta);
            DeleteObject(magenta);
            DrawIconEx(hdc, 0, 0, icon, w, h, 0, NULL, DI_NORMAL);
            memcpy(px, dib_bits, (size_t)total * sizeof(DWORD));
            for (i = 0; i < total; i++)
            {
                if ((px[i] & 0x00FFFFFFu) == 0x00FF00FFu)
                {
                    mask_px[i] = 0x00FFFFFFu;
                    px[i] = 0;
                }
                else
                {
                    mask_px[i] = 0;
                    px[i] |= 0xFF000000u;
                }
            }
            SelectObject(hdc, old);
            DeleteObject(tmp);
            dib_bits = NULL;
        }
    }

    sr = GetRValue(g_icon_src);
    sg = GetGValue(g_icon_src);
    sb = GetBValue(g_icon_src);
    dr = GetRValue(g_icon_dst);
    dg = GetGValue(g_icon_dst);
    db = GetBValue(g_icon_dst);

    {
        BOOL any_alpha = FALSE;
        for (i = 0; i < total; i++)
        {
            if ((px[i] & 0xFF000000u) != 0)
            {
                any_alpha = TRUE;
                break;
            }
        }

        for (i = 0; i < total; i++)
        {
            BYTE a = (BYTE)((px[i] >> 24) & 0xFFu);
            BYTE r = (BYTE)((px[i] >> 16) & 0xFFu);
            BYTE g = (BYTE)((px[i] >> 8) & 0xFFu);
            BYTE b = (BYTE)(px[i] & 0xFFu);

            if (any_alpha)
            {
                if (a == 0)
                    continue;
            }
            else
            {
                if ((mask_px[i] & 0x00FFFFFFu) != 0)
                    continue;
                a = 0xFFu;
            }

            if (abs((int)r - (int)sr) <= 24 &&
                abs((int)g - (int)sg) <= 24 &&
                abs((int)b - (int)sb) <= 24)
            {
                px[i] = ((DWORD)a << 24) | ((DWORD)dr << 16) | ((DWORD)dg << 8) | (DWORD)db;
            }
            else if (!any_alpha)
            {
                px[i] |= 0xFF000000u;
            }
        }
    }

    hbm_new_col = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &dib_bits, NULL, 0);
    if (hbm_new_col && dib_bits)
    {
        ICONINFO ii_new;
        memcpy(dib_bits, px, (size_t)total * sizeof(DWORD));
        ZeroMemory(&ii_new, sizeof(ii_new));
        ii_new.fIcon = ii.fIcon;
        ii_new.xHotspot = ii.xHotspot;
        ii_new.yHotspot = ii.yHotspot;
        ii_new.hbmMask = ii.hbmMask;
        ii_new.hbmColor = hbm_new_col;
        out_icon = CreateIconIndirect(&ii_new);
        if (out_icon)
        {
            if (g_owned_icon_count < MAX_RECOLORED_ICONS)
                g_owned_icons[g_owned_icon_count++] = out_icon;
        }
        else
        {
            out_icon = icon;
        }
    }

    if (hbm_new_col)
        DeleteObject(hbm_new_col);
    DeleteDC(hdc);

done:
    if (ii.hbmColor)
        DeleteObject(ii.hbmColor);
    if (ii.hbmMask)
        DeleteObject(ii.hbmMask);
    free(px);
    free(mask_px);
    return out_icon;
}

// subclass flags stored in dwRefData
#define SUB_KIND_CONTAINER 1
#define SUB_KIND_BUTTON 2
#define SUB_KIND_AVIUTL_BTN 3

static LRESULT CALLBACK darken_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                             UINT_PTR id, DWORD_PTR ref_data)
{
    (void)id;

    if (msg == WM_NCDESTROY)
    {
        RemoveWindowSubclass(hwnd, darken_subclass_proc, id);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    if (!g_active)
        return DefSubclassProc(hwnd, msg, wp, lp);

    if (ref_data == SUB_KIND_BUTTON)
    {
        if (msg == BM_SETIMAGE && wp == IMAGE_ICON && lp != 0)
        {
            HICON recolored = darken_theme_recolor_icon((HICON)lp, NULL);
            lp = (LPARAM)recolored;
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    if (ref_data == SUB_KIND_AVIUTL_BTN)
    {
        if (msg == WM_PAINT)
        {
            PAINTSTRUCT ps;
            RECT rc;
            HDC hdc = BeginPaint(hwnd, &ps);
            HICON hIcon = (HICON)GetWindowLongPtrA(hwnd, 0);
            BOOL pressed = (GetCapture() == hwnd);
            GetClientRect(hwnd, &rc);
            if (g_real_FillRect)
                g_real_FillRect(hdc, &rc, pressed ? g_br_menu_hot : g_br_btn);
            if (hIcon)
                DrawIconEx(hdc, 0, 0, hIcon, rc.right, rc.bottom, 0, NULL, DI_NORMAL);
            hooked_DrawEdge(hdc, &rc, pressed ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);
            EndPaint(hwnd, &ps);
            return 0;
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    switch (msg)
    {
    case WM_ACTIVATE:
        apply_dwm_attrs(hwnd, LOWORD(wp) != WA_INACTIVE);
        break;

    case WM_NCACTIVATE:
    {
        LRESULT lr = DefSubclassProc(hwnd, msg, wp, lp);
        apply_dwm_attrs(hwnd, wp != FALSE);
        paint_menubar_bottom_line(hwnd);
        return lr;
    }

    case WM_NCPAINT:
    {
        LRESULT lr = DefSubclassProc(hwnd, msg, wp, lp);
        paint_menubar_bottom_line(hwnd);
        return lr;
    }

    case WM_UAHDRAWMENU:
    {
        UahMenu *um = (UahMenu *)lp;
        MENUBARINFO mbi;
        RECT rc_win, rc_bar;
        if (um && um->hdc && GetWindowRect(hwnd, &rc_win))
        {
            ZeroMemory(&mbi, sizeof(mbi));
            mbi.cbSize = sizeof(mbi);
            if (GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi))
            {
                rc_bar.left = mbi.rcBar.left - rc_win.left;
                rc_bar.top = mbi.rcBar.top - rc_win.top;
                rc_bar.right = mbi.rcBar.right - rc_win.left;
                rc_bar.bottom = mbi.rcBar.bottom - rc_win.top + 1;
                if (g_real_FillRect)
                    g_real_FillRect(um->hdc, &rc_bar, g_br_menu);
                return TRUE;
            }
        }
        break;
    }

    case WM_UAHDRAWMENUITEM:
    {
        UahDrawMenuItem *udmi = (UahDrawMenuItem *)lp;
        wchar_t text[128] = {0};
        MENUITEMINFOW mii;
        BOOL hot, disabled;
        HBRUSH bg_br;
        COLORREF fg_col;
        DWORD dt_flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;

        if (!udmi || !udmi->dis.hDC || !udmi->um.hmenu)
            break;

        ZeroMemory(&mii, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STRING;
        mii.dwTypeData = text;
        mii.cch = 127;
        GetMenuItemInfoW(udmi->um.hmenu, (UINT)udmi->umi.position, TRUE, &mii);

        hot = (udmi->dis.itemState & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
        disabled = (udmi->dis.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
        if (udmi->dis.itemState & ODS_NOACCEL)
            dt_flags |= DT_HIDEPREFIX;

        bg_br = (hot && !disabled) ? g_br_menu_hot : g_br_menu;
        fg_col = disabled ? g_col_menu_dis_fg : (hot ? g_col_menu_hot_fg : g_col_menu_fg);

        if (g_real_FillRect)
            g_real_FillRect(udmi->dis.hDC, &udmi->dis.rcItem, bg_br);
        SetBkMode(udmi->dis.hDC, TRANSPARENT);
        SetTextColor(udmi->dis.hDC, fg_col);
        DrawTextW(udmi->dis.hDC, text, -1, &udmi->dis.rcItem, dt_flags);
        return TRUE;
    }

    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wp;
        RECT rc;
        if (hdc && g_br_dialog)
        {
            GetClientRect(hwnd, &rc);
            if (g_real_FillRect)
                g_real_FillRect(hdc, &rc, g_br_dialog);
            return 1;
        }
        break;
    }

    case WM_CTLCOLORDLG:
        if (wp)
        {
            SetTextColor((HDC)wp, g_col_static_fg);
            SetBkColor((HDC)wp, g_col_dialog);
        }
        return (LRESULT)g_br_dialog;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (wp)
        {
            BOOL dis = (lp != 0 && !IsWindowEnabled((HWND)lp));
            SetTextColor((HDC)wp, dis ? g_col_disabled_fg : g_col_static_fg);
            SetBkColor((HDC)wp, g_col_dialog);
        }
        return (LRESULT)g_br_dialog;

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (wp)
        {
            SetTextColor((HDC)wp, g_col_edit_fg);
            SetBkColor((HDC)wp, g_col_edit_bg);
        }
        return (LRESULT)g_br_edit;

    case WM_NOTIFY:
    {
        NMHDR *nm = (NMHDR *)lp;
        if (nm && nm->code == NM_CUSTOMDRAW && nm->hwndFrom)
        {
            char cls[64] = {0};
            GetClassNameA(nm->hwndFrom, cls, sizeof(cls));
            if (_stricmp(cls, "msctls_trackbar32") == 0)
            {
                NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lp;
                if (cd->dwDrawStage == CDDS_PREPAINT)
                {
                    return CDRF_NOTIFYITEMDRAW;
                }
                if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
                {
                    if (cd->dwItemSpec == TBCD_CHANNEL)
                    {
                        HBRUSH br = CreateSolidBrush(g_col_track_gutter);
                        if (g_real_FillRect)
                            g_real_FillRect(cd->hdc, &cd->rc, br);
                        DeleteObject(br);
                        hooked_DrawEdge(cd->hdc, &cd->rc, EDGE_SUNKEN, BF_RECT);
                        return CDRF_SKIPDEFAULT;
                    }
                    if (cd->dwItemSpec == TBCD_THUMB)
                    {
                        BOOL hot = (cd->uItemState & (CDIS_HOT | CDIS_SELECTED)) != 0;
                        HBRUSH br = CreateSolidBrush(hot ? g_col_track_hot : g_col_track_thumb);
                        if (g_real_FillRect)
                            g_real_FillRect(cd->hdc, &cd->rc, br);
                        DeleteObject(br);
                        hooked_DrawEdge(cd->hdc, &cd->rc, EDGE_RAISED, BF_RECT);
                        return CDRF_SKIPDEFAULT;
                    }
                }
            }
        }
        break;
    }
    }

    return DefSubclassProc(hwnd, msg, wp, lp);
}

void darken_theme_apply_window(HWND hwnd)
{
    char cls[64] = {0};
    LONG style;

    if (!g_active || !hwnd || !IsWindow(hwnd))
        return;

    GetClassNameA(hwnd, cls, sizeof(cls));
    style = GetWindowLongA(hwnd, GWL_STYLE);

    if (pAllowDarkModeForWindow)
        pAllowDarkModeForWindow(hwnd, g_is_dark);

    if ((style & WS_CHILD) == 0)
    {
        apply_dwm_attrs(hwnd, GetActiveWindow() == hwnd);
    }

    if (_stricmp(cls, "AviUtl") == 0 ||
        _stricmp(cls, "AviUtlFilterClass") == 0 ||
        _stricmp(cls, "ExtendedFilterClass") == 0 ||
        _stricmp(cls, "#32770") == 0)
    {
        if (pSetWindowTheme)
            pSetWindowTheme(hwnd, g_is_dark ? L"DarkMode_Explorer" : NULL, NULL);
        SetWindowSubclass(hwnd, darken_subclass_proc, 1, SUB_KIND_CONTAINER);
    }
    else if (_stricmp(cls, "AviUtlButton") == 0)
    {
        HICON ic = (HICON)GetWindowLongPtrA(hwnd, 0);
        if (ic)
            SetWindowLongPtrA(hwnd, 0, (LONG_PTR)darken_theme_recolor_icon(ic, NULL));
        SetWindowSubclass(hwnd, darken_subclass_proc, 1, SUB_KIND_AVIUTL_BTN);
    }
    else if (_stricmp(cls, "Button") == 0)
    {
        UINT bt = (UINT)(style & BS_TYPEMASK);
        if (bt == BS_CHECKBOX || bt == BS_AUTOCHECKBOX ||
            bt == BS_RADIOBUTTON || bt == BS_AUTORADIOBUTTON ||
            bt == BS_3STATE || bt == BS_AUTO3STATE ||
            bt == BS_GROUPBOX)
        {
            // disable visual-styles override on checkboxes/radios/groupboxes so WM_CTLCOLORSTATIC sets light text
            if (pSetWindowTheme)
                pSetWindowTheme(hwnd, L"", L"");
        }
        else if (pSetWindowTheme)
        {
            pSetWindowTheme(hwnd, g_is_dark ? L"DarkMode_Explorer" : NULL, NULL);
        }
        SetWindowSubclass(hwnd, darken_subclass_proc, 1, SUB_KIND_BUTTON);
    }
    else if (_stricmp(cls, "ComboBox") == 0)
    {
        if (pSetWindowTheme)
            pSetWindowTheme(hwnd, g_is_dark ? L"DarkMode_CFD" : NULL, NULL);
    }
    else if (_stricmp(cls, "Edit") == 0 ||
             _stricmp(cls, "ListBox") == 0 ||
             _stricmp(cls, "ScrollBar") == 0 ||
             _stricmp(cls, "SysListView32") == 0 ||
             _stricmp(cls, "SysTreeView32") == 0 ||
             _stricmp(cls, "msctls_updown32") == 0)
    {
        if (pSetWindowTheme)
            pSetWindowTheme(hwnd, g_is_dark ? L"DarkMode_Explorer" : NULL, NULL);
    }
}

static LRESULT CALLBACK darken_cbt_proc(int code, WPARAM wp, LPARAM lp)
{
    if (g_active && (code == HCBT_CREATEWND || code == HCBT_ACTIVATE))
    {
        darken_theme_apply_window((HWND)wp);
    }
    return g_cbt_hook ? CallNextHookEx(g_cbt_hook, code, wp, lp) : 0;
}

static void init_win32_dark_apis(void)
{
    HMODULE user32 = GetModuleHandleA("USER32.dll");
    if (user32 && !g_real_GetSysColor)
    {
        g_real_GetSysColor = (GetSysColorFn)GetProcAddress(user32, "GetSysColor");
        g_real_GetSysColorBrush = (GetSysColorBrushFn)GetProcAddress(user32, "GetSysColorBrush");
        g_real_FillRect = (FillRectFn)GetProcAddress(user32, "FillRect");
        g_real_DrawEdge = (DrawEdgeFn)GetProcAddress(user32, "DrawEdge");
    }

    if (!g_uxtheme)
        g_uxtheme = LoadLibraryA("uxtheme.dll");
    if (g_uxtheme)
    {
        pSetWindowTheme = (SetWindowThemeFn)GetProcAddress(g_uxtheme, "SetWindowTheme");
        pAllowDarkModeForWindow = (AllowDarkModeForWindowFn)GetProcAddress(g_uxtheme, MAKEINTRESOURCEA(133));
        pSetPreferredAppMode = (SetPreferredAppModeFn)GetProcAddress(g_uxtheme, MAKEINTRESOURCEA(135));
        pFlushMenuThemes = (FlushMenuThemesFn)GetProcAddress(g_uxtheme, MAKEINTRESOURCEA(136));
    }

    if (!g_dwmapi)
        g_dwmapi = LoadLibraryA("dwmapi.dll");
    if (g_dwmapi)
    {
        pDwmSetWindowAttribute =
            (DwmSetWindowAttributeFn)GetProcAddress(g_dwmapi, "DwmSetWindowAttribute");
    }
}

static void utf8_to_wide_rel(const char *rel_u8, wchar_t *out_w, int cap)
{
    if (!out_w || cap <= 0)
        return;
    out_w[0] = L'\0';
    if (!rel_u8 || !rel_u8[0])
        return;
    if (MultiByteToWideChar(CP_UTF8, 0, rel_u8, -1, out_w, cap) <= 0)
        MultiByteToWideChar(CP_ACP, 0, rel_u8, -1, out_w, cap);
}

void darken_theme_on_aul_detected(const char *aul_path)
{
    wchar_t aul_w[MAX_PATH];
    wchar_t base_dir[MAX_PATH];
    wchar_t settings_xml[MAX_PATH];
    wchar_t skin_root_xml[MAX_PATH];
    wchar_t skin_dir[MAX_PATH];
    wchar_t *slash;
    char *settings_u8 = NULL;
    char *skin_u8 = NULL;
    char skin_rel[MAX_PATH] = {0};
    char round_str[32] = {0};

    if (g_active || !aul_path || !aul_path[0])
        return;

    if (MultiByteToWideChar(CP_ACP, 0, aul_path, -1, aul_w, MAX_PATH) <= 0)
        return;
    lstrcpynW(base_dir, aul_w, MAX_PATH);
    slash = wcsrchr(base_dir, L'\\');
    if (slash)
        *(slash + 1) = L'\0';
    else
        base_dir[0] = L'\0';

    _snwprintf(settings_xml, MAX_PATH, L"%sDarkenWindow\\DarkenWindowSettings.xml", base_dir);
    settings_xml[MAX_PATH - 1] = L'\0';
    if (GetFileAttributesW(settings_xml) == INVALID_FILE_ATTRIBUTES)
    {
        // also check parent directory if DarkenWindow.aul was placed inside plugins/
        _snwprintf(settings_xml, MAX_PATH, L"%s..\\DarkenWindow\\DarkenWindowSettings.xml", base_dir);
        settings_xml[MAX_PATH - 1] = L'\0';
        if (GetFileAttributesW(settings_xml) != INVALID_FILE_ATTRIBUTES)
        {
            _snwprintf(base_dir, MAX_PATH, L"%s..\\", base_dir);
            base_dir[MAX_PATH - 1] = L'\0';
        }
    }

    g_named_count = 0;
    g_skip_icon_count = 0;

    settings_u8 = read_xml_utf8(settings_xml);
    if (settings_u8)
    {
        const char *p;
        strip_xml_comments(settings_u8);
        p = strstr(settings_u8, "<Settings");
        if (p)
        {
            const char *end = strchr(p, '>');
            if (end)
            {
                xml_get_attr(p, end, "skin", skin_rel, sizeof(skin_rel));
                if (xml_get_attr(p, end, "roundMode", round_str, sizeof(round_str)))
                    g_round_mode = (_stricmp(round_str, "ON") == 0 || _stricmp(round_str, "YES") == 0);
            }
        }
        free(settings_u8);
    }

    if (skin_rel[0])
    {
        wchar_t skin_rel_w[MAX_PATH];
        utf8_to_wide_rel(skin_rel, skin_rel_w, MAX_PATH);
        _snwprintf(skin_root_xml, MAX_PATH, L"%sDarkenWindow\\%s", base_dir, skin_rel_w);
        skin_root_xml[MAX_PATH - 1] = L'\0';
        _snwprintf(skin_dir, MAX_PATH, L"%sDarkenWindow\\Skin\\", base_dir);
        skin_dir[MAX_PATH - 1] = L'\0';

        skin_u8 = read_xml_utf8(skin_root_xml);
        if (skin_u8)
        {
            const char *p = skin_u8;
            strip_xml_comments(skin_u8);
            while ((p = strchr(p, '<')) != NULL)
            {
                const char *end = strchr(p, '>');
                char fn[MAX_PATH];
                if (!end)
                    break;
                if (_strnicmp(p + 1, "Skin", 4) == 0 && (p[5] == ' ' || p[5] == '\t'))
                {
                    if (xml_get_attr(p, end, "fileName", fn, sizeof(fn)) && fn[0])
                    {
                        wchar_t fn_w[MAX_PATH];
                        wchar_t leaf_w[MAX_PATH];
                        utf8_to_wide_rel(fn, fn_w, MAX_PATH);
                        _snwprintf(leaf_w, MAX_PATH, L"%s%s", skin_dir, fn_w);
                        leaf_w[MAX_PATH - 1] = L'\0';
                        parse_skin_leaf_xml(leaf_w);
                    }
                }
                p = end + 1;
            }
            free(skin_u8);
        }
        else
        {
            // single-file skin xml
            parse_skin_leaf_xml(skin_root_xml);
        }
    }

    init_win32_dark_apis();
    resolve_palette_from_named_colors();
    g_active = TRUE;

    if (pSetPreferredAppMode)
        pSetPreferredAppMode(g_is_dark ? 2 : 0);
    if (pFlushMenuThemes)
        pFlushMenuThemes();

    if (!g_cbt_hook)
        g_cbt_hook = SetWindowsHookExW(WH_CBT, darken_cbt_proc, NULL, GetCurrentThreadId());

    darken_theme_hook_module(GetModuleHandleA(NULL));
    dbg_info("DarkenWindow skin loaded ('%s', dark=%d)",
             skin_rel[0] ? skin_rel : "default", (int)g_is_dark);
}

void darken_theme_shutdown(void)
{
    int i;
    g_active = FALSE;
    if (g_cbt_hook)
    {
        UnhookWindowsHookEx(g_cbt_hook);
        g_cbt_hook = NULL;
    }
    if (g_br_dialog)
    {
        DeleteObject(g_br_dialog);
        g_br_dialog = NULL;
    }
    if (g_br_edit)
    {
        DeleteObject(g_br_edit);
        g_br_edit = NULL;
    }
    if (g_br_btn)
    {
        DeleteObject(g_br_btn);
        g_br_btn = NULL;
    }
    if (g_br_menu)
    {
        DeleteObject(g_br_menu);
        g_br_menu = NULL;
    }
    if (g_br_menu_hot)
    {
        DeleteObject(g_br_menu_hot);
        g_br_menu_hot = NULL;
    }
    for (i = 0; i < 32; i++)
    {
        if (g_sys_brushes[i])
        {
            DeleteObject(g_sys_brushes[i]);
            g_sys_brushes[i] = NULL;
        }
    }
    for (i = 0; i < g_owned_icon_count; i++)
    {
        if (g_owned_icons[i])
            DestroyIcon(g_owned_icons[i]);
    }
    g_owned_icon_count = 0;
}

BOOL darken_theme_is_active(void)
{
    return g_active;
}
