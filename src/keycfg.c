#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "keycfg.h"
#include "aviutl_dlg.h"
#include "i18n.h"
#include "plugin_manager.h"

#define KEYCFG_MAX_BINDINGS 512
#define KEYCFG_MAX_DEFAULTS 512
#define KEYCFG_MAX_MENU_ITEMS 1024
#define KEYCFG_KEY_TABLE_COUNT 90
#define KEYCFG_FILE_HEADER_LEN 0x24
#define KEYCFG_FILE_MAX_BYTES 0x5800

#define KEYCFG_MAGIC_V03 "AviUtl KeyConfigFile version 0.3   "
#define KEYCFG_MAGIC_V02 "AviUtl KeyConfigFile version 0.2   "

#define IDC_KEYCFG_CMD_LIST 32600
#define IDC_KEYCFG_KEY_LIST 32601
#define IDC_KEYCFG_CUR_LIST 32602
#define IDC_KEYCFG_CHK_SHIFT 32200
#define IDC_KEYCFG_CHK_CTRL 32201
#define IDC_KEYCFG_CHK_ALT 32202
#define IDC_KEYCFG_BTN_RESET 32700

typedef struct KeyTableEntry
{
    UINT vk;
    const wchar_t *name_w;
} KeyTableEntry;

typedef struct KeyBinding
{
    UINT flags;
    UINT vk;
    int cmd_id;
} KeyBinding;

typedef struct EnumMenuCmd
{
    int cmd_id;
    wchar_t label_w[320];
    char label_sjis[320];
} EnumMenuCmd;

// 0x00474c90 & 0x00474e00: exact 90-entry selectable key table in aviutl.exe 1.10
static const KeyTableEntry k_key_table[KEYCFG_KEY_TABLE_COUNT] = {
    {0x00, L""},
    {'A', L"A"},
    {'B', L"B"},
    {'C', L"C"},
    {'D', L"D"},
    {'E', L"E"},
    {'F', L"F"},
    {'G', L"G"},
    {'H', L"H"},
    {'I', L"I"},
    {'J', L"J"},
    {'K', L"K"},
    {'L', L"L"},
    {'M', L"M"},
    {'N', L"N"},
    {'O', L"O"},
    {'P', L"P"},
    {'Q', L"Q"},
    {'R', L"R"},
    {'S', L"S"},
    {'T', L"T"},
    {'U', L"U"},
    {'V', L"V"},
    {'W', L"W"},
    {'X', L"X"},
    {'Y', L"Y"},
    {'Z', L"Z"},
    {'0', L"0"},
    {'1', L"1"},
    {'2', L"2"},
    {'3', L"3"},
    {'4', L"4"},
    {'5', L"5"},
    {'6', L"6"},
    {'7', L"7"},
    {'8', L"8"},
    {'9', L"9"},
    {VK_BACK, L"BS"},
    {VK_TAB, L"Tab"},
    {VK_RETURN, L"Enter"},
    {VK_SPACE, L"Space"},
    {VK_INSERT, L"Ins"},
    {VK_DELETE, L"Del"},
    {VK_HOME, L"Home"},
    {VK_END, L"End"},
    {VK_PRIOR, L"PU"},
    {VK_NEXT, L"PD"},
    {VK_UP, L"\x2191"},
    {VK_DOWN, L"\x2193"},
    {VK_LEFT, L"\x2190"},
    {VK_RIGHT, L"\x2192"},
    {0xBD, L"-"},
    {0xDE, L"^"},
    {0xDC, L"\\"},
    {0xC0, L"@"},
    {0xDB, L"["},
    {0xBB, L";"},
    {0xBA, L":"},
    {0xDD, L"]"},
    {0xBC, L","},
    {0xBE, L"."},
    {0xBF, L"/"},
    {0xE2, L"\xff3c"},
    {VK_F1, L"F1"},
    {VK_F2, L"F2"},
    {VK_F3, L"F3"},
    {VK_F4, L"F4"},
    {VK_F5, L"F5"},
    {VK_F6, L"F6"},
    {VK_F7, L"F7"},
    {VK_F8, L"F8"},
    {VK_F9, L"F9"},
    {VK_F10, L"F10"},
    {VK_F11, L"F11"},
    {VK_F12, L"F12"},
    {VK_NUMPAD0, L"0(\x30c6)"},
    {VK_NUMPAD1, L"1(\x30c6)"},
    {VK_NUMPAD2, L"2(\x30c6)"},
    {VK_NUMPAD3, L"3(\x30c6)"},
    {VK_NUMPAD4, L"4(\x30c6)"},
    {VK_NUMPAD5, L"5(\x30c6)"},
    {VK_NUMPAD6, L"6(\x30c6)"},
    {VK_NUMPAD7, L"7(\x30c6)"},
    {VK_NUMPAD8, L"8(\x30c6)"},
    {VK_NUMPAD9, L"9(\x30c6)"},
    {VK_DIVIDE, L"/(\x30c6)"},
    {VK_MULTIPLY, L"*(\x30c6)"},
    {VK_SUBTRACT, L"-(\x30c6)"},
    {VK_ADD, L"+(\x30c6)"},
    {VK_DECIMAL, L".(\x30c6)"},
};

