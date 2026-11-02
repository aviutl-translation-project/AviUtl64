#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "i18n.h"
#include "aviutl_dlg.h"

#define I18N_MAX_PACKS 16
#define I18N_CACHE_LOW 256
#define I18N_RING_SLOTS 8

typedef struct LangPackEntry
{
    BOOL valid;
    BOOL is_exe;
    LANGID langid;
    char path[MAX_PATH];
    wchar_t name_w[260];
    wchar_t credit_w[260];
} LangPackEntry;

static HINSTANCE g_exe_mod = NULL;
static HMODULE g_active_mod = NULL;
static int g_pack_count = 0;
static int g_active_pack = 0;
static BOOL g_is_auto = TRUE;
static LangPackEntry g_packs[I18N_MAX_PACKS];

static wchar_t g_wcache_low[I18N_CACHE_LOW][260];
static BOOL g_wcache_valid[I18N_CACHE_LOW];
static wchar_t g_wring[I18N_RING_SLOTS][512];
static int g_wring_idx = 0;

static char g_aring[I18N_RING_SLOTS][512];
static int g_aring_idx = 0;

static void get_exe_dir(char *out_dir, size_t cap)
{
    char mod[MAX_PATH];
    char *slash;
    if (!out_dir || cap == 0)
        return;
    out_dir[0] = '\0';
    if (GetModuleFileNameA(NULL, mod, MAX_PATH) > 0)
    {
        slash = strrchr(mod, '\\');
        if (slash)
            *(slash + 1) = '\0';
        else
            mod[0] = '\0';
        lstrcpynA(out_dir, mod, (int)cap);
    }
}

static void get_ini_path_w(wchar_t *out_path, size_t cap_cch)
{
    wchar_t mod[MAX_PATH];
    wchar_t *slash;
    if (!out_path || cap_cch == 0)
        return;
    out_path[0] = L'\0';
    if (GetModuleFileNameW(NULL, mod, MAX_PATH) > 0)
    {
        slash = wcsrchr(mod, L'\\');
        if (slash)
            *(slash + 1) = L'\0';
        else
            mod[0] = L'\0';
        _snwprintf(out_path, cap_cch, L"%saviutl.ini", mod);
        out_path[cap_cch - 1] = L'\0';
    }
    else
    {
        lstrcpynW(out_path, L".\\aviutl.ini", (int)cap_cch);
    }
}

static const char *basename_dbcs(const char *path)
{
    const char *base;
    const unsigned char *p;
    if (!path)
        return "";
    base = path;
    p = (const unsigned char *)path;
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
            if (*p == '\\' || *p == '/' || *p == ':')
                base = (const char *)(p + 1);
            p++;
        }
    }
    return base;
}

static BOOL CALLBACK enum_first_lang_proc(HMODULE hModule, LPCWSTR lpType,
                                          LPCWSTR lpName, WORD wIDLanguage,
                                          LONG_PTR lParam)
{
    (void)hModule;
    (void)lpType;
    (void)lpName;
    if (lParam)
    {
        *(WORD *)lParam = wIDLanguage;
    }
    return FALSE;
}

static HRSRC find_res_any_lang(HMODULE hMod, LPCWSTR type, LPCWSTR name, WORD *out_lang)
{
    WORD found_lang = 0xFFFFu;
    HRSRC hRes = NULL;

    if (!hMod)
        return NULL;

    EnumResourceLanguagesW(hMod, type, name, enum_first_lang_proc, (LONG_PTR)&found_lang);
    if (found_lang != 0xFFFFu)
    {
        if (out_lang)
            *out_lang = found_lang;
        hRes = FindResourceExW(hMod, type, name, found_lang);
        if (hRes)
            return hRes;
    }
    hRes = FindResourceW(hMod, name, type);
    if (hRes && out_lang && *out_lang == 0)
    {
        *out_lang = 0x0411;
    }
    return hRes;
}

