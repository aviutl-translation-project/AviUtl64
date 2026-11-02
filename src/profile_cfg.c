#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "profile_cfg.h"
#include "plugin_manager.h"
#include "aviutl_dlg.h"
#include "i18n.h"
#include "debug_console.h"

typedef struct ProfileBlobEntry
{
    char name[260];
    uint8_t *blob;
    size_t blob_size;
} ProfileBlobEntry;

static ProfileBlobEntry g_global[PROFILE_MAX_COUNT];
static int g_global_count = 1;
static int g_active_global = 0;

static ProfileBlobEntry g_proj_slots[PROFILE_MAX_COUNT];
static int g_proj_active_count = 0;

static uint8_t *g_slot0_live_blob = NULL;
static size_t g_slot0_live_size = 0;
static int g_applied_slot = 0;
static BOOL g_initialized = FALSE;

static void get_exe_dir_file(const char *rel_name, char *out_path, size_t out_cap)
{
    char mod[MAX_PATH];
    char *slash;

    if (!out_path || out_cap == 0)
        return;
    out_path[0] = '\0';

    if (GetModuleFileNameA(NULL, mod, MAX_PATH) > 0)
    {
        slash = strrchr(mod, '\\');
        if (slash)
            *(slash + 1) = '\0';
        else
            mod[0] = '\0';
        _snprintf(out_path, out_cap, "%s%s", mod, rel_name ? rel_name : "");
        out_path[out_cap - 1] = '\0';
    }
    else
    {
        _snprintf(out_path, out_cap, ".\\%s", rel_name ? rel_name : "");
        out_path[out_cap - 1] = '\0';
    }
}

static void build_global_cfg_path(int idx, char *out_path, size_t out_cap)
{
    char fname[300];
    if (idx <= 0)
    {
        get_exe_dir_file("aviutl_default.sav", out_path, out_cap);
        return;
    }
    _snprintf(fname, sizeof(fname), "%s.cfg", g_global[idx].name);
    fname[sizeof(fname) - 1] = '\0';
    get_exe_dir_file(fname, out_path, out_cap);
}

static void free_blob_entry(ProfileBlobEntry *e)
{
    if (!e)
        return;
    if (e->blob)
    {
        free(e->blob);
        e->blob = NULL;
    }
    e->blob_size = 0;
    e->name[0] = '\0';
}

static BOOL set_entry_blob(ProfileBlobEntry *e, const uint8_t *blob, size_t sz)
{
    uint8_t *nb = NULL;
    if (!e)
        return FALSE;
    if (blob && sz > 0)
    {
        nb = (uint8_t *)malloc(sz);
        if (!nb)
            return FALSE;
        memcpy(nb, blob, sz);
    }
    if (e->blob)
        free(e->blob);
    e->blob = nb;
    e->blob_size = (nb != NULL) ? sz : 0;
    return TRUE;
}

static BOOL read_file_bytes(const char *path, uint8_t **out_buf, size_t *out_sz)
{
    HANDLE hFile;
    DWORD sz_lo, sz_hi = 0, got = 0;
    uint8_t *buf;

    if (!path || !path[0] || !out_buf || !out_sz)
        return FALSE;
    *out_buf = NULL;
    *out_sz = 0;

    hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;

    sz_lo = GetFileSize(hFile, &sz_hi);
    if (sz_lo == INVALID_FILE_SIZE || sz_hi != 0 || sz_lo < PROFILE_CFG_HEADER_SIZE ||
        sz_lo > (64u * 1024u * 1024u))
    {
        CloseHandle(hFile);
        return FALSE;
    }

    buf = (uint8_t *)malloc((size_t)sz_lo);
    if (!buf)
    {
        CloseHandle(hFile);
        return FALSE;
    }
    if (!ReadFile(hFile, buf, sz_lo, &got, NULL) || got != sz_lo ||
        memcmp(buf, PROFILE_CFG_MAGIC, PROFILE_CFG_HEADER_SIZE) != 0)
    {
        free(buf);
        CloseHandle(hFile);
        return FALSE;
    }

    CloseHandle(hFile);
    *out_buf = buf;
    *out_sz = (size_t)sz_lo;
    return TRUE;
}