// 0x004825d8: exact 35 built-in default shortcuts in aviutl.exe 1.10
static const KeyBinding k_builtin_defaults[] = {
    {0, 0x25, 9001},
    {0, 0x27, 9002},
    {0, 0x26, 5141},
    {0, 0x28, 5142},
    {0, 0x21, 5188},
    {0, 0x22, 5187},
    {0, 0x24, 5396},
    {0, 0x23, 5397},
    {2, 0x43, 1020},
    {2, 0x56, 5117},
    {2, 0x41, 5112},
    {2, 0x47, 5131},
    {2, 0x4F, 5097},
    {2, 0x53, 1003},
    {0, 0xDB, 5110},
    {0, 0xDD, 5111},
    {0, 0x60, 5196},
    {0, 0x61, 5197},
    {0, 0x62, 5198},
    {0, 0x63, 5199},
    {0, 0x64, 5200},
    {0, 0x65, 5201},
    {2, 0x5A, 5147},
    {0, 0x6B, 9107},
    {0, 0x6D, 9108},
    {0, 0x30, 5206},
    {0, 0x31, 5207},
    {0, 0x32, 5208},
    {0, 0x33, 5209},
    {0, 0x34, 5210},
    {0, 0x35, 5211},
    {0, 0x36, 5212},
    {0, 0x37, 5213},
    {0, 0x38, 5214},
    {0, 0x39, 5215},
};

static KeyBinding g_bindings[KEYCFG_MAX_BINDINGS];
static KeyBinding g_defaults[KEYCFG_MAX_DEFAULTS];
static int g_default_count = 0;

static void wide_to_cp932(const wchar_t *src, char *dst, int cap)
{
    if (!dst || cap <= 0)
        return;
    dst[0] = '\0';
    if (!src || !src[0])
        return;
    if (WideCharToMultiByte(932, 0, src, -1, dst, cap, NULL, NULL) <= 0)
    {
        WideCharToMultiByte(CP_ACP, 0, src, -1, dst, cap, NULL, NULL);
    }
}

static void get_keycfg_file_path(char *out_path, size_t cap)
{
    char mod[MAX_PATH];
    char *slash;
    if (!out_path || cap == 0)
        return;
    out_path[0] = '\0';
    if (GetModuleFileNameA(NULL, mod, MAX_PATH) > 0)
    {
        slash = strrchr(mod, '\\');
        if (slash)
            *(slash + 1) = '\0';
        else
            mod[0] = '\0';
        _snprintf(out_path, cap, "%saviutl.key", mod);
        out_path[cap - 1] = '\0';
    }
    else
    {
        lstrcpynA(out_path, "aviutl.key", (int)cap);
    }
}

static int vk_to_key_index(UINT vk)
{
    int i;
    if (vk == 0)
        return 0;
    for (i = 1; i < KEYCFG_KEY_TABLE_COUNT; i++)
    {
        if (k_key_table[i].vk == vk)
            return i;
    }
    return 0;
}

static void build_default_table(void)
{
    int i, mcount;

    g_default_count = 0;
    ZeroMemory(g_defaults, sizeof(g_defaults));

    for (i = 0; i < (int)(sizeof(k_builtin_defaults) / sizeof(k_builtin_defaults[0])); i++)
    {
        g_defaults[g_default_count++] = k_builtin_defaults[i];
    }

    // 0x00423330: append plugin add_menu_item default shortcuts if key is valid & unused
    mcount = plugin_manager_get_menu_item_count();
    for (i = 0; i < mcount && g_default_count < KEYCFG_MAX_DEFAULTS; i++)
    {
        PluginMenuItem it;
        UINT flg;
        int k;
        BOOL exists = FALSE;

        if (!plugin_manager_get_menu_item(i, &it) || it.def_key == 0)
            continue;
        if (vk_to_key_index((UINT)it.def_key) <= 0)
            continue;

        flg = (UINT)(it.flag & 7);
        for (k = 0; k < g_default_count; k++)
        {
            if (g_defaults[k].vk == (UINT)it.def_key && g_defaults[k].flags == flg)
            {
                exists = TRUE;
                break;
            }
        }
        if (!exists)
        {
            g_defaults[g_default_count].flags = flg;
            g_defaults[g_default_count].vk = (UINT)it.def_key;
            g_defaults[g_default_count].cmd_id = PLUGIN_CMD_MENU_ITEM_BASE + i;
            g_default_count++;
        }
    }
}

static void reset_bindings_to_defaults(void)
{
    int i;
    ZeroMemory(g_bindings, sizeof(g_bindings));
    for (i = 0; i < g_default_count && i < KEYCFG_MAX_BINDINGS; i++)
    {
        g_bindings[i] = g_defaults[i];
    }
}