static BOOL load_string_from_mod(HMODULE hMod, UINT id, wchar_t *out_buf, int out_cap)
{
    UINT bundle_id;
    HRSRC hRes;
    HGLOBAL hGlb;
    const WORD *p;
    DWORD byte_sz, word_cnt, pos = 0;
    UINT idx_in_bundle, k, slen;

    if (!out_buf || out_cap <= 0)
        return FALSE;
    out_buf[0] = L'\0';
    if (!hMod)
        return FALSE;

    bundle_id = (id >> 4) + 1u;
    idx_in_bundle = id & 15u;
    hRes = find_res_any_lang(hMod, (LPCWSTR)RT_STRING, MAKEINTRESOURCEW(bundle_id), NULL);
    if (!hRes)
        return FALSE;

    byte_sz = SizeofResource(hMod, hRes);
    hGlb = LoadResource(hMod, hRes);
    if (!hGlb || byte_sz < 2)
        return FALSE;

    p = (const WORD *)LockResource(hGlb);
    if (!p)
        return FALSE;

    word_cnt = byte_sz / 2u;
    for (k = 0; k < idx_in_bundle; k++)
    {
        if (pos >= word_cnt)
            return FALSE;
        pos += 1u + (DWORD)p[pos];
    }
    if (pos >= word_cnt)
        return FALSE;

    slen = (UINT)p[pos++];
    if (slen == 0 || pos + slen > word_cnt)
        return FALSE;

    if ((int)slen >= out_cap)
        slen = (UINT)(out_cap - 1);
    memcpy(out_buf, p + pos, (size_t)slen * sizeof(wchar_t));
    out_buf[slen] = L'\0';
    return TRUE;
}

static void unescape_rc_newlines(wchar_t *s)
{
    wchar_t *r = s;
    wchar_t *w = s;
    if (!s)
        return;
    while (*r)
    {
        if (r[0] == L'\\' && r[1] == L'n')
        {
            *w++ = L'\n';
            r += 2;
        }
        else if (r[0] == L'\\' && r[1] == L'r')
        {
            *w++ = L'\r';
            r += 2;
        }
        else
        {
            *w++ = *r++;
        }
    }
    *w = L'\0';
}

static void clear_string_cache(void)
{
    ZeroMemory(g_wcache_low, sizeof(g_wcache_low));
    ZeroMemory(g_wcache_valid, sizeof(g_wcache_valid));
}

static void scan_aul_dir(const char *dir)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE hFind;

    if (!dir || !dir[0] || g_pack_count >= I18N_MAX_PACKS)
        return;

    _snprintf(pattern, sizeof(pattern), "%s*.aul", dir);
    pattern[sizeof(pattern) - 1] = '\0';

    hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        char full[MAX_PATH];
        HMODULE hMod;
        WORD lang = 0;
        HRSRC hRes;
        wchar_t nm[260], cr[260];

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (g_pack_count >= I18N_MAX_PACKS)
            break;

        _snprintf(full, sizeof(full), "%s%s", dir, fd.cFileName);
        full[sizeof(full) - 1] = '\0';

        // LOAD_LIBRARY_AS_DATAFILE allows 64-bit reaviutl to load 32-bit .aul resource DLLs
        hMod = LoadLibraryExA(full, NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (!hMod)
            continue;

        hRes = find_res_any_lang(hMod, (LPCWSTR)RT_STRING, MAKEINTRESOURCEW(1), &lang);
        if (hRes && load_string_from_mod(hMod, 0, nm, 260) && nm[0])
        {
            LangPackEntry *e = &g_packs[g_pack_count++];
            ZeroMemory(e, sizeof(*e));
            e->valid = TRUE;
            e->is_exe = FALSE;
            e->langid = (lang != 0xFFFFu && lang != 0) ? lang : 0x0409;
            lstrcpynA(e->path, full, sizeof(e->path));
            lstrcpynW(e->name_w, nm, 260);
            if (load_string_from_mod(hMod, 1, cr, 260))
            {
                lstrcpynW(e->credit_w, cr, 260);
            }
        }
        FreeLibrary(hMod);
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
}