static BOOL write_file_bytes(const char *path, const uint8_t *buf, size_t sz)
{
    HANDLE hFile;
    DWORD written = 0;
    BOOL ok;

    if (!path || !path[0] || !buf || sz == 0)
        return FALSE;
    hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;

    ok = WriteFile(hFile, buf, (DWORD)sz, &written, NULL) && (size_t)written == sz;
    CloseHandle(hFile);
    if (!ok)
        DeleteFileA(path);
    return ok;
}

static void save_active_name_to_ini(void)
{
    char ini[MAX_PATH];
    get_exe_dir_file("aviutl.ini", ini, sizeof(ini));
    WritePrivateProfileStringA("system", "profile",
                               (g_active_global > 0 && g_active_global < g_global_count)
                                   ? g_global[g_active_global].name
                                   : "",
                               ini);
}

static int cmp_cfg_names(const void *a, const void *b)
{
    const ProfileBlobEntry *pa = (const ProfileBlobEntry *)a;
    const ProfileBlobEntry *pb = (const ProfileBlobEntry *)b;
    return _stricmp(pa->name, pb->name);
}

static const char *default_profile_label(void)
{
    const char *s = i18n_get_str(10);
    return (s && s[0]) ? s : "Default";
}

void profile_cfg_init(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE hFind;
    char pattern[MAX_PATH];
    char def_path[MAX_PATH];
    char ini[MAX_PATH];
    char saved_prof[260];
    uint8_t *blob = NULL;
    size_t bsz = 0;
    int i;

    if (g_initialized)
        profile_cfg_shutdown();

    ZeroMemory(g_global, sizeof(g_global));
    ZeroMemory(g_proj_slots, sizeof(g_proj_slots));
    g_proj_active_count = 0;
    g_applied_slot = 0;

    lstrcpynA(g_global[0].name, default_profile_label(), sizeof(g_global[0].name));
    g_global_count = 1;
    g_active_global = 0;

    build_global_cfg_path(0, def_path, sizeof(def_path));
    if (read_file_bytes(def_path, &blob, &bsz))
    {
        g_global[0].blob = blob;
        g_global[0].blob_size = bsz;
    }
    else if (plugin_manager_serialize_cfg_blob(&blob, &bsz))
    {
        g_global[0].blob = blob;
        g_global[0].blob_size = bsz;
    }

    get_exe_dir_file("*.cfg", pattern, sizeof(pattern));
    hFind = FindFirstFileA(pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            char *dot;
            char full[MAX_PATH];
            uint8_t *cblob = NULL;
            size_t csz = 0;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (g_global_count >= PROFILE_MAX_COUNT)
                break;

            lstrcpynA(g_global[g_global_count].name, fd.cFileName,
                      sizeof(g_global[g_global_count].name));
            dot = strrchr(g_global[g_global_count].name, '.');
            if (!dot || _stricmp(dot, ".cfg") != 0)
                continue;
            *dot = '\0';
            if (!g_global[g_global_count].name[0])
                continue;

            get_exe_dir_file(fd.cFileName, full, sizeof(full));
            if (read_file_bytes(full, &cblob, &csz))
            {
                g_global[g_global_count].blob = cblob;
                g_global[g_global_count].blob_size = csz;
                g_global_count++;
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    if (g_global_count > 2)
    {
        qsort(&g_global[1], (size_t)(g_global_count - 1),
              sizeof(ProfileBlobEntry), cmp_cfg_names);
    }

    get_exe_dir_file("aviutl.ini", ini, sizeof(ini));
    saved_prof[0] = '\0';
    GetPrivateProfileStringA("system", "profile", "", saved_prof, sizeof(saved_prof), ini);
    if (saved_prof[0])
    {
        for (i = 1; i < g_global_count; i++)
        {
            if (_stricmp(g_global[i].name, saved_prof) == 0)
            {
                g_active_global = i;
                break;
            }
        }
    }

    if (g_global[g_active_global].blob && g_global[g_active_global].blob_size > 0)
    {
        plugin_manager_apply_cfg_blob(g_global[g_active_global].blob,
                                      g_global[g_active_global].blob_size, TRUE);
    }

    g_initialized = TRUE;
}

void profile_cfg_shutdown(void)
{
    int i;
    for (i = 0; i < PROFILE_MAX_COUNT; i++)
    {
        free_blob_entry(&g_global[i]);
        free_blob_entry(&g_proj_slots[i]);
    }
    if (g_slot0_live_blob)
    {
        free(g_slot0_live_blob);
        g_slot0_live_blob = NULL;
    }
    g_slot0_live_size = 0;
    g_global_count = 1;
    g_active_global = 0;
    g_proj_active_count = 0;
    g_applied_slot = 0;
    g_initialized = FALSE;
}

int profile_cfg_get_count(void)
{
    return (g_global_count > 0) ? g_global_count : 1;
}

const char *profile_cfg_get_name(int index)
{
    if (index == 0)
        return default_profile_label();
    if (index < 0 || index >= g_global_count)
        return "";
    return g_global[index].name;
}

int profile_cfg_find_by_name(const char *name)
{
    int i;
    if (!name || !name[0])
        return 0;
    if (_stricmp(name, default_profile_label()) == 0 ||
        lstrcmpA(name, "\x83\x66\x83\x74\x83\x48\x83\x8b\x83\x67") == 0)
    {
        return 0;
    }
    for (i = 1; i < g_global_count; i++)
    {
        if (_stricmp(g_global[i].name, name) == 0)
            return i;
    }
    return -1;
}

int profile_cfg_get_active(void)
{
    return g_active_global;
}

void profile_cfg_save_active(void)
{
    uint8_t *blob = NULL;
    size_t sz = 0;
    char path[MAX_PATH];
    int s;

    if (!g_initialized)
        return;
    profile_cfg_restore_active();

    if (!plugin_manager_serialize_cfg_blob(&blob, &sz) || !blob || sz == 0)
        return;

    if (g_global[g_active_global].blob)
        free(g_global[g_active_global].blob);
    g_global[g_active_global].blob = blob;
    g_global[g_active_global].blob_size = sz;

    build_global_cfg_path(g_active_global, path, sizeof(path));
    write_file_bytes(path, blob, sz);
    save_active_name_to_ini();

    if (g_active_global > 0)
    {
        for (s = 1; s < PROFILE_MAX_COUNT; s++)
        {
            if (g_proj_slots[s].name[0] &&
                _stricmp(g_proj_slots[s].name, g_global[g_active_global].name) == 0)
            {
                set_entry_blob(&g_proj_slots[s], blob, sz);
            }
        }
    }
}

BOOL profile_cfg_select(int index, BOOL save_current_first)
{
    char path[MAX_PATH];
    uint8_t *disk_blob = NULL;
    size_t disk_sz = 0;

    if (index < 0 || index >= g_global_count)
        return FALSE;

    profile_cfg_restore_active();
    if (save_current_first)
    {
        profile_cfg_save_active();
    }

    g_active_global = index;
    save_active_name_to_ini();

    if (index > 0)
    {
        build_global_cfg_path(index, path, sizeof(path));
        if (read_file_bytes(path, &disk_blob, &disk_sz))
        {
            if (g_global[index].blob)
                free(g_global[index].blob);
            g_global[index].blob = disk_blob;
            g_global[index].blob_size = disk_sz;
        }
    }

    if (g_slot0_live_blob)
    {
        free(g_slot0_live_blob);
        g_slot0_live_blob = NULL;
        g_slot0_live_size = 0;
    }
    g_applied_slot = 0;

    if (g_global[index].blob && g_global[index].blob_size > 0)
    {
        return plugin_manager_apply_cfg_blob(g_global[index].blob,
                                             g_global[index].blob_size, TRUE);
    }
    return TRUE;
}

static BOOL validate_profile_name(HWND owner, const char *raw_name, char *out_clean, int cap)
{
    static const wchar_t kInvalidListW[] = L"\\ / : * ? \" < > |";
    const unsigned char *p;
    int len;

    if (!raw_name || !out_clean || cap <= 0)
        return FALSE;

    while (*raw_name == ' ' || *raw_name == '\t')
        raw_name++;
    lstrcpynA(out_clean, raw_name, cap);
    len = (int)strlen(out_clean);
    while (len > 0 && (out_clean[len - 1] == ' ' || out_clean[len - 1] == '\t'))
    {
        out_clean[--len] = '\0';
    }
    if (len == 0)
        return FALSE;

    p = (const unsigned char *)out_clean;
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
            unsigned char ch = *p++;
            if (ch == '\\' || ch == '/' || ch == ':' || ch == '*' ||
                ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|' || ch < 0x20)
            {
                if (owner)
                {
                    wchar_t msg[512];
                    const wchar_t *fmt = i18n_get_wstr(1022);
                    if (fmt && fmt[0])
                        _snwprintf(msg, 512, fmt, kInvalidListW);
                    else
                        _snwprintf(msg, 512, L"Invalid characters in profile name:\n\n%s", kInvalidListW);
                    msg[511] = L'\0';
                    aviutl_msgbox_w(owner, msg, L"AviUtl", MB_OK | MB_ICONWARNING);
                }
                return FALSE;
            }
        }
    }

    if (_stricmp(out_clean, default_profile_label()) == 0 ||
        lstrcmpA(out_clean, "\x83\x66\x83\x74\x83\x48\x83\x8b\x83\x67") == 0)
    {
        return FALSE;
    }
    return TRUE;
}

static int upsert_global_profile(const char *clean_name, const uint8_t *blob, size_t sz)
{
    int idx = profile_cfg_find_by_name(clean_name);
    char path[MAX_PATH];

    if (idx <= 0)
    {
        if (g_global_count >= PROFILE_MAX_COUNT)
            return -1;
        idx = g_global_count++;
        ZeroMemory(&g_global[idx], sizeof(g_global[idx]));
        lstrcpynA(g_global[idx].name, clean_name, sizeof(g_global[idx].name));
    }

    if (!set_entry_blob(&g_global[idx], blob, sz))
        return -1;

    build_global_cfg_path(idx, path, sizeof(path));
    if (!write_file_bytes(path, blob, sz))
        return -1;

    return idx;
}

int profile_cfg_create_interactive(HWND owner, const char *initial_name)
{
    char raw[260];
    char clean[260];
    uint8_t *blob = NULL;
    size_t sz = 0;
    int idx;

    profile_cfg_restore_active();

    if (initial_name && initial_name[0])
    {
        lstrcpynA(raw, initial_name, sizeof(raw));
    }
    else
    {
        AviUtlDlgState st;
        aviutl_dlg_init(&st);
        if (g_active_global > 0)
        {
            aviutl_dlg_set_str_sjis(&st, 0, g_global[g_active_global].name);
        }
        if (aviutl_dlg_run(owner, "STRINGS", &st) != IDOK)
            return -1;
        aviutl_dlg_get_str_sjis(&st, 0, raw, sizeof(raw));
    }

    if (!validate_profile_name(owner, raw, clean, sizeof(clean)))
        return -1;

    if (!plugin_manager_serialize_cfg_blob(&blob, &sz) || !blob || sz == 0)
        return -1;

    idx = upsert_global_profile(clean, blob, sz);
    free(blob);
    if (idx < 0)
        return -1;

    g_active_global = idx;
    save_active_name_to_ini();
    return idx;
}

BOOL profile_cfg_delete_active(EDITP *ep)
{
    char path[MAX_PATH];
    char del_name[260];
    int del_idx = g_active_global;
    int i, s;

    if (del_idx <= 0 || del_idx >= g_global_count)
        return FALSE;

    profile_cfg_restore_active();
    lstrcpynA(del_name, g_global[del_idx].name, sizeof(del_name));
    build_global_cfg_path(del_idx, path, sizeof(path));
    DeleteFileA(path);

    for (s = 1; s < PROFILE_MAX_COUNT; s++)
    {
        if (g_proj_slots[s].name[0] && _stricmp(g_proj_slots[s].name, del_name) == 0)
        {
            if (ep && editp_is_editing(ep))
            {
                int fn = editp_get_frame_n(ep);
                int f;
                for (f = 0; f < fn; f++)
                {
                    if (editp_get_frame_config(ep, f) == (BYTE)s)
                        editp_set_frame_config(ep, f, 0);
                }
            }
            free_blob_entry(&g_proj_slots[s]);
            if (g_proj_active_count > 0)
                g_proj_active_count--;
        }
    }

    free_blob_entry(&g_global[del_idx]);
    for (i = del_idx; i + 1 < g_global_count; i++)
    {
        g_global[i] = g_global[i + 1];
    }
    ZeroMemory(&g_global[g_global_count - 1], sizeof(ProfileBlobEntry));
    g_global_count--;

    g_active_global = 0;
    save_active_name_to_ini();
    if (g_global[0].blob && g_global[0].blob_size > 0)
    {
        plugin_manager_apply_cfg_blob(g_global[0].blob, g_global[0].blob_size, TRUE);
    }
    return TRUE;
}

BOOL profile_cfg_reset_active(void)
{
    profile_cfg_restore_active();
    plugin_manager_reset_all_filters_to_defaults();
    profile_cfg_save_active();
    return TRUE;
}

void profile_cfg_clear_project_slots(void)
{
    int s;
    profile_cfg_restore_active();
    for (s = 0; s < PROFILE_MAX_COUNT; s++)
    {
        free_blob_entry(&g_proj_slots[s]);
    }
    g_proj_active_count = 0;
}

BOOL profile_cfg_has_project_slot(int slot)
{
    if (slot == 0)
        return TRUE;
    if (slot < 1 || slot >= PROFILE_MAX_COUNT)
        return FALSE;
    return g_proj_slots[slot].name[0] != '\0' &&
           g_proj_slots[slot].blob != NULL &&
           g_proj_slots[slot].blob_size > 0;
}

const char *profile_cfg_get_slot_name(int slot)
{
    if (slot >= 1 && slot < PROFILE_MAX_COUNT && g_proj_slots[slot].name[0] != '\0')
    {
        return g_proj_slots[slot].name;
    }
    return profile_cfg_get_name(g_active_global);
}

BOOL profile_cfg_get_slot_blob(int slot, const uint8_t **out_blob, size_t *out_size)
{
    if (!out_blob || !out_size)
        return FALSE;
    *out_blob = NULL;
    *out_size = 0;

    if (slot == 0)
    {
        profile_cfg_restore_active();
        if (g_slot0_live_blob)
        {
            free(g_slot0_live_blob);
            g_slot0_live_blob = NULL;
            g_slot0_live_size = 0;
        }
        if (!plugin_manager_serialize_cfg_blob(&g_slot0_live_blob, &g_slot0_live_size))
            return FALSE;
        *out_blob = g_slot0_live_blob;
        *out_size = g_slot0_live_size;
        return TRUE;
    }

    if (!profile_cfg_has_project_slot(slot))
        return FALSE;
    *out_blob = g_proj_slots[slot].blob;
    *out_size = g_proj_slots[slot].blob_size;
    return TRUE;
}

BOOL profile_cfg_set_project_slot(int slot, const char *name,
                                  const uint8_t *blob, size_t blob_size)
{
    BOOL was_set;
    if (slot < 1 || slot >= PROFILE_MAX_COUNT || !name || !name[0] || !blob || blob_size == 0)
        return FALSE;

    was_set = (g_proj_slots[slot].name[0] != '\0');
    if (!set_entry_blob(&g_proj_slots[slot], blob, blob_size))
        return FALSE;
    lstrcpynA(g_proj_slots[slot].name, name, sizeof(g_proj_slots[slot].name));
    if (!was_set)
        g_proj_active_count++;
    return TRUE;
}

static int bind_blob_to_project_slot(const char *name, const uint8_t *blob, size_t sz)
{
    int s, free_slot = -1;
    for (s = 1; s < PROFILE_MAX_COUNT; s++)
    {
        if (g_proj_slots[s].name[0] && _stricmp(g_proj_slots[s].name, name) == 0)
        {
            if (!profile_cfg_set_project_slot(s, name, blob, sz))
                return -1;
            return s;
        }
        if (free_slot < 0 && g_proj_slots[s].name[0] == '\0')
        {
            free_slot = s;
        }
    }
    if (free_slot < 1)
        return -1;
    if (!profile_cfg_set_project_slot(free_slot, name, blob, sz))
        return -1;
    return free_slot;
}

int profile_cfg_bind_selection_new_profile(HWND owner, EDITP *ep)
{
    AviUtlDlgState st;
    char raw[260], clean[260];
    uint8_t *blob = NULL;
    size_t sz = 0;
    int s_frm = 0, e_frm = 0, f, slot;

    if (!ep || !editp_is_editing(ep))
        return -1;

    profile_cfg_restore_active();

    aviutl_dlg_init(&st);
    if (aviutl_dlg_run(owner, "STRINGS", &st) != IDOK)
        return -1;
    aviutl_dlg_get_str_sjis(&st, 0, raw, sizeof(raw));
    if (!validate_profile_name(owner, raw, clean, sizeof(clean)))
        return -1;

    if (!plugin_manager_serialize_cfg_blob(&blob, &sz) || !blob || sz == 0)
        return -1;

    upsert_global_profile(clean, blob, sz);
    slot = bind_blob_to_project_slot(clean, blob, sz);
    free(blob);
    if (slot < 1)
        return -1;

    // restore saved base active profile onto slot 0 so unselected frames keep base settings
    if (g_global[g_active_global].blob && g_global[g_active_global].blob_size > 0)
    {
        plugin_manager_apply_cfg_blob(g_global[g_active_global].blob,
                                      g_global[g_active_global].blob_size, TRUE);
    }

    editp_get_select_frame(ep, &s_frm, &e_frm);
    if (s_frm <= e_frm)
    {
        editp_push_undo(ep);
        for (f = s_frm; f <= e_frm; f++)
        {
            editp_set_frame_config(ep, f, (BYTE)slot);
        }
    }
    return slot;
}

int profile_cfg_resolve_or_bind_name(const char *name)
{
    int s, gidx;

    if (!name || !name[0])
        return 0;
    if (_stricmp(name, profile_cfg_get_name(g_active_global)) == 0)
        return 0;

    for (s = 1; s < PROFILE_MAX_COUNT; s++)
    {
        if (g_proj_slots[s].name[0] && _stricmp(g_proj_slots[s].name, name) == 0)
            return s;
    }

    gidx = profile_cfg_find_by_name(name);
    if (gidx == 0)
        return 0;
    if (gidx > 0 && g_global[gidx].blob && g_global[gidx].blob_size > 0)
    {
        return bind_blob_to_project_slot(g_global[gidx].name,
                                         g_global[gidx].blob,
                                         g_global[gidx].blob_size);
    }
    return -1;
}

int profile_cfg_get_applied_slot(void)
{
    return g_applied_slot;
}

void profile_cfg_apply_slot(int slot)
{
    if (slot < 0 || slot >= PROFILE_MAX_COUNT)
        slot = 0;
    if (slot > 0 && !profile_cfg_has_project_slot(slot))
        slot = 0;
    if (slot == g_applied_slot)
        return;

    if (g_applied_slot == 0)
    {
        if (g_slot0_live_blob)
        {
            free(g_slot0_live_blob);
            g_slot0_live_blob = NULL;
            g_slot0_live_size = 0;
        }
        plugin_manager_serialize_cfg_blob(&g_slot0_live_blob, &g_slot0_live_size);
    }

    if (slot == 0)
    {
        if (g_slot0_live_blob && g_slot0_live_size > 0)
        {
            plugin_manager_apply_cfg_blob(g_slot0_live_blob, g_slot0_live_size, FALSE);
        }
        else if (g_global[g_active_global].blob && g_global[g_active_global].blob_size > 0)
        {
            plugin_manager_apply_cfg_blob(g_global[g_active_global].blob,
                                          g_global[g_active_global].blob_size, FALSE);
        }
        g_applied_slot = 0;
    }
    else
    {
        plugin_manager_apply_cfg_blob(g_proj_slots[slot].blob,
                                      g_proj_slots[slot].blob_size, FALSE);
        g_applied_slot = slot;
    }
}

void profile_cfg_apply_for_frame(EDITP *ep, int frame_num)
{
    int slot;
    if (!g_initialized || !ep || g_proj_active_count <= 0)
    {
        if (g_applied_slot != 0)
            profile_cfg_apply_slot(0);
        return;
    }
    slot = (int)editp_get_frame_config(ep, frame_num);
    profile_cfg_apply_slot(slot);
}

void profile_cfg_restore_active(void)
{
    if (!g_initialized || g_applied_slot == 0)
        return;
    profile_cfg_apply_slot(0);
}