// 0x00432f20: recursively enumerate menu bar items into "[Top][Item]" or "[Top][Sub][Item]"
static void enum_menu_recursive(HMENU hmenu, const wchar_t *parent_w, const wchar_t *root_w,
                                EnumMenuCmd *out_items, int *io_count, int max_items)
{
    int i, n;
    if (!hmenu || !out_items || !io_count)
        return;

    n = GetMenuItemCount(hmenu);
    for (i = 0; i < n && *io_count < max_items; i++)
    {
        HMENU sub = GetSubMenu(hmenu, i);
        UINT id = GetMenuItemID(hmenu, i);
        wchar_t text[260];

        if ((int)id >= 1)
        {
            // skip MRU entries (5596..5606) and Clear MRU (1073) matching 0x00432f20
            if ((id < 5596 || id > 5606) && id != 1073)
            {
                text[0] = L'\0';
                if (GetMenuStringW(hmenu, (UINT)i, text, 260, MF_BYPOSITION) > 0)
                {
                    EnumMenuCmd *dst = &out_items[*io_count];
                    dst->cmd_id = (int)id;
                    if (!root_w || root_w == parent_w || lstrcmpW(root_w, parent_w) == 0)
                    {
                        _snwprintf(dst->label_w, 320, L"[%s][%s]",
                                   parent_w ? parent_w : L"", text);
                    }
                    else
                    {
                        _snwprintf(dst->label_w, 320, L"[%s][%s][%s]",
                                   root_w, parent_w ? parent_w : L"", text);
                    }
                    dst->label_w[319] = L'\0';
                    wide_to_cp932(dst->label_w, dst->label_sjis, sizeof(dst->label_sjis));
                    (*io_count)++;
                }
            }
        }

        if (sub && GetMenuItemCount(sub) > 0)
        {
            text[0] = L'\0';
            if (GetMenuStringW(hmenu, (UINT)i, text, 260, MF_BYPOSITION) > 0)
            {
                const wchar_t *next_root = root_w ? root_w : text;
                enum_menu_recursive(sub, text, next_root, out_items, io_count, max_items);
            }
        }
    }
}

static int enumerate_main_menu(HWND main_hwnd, EnumMenuCmd *out_items, int max_items)
{
    int count = 0;
    HMENU bar = main_hwnd ? GetMenu(main_hwnd) : NULL;
    if (bar)
    {
        enum_menu_recursive(bar, NULL, NULL, out_items, &count, max_items);
    }
    return count;
}

// 0x00433370 lines 110..113: dynamic command IDs saved with 0x80 + menu label string
static BOOL is_dynamic_menu_cmd_id(int cmd_id)
{
    if (cmd_id > 10000 && cmd_id < 10129)
        return TRUE;
    if (cmd_id > 11000 && cmd_id < 11129)
        return TRUE;
    if (cmd_id > 14000 && cmd_id < 14129)
        return TRUE;
    if (cmd_id > 15000 && cmd_id < 15129)
        return TRUE;
    if (cmd_id > 5295 && cmd_id < 5328)
        return TRUE;
    if (cmd_id > 5495 && cmd_id < 5528)
        return TRUE;
    if (cmd_id > 8005 && cmd_id < 8102)
        return TRUE;
    if ((unsigned int)(cmd_id - 16000) < 256u)
        return TRUE;
    return FALSE;
}

static BOOL bindings_match_defaults(void)
{
    int i, k, active_n = 0;

    for (i = 0; i < KEYCFG_MAX_BINDINGS; i++)
    {
        if (g_bindings[i].cmd_id != 0)
            active_n++;
    }
    if (active_n != g_default_count)
        return FALSE;

    for (i = 0; i < g_default_count; i++)
    {
        BOOL found = FALSE;
        for (k = 0; k < KEYCFG_MAX_BINDINGS; k++)
        {
            if (g_bindings[k].cmd_id == g_defaults[i].cmd_id &&
                g_bindings[k].vk == g_defaults[i].vk &&
                (g_bindings[k].flags & 7) == (g_defaults[i].flags & 7))
            {
                found = TRUE;
                break;
            }
        }
        if (!found)
            return FALSE;
    }
    return TRUE;
}

// 0x00433370: serialize g_bindings into AviUtl KeyConfigFile version 0.3 binary format
int keycfg_serialize(HWND main_hwnd, unsigned char *out_buf, int out_cap)
{
    EnumMenuCmd *menus;
    int menu_n, i, pos;

    if (!out_buf || out_cap < KEYCFG_FILE_HEADER_LEN)
        return 0;

    build_default_table();
    if (bindings_match_defaults())
        return 0;

    menus = (EnumMenuCmd *)calloc(KEYCFG_MAX_MENU_ITEMS, sizeof(EnumMenuCmd));
    menu_n = menus ? enumerate_main_menu(main_hwnd, menus, KEYCFG_MAX_MENU_ITEMS) : 0;

    memset(out_buf, 0, KEYCFG_FILE_HEADER_LEN);
    memcpy(out_buf, KEYCFG_MAGIC_V03, KEYCFG_FILE_HEADER_LEN);
    pos = KEYCFG_FILE_HEADER_LEN;

    for (i = 0; i < KEYCFG_MAX_BINDINGS; i++)
    {
        int cmd = g_bindings[i].cmd_id;
        unsigned char flg = (unsigned char)(g_bindings[i].flags & 0x0F);
        unsigned char vk = (unsigned char)(g_bindings[i].vk & 0xFF);
        const char *dyn_str = NULL;
        int slen = 0;

        if (cmd == 0 || vk == 0)
            continue;

        if (is_dynamic_menu_cmd_id(cmd) && menus)
        {
            int m;
            for (m = 0; m < menu_n; m++)
            {
                if (menus[m].cmd_id == cmd && menus[m].label_sjis[0])
                {
                    dyn_str = menus[m].label_sjis;
                    slen = (int)strlen(dyn_str) + 1;
                    break;
                }
            }
        }

        if (pos + 4 + slen > out_cap)
            break;

        out_buf[pos + 0] = dyn_str ? (unsigned char)(flg | 0x80) : flg;
        out_buf[pos + 1] = vk;
        out_buf[pos + 2] = (unsigned char)(cmd & 0xFF);
        out_buf[pos + 3] = (unsigned char)((cmd >> 8) & 0xFF);
        pos += 4;

        if (dyn_str)
        {
            memcpy(out_buf + pos, dyn_str, (size_t)slen);
            pos += slen;
        }
    }

    free(menus);
    return pos;
}