static void activate_pack_index(int idx)
{
    if (g_active_mod && g_active_mod != (HMODULE)g_exe_mod)
    {
        FreeLibrary(g_active_mod);
        g_active_mod = NULL;
    }

    if (idx < 0 || idx >= g_pack_count || !g_packs[idx].valid)
    {
        idx = 0;
    }
    g_active_pack = idx;

    if (idx > 0 && !g_packs[idx].is_exe && g_packs[idx].path[0])
    {
        g_active_mod = LoadLibraryExA(g_packs[idx].path, NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (!g_active_mod)
        {
            g_active_pack = 0;
        }
    }
    clear_string_cache();
}

static void select_from_ini(void)
{
    wchar_t ini_w[MAX_PATH];
    wchar_t res_w[260] = {0};
    int i, chosen = -1;

    get_ini_path_w(ini_w, MAX_PATH);
    GetPrivateProfileStringW(L"system", L"resource", L"", res_w, 260, ini_w);

    if (res_w[0] != L'\0')
    {
        g_is_auto = FALSE;
        if (res_w[0] == L'@')
        {
            UINT want_lang = (UINT)wcstoul(res_w + 1, NULL, 16);
            for (i = 0; i < g_pack_count; i++)
            {
                if (g_packs[i].valid && g_packs[i].langid == (LANGID)want_lang)
                {
                    chosen = i;
                    break;
                }
            }
        }
        else
        {
            for (i = 0; i < g_pack_count; i++)
            {
                if (g_packs[i].valid && lstrcmpW(g_packs[i].name_w, res_w) == 0)
                {
                    chosen = i;
                    break;
                }
            }
        }
    }

    if (chosen < 0)
    {
        LANGID sys_lang = GetSystemDefaultLangID();
        g_is_auto = TRUE;
        chosen = 0;
        for (i = 0; i < g_pack_count; i++)
        {
            if (g_packs[i].valid && g_packs[i].langid == sys_lang)
            {
                chosen = i;
                break;
            }
        }
    }

    activate_pack_index(chosen);
}

void i18n_init(HINSTANCE hInst)
{
    char exe_dir[MAX_PATH];
    char plug_dir[MAX_PATH];
    WORD exe_lang = 0x0411;

    i18n_shutdown();
    g_exe_mod = hInst ? hInst : GetModuleHandleA(NULL);
    ZeroMemory(g_packs, sizeof(g_packs));
    g_pack_count = 0;

    // slot 0: built-in resources in aviutl.exe / reaviutl.exe (0x004541e0)
    if (find_res_any_lang(g_exe_mod, (LPCWSTR)RT_STRING, MAKEINTRESOURCEW(1), &exe_lang))
    {
        LangPackEntry *e = &g_packs[g_pack_count++];
        e->valid = TRUE;
        e->is_exe = TRUE;
        e->langid = (exe_lang != 0xFFFFu && exe_lang != 0) ? exe_lang : 0x0411;
        load_string_from_mod(g_exe_mod, 0, e->name_w, 260);
        load_string_from_mod(g_exe_mod, 1, e->credit_w, 260);
        if (!e->name_w[0])
            lstrcpyW(e->name_w, L"\x65e5\x672c\x8a9e");
    }

    get_exe_dir(exe_dir, sizeof(exe_dir));
    if (exe_dir[0])
    {
        scan_aul_dir(exe_dir);
        _snprintf(plug_dir, sizeof(plug_dir), "%splugins\\", exe_dir);
        plug_dir[sizeof(plug_dir) - 1] = '\0';
        scan_aul_dir(plug_dir);
    }

    select_from_ini();
}

void i18n_shutdown(void)
{
    if (g_active_mod && g_active_mod != (HMODULE)g_exe_mod)
    {
        FreeLibrary(g_active_mod);
    }
    g_active_mod = NULL;
    g_pack_count = 0;
    g_active_pack = 0;
    clear_string_cache();
}

LANGID i18n_get_active_langid(void)
{
    if (g_active_pack >= 0 && g_active_pack < g_pack_count && g_packs[g_active_pack].valid)
    {
        return g_packs[g_active_pack].langid;
    }
    return 0x0411;
}

BOOL i18n_is_japanese(void)
{
    return PRIMARYLANGID(i18n_get_active_langid()) == LANG_JAPANESE;
}

const wchar_t *i18n_get_wstr(UINT id)
{
    wchar_t *dst;
    BOOL ok = FALSE;

    if (!g_exe_mod)
        g_exe_mod = GetModuleHandleA(NULL);

    if (id < I18N_CACHE_LOW)
    {
        if (g_wcache_valid[id])
            return g_wcache_low[id];
        dst = g_wcache_low[id];
        if (g_active_mod)
            ok = load_string_from_mod(g_active_mod, id, dst, 260);
        if (!ok)
            ok = load_string_from_mod(g_exe_mod, id, dst, 260);
        if (!ok)
            dst[0] = L'\0';
        unescape_rc_newlines(dst);
        g_wcache_valid[id] = TRUE;
        return dst;
    }

    dst = g_wring[g_wring_idx];
    g_wring_idx = (g_wring_idx + 1) & (I18N_RING_SLOTS - 1);
    if (g_active_mod)
        ok = load_string_from_mod(g_active_mod, id, dst, 512);
    if (!ok)
        ok = load_string_from_mod(g_exe_mod, id, dst, 512);
    if (!ok)
        dst[0] = L'\0';
    unescape_rc_newlines(dst);
    return dst;
}

const char *i18n_get_str(UINT id)
{
    const wchar_t *w = i18n_get_wstr(id);
    char *dst = g_aring[g_aring_idx];
    g_aring_idx = (g_aring_idx + 1) & (I18N_RING_SLOTS - 1);

    if (!w || !w[0])
    {
        dst[0] = '\0';
        return dst;
    }
    if (WideCharToMultiByte(932, 0, w, -1, dst, 512, NULL, NULL) <= 0)
    {
        WideCharToMultiByte(CP_ACP, 0, w, -1, dst, 512, NULL, NULL);
    }
    return dst;
}

HMENU i18n_load_menu(const char *res_name)
{
    wchar_t wname[128];
    HRSRC hRes = NULL;
    HMODULE hSrc = NULL;
    HGLOBAL hGlb;
    const MENUTEMPLATEW *tpl;

    if (!res_name || !res_name[0])
        return NULL;
    if (!g_exe_mod)
        g_exe_mod = GetModuleHandleA(NULL);

    MultiByteToWideChar(CP_ACP, 0, res_name, -1, wname, 128);

    if (g_active_mod)
    {
        hRes = find_res_any_lang(g_active_mod, (LPCWSTR)RT_MENU, wname, NULL);
        if (hRes)
            hSrc = g_active_mod;
    }
    if (!hRes)
    {
        hRes = find_res_any_lang(g_exe_mod, (LPCWSTR)RT_MENU, wname, NULL);
        if (hRes)
            hSrc = g_exe_mod;
    }
    if (!hRes || !hSrc)
        return NULL;

    hGlb = LoadResource(hSrc, hRes);
    if (!hGlb)
        return NULL;
    tpl = (const MENUTEMPLATEW *)LockResource(hGlb);
    if (!tpl)
        return NULL;

    return LoadMenuIndirectW(tpl);
}

const DLGTEMPLATE *i18n_load_dialog_template(const char *res_name)
{
    wchar_t wname[128];
    HRSRC hRes = NULL;
    HMODULE hSrc = NULL;
    HGLOBAL hGlb;

    if (!res_name || !res_name[0])
        return NULL;
    if (!g_exe_mod)
        g_exe_mod = GetModuleHandleA(NULL);

    MultiByteToWideChar(CP_ACP, 0, res_name, -1, wname, 128);

    if (g_active_mod)
    {
        hRes = find_res_any_lang(g_active_mod, (LPCWSTR)RT_DIALOG, wname, NULL);
        if (hRes)
            hSrc = g_active_mod;
    }
    if (!hRes)
    {
        hRes = find_res_any_lang(g_exe_mod, (LPCWSTR)RT_DIALOG, wname, NULL);
        if (hRes)
            hSrc = g_exe_mod;
    }
    if (!hRes || !hSrc)
        return NULL;

    hGlb = LoadResource(hSrc, hRes);
    if (!hGlb)
        return NULL;
    return (const DLGTEMPLATE *)LockResource(hGlb);
}

// 0x004548f0: "LANG" dialog (ID 106)
BOOL i18n_show_lang_dialog(HWND owner)
{
    AviUtlDlgState st;
    wchar_t labels[I18N_MAX_PACKS + 1][320];
    const wchar_t *ptrs[I18N_MAX_PACKS + 2];
    int i, cur_sel = 0;

    aviutl_dlg_init(&st);

    lstrcpynW(labels[0], i18n_get_wstr(11), 320);
    if (!labels[0][0])
        lstrcpyW(labels[0], L"Automatic");
    ptrs[0] = labels[0];

    for (i = 0; i < g_pack_count; i++)
    {
        wchar_t lang_desc[128] = {0};
        VerLanguageNameW(g_packs[i].langid, lang_desc, 128);
        _snwprintf(labels[i + 1], 320, L"%s  ( %s )",
                   g_packs[i].name_w,
                   lang_desc[0] ? lang_desc : L"Unknown");
        labels[i + 1][319] = L'\0';
        ptrs[i + 1] = labels[i + 1];
        if (!g_is_auto && i == g_active_pack)
        {
            cur_sel = i + 1;
        }
    }
    ptrs[g_pack_count + 1] = NULL;

    st.combo_items[0] = ptrs;
    st.combo_sel[0] = cur_sel;

    if (aviutl_dlg_run(owner, "LANG", &st) != IDOK)
    {
        return FALSE;
    }

    {
        wchar_t ini_w[MAX_PATH];
        int chosen = st.combo_sel[0] - 1;
        get_ini_path_w(ini_w, MAX_PATH);

        if (chosen < 0 || chosen >= g_pack_count)
        {
            WritePrivateProfileStringW(L"system", L"resource", L"", ini_w);
        }
        else
        {
            BOOL bad_conv = FALSE;
            char tmp_acp[260];
            WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
                                g_packs[chosen].name_w, -1,
                                tmp_acp, sizeof(tmp_acp), NULL, &bad_conv);
            if (bad_conv)
            {
                wchar_t tag[32];
                wsprintfW(tag, L"@%04X", (unsigned)g_packs[chosen].langid);
                WritePrivateProfileStringW(L"system", L"resource", tag, ini_w);
            }
            else
            {
                WritePrivateProfileStringW(L"system", L"resource", g_packs[chosen].name_w, ini_w);
            }
        }
    }

    select_from_ini();
    return TRUE;
}