// 0x004330e0: parse AviUtl KeyConfigFile v0.2 / v0.3 buffer (or fall back to defaults)
BOOL keycfg_load_from_memory(HWND main_hwnd, const unsigned char *buf, int len)
{
    EnumMenuCmd *menus = NULL;
    int menu_n = 0;
    int off, out_idx = 0;

    build_default_table();
    ZeroMemory(g_bindings, sizeof(g_bindings));

    if (!buf || len <= KEYCFG_FILE_HEADER_LEN)
    {
        reset_bindings_to_defaults();
        return FALSE;
    }

    if (memcmp(buf, KEYCFG_MAGIC_V03, KEYCFG_FILE_HEADER_LEN) != 0 &&
        memcmp(buf, KEYCFG_MAGIC_V02, KEYCFG_FILE_HEADER_LEN) != 0)
    {
        reset_bindings_to_defaults();
        return FALSE;
    }

    menus = (EnumMenuCmd *)calloc(KEYCFG_MAX_MENU_ITEMS, sizeof(EnumMenuCmd));
    if (menus)
    {
        menu_n = enumerate_main_menu(main_hwnd, menus, KEYCFG_MAX_MENU_ITEMS);
    }

    off = KEYCFG_FILE_HEADER_LEN;
    while (off + 4 <= len && out_idx < KEYCFG_MAX_BINDINGS)
    {
        unsigned char raw_flg = buf[off + 0];
        unsigned char vk = buf[off + 1];
        int cmd = (int)buf[off + 2] | ((int)buf[off + 3] << 8);
        UINT flg = (UINT)(raw_flg & 0x7F);
        BOOL keep = (cmd != 0 && vk != 0);

        off += 4;

        if (raw_flg & 0x80)
        {
            const char *s = (const char *)(buf + off);
            int max_rem = len - off;
            int slen = 0;
            int m, prev;
            BOOL matched = FALSE;

            while (slen < max_rem && s[slen] != '\0')
                slen++;
            if (slen >= max_rem)
                break; // malformed unterminated string
            off += slen + 1;

            for (m = 0; m < menu_n; m++)
            {
                if (lstrcmpA(menus[m].label_sjis, s) == 0)
                {
                    cmd = menus[m].cmd_id;
                    matched = TRUE;
                    break;
                }
            }

            if (!matched)
            {
                // 0x004330e0 lines 73..109: drop unresolved dynamic item unless static ID exists in menu
                BOOL id_in_menu = FALSE;
                if (!is_dynamic_menu_cmd_id(cmd))
                {
                    for (m = 0; m < menu_n; m++)
                    {
                        if (menus[m].cmd_id == cmd)
                        {
                            id_in_menu = TRUE;
                            break;
                        }
                    }
                }
                if (!id_in_menu)
                    keep = FALSE;
                for (prev = 0; prev < out_idx; prev++)
                {
                    if (g_bindings[prev].cmd_id == cmd)
                        keep = FALSE;
                }
            }
            else
            {
                // overwrite any earlier slot that had the same resolved cmd_id
                for (prev = 0; prev < out_idx; prev++)
                {
                    if (g_bindings[prev].cmd_id == cmd)
                    {
                        g_bindings[prev].flags = flg;
                        g_bindings[prev].vk = vk;
                        g_bindings[prev].cmd_id = cmd;
                        keep = FALSE;
                        break;
                    }
                }
            }
        }

        if (keep)
        {
            g_bindings[out_idx].flags = flg;
            g_bindings[out_idx].vk = vk;
            g_bindings[out_idx].cmd_id = cmd;
            out_idx++;
        }
    }

    free(menus);
    return TRUE;
}

void keycfg_init_and_load(HWND main_hwnd)
{
    char path[MAX_PATH];
    HANDLE hf;

    build_default_table();
    get_keycfg_file_path(path, sizeof(path));

    hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
    {
        reset_bindings_to_defaults();
        return;
    }

    {
        unsigned char *buf = (unsigned char *)calloc(1, KEYCFG_FILE_MAX_BYTES);
        DWORD got = 0;
        if (buf && ReadFile(hf, buf, KEYCFG_FILE_MAX_BYTES, &got, NULL) && got > 0)
        {
            keycfg_load_from_memory(main_hwnd, buf, (int)got);
        }
        else
        {
            reset_bindings_to_defaults();
        }
        free(buf);
    }
    CloseHandle(hf);
}

// 0x004335f0: translate WM_KEYDOWN / WM_SYSKEYDOWN into WM_COMMAND
BOOL keycfg_translate_keydown(HWND main_hwnd, UINT msg, UINT vk)
{
    MSG peek_msg;
    int i, matched = -1;
    UINT cur_mods = 0;

    if (!main_hwnd || (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN))
        return FALSE;

    // coalesce auto-repeat bursts if another identical keydown is already queued
    if (PeekMessageA(&peek_msg, main_hwnd, msg, msg, PM_NOREMOVE) &&
        (UINT)peek_msg.wParam == vk)
    {
        return TRUE;
    }

    if (GetKeyState(VK_SHIFT) < 0)
        cur_mods |= 1;
    if (GetKeyState(VK_CONTROL) < 0)
        cur_mods |= 2;
    if (GetKeyState(VK_MENU) < 0)
        cur_mods |= 4;

    for (i = 0; i < KEYCFG_MAX_BINDINGS; i++)
    {
        if (g_bindings[i].cmd_id == 0 || g_bindings[i].vk != vk)
            continue;
        if ((g_bindings[i].flags & 7) == 0 && matched < 0 && cur_mods == 0)
        {
            matched = i;
        }
        if ((g_bindings[i].flags & 7) == cur_mods)
        {
            matched = i;
        }
    }

    if (matched >= 0)
    {
        PostMessageA(main_hwnd, WM_COMMAND, (WPARAM)g_bindings[matched].cmd_id, 0);
        return TRUE;
    }
    return FALSE;
}

// KEYCFGLIST dialog (0x004340a0 / 0x00433be0 / 0x00433940)

typedef struct KeyCfgDlgCtx
{
    EnumMenuCmd *menus;
    int menu_n;
    HWND hdlg;
    HWND h_cmd_list;
    HWND h_key_list;
    HWND h_cur_list;
    WNDPROC orig_lb_proc;
    int prev_cmd_sel;
    int prev_key_sel;
} KeyCfgDlgCtx;

static KeyCfgDlgCtx *g_active_keydlg = NULL;

static void format_binding_text(UINT flags, int key_idx, wchar_t *out_w, int cap)
{
    out_w[0] = L'\0';
    if (key_idx <= 0 || key_idx >= KEYCFG_KEY_TABLE_COUNT)
        return;
    if (flags & 1)
        lstrcatW(out_w, L"Sft+");
    if (flags & 2)
        lstrcatW(out_w, L"Ctl+");
    if (flags & 4)
        lstrcatW(out_w, L"Alt+");
    lstrcatW(out_w, k_key_table[key_idx].name_w);
    (void)cap;
}

static int find_binding_slot_by_cmd(int cmd_id)
{
    int i;
    if (cmd_id == 0)
        return -1;
    for (i = 0; i < KEYCFG_MAX_BINDINGS; i++)
    {
        if (g_bindings[i].cmd_id == cmd_id)
            return i;
    }
    return -1;
}

static int find_free_binding_slot(void)
{
    int i;
    for (i = 0; i < KEYCFG_MAX_BINDINGS; i++)
    {
        if (g_bindings[i].cmd_id == 0)
            return i;
    }
    return -1;
}

// 0x00433700: populate middle listbox (32602) with current shortcut strings
static void populate_cur_binding_listbox(HWND hdlg, KeyCfgDlgCtx *ctx)
{
    int i;
    SendDlgItemMessageW(hdlg, IDC_KEYCFG_CUR_LIST, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < ctx->menu_n; i++)
    {
        int bslot = find_binding_slot_by_cmd(ctx->menus[i].cmd_id);
        wchar_t label[64];
        label[0] = L'\0';
        if (bslot >= 0)
        {
            int kidx = vk_to_key_index(g_bindings[bslot].vk);
            if (kidx > 0)
            {
                format_binding_text(g_bindings[bslot].flags, kidx, label, 64);
            }
        }
        SendDlgItemMessageW(hdlg, IDC_KEYCFG_CUR_LIST, LB_ADDSTRING, 0, (LPARAM)label);
    }
    SendDlgItemMessageW(hdlg, IDC_KEYCFG_CUR_LIST, LB_SETCURSEL, (WPARAM)-1, 0);
}

// 0x00433850: update single row in middle listbox (32602) & keep scroll synced
static void update_cur_binding_row(HWND hdlg, int row, UINT flags, int key_idx)
{
    wchar_t label[64];
    int top_idx;

    format_binding_text(flags, key_idx, label, 64);
    SendDlgItemMessageW(hdlg, IDC_KEYCFG_CUR_LIST, LB_DELETESTRING, (WPARAM)row, 0);
    SendDlgItemMessageW(hdlg, IDC_KEYCFG_CUR_LIST, LB_INSERTSTRING, (WPARAM)row, (LPARAM)label);
    SendDlgItemMessageW(hdlg, IDC_KEYCFG_CUR_LIST, LB_SETCURSEL, (WPARAM)-1, 0);

    top_idx = (int)SendDlgItemMessageW(hdlg, IDC_KEYCFG_CMD_LIST, LB_GETTOPINDEX, 0, 0);
    SendDlgItemMessageW(hdlg, IDC_KEYCFG_CUR_LIST, LB_SETTOPINDEX, (WPARAM)top_idx, 0);
}