// 0x00405450 case 107: Language Resource Information
void i18n_show_lang_info(HWND owner)
{
    wchar_t cap[260];
    wchar_t body[4096];
    int i, ext_count = 0;

    lstrcpynW(cap, i18n_get_wstr(1034), 260);
    if (!cap[0])
        lstrcpyW(cap, L"AviUtl");

    body[0] = L'\0';
    for (i = 1; i < g_pack_count; i++)
    {
        wchar_t wbase[MAX_PATH];
        wchar_t line[600];
        const char *base;

        if (!g_packs[i].valid || g_packs[i].is_exe)
            continue;
        base = basename_dbcs(g_packs[i].path);
        if (MultiByteToWideChar(932, 0, base, -1, wbase, MAX_PATH) <= 0)
            MultiByteToWideChar(CP_ACP, 0, base, -1, wbase, MAX_PATH);

        _snwprintf(line, 600, L"%s ( %s )\n",
                   g_packs[i].credit_w[0] ? g_packs[i].credit_w : g_packs[i].name_w,
                   wbase);
        line[599] = L'\0';
        if (lstrlenW(body) + lstrlenW(line) + 2 < 4096)
        {
            lstrcatW(body, line);
            ext_count++;
        }
    }

    if (ext_count == 0)
    {
        lstrcpynW(body, i18n_get_wstr(1020), 4096);
    }

    aviutl_msgbox_w(owner, body, cap, MB_OK);
}