// 0x00433940: subclass proc for synchronized scrolling & Backspace/Delete unbind
static LRESULT CALLBACK keycfg_listbox_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    KeyCfgDlgCtx *ctx = g_active_keydlg;
    if (!ctx || !ctx->orig_lb_proc)
        return DefWindowProcW(hwnd, msg, wp, lp);

    if (msg == WM_CHAR)
    {
        if (hwnd != ctx->h_key_list)
        {
            SendMessageW(ctx->h_key_list, WM_CHAR, wp, lp);
            return 1;
        }
        if (wp == VK_BACK)
        {
            SendMessageW(ctx->h_key_list, LB_SETCURSEL, 0, 0);
            SendMessageW(ctx->hdlg, WM_COMMAND,
                         MAKEWPARAM(IDC_KEYCFG_KEY_LIST, LBN_SELCHANGE),
                         (LPARAM)ctx->h_key_list);
            return 1;
        }
    }
    else if (msg == WM_KEYDOWN || msg == WM_KEYUP)
    {
        if (msg == WM_KEYDOWN && wp == VK_DELETE)
        {
            PostMessageW(hwnd, WM_CHAR, VK_BACK, 0);
        }
        if (hwnd == ctx->h_cmd_list)
        {
            LRESULT lr = CallWindowProcW(ctx->orig_lb_proc, hwnd, msg, wp, lp);
            WPARAM top = (WPARAM)SendMessageW(ctx->h_cmd_list, LB_GETTOPINDEX, 0, 0);
            SendMessageW(ctx->h_cur_list, LB_SETTOPINDEX, top, 0);
            return lr;
        }
        if (hwnd == ctx->h_cur_list)
        {
            LRESULT lr = CallWindowProcW(ctx->orig_lb_proc, hwnd, msg, wp, lp);
            WPARAM top = (WPARAM)SendMessageW(ctx->h_cur_list, LB_GETTOPINDEX, 0, 0);
            SendMessageW(ctx->h_cmd_list, LB_SETTOPINDEX, top, 0);
            return lr;
        }
    }
    else if (msg == WM_VSCROLL && hwnd == ctx->h_cur_list)
    {
        LRESULT lr = CallWindowProcW(ctx->orig_lb_proc, hwnd, msg, wp, lp);
        WPARAM top = (WPARAM)SendMessageW(ctx->h_cur_list, LB_GETTOPINDEX, 0, 0);
        SendMessageW(ctx->h_cmd_list, LB_SETTOPINDEX, top, 0);
        return lr;
    }
    else if (msg == WM_MOUSEWHEEL)
    {
        if (hwnd == ctx->h_cmd_list)
        {
            LRESULT lr = CallWindowProcW(ctx->orig_lb_proc, hwnd, msg, wp, lp);
            int cur_top = (int)SendMessageW(ctx->h_cmd_list, LB_GETTOPINDEX, 0, 0);
            int delta = (short)HIWORD(wp);
            int new_top = cur_top - (delta / 30);
            if (new_top < 0)
                new_top = 0;
            SendMessageW(ctx->h_cmd_list, LB_SETTOPINDEX, (WPARAM)new_top, 0);
            SendMessageW(ctx->h_cur_list, LB_SETTOPINDEX, (WPARAM)new_top, 0);
            return lr;
        }
        if (hwnd == ctx->h_cur_list)
        {
            LRESULT lr = CallWindowProcW(ctx->orig_lb_proc, hwnd, msg, wp, lp);
            WPARAM top = (WPARAM)SendMessageW(ctx->h_cur_list, LB_GETTOPINDEX, 0, 0);
            SendMessageW(ctx->h_cmd_list, LB_SETTOPINDEX, top, 0);
            return lr;
        }
    }

    return CallWindowProcW(ctx->orig_lb_proc, hwnd, msg, wp, lp);
}

// 0x00433be0: KEYCFGLIST command handler
static BOOL on_keycfg_dlg_cmd(HWND hdlg, int cmd_id, AviUtlDlgState *st, void *user)
{
    KeyCfgDlgCtx *ctx = (KeyCfgDlgCtx *)user;
    int cmd_sel, cur_sel, key_sel;

    if (!ctx)
        return TRUE;

    if (cmd_id == AVIUTL_DLG_CMD_EXIT)
    {
        if (ctx->orig_lb_proc)
        {
            if (ctx->h_cmd_list && IsWindow(ctx->h_cmd_list))
                SetWindowLongPtrW(ctx->h_cmd_list, GWLP_WNDPROC, (LONG_PTR)ctx->orig_lb_proc);
            if (ctx->h_key_list && IsWindow(ctx->h_key_list))
                SetWindowLongPtrW(ctx->h_key_list, GWLP_WNDPROC, (LONG_PTR)ctx->orig_lb_proc);
            if (ctx->h_cur_list && IsWindow(ctx->h_cur_list))
                SetWindowLongPtrW(ctx->h_cur_list, GWLP_WNDPROC, (LONG_PTR)ctx->orig_lb_proc);
        }
        g_active_keydlg = NULL;
        return TRUE;
    }

    if (cmd_id == AVIUTL_DLG_CMD_INIT)
    {
        int i;
        ctx->hdlg = hdlg;
        ctx->h_cmd_list = GetDlgItem(hdlg, IDC_KEYCFG_CMD_LIST);
        ctx->h_key_list = GetDlgItem(hdlg, IDC_KEYCFG_KEY_LIST);
        ctx->h_cur_list = GetDlgItem(hdlg, IDC_KEYCFG_CUR_LIST);
        ctx->prev_cmd_sel = -1;
        ctx->prev_key_sel = -1;

        SendDlgItemMessageW(hdlg, IDC_KEYCFG_CMD_LIST, LB_RESETCONTENT, 0, 0);
        for (i = 0; i < ctx->menu_n; i++)
        {
            SendDlgItemMessageW(hdlg, IDC_KEYCFG_CMD_LIST, LB_ADDSTRING, 0,
                                (LPARAM)ctx->menus[i].label_w);
        }
        if (ctx->menu_n > 0)
            SendDlgItemMessageW(hdlg, IDC_KEYCFG_CMD_LIST, LB_SETCURSEL, 0, 0);

        SendDlgItemMessageW(hdlg, IDC_KEYCFG_KEY_LIST, LB_RESETCONTENT, 0, 0);
        for (i = 0; i < KEYCFG_KEY_TABLE_COUNT; i++)
        {
            SendDlgItemMessageW(hdlg, IDC_KEYCFG_KEY_LIST, LB_ADDSTRING, 0,
                                (LPARAM)k_key_table[i].name_w);
        }

        populate_cur_binding_listbox(hdlg, ctx);

        g_active_keydlg = ctx;
        if (ctx->h_cmd_list)
        {
            ctx->orig_lb_proc = (WNDPROC)GetWindowLongPtrW(ctx->h_cmd_list, GWLP_WNDPROC);
            SetWindowLongPtrW(ctx->h_cmd_list, GWLP_WNDPROC, (LONG_PTR)keycfg_listbox_subclass_proc);
        }
        if (ctx->h_key_list)
            SetWindowLongPtrW(ctx->h_key_list, GWLP_WNDPROC, (LONG_PTR)keycfg_listbox_subclass_proc);
        if (ctx->h_cur_list)
            SetWindowLongPtrW(ctx->h_cur_list, GWLP_WNDPROC, (LONG_PTR)keycfg_listbox_subclass_proc);
    }

    cmd_sel = (int)SendDlgItemMessageW(hdlg, IDC_KEYCFG_CMD_LIST, LB_GETCURSEL, 0, 0);
    cur_sel = (int)SendDlgItemMessageW(hdlg, IDC_KEYCFG_CUR_LIST, LB_GETCURSEL, 0, 0);
    if (cur_sel >= 0)
    {
        SendDlgItemMessageW(hdlg, IDC_KEYCFG_CMD_LIST, LB_SETCURSEL, (WPARAM)cur_sel, 0);
        SendDlgItemMessageW(hdlg, IDC_KEYCFG_CUR_LIST, LB_SETCURSEL, (WPARAM)-1, 0);
        cmd_sel = cur_sel;
    }

    if (cmd_id == IDC_KEYCFG_BTN_RESET)
    {
        int top_idx = (int)SendDlgItemMessageW(hdlg, IDC_KEYCFG_CMD_LIST, LB_GETTOPINDEX, 0, 0);
        reset_bindings_to_defaults();
        ctx->prev_cmd_sel = -1;
        ctx->prev_key_sel = -1;
        populate_cur_binding_listbox(hdlg, ctx);
        if (top_idx >= 0)
            SendDlgItemMessageW(hdlg, IDC_KEYCFG_CUR_LIST, LB_SETTOPINDEX, (WPARAM)top_idx, 0);
    }

    if (cmd_sel >= 0 && cmd_sel < ctx->menu_n && cmd_sel != ctx->prev_cmd_sel)
    {
        int bslot = find_binding_slot_by_cmd(ctx->menus[cmd_sel].cmd_id);
        int kidx = 0;
        UINT flg = 0;

        if (bslot >= 0)
        {
            kidx = vk_to_key_index(g_bindings[bslot].vk);
            if (kidx > 0)
                flg = g_bindings[bslot].flags;
        }
        ctx->prev_cmd_sel = cmd_sel;
        ctx->prev_key_sel = kidx;

        SendDlgItemMessageW(hdlg, IDC_KEYCFG_KEY_LIST, LB_SETCURSEL, (WPARAM)kidx, 0);
        st->checks[0] = (flg & 1) ? 1 : 0;
        st->checks[1] = (flg & 2) ? 1 : 0;
        st->checks[2] = (flg & 4) ? 1 : 0;
        CheckDlgButton(hdlg, IDC_KEYCFG_CHK_SHIFT, st->checks[0] ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_KEYCFG_CHK_CTRL, st->checks[1] ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_KEYCFG_CHK_ALT, st->checks[2] ? BST_CHECKED : BST_UNCHECKED);
    }

    key_sel = (int)SendDlgItemMessageW(hdlg, IDC_KEYCFG_KEY_LIST, LB_GETCURSEL, 0, 0);
    if (cmd_sel >= 0 && cmd_sel < ctx->menu_n &&
        key_sel >= 0 && key_sel < KEYCFG_KEY_TABLE_COUNT &&
        key_sel != ctx->prev_key_sel)
    {
        int target_cmd = ctx->menus[cmd_sel].cmd_id;
        int bslot = find_binding_slot_by_cmd(target_cmd);
        ctx->prev_key_sel = key_sel;

        if (bslot < 0)
            bslot = find_free_binding_slot();

        if (bslot >= 0)
        {
            if (k_key_table[key_sel].vk == 0)
            {
                g_bindings[bslot].cmd_id = 0;
                g_bindings[bslot].vk = 0;
                g_bindings[bslot].flags = 0;
                update_cur_binding_row(hdlg, cmd_sel, 0, 0);
            }
            else
            {
                UINT flg = 0;
                if (st->checks[0])
                    flg |= 1;
                if (st->checks[1])
                    flg |= 2;
                if (st->checks[2])
                    flg |= 4;
                g_bindings[bslot].cmd_id = target_cmd;
                g_bindings[bslot].vk = k_key_table[key_sel].vk;
                g_bindings[bslot].flags = flg;
                update_cur_binding_row(hdlg, cmd_sel, flg, key_sel);
            }
        }
    }

    if ((cmd_id == IDC_KEYCFG_CHK_SHIFT ||
         cmd_id == IDC_KEYCFG_CHK_CTRL ||
         cmd_id == IDC_KEYCFG_CHK_ALT) &&
        cmd_sel >= 0 && cmd_sel < ctx->menu_n)
    {
        int bslot = find_binding_slot_by_cmd(ctx->menus[cmd_sel].cmd_id);
        if (bslot >= 0 && ctx->prev_key_sel > 0)
        {
            UINT flg = 0;
            if (st->checks[0])
                flg |= 1;
            if (st->checks[1])
                flg |= 2;
            if (st->checks[2])
                flg |= 4;
            g_bindings[bslot].flags = flg;
            update_cur_binding_row(hdlg, cmd_sel, flg, ctx->prev_key_sel);
        }
    }

    return TRUE;
}

static const wchar_t *find_menu_label_w(const EnumMenuCmd *menus, int menu_n, int cmd_id)
{
    int i;
    for (i = 0; i < menu_n; i++)
    {
        if (menus[i].cmd_id == cmd_id)
            return menus[i].label_w;
    }
    return L"";
}

// 0x004340a0: run KEYCFGLIST dialog, validate duplicates, and save/delete aviutl.key
void keycfg_run_dialog(HWND main_hwnd)
{
    KeyCfgDlgCtx ctx;
    char key_path[MAX_PATH];

    build_default_table();
    get_keycfg_file_path(key_path, sizeof(key_path));

    ZeroMemory(&ctx, sizeof(ctx));
    ctx.menus = (EnumMenuCmd *)calloc(KEYCFG_MAX_MENU_ITEMS, sizeof(EnumMenuCmd));
    if (!ctx.menus)
        return;
    ctx.menu_n = enumerate_main_menu(main_hwnd, ctx.menus, KEYCFG_MAX_MENU_ITEMS);

    for (;;)
    {
        AviUtlDlgState st;
        INT_PTR res;
        int i, j, dup_cmd1 = 0, dup_cmd2 = 0;

        aviutl_dlg_init(&st);
        st.on_cmd = on_keycfg_dlg_cmd;
        st.user = &ctx;

        res = aviutl_dlg_run(main_hwnd, "KEYCFGLIST", &st);
        if (res != IDOK)
        {
            // restore saved aviutl.key (or defaults) when user cancels
            keycfg_init_and_load(main_hwnd);
            break;
        }

        // check for duplicate (flags, vk) bindings across active entries (0x004340a0 lines 78..126)
        for (i = 0; i < KEYCFG_MAX_BINDINGS && dup_cmd1 == 0; i++)
        {
            if (g_bindings[i].cmd_id == 0 || g_bindings[i].vk == 0)
                continue;
            for (j = i + 1; j < KEYCFG_MAX_BINDINGS; j++)
            {
                if (g_bindings[j].cmd_id == 0 || g_bindings[j].vk == 0)
                    continue;
                if (g_bindings[i].vk == g_bindings[j].vk &&
                    (g_bindings[i].flags & 7) == (g_bindings[j].flags & 7))
                {
                    dup_cmd1 = g_bindings[i].cmd_id;
                    dup_cmd2 = g_bindings[j].cmd_id;
                    break;
                }
            }
        }

        if (dup_cmd1 != 0 && dup_cmd2 != 0)
        {
            wchar_t msg[1024];
            const wchar_t *fmt = i18n_get_wstr(1045);
            const wchar_t *lbl1 = find_menu_label_w(ctx.menus, ctx.menu_n, dup_cmd1);
            const wchar_t *lbl2 = find_menu_label_w(ctx.menus, ctx.menu_n, dup_cmd2);
            if (fmt && fmt[0])
                _snwprintf(msg, 1024, fmt, lbl1, lbl2);
            else
                _snwprintf(msg, 1024, L"Duplicate shortcut key:\n\n%s\n%s\n", lbl1, lbl2);
            msg[1023] = L'\0';
            aviutl_msgbox_w(main_hwnd, msg, L"AviUtl", MB_OK | MB_ICONEXCLAMATION);
            continue;
        }

        // serialize & write aviutl.key (or delete if identical to defaults)
        {
            unsigned char *buf = (unsigned char *)calloc(1, KEYCFG_FILE_MAX_BYTES);
            int bytes = buf ? keycfg_serialize(main_hwnd, buf, KEYCFG_FILE_MAX_BYTES) : 0;
            if (bytes <= 0)
            {
                DeleteFileA(key_path);
            }
            else
            {
                HANDLE hf = CreateFileA(key_path, GENERIC_WRITE, 0, NULL,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hf != INVALID_HANDLE_VALUE)
                {
                    DWORD written = 0;
                    WriteFile(hf, buf, (DWORD)bytes, &written, NULL);
                    CloseHandle(hf);
                }
            }
            free(buf);
        }
        break;
    }

    free(ctx.menus);
}
