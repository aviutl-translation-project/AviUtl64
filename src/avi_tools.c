#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <vfw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avi_tools.h"
#include "plugin_manager.h"
#include "aup_project.h"
#include "aviutl_dlg.h"
#include "i18n.h"
#include "debug_console.h"

#define IDC_BATCH_DRAG_COMMIT 0x7FCE

typedef struct BatchJob
{
    char aup_path[MAX_PATH];
    char out_path[MAX_PATH];
    char auo_name[128];
    int out_flags;
    int is_temp_aup;
    int has_avi_opts;
    AviSaveOptions avi_opts;
} BatchJob;

static void sjis_to_wide_at(const char *src, wchar_t *dst, int cap)
{
    if (!dst || cap <= 0)
        return;
    dst[0] = L'\0';
    if (!src || !src[0])
        return;
    if (MultiByteToWideChar(932, 0, src, -1, dst, cap) <= 0)
    {
        MultiByteToWideChar(CP_ACP, 0, src, -1, dst, cap);
    }
}

static void wide_to_sjis_at(const wchar_t *src, char *dst, int cap)
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

static const char *path_basename_dbcs_at(const char *path)
{
    const char *base;
    const unsigned char *p;

    if (!path)
        return "";
    base = path;
    for (p = (const unsigned char *)path; *p; p++)
    {
        if (IsDBCSLeadByteEx(932, *p) && p[1])
        {
            p++;
            continue;
        }
        if (*p == '\\' || *p == '/')
            base = (const char *)(p + 1);
    }
    return base;
}

static const char *path_ext_dbcs_at(const char *path)
{
    const char *base = path_basename_dbcs_at(path);
    const char *dot = strrchr(base, '.');
    return dot ? dot : "";
}

static void get_exe_dir_at(char *out_dir, size_t cap)
{
    char mod[MAX_PATH] = {0};
    char *slash;

    if (!out_dir || cap == 0)
        return;
    out_dir[0] = '\0';
    GetModuleFileNameA(NULL, mod, MAX_PATH);
    slash = (char *)path_basename_dbcs_at(mod);
    if (slash > mod)
    {
        size_t len = (size_t)(slash - mod - 1);
        if (len >= cap)
            len = cap - 1;
        memcpy(out_dir, mod, len);
        out_dir[len] = '\0';
    }
    else
    {
        lstrcpynA(out_dir, ".", (int)cap);
    }
}

static void get_ini_path_at(char *out_ini, size_t cap)
{
    char dir[MAX_PATH];
    get_exe_dir_at(dir, sizeof(dir));
    _snprintf(out_ini, cap, "%s\\aviutl.ini", dir);
    out_ini[cap - 1] = '\0';
}

static DWORD get_save_ofn_flags_at(void)
{
    char ini[MAX_PATH];
    DWORD flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    get_ini_path_at(ini, sizeof(ini));
    if (GetPrivateProfileIntA("system", " overwritecheck", 0, ini) == 0 &&
        GetPrivateProfileIntA("system", "nooverwritecheck", 0, ini) == 0)
    {
        flags |= OFN_OVERWRITEPROMPT;
    }
    return flags;
}

static BOOL paths_equal_canon(const char *a, const char *b)
{
    char ca[MAX_PATH], cb[MAX_PATH];
    if (!a || !a[0] || !b || !b[0])
        return FALSE;
    if (GetFullPathNameA(a, MAX_PATH, ca, NULL) == 0)
        lstrcpynA(ca, a, MAX_PATH);
    if (GetFullPathNameA(b, MAX_PATH, cb, NULL) == 0)
        lstrcpynA(cb, b, MAX_PATH);
    return lstrcmpiA(ca, cb) == 0;
}

static void center_dlg_over_owner_at(HWND hdlg, HWND owner)
{
    if (owner && IsWindow(owner))
    {
        RECT rc_d, rc_o;
        int x, y;
        GetWindowRect(hdlg, &rc_d);
        GetWindowRect(owner, &rc_o);
        x = ((rc_d.left - rc_d.right) + rc_o.right + rc_o.left) / 2;
        y = ((rc_d.top - rc_d.bottom) + rc_o.bottom + rc_o.top) / 2;
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        SetWindowPos(hdlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void encode_ini_escaped(const char *src, char *dst, size_t cap)
{
    size_t i = 0, o = 0;
    if (!dst || cap == 0)
        return;
    if (!src)
    {
        dst[0] = '\0';
        return;
    }
    while (src[i] && o + 3 < cap)
    {
        if (src[i] == '\r')
        {
            dst[o++] = '\\';
            dst[o++] = 'r';
        }
        else if (src[i] == '\n')
        {
            dst[o++] = '\\';
            dst[o++] = 'n';
        }
        else if (src[i] == '\\')
        {
            dst[o++] = '\\';
            dst[o++] = '\\';
        }
        else
        {
            dst[o++] = src[i];
        }
        i++;
    }
    dst[o] = '\0';
}

static void decode_ini_escaped(const char *src, char *dst, size_t cap)
{
    size_t i = 0, o = 0;
    if (!dst || cap == 0)
        return;
    if (!src)
    {
        dst[0] = '\0';
        return;
    }
    while (src[i] && o + 1 < cap)
    {
        if (src[i] == '\\' && src[i + 1])
        {
            if (src[i + 1] == 'r')
            {
                dst[o++] = '\r';
                i += 2;
                continue;
            }
            if (src[i + 1] == 'n')
            {
                dst[o++] = '\n';
                i += 2;
                continue;
            }
            if (src[i + 1] == '\\')
            {
                dst[o++] = '\\';
                i += 2;
                continue;
            }
        }
        dst[o++] = src[i++];
    }
    dst[o] = '\0';
}

static int load_batch_queue_ini(BatchJob *jobs, int max_jobs)
{
    char ini[MAX_PATH], key[64], esc[600];
    int count, i, n = 0;

    if (!jobs || max_jobs <= 0)
        return 0;
    get_ini_path_at(ini, sizeof(ini));
    count = GetPrivateProfileIntA("batch", "count", 0, ini);
    if (count < 0)
        count = 0;
    if (count > max_jobs)
        count = max_jobs;

    for (i = 0; i < count; i++)
    {
        BatchJob *jb = &jobs[n];
        ZeroMemory(jb, sizeof(*jb));
        video_export_get_default_avi_options(&jb->avi_opts);

        wsprintfA(key, "job%d.aup", i);
        GetPrivateProfileStringA("batch", key, "", jb->aup_path, MAX_PATH, ini);
        if (!jb->aup_path[0])
            continue;

        wsprintfA(key, "job%d.out", i);
        GetPrivateProfileStringA("batch", key, "", jb->out_path, MAX_PATH, ini);

        wsprintfA(key, "job%d.auo", i);
        GetPrivateProfileStringA("batch", key, "", jb->auo_name, sizeof(jb->auo_name), ini);

        wsprintfA(key, "job%d.flags", i);
        jb->out_flags = GetPrivateProfileIntA("batch", key,
                                              OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO | OUTPUT_INFO_FLAG_BATCH,
                                              ini);

        wsprintfA(key, "job%d.temp", i);
        jb->is_temp_aup = GetPrivateProfileIntA("batch", key, 0, ini);

        wsprintfA(key, "job%d.has_opts", i);
        jb->has_avi_opts = GetPrivateProfileIntA("batch", key, 0, ini);
        if (jb->has_avi_opts)
        {
            wsprintfA(key, "job%d.norecomp_v", i);
            jb->avi_opts.no_recompress_video = GetPrivateProfileIntA("batch", key, 0, ini) != 0;
            wsprintfA(key, "job%d.norecomp_a", i);
            jb->avi_opts.no_recompress_audio = GetPrivateProfileIntA("batch", key, 0, ini) != 0;
            wsprintfA(key, "job%d.no_aud", i);
            jb->avi_opts.no_audio = GetPrivateProfileIntA("batch", key, 0, ini) != 0;
            wsprintfA(key, "job%d.ileave", i);
            jb->avi_opts.interleave_frames = GetPrivateProfileIntA("batch", key, 1, ini);
            if (jb->avi_opts.interleave_frames < 1)
                jb->avi_opts.interleave_frames = 1;
            wsprintfA(key, "job%d.wav_side", i);
            jb->avi_opts.output_wav_sidecar = GetPrivateProfileIntA("batch", key, 0, ini) != 0;
            wsprintfA(key, "job%d.log", i);
            jb->avi_opts.output_log = GetPrivateProfileIntA("batch", key, 0, ini) != 0;

            wsprintfA(key, "job%d.title", i);
            esc[0] = '\0';
            GetPrivateProfileStringA("batch", key, "", esc, sizeof(esc), ini);
            decode_ini_escaped(esc, jb->avi_opts.exinfo.title, sizeof(jb->avi_opts.exinfo.title));

            wsprintfA(key, "job%d.comment", i);
            esc[0] = '\0';
            GetPrivateProfileStringA("batch", key, "", esc, sizeof(esc), ini);
            decode_ini_escaped(esc, jb->avi_opts.exinfo.comment, sizeof(jb->avi_opts.exinfo.comment));

            wsprintfA(key, "job%d.copyright", i);
            esc[0] = '\0';
            GetPrivateProfileStringA("batch", key, "", esc, sizeof(esc), ini);
            decode_ini_escaped(esc, jb->avi_opts.exinfo.copyright, sizeof(jb->avi_opts.exinfo.copyright));
        }
        n++;
    }
    return n;
}

static void save_batch_queue_ini(const BatchJob *jobs, int count)
{
    char ini[MAX_PATH], key[64], val[32], esc[600];
    int i;

    get_ini_path_at(ini, sizeof(ini));
    WritePrivateProfileStringA("batch", NULL, NULL, ini);

    if (count < 0)
        count = 0;
    if (count > BATCH_MAX_ITEMS)
        count = BATCH_MAX_ITEMS;

    wsprintfA(val, "%d", count);
    WritePrivateProfileStringA("batch", "count", val, ini);

    for (i = 0; i < count; i++)
    {
        const BatchJob *jb = &jobs[i];

        wsprintfA(key, "job%d.aup", i);
        WritePrivateProfileStringA("batch", key, jb->aup_path, ini);

        wsprintfA(key, "job%d.out", i);
        WritePrivateProfileStringA("batch", key, jb->out_path, ini);

        wsprintfA(key, "job%d.auo", i);
        WritePrivateProfileStringA("batch", key, jb->auo_name, ini);

        wsprintfA(key, "job%d.flags", i);
        wsprintfA(val, "%d", jb->out_flags);
        WritePrivateProfileStringA("batch", key, val, ini);

        wsprintfA(key, "job%d.temp", i);
        wsprintfA(val, "%d", jb->is_temp_aup ? 1 : 0);
        WritePrivateProfileStringA("batch", key, val, ini);

        wsprintfA(key, "job%d.has_opts", i);
        wsprintfA(val, "%d", jb->has_avi_opts ? 1 : 0);
        WritePrivateProfileStringA("batch", key, val, ini);

        if (jb->has_avi_opts)
        {
            wsprintfA(key, "job%d.norecomp_v", i);
            wsprintfA(val, "%d", jb->avi_opts.no_recompress_video ? 1 : 0);
            WritePrivateProfileStringA("batch", key, val, ini);

            wsprintfA(key, "job%d.norecomp_a", i);
            wsprintfA(val, "%d", jb->avi_opts.no_recompress_audio ? 1 : 0);
            WritePrivateProfileStringA("batch", key, val, ini);

            wsprintfA(key, "job%d.no_aud", i);
            wsprintfA(val, "%d", jb->avi_opts.no_audio ? 1 : 0);
            WritePrivateProfileStringA("batch", key, val, ini);

            wsprintfA(key, "job%d.ileave", i);
            wsprintfA(val, "%d", jb->avi_opts.interleave_frames);
            WritePrivateProfileStringA("batch", key, val, ini);

            wsprintfA(key, "job%d.wav_side", i);
            wsprintfA(val, "%d", jb->avi_opts.output_wav_sidecar ? 1 : 0);
            WritePrivateProfileStringA("batch", key, val, ini);

            wsprintfA(key, "job%d.log", i);
            wsprintfA(val, "%d", jb->avi_opts.output_log ? 1 : 0);
            WritePrivateProfileStringA("batch", key, val, ini);

            wsprintfA(key, "job%d.title", i);
            encode_ini_escaped(jb->avi_opts.exinfo.title, esc, sizeof(esc));
            WritePrivateProfileStringA("batch", key, esc, ini);

            wsprintfA(key, "job%d.comment", i);
            encode_ini_escaped(jb->avi_opts.exinfo.comment, esc, sizeof(esc));
            WritePrivateProfileStringA("batch", key, esc, ini);

            wsprintfA(key, "job%d.copyright", i);
            encode_ini_escaped(jb->avi_opts.exinfo.copyright, esc, sizeof(esc));
            WritePrivateProfileStringA("batch", key, esc, ini);
        }
    }
}

static void show_max_items_warning(HWND owner, UINT str_id, int max_n)
{
    const wchar_t *fmt = i18n_get_wstr(str_id);
    wchar_t msg[512];
    if (fmt && fmt[0])
        wsprintfW(msg, fmt, max_n);
    else
        wsprintfW(msg, L"Maximum count is %d.", max_n);
    aviutl_msgbox_w(owner, msg, L"AviUtl", MB_OK | MB_ICONWARNING);
}

BOOL avi_tools_batch_register_ex(HWND owner, EDITP *ep,
                                 const char *out_path, const char *auo_name,
                                 int out_flags, const AviSaveOptions *opts)
{
    BatchJob jobs[BATCH_MAX_ITEMS];
    char exe_dir[MAX_PATH], snap_path[MAX_PATH], abs_out[MAX_PATH];
    char old_proj[MAX_PATH], old_out[MAX_PATH];
    int count, slot_num = -1, n;
    BatchJob *jb;

    if (!ep || !editp_is_editing(ep) || !out_path || !out_path[0])
        return FALSE;

    count = load_batch_queue_ini(jobs, BATCH_MAX_ITEMS);
    if (count >= BATCH_MAX_ITEMS)
    {
        show_max_items_warning(owner, 1010, BATCH_MAX_ITEMS);
        return FALSE;
    }

    if (GetFullPathNameA(out_path, MAX_PATH, abs_out, NULL) == 0)
        lstrcpynA(abs_out, out_path, MAX_PATH);

    get_exe_dir_at(exe_dir, sizeof(exe_dir));
    for (n = 0; n < 1000; n++)
    {
        int k;
        BOOL used = FALSE;
        _snprintf(snap_path, sizeof(snap_path), "%s\\batch%d.aup", exe_dir, n);
        snap_path[sizeof(snap_path) - 1] = '\0';
        if (GetFileAttributesA(snap_path) != INVALID_FILE_ATTRIBUTES)
            continue;
        for (k = 0; k < count; k++)
        {
            if (paths_equal_canon(jobs[k].aup_path, snap_path))
            {
                used = TRUE;
                break;
            }
        }
        if (!used)
        {
            slot_num = n;
            break;
        }
    }
    if (slot_num < 0)
    {
        _snprintf(snap_path, sizeof(snap_path), "%s\\batch_%lu.aup",
                  exe_dir, (unsigned long)GetTickCount());
        snap_path[sizeof(snap_path) - 1] = '\0';
    }

    lstrcpynA(old_proj, editp_get_project_path(ep) ? editp_get_project_path(ep) : "", MAX_PATH);
    lstrcpynA(old_out, editp_get_output_path(ep) ? editp_get_output_path(ep) : "", MAX_PATH);

    editp_set_output_path(ep, abs_out);
    if (!plugin_manager_save_aup(snap_path, ep))
    {
        editp_set_project_path(ep, old_proj);
        editp_set_output_path(ep, old_out);
        aviutl_msgbox_w(owner, i18n_get_wstr(1002), L"AviUtl", MB_OK | MB_ICONERROR);
        return FALSE;
    }
    editp_set_project_path(ep, old_proj);
    editp_set_output_path(ep, old_out);

    jb = &jobs[count++];
    ZeroMemory(jb, sizeof(*jb));
    lstrcpynA(jb->aup_path, snap_path, MAX_PATH);
    lstrcpynA(jb->out_path, abs_out, MAX_PATH);
    if (auo_name && auo_name[0])
        lstrcpynA(jb->auo_name, auo_name, sizeof(jb->auo_name));
    jb->out_flags = out_flags ? (out_flags | OUTPUT_INFO_FLAG_BATCH)
                              : (OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO | OUTPUT_INFO_FLAG_BATCH);
    jb->is_temp_aup = 1;
    if (opts)
    {
        jb->has_avi_opts = 1;
        jb->avi_opts = *opts;
    }
    else
    {
        video_export_get_default_avi_options(&jb->avi_opts);
    }

    save_batch_queue_ini(jobs, count);
    dbg_info("Registered batch item #%d: '%s' -> '%s'%s%s",
             count, jb->aup_path, jb->out_path,
             jb->auo_name[0] ? " via " : "", jb->auo_name);
    return TRUE;
}

BOOL avi_tools_batch_register_current(HWND owner, EDITP *ep,
                                      const char *out_path, int out_flags,
                                      const AviSaveOptions *opts)
{
    return avi_tools_batch_register_ex(owner, ep, out_path, NULL, out_flags, opts);
}

static void extract_auo_first_pattern(const char *dbl_nul_filter, char *out_desc, size_t desc_cap,
                                      char *out_pat, size_t pat_cap)
{
    const char *p2;
    if (out_desc && desc_cap > 0)
        out_desc[0] = '\0';
    if (out_pat && pat_cap > 0)
        out_pat[0] = '\0';
    if (!dbl_nul_filter || !dbl_nul_filter[0])
        return;
    if (out_desc && desc_cap > 0)
        lstrcpynA(out_desc, dbl_nul_filter, (int)desc_cap);
    p2 = dbl_nul_filter + strlen(dbl_nul_filter) + 1;
    if (*p2 && out_pat && pat_cap > 0)
        lstrcpynA(out_pat, p2, (int)pat_cap);
}

static void infer_output_target_from_path(const char *out_path, char *out_auo_name, size_t auo_cap,
                                          int *io_flags)
{
    const char *ext;
    int i, n;

    if (out_auo_name && auo_cap > 0)
        out_auo_name[0] = '\0';
    if (!out_path || !out_path[0])
        return;

    ext = path_ext_dbcs_at(out_path);
    if (lstrcmpiA(ext, ".wav") == 0)
    {
        if (io_flags)
            *io_flags = OUTPUT_INFO_FLAG_AUDIO | OUTPUT_INFO_FLAG_BATCH;
        return;
    }
    if (lstrcmpiA(ext, ".avi") == 0)
    {
        if (io_flags)
            *io_flags = OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO | OUTPUT_INFO_FLAG_BATCH;
        return;
    }

    n = video_export_get_auo_count();
    for (i = 0; i < n; i++)
    {
        AuoPluginSummary sum;
        char desc[256], pat[128];
        if (!video_export_get_auo_summary(i, &sum))
            continue;
        extract_auo_first_pattern(sum.filefilter, desc, sizeof(desc), pat, sizeof(pat));
        if (ext[0] && pat[0])
        {
            const char *pdot = strchr(pat, '.');
            if (pdot)
            {
                char pext[32];
                size_t k = 0;
                while (pdot[k] && pdot[k] != ';' && pdot[k] != ' ' && k + 1 < sizeof(pext))
                {
                    pext[k] = pdot[k];
                    k++;
                }
                pext[k] = '\0';
                if (lstrcmpiA(ext, pext) == 0)
                {
                    if (out_auo_name && auo_cap > 0)
                        lstrcpynA(out_auo_name, sum.name, (int)auo_cap);
                    if (io_flags)
                        *io_flags = OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO | OUTPUT_INFO_FLAG_BATCH;
                    return;
                }
            }
        }
    }
    if (io_flags)
        *io_flags = OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO | OUTPUT_INFO_FLAG_BATCH;
}

static BOOL peek_aup_output_path(const char *aup_path, char *out_save_path, size_t save_cap)
{
    HANDLE hf;
    unsigned char buf[36 + AUP_UNCOMPRESSED_PREFIX];
    DWORD rd = 0;

    if (out_save_path && save_cap > 0)
        out_save_path[0] = '\0';
    if (!aup_path || !aup_path[0])
        return FALSE;

    hf = CreateFileA(aup_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
        return FALSE;

    ZeroMemory(buf, sizeof(buf));
    if (!ReadFile(hf, buf, sizeof(buf), &rd, NULL) || rd < 36 + 0x108 + 4)
    {
        CloseHandle(hf);
        return FALSE;
    }
    CloseHandle(hf);

    if (memcmp(buf, AUP_MAGIC_018, strlen(AUP_MAGIC_018)) != 0)
        return FALSE;

    if (out_save_path && save_cap > 0)
    {
        const char *embedded_out = (const char *)(buf + 36 + 0x0108);
        lstrcpynA(out_save_path, embedded_out, (int)save_cap);
    }
    return TRUE;
}

static void execute_post_batch_action(HWND owner, int action_idx)
{
    // 0: 129 None, 1: 130 Shutdown, 2: 131 Hibernate, 3: 132 Suspend,
    // 4: 133 Exit AviUtl, 5: 145 Exit & Hibernate, 6: 146 Exit & Suspend
    if (action_idx <= 0)
        return;

    if (action_idx == 4 || action_idx == 5 || action_idx == 6)
    {
        if (owner && IsWindow(owner))
            PostMessageA(owner, WM_CLOSE, 0, 0);
    }

    if (action_idx == 1)
    {
        HMODULE hAdv = LoadLibraryA("advapi32.dll");
        if (hAdv)
        {
            typedef BOOL(WINAPI * OpenProcTokFn)(HANDLE, DWORD, PHANDLE);
            typedef BOOL(WINAPI * LookupPrivFn)(LPCSTR, LPCSTR, PLUID);
            typedef BOOL(WINAPI * AdjTokPrivFn)(HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD, PTOKEN_PRIVILEGES, PDWORD);
            OpenProcTokFn fnOpen = (OpenProcTokFn)(void *)GetProcAddress(hAdv, "OpenProcessToken");
            LookupPrivFn fnLook = (LookupPrivFn)(void *)GetProcAddress(hAdv, "LookupPrivilegeValueA");
            AdjTokPrivFn fnAdj = (AdjTokPrivFn)(void *)GetProcAddress(hAdv, "AdjustTokenPrivileges");
            HANDLE hTok = NULL;
            if (fnOpen && fnLook && fnAdj &&
                fnOpen(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hTok))
            {
                TOKEN_PRIVILEGES tp;
                ZeroMemory(&tp, sizeof(tp));
                if (fnLook(NULL, "SeShutdownPrivilege", &tp.Privileges[0].Luid))
                {
                    tp.PrivilegeCount = 1;
                    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                    fnAdj(hTok, FALSE, &tp, 0, NULL, NULL);
                }
                CloseHandle(hTok);
            }
            FreeLibrary(hAdv);
        }
        ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF, 0);
    }
    else if (action_idx == 2 || action_idx == 5 || action_idx == 3 || action_idx == 6)
    {
        BOOLEAN hibernate = (action_idx == 2 || action_idx == 5) ? TRUE : FALSE;
        HMODULE hPow = LoadLibraryA("powrprof.dll");
        if (hPow)
        {
            typedef BOOLEAN(WINAPI * SetSuspendStateFn)(BOOLEAN, BOOLEAN, BOOLEAN);
            SetSuspendStateFn fnSusp =
                (SetSuspendStateFn)(void *)GetProcAddress(hPow, "SetSuspendState");
            if (fnSusp)
                fnSusp(hibernate, FALSE, FALSE);
            FreeLibrary(hPow);
        }
    }
}

typedef struct BatchDlgCtx
{
    HWND owner;
    EDITP *ep;
    AviToolsUiRefreshFn on_refresh;
    BatchJob jobs[BATCH_MAX_ITEMS];
    int count;
    BOOL running;
    BOOL abort_requested;
    UINT drag_msg;
    HWND drag_icon_hwnd;
    int drag_src_idx;
    int drag_dst_idx;
    POINT lb_origin;
} BatchDlgCtx;

static void refresh_batch_listbox(HWND hdlg, BatchDlgCtx *ctx, int new_sel)
{
    HWND hLb = GetDlgItem(hdlg, 32600);
    HDC hdc;
    HFONT hFont;
    HGDIOBJ hOld = NULL;
    int i, top_idx, max_px = 0;

    if (!hLb || !ctx)
        return;

    top_idx = (int)SendMessageW(hLb, LB_GETTOPINDEX, 0, 0);
    SendMessageW(hLb, LB_RESETCONTENT, 0, 0);

    hdc = GetDC(hLb);
    hFont = (HFONT)SendMessageW(hLb, WM_GETFONT, 0, 0);
    if (hdc && hFont)
        hOld = SelectObject(hdc, hFont);

    for (i = 0; i < ctx->count; i++)
    {
        wchar_t waup[MAX_PATH], wout[MAX_PATH], wline[MAX_PATH * 2 + 64];
        const wchar_t *w_unspec = i18n_get_wstr(135);
        SIZE sz = {0};

        sjis_to_wide_at(path_basename_dbcs_at(ctx->jobs[i].aup_path), waup, MAX_PATH);
        if (ctx->jobs[i].out_path[0])
        {
            sjis_to_wide_at(ctx->jobs[i].out_path, wout, MAX_PATH);
        }
        else
        {
            lstrcpynW(wout, (w_unspec && w_unspec[0]) ? w_unspec : L"[No Output File]", MAX_PATH);
        }

        if (ctx->jobs[i].auo_name[0])
        {
            wchar_t wauo[128];
            sjis_to_wide_at(ctx->jobs[i].auo_name, wauo, 128);
            _snwprintf(wline, sizeof(wline) / sizeof(wline[0]), L"%s -> %s [%s]",
                       waup, wout, wauo);
        }
        else
        {
            _snwprintf(wline, sizeof(wline) / sizeof(wline[0]), L"%s -> %s", waup, wout);
        }
        wline[(sizeof(wline) / sizeof(wline[0])) - 1] = L'\0';

        SendMessageW(hLb, LB_ADDSTRING, 0, (LPARAM)wline);
        if (hdc && GetTextExtentPoint32W(hdc, wline, lstrlenW(wline), &sz))
        {
            if (sz.cx + 16 > max_px)
                max_px = sz.cx + 16;
        }
    }

    if (hdc)
    {
        if (hOld)
            SelectObject(hdc, hOld);
        ReleaseDC(hLb, hdc);
    }

    SendMessageW(hLb, LB_SETHORIZONTALEXTENT, (WPARAM)max_px, 0);
    if (top_idx >= 0)
        SendMessageW(hLb, LB_SETTOPINDEX, (WPARAM)top_idx, 0);
    if (ctx->count > 0)
    {
        if (new_sel < 0)
            new_sel = 0;
        if (new_sel >= ctx->count)
            new_sel = ctx->count - 1;
        SendMessageW(hLb, LB_SETCURSEL, (WPARAM)new_sel, 0);
    }
}

static void update_batch_buttons(HWND hdlg, BatchDlgCtx *ctx)
{
    BOOL idle = !ctx->running;
    BOOL has_items = (ctx->count > 0);
    int sel = (int)SendDlgItemMessageW(hdlg, 32600, LB_GETCURSEL, 0, 0);
    BOOL has_sel = (sel >= 0 && sel < ctx->count);

    EnableWindow(GetDlgItem(hdlg, 32701), idle && has_sel && sel > 0);
    EnableWindow(GetDlgItem(hdlg, 32702), idle && has_sel && sel + 1 < ctx->count);
    EnableWindow(GetDlgItem(hdlg, 32709), idle);
    EnableWindow(GetDlgItem(hdlg, 32704), idle && has_sel);
    EnableWindow(GetDlgItem(hdlg, 32700), idle && has_items);
    EnableWindow(GetDlgItem(hdlg, 32300), idle && has_sel);
    EnableWindow(GetDlgItem(hdlg, 32710), idle && has_items);
    EnableWindow(GetDlgItem(hdlg, 32711), ctx->running);
    EnableWindow(GetDlgItem(hdlg, 32713), idle);
}

static void move_batch_job(BatchDlgCtx *ctx, int src, int dst)
{
    BatchJob tmp;
    int k;
    if (!ctx || src < 0 || src >= ctx->count || dst < 0 || dst >= ctx->count || src == dst)
        return;
    tmp = ctx->jobs[src];
    if (src < dst)
    {
        for (k = src; k < dst; k++)
            ctx->jobs[k] = ctx->jobs[k + 1];
    }
    else
    {
        for (k = src; k > dst; k--)
            ctx->jobs[k] = ctx->jobs[k - 1];
    }
    ctx->jobs[dst] = tmp;
    save_batch_queue_ini(ctx->jobs, ctx->count);
}

static BOOL append_aup_to_batch(HWND hdlg, BatchDlgCtx *ctx, const char *aup_file)
{
    char abs_aup[MAX_PATH], out_file[MAX_PATH] = {0};
    BatchJob *jb;

    if (!ctx || !aup_file || !aup_file[0])
        return FALSE;
    if (ctx->count >= BATCH_MAX_ITEMS)
    {
        show_max_items_warning(hdlg, 1010, BATCH_MAX_ITEMS);
        return FALSE;
    }
    if (GetFullPathNameA(aup_file, MAX_PATH, abs_aup, NULL) == 0)
        lstrcpynA(abs_aup, aup_file, MAX_PATH);

    if (!peek_aup_output_path(abs_aup, out_file, sizeof(out_file)))
    {
        aviutl_msgbox_w(hdlg, i18n_get_wstr(1003), L"AviUtl", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    jb = &ctx->jobs[ctx->count++];
    ZeroMemory(jb, sizeof(*jb));
    lstrcpynA(jb->aup_path, abs_aup, MAX_PATH);
    lstrcpynA(jb->out_path, out_file, MAX_PATH);
    jb->out_flags = OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO | OUTPUT_INFO_FLAG_BATCH;
    infer_output_target_from_path(jb->out_path, jb->auo_name, sizeof(jb->auo_name), &jb->out_flags);
    jb->is_temp_aup = 0;
    video_export_get_default_avi_options(&jb->avi_opts);
    return TRUE;
}

static void batch_change_job_output(HWND hdlg, BatchDlgCtx *ctx, int idx)
{
    char filter_buf[8192];
    size_t pos = 0;
    OPENFILENAMEA ofn;
    char file[MAX_PATH] = {0};
    int auo_n, i, cur_filter = 1;
    BatchJob *jb;

    if (!ctx || idx < 0 || idx >= ctx->count)
        return;
    jb = &ctx->jobs[idx];

    // 1: AVI, 2: WAV, 3..(2+auo_n): AUO plugins, last: All Files
    {
        static const char f1[] = "AVI File (*.avi)\0*.avi\0Wave File (*.wav)\0*.wav\0";
        memcpy(filter_buf, f1, sizeof(f1) - 1);
        pos = sizeof(f1) - 1;
    }

    if (lstrcmpiA(path_ext_dbcs_at(jb->out_path), ".wav") == 0 && !jb->auo_name[0])
        cur_filter = 2;

    auo_n = video_export_get_auo_count();
    for (i = 0; i < auo_n; i++)
    {
        AuoPluginSummary sum;
        char desc[256], pat[128], label[320];
        size_t l1, l2;
        if (!video_export_get_auo_summary(i, &sum))
            continue;
        extract_auo_first_pattern(sum.filefilter, desc, sizeof(desc), pat, sizeof(pat));
        if (!pat[0])
            lstrcpyA(pat, "*.*");
        _snprintf(label, sizeof(label), "%s (%s)", sum.name, pat);
        label[sizeof(label) - 1] = '\0';
        l1 = strlen(label) + 1;
        l2 = strlen(pat) + 1;
        if (pos + l1 + l2 + 32 < sizeof(filter_buf))
        {
            memcpy(filter_buf + pos, label, l1);
            pos += l1;
            memcpy(filter_buf + pos, pat, l2);
            pos += l2;
            if (jb->auo_name[0] && lstrcmpA(jb->auo_name, sum.name) == 0)
                cur_filter = 3 + i;
        }
    }
    {
        static const char fall[] = "All Files (*.*)\0*.*\0";
        memcpy(filter_buf + pos, fall, sizeof(fall));
        pos += sizeof(fall);
    }

    lstrcpynA(file, jb->out_path, sizeof(file));
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hdlg;
    ofn.lpstrFilter = filter_buf;
    ofn.nFilterIndex = (DWORD)cur_filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrDefExt = "avi";
    ofn.Flags = get_save_ofn_flags_at();

    if (!GetSaveFileNameA(&ofn))
        return;

    lstrcpynA(jb->out_path, file, MAX_PATH);
    if (ofn.nFilterIndex == 1)
    {
        jb->auo_name[0] = '\0';
        jb->out_flags = OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO | OUTPUT_INFO_FLAG_BATCH;
    }
    else if (ofn.nFilterIndex == 2)
    {
        jb->auo_name[0] = '\0';
        jb->out_flags = OUTPUT_INFO_FLAG_AUDIO | OUTPUT_INFO_FLAG_BATCH;
    }
    else if ((int)ofn.nFilterIndex >= 3 && (int)ofn.nFilterIndex < 3 + auo_n)
    {
        const char *aname = video_export_get_auo_name((int)ofn.nFilterIndex - 3);
        lstrcpynA(jb->auo_name, aname ? aname : "", sizeof(jb->auo_name));
        jb->out_flags = OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO | OUTPUT_INFO_FLAG_BATCH;
    }
    else
    {
        infer_output_target_from_path(jb->out_path, jb->auo_name, sizeof(jb->auo_name), &jb->out_flags);
    }

    save_batch_queue_ini(ctx->jobs, ctx->count);
    refresh_batch_listbox(hdlg, ctx, idx);
    update_batch_buttons(hdlg, ctx);
}

static void run_batch_queue_now(HWND hdlg, BatchDlgCtx *ctx)
{
    char exe_dir[MAX_PATH], restore_aup[MAX_PATH];
    char prev_proj[MAX_PATH] = {0}, prev_out[MAX_PATH] = {0};
    BOOL had_open_session = FALSE, saved_restore = FALSE;
    BOOL auto_del, any_failed = FALSE;
    int i, ran_count = 0, post_action = 0;

    if (!ctx || !ctx->ep || ctx->running || ctx->count <= 0)
        return;

    ctx->running = TRUE;
    ctx->abort_requested = FALSE;
    update_batch_buttons(hdlg, ctx);

    get_exe_dir_at(exe_dir, sizeof(exe_dir));
    _snprintf(restore_aup, sizeof(restore_aup), "%s\\batch_restore_session.aup", exe_dir);
    restore_aup[sizeof(restore_aup) - 1] = '\0';

    if (editp_is_editing(ctx->ep))
    {
        had_open_session = TRUE;
        lstrcpynA(prev_proj, editp_get_project_path(ctx->ep) ? editp_get_project_path(ctx->ep) : "", MAX_PATH);
        lstrcpynA(prev_out, editp_get_output_path(ctx->ep) ? editp_get_output_path(ctx->ep) : "", MAX_PATH);
        saved_restore = plugin_manager_save_aup(restore_aup, ctx->ep);
    }

    i = 0;
    while (i < ctx->count && !ctx->abort_requested)
    {
        BatchJob job = ctx->jobs[i];
        BOOL job_ok = FALSE;

        SendDlgItemMessageW(hdlg, 32600, LB_SETCURSEL, (WPARAM)i, 0);

        if (!job.out_path[0])
        {
            any_failed = TRUE;
            i++;
            continue;
        }

        {
            const wchar_t *fmt_out = i18n_get_wstr(1042);
            wchar_t wname[MAX_PATH], wstatus[MAX_PATH + 128];
            sjis_to_wide_at(path_basename_dbcs_at(job.out_path), wname, MAX_PATH);
            if (fmt_out && fmt_out[0])
                wsprintfW(wstatus, fmt_out, wname);
            else
                wsprintfW(wstatus, L"Outputting \"%s\"...", wname);
            SetDlgItemTextW(hdlg, 32400, wstatus);
        }

        if (!plugin_manager_load_aup(job.aup_path, ctx->ep))
        {
            dbg_error("Batch item %d failed to load project '%s'", i, job.aup_path);
            any_failed = TRUE;
            i++;
            continue;
        }
        if (ctx->on_refresh && ctx->owner)
            ctx->on_refresh(ctx->owner);

        ran_count++;
        if (job.auo_name[0])
        {
            int auo_idx = video_export_find_auo_by_name(job.auo_name);
            if (auo_idx >= 0)
            {
                job_ok = video_export_auo_file(ctx->owner, ctx->ep, auo_idx, job.out_path,
                                               job.out_flags | OUTPUT_INFO_FLAG_BATCH);
            }
            else
            {
                dbg_error("Batch item %d: output plugin '%s' not found", i, job.auo_name);
                job_ok = FALSE;
            }
        }
        else if (lstrcmpiA(path_ext_dbcs_at(job.out_path), ".wav") == 0 ||
                 (job.out_flags & (OUTPUT_INFO_FLAG_VIDEO | OUTPUT_INFO_FLAG_AUDIO)) == OUTPUT_INFO_FLAG_AUDIO)
        {
            job_ok = video_export_wav_file(ctx->owner, ctx->ep, job.out_path);
        }
        else
        {
            job_ok = video_export_avi_file_with_options(
                ctx->owner, ctx->ep, job.out_path,
                job.out_flags | OUTPUT_INFO_FLAG_BATCH,
                job.has_avi_opts ? &job.avi_opts : NULL);
        }

        if (ctx->abort_requested)
            break;

        auto_del = (IsDlgButtonChecked(hdlg, 32200) == BST_CHECKED);
        if (job_ok)
        {
            if (auto_del)
            {
                int k;
                if (ctx->jobs[i].is_temp_aup && ctx->jobs[i].aup_path[0])
                    DeleteFileA(ctx->jobs[i].aup_path);
                for (k = i; k + 1 < ctx->count; k++)
                    ctx->jobs[k] = ctx->jobs[k + 1];
                ctx->count--;
                save_batch_queue_ini(ctx->jobs, ctx->count);
                refresh_batch_listbox(hdlg, ctx, i);
                continue;
            }
        }
        else
        {
            any_failed = TRUE;
        }
        i++;
    }

    if (had_open_session && saved_restore)
    {
        plugin_manager_load_aup(restore_aup, ctx->ep);
        editp_set_project_path(ctx->ep, prev_proj);
        editp_set_output_path(ctx->ep, prev_out);
        DeleteFileA(restore_aup);
    }
    else
    {
        if (saved_restore)
            DeleteFileA(restore_aup);
        plugin_manager_notify_file_close(ctx->ep);
        editp_close_video(ctx->ep);
    }
    if (ctx->on_refresh && ctx->owner)
        ctx->on_refresh(ctx->owner);

    ctx->running = FALSE;
    update_batch_buttons(hdlg, ctx);

    if (ctx->abort_requested)
    {
        SetDlgItemTextW(hdlg, 32400, i18n_get_wstr(1040));
        return;
    }

    SetDlgItemTextW(hdlg, 32400, i18n_get_wstr(any_failed ? 1036 : 1041));

    post_action = (int)SendDlgItemMessageW(hdlg, 32500, CB_GETCURSEL, 0, 0);
    if (ran_count > 0 && post_action > 0)
    {
        HWND owner = ctx->owner;
        if (post_action == 4 || post_action == 5 || post_action == 6)
            EndDialog(hdlg, IDOK);
        execute_post_batch_action(owner, post_action);
    }
}

static BOOL handle_batch_drag_msg(HWND hdlg, BatchDlgCtx *ctx, DRAGLISTINFO *dli)
{
    if (!ctx || !dli || ctx->running)
        return FALSE;

    switch (dli->uNotification)
    {
    case DL_BEGINDRAG:
    {
        RECT rc_lb;
        GetWindowRect(dli->hWnd, &rc_lb);
        ctx->lb_origin.x = rc_lb.left;
        ctx->lb_origin.y = rc_lb.top;
        ScreenToClient(hdlg, &ctx->lb_origin);
        ctx->drag_src_idx = LBItemFromPt(dli->hWnd, dli->ptCursor, TRUE);
        SetWindowLongPtrW(hdlg, DWLP_MSGRESULT, TRUE);
        return TRUE;
    }
    case DL_DRAGGING:
    {
        int item = LBItemFromPt(dli->hWnd, dli->ptCursor, TRUE);
        ctx->drag_dst_idx = item;
        if (ctx->drag_icon_hwnd)
        {
            if (item >= 0)
            {
                RECT rc_item;
                if (SendMessageW(dli->hWnd, LB_GETITEMRECT, (WPARAM)item, (LPARAM)&rc_item) != LB_ERR)
                {
                    SetWindowPos(ctx->drag_icon_hwnd, HWND_TOP,
                                 ctx->lb_origin.x - 10, ctx->lb_origin.y + rc_item.top - 6,
                                 16, 16, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                }
            }
            else
            {
                ShowWindow(ctx->drag_icon_hwnd, SW_HIDE);
            }
        }
        if (item != -1)
        {
            SetCursor(LoadCursorA(NULL, IDC_ARROW));
            SetWindowLongPtrW(hdlg, DWLP_MSGRESULT, 0);
            return TRUE;
        }
        SetWindowLongPtrW(hdlg, DWLP_MSGRESULT, DL_STOPCURSOR);
        return TRUE;
    }
    case DL_DROPPED:
    {
        int item = LBItemFromPt(dli->hWnd, dli->ptCursor, TRUE);
        if (ctx->drag_icon_hwnd)
            ShowWindow(ctx->drag_icon_hwnd, SW_HIDE);
        if (item != -1 && ctx->drag_src_idx != -1 && item != ctx->drag_src_idx)
        {
            ctx->drag_dst_idx = item;
            PostMessageW(hdlg, WM_COMMAND, IDC_BATCH_DRAG_COMMIT, 0);
        }
        SetWindowLongPtrW(hdlg, DWLP_MSGRESULT, 0);
        return TRUE;
    }
    case DL_CANCELDRAG:
        if (ctx->drag_icon_hwnd)
            ShowWindow(ctx->drag_icon_hwnd, SW_HIDE);
        SetWindowLongPtrW(hdlg, DWLP_MSGRESULT, 0);
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK BatchDlgProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    BatchDlgCtx *ctx = (BatchDlgCtx *)GetWindowLongPtrW(hdlg, GWLP_USERDATA);

    if (ctx && ctx->drag_msg != 0 && msg == ctx->drag_msg && lp != 0)
    {
        return handle_batch_drag_msg(hdlg, ctx, (DRAGLISTINFO *)lp);
    }

    if (msg == WM_INITDIALOG)
    {
        static const UINT action_ids[7] = {129, 130, 131, 132, 133, 145, 146};
        char ini[MAX_PATH];
        int i, batchdel;
        HWND hLb;

        ctx = (BatchDlgCtx *)lp;
        SetWindowLongPtrW(hdlg, GWLP_USERDATA, (LONG_PTR)ctx);
        center_dlg_over_owner_at(hdlg, ctx->owner);
        DragAcceptFiles(hdlg, TRUE);

        ctx->drag_icon_hwnd = GetDlgItem(hdlg, 32715);
        if (ctx->drag_icon_hwnd)
        {
            HICON hDrag = (HICON)LoadImageA(GetModuleHandleA(NULL), "ICON_DRAG",
                                            IMAGE_ICON, 16, 16, 0);
            if (hDrag)
                SendMessageW(ctx->drag_icon_hwnd, STM_SETICON, (WPARAM)hDrag, 0);
            ShowWindow(ctx->drag_icon_hwnd, SW_HIDE);
        }

        hLb = GetDlgItem(hdlg, 32600);
        if (hLb)
            MakeDragList(hLb);

        for (i = 0; i < 7; i++)
        {
            SendDlgItemMessageW(hdlg, 32500, CB_ADDSTRING, 0,
                                (LPARAM)i18n_get_wstr(action_ids[i]));
        }
        SendDlgItemMessageW(hdlg, 32500, CB_SETCURSEL, 0, 0);

        get_ini_path_at(ini, sizeof(ini));
        batchdel = GetPrivateProfileIntA("system", "batchdel", 1, ini);
        CheckDlgButton(hdlg, 32200, batchdel ? BST_CHECKED : BST_UNCHECKED);

        SetDlgItemTextW(hdlg, 32400, i18n_get_wstr(1040));

        ctx->count = load_batch_queue_ini(ctx->jobs, BATCH_MAX_ITEMS);
        refresh_batch_listbox(hdlg, ctx, 0);
        update_batch_buttons(hdlg, ctx);
        return TRUE;
    }

    if (msg == WM_DROPFILES && ctx && !ctx->running)
    {
        HDROP hDrop = (HDROP)wp;
        UINT nfiles = DragQueryFileA(hDrop, 0xFFFFFFFFu, NULL, 0);
        UINT i;
        BOOL added = FALSE;
        for (i = 0; i < nfiles; i++)
        {
            char fpath[MAX_PATH] = {0};
            if (DragQueryFileA(hDrop, i, fpath, MAX_PATH) > 0)
            {
                if (append_aup_to_batch(hdlg, ctx, fpath))
                    added = TRUE;
                else if (ctx->count >= BATCH_MAX_ITEMS)
                    break;
            }
        }
        DragFinish(hDrop);
        if (added)
        {
            save_batch_queue_ini(ctx->jobs, ctx->count);
            refresh_batch_listbox(hdlg, ctx, ctx->count - 1);
            update_batch_buttons(hdlg, ctx);
        }
        return TRUE;
    }

    if (msg == WM_CLOSE && ctx)
    {
        if (ctx->running)
        {
            ctx->abort_requested = TRUE;
            video_export_request_abort();
            return TRUE;
        }
        EndDialog(hdlg, IDCANCEL);
        return TRUE;
    }

    if (msg == WM_COMMAND && ctx)
    {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        if (id == 32600)
        {
            if (code == LBN_SELCHANGE)
                update_batch_buttons(hdlg, ctx);
            else if (code == LBN_DBLCLK && !ctx->running)
            {
                int sel = (int)SendDlgItemMessageW(hdlg, 32600, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < ctx->count)
                    batch_change_job_output(hdlg, ctx, sel);
            }
            return TRUE;
        }

        if (id == IDC_BATCH_DRAG_COMMIT && !ctx->running)
        {
            move_batch_job(ctx, ctx->drag_src_idx, ctx->drag_dst_idx);
            refresh_batch_listbox(hdlg, ctx, ctx->drag_dst_idx);
            update_batch_buttons(hdlg, ctx);
            return TRUE;
        }

        if (id == 32200 && code == BN_CLICKED)
        {
            char ini[MAX_PATH];
            get_ini_path_at(ini, sizeof(ini));
            WritePrivateProfileStringA("system", "batchdel",
                                       (IsDlgButtonChecked(hdlg, 32200) == BST_CHECKED) ? "1" : "0",
                                       ini);
            return TRUE;
        }

        if (id == 32701 && code == BN_CLICKED && !ctx->running) // move Up
        {
            int sel = (int)SendDlgItemMessageW(hdlg, 32600, LB_GETCURSEL, 0, 0);
            if (sel > 0 && sel < ctx->count)
            {
                move_batch_job(ctx, sel, sel - 1);
                refresh_batch_listbox(hdlg, ctx, sel - 1);
                update_batch_buttons(hdlg, ctx);
            }
            return TRUE;
        }

        if (id == 32702 && code == BN_CLICKED && !ctx->running) // move Down
        {
            int sel = (int)SendDlgItemMessageW(hdlg, 32600, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel + 1 < ctx->count)
            {
                move_batch_job(ctx, sel, sel + 1);
                refresh_batch_listbox(hdlg, ctx, sel + 1);
                update_batch_buttons(hdlg, ctx);
            }
            return TRUE;
        }

        if (id == 32709 && code == BN_CLICKED && !ctx->running) // add .aup
        {
            char *fbuf = (char *)calloc(1, 32768);
            if (fbuf)
            {
                OPENFILENAMEA ofn;
                static const char flt[] =
                    "AviUtl ProjectFile (*.aup)\0*.aup\0All Files (*.*)\0*.*\0";
                ZeroMemory(&ofn, sizeof(ofn));
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hdlg;
                ofn.lpstrFilter = flt;
                ofn.nFilterIndex = 1;
                ofn.lpstrFile = fbuf;
                ofn.nMaxFile = 32768;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT |
                            OFN_EXPLORER | OFN_NOCHANGEDIR;
                if (GetOpenFileNameA(&ofn))
                {
                    const char *p = fbuf + strlen(fbuf) + 1;
                    BOOL added = FALSE;
                    if (*p == '\0')
                    {
                        added = append_aup_to_batch(hdlg, ctx, fbuf);
                    }
                    else
                    {
                        while (*p)
                        {
                            char full[MAX_PATH];
                            _snprintf(full, sizeof(full), "%s\\%s", fbuf, p);
                            full[sizeof(full) - 1] = '\0';
                            if (append_aup_to_batch(hdlg, ctx, full))
                                added = TRUE;
                            else if (ctx->count >= BATCH_MAX_ITEMS)
                                break;
                            p += strlen(p) + 1;
                        }
                    }
                    if (added)
                    {
                        save_batch_queue_ini(ctx->jobs, ctx->count);
                        refresh_batch_listbox(hdlg, ctx, ctx->count - 1);
                        update_batch_buttons(hdlg, ctx);
                    }
                }
                free(fbuf);
            }
            return TRUE;
        }

        if (id == 32704 && code == BN_CLICKED && !ctx->running) // delete
        {
            int sel = (int)SendDlgItemMessageW(hdlg, 32600, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < ctx->count)
            {
                int k;
                if (ctx->jobs[sel].is_temp_aup && ctx->jobs[sel].aup_path[0])
                    DeleteFileA(ctx->jobs[sel].aup_path);
                for (k = sel; k + 1 < ctx->count; k++)
                    ctx->jobs[k] = ctx->jobs[k + 1];
                ctx->count--;
                save_batch_queue_ini(ctx->jobs, ctx->count);
                refresh_batch_listbox(hdlg, ctx, sel);
                update_batch_buttons(hdlg, ctx);
            }
            return TRUE;
        }

        if (id == 32700 && code == BN_CLICKED && !ctx->running) // delete All
        {
            if (ctx->count > 0 &&
                aviutl_msgbox_w(hdlg, i18n_get_wstr(1037), L"AviUtl",
                                MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                int k;
                for (k = 0; k < ctx->count; k++)
                {
                    if (ctx->jobs[k].is_temp_aup && ctx->jobs[k].aup_path[0])
                        DeleteFileA(ctx->jobs[k].aup_path);
                }
                ctx->count = 0;
                save_batch_queue_ini(ctx->jobs, 0);
                refresh_batch_listbox(hdlg, ctx, 0);
                update_batch_buttons(hdlg, ctx);
            }
            return TRUE;
        }

        if (id == 32300 && code == BN_CLICKED && !ctx->running) // change Output
        {
            int sel = (int)SendDlgItemMessageW(hdlg, 32600, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < ctx->count)
                batch_change_job_output(hdlg, ctx, sel);
            return TRUE;
        }

        if (id == 32710 && code == BN_CLICKED && !ctx->running) // start
        {
            run_batch_queue_now(hdlg, ctx);
            return TRUE;
        }

        if (id == 32711 && code == BN_CLICKED) // stop
        {
            if (ctx->running)
            {
                ctx->abort_requested = TRUE;
                video_export_request_abort();
            }
            return TRUE;
        }

        if ((id == 32713 || id == IDCANCEL) && !ctx->running) // close
        {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

void avi_tools_run_batch_dialog(HWND owner, EDITP *ep, AviToolsUiRefreshFn on_refresh)
{
    const DLGTEMPLATE *tmpl = i18n_load_dialog_template("BATCH");
    BatchDlgCtx *ctx;

    if (!tmpl || !ep)
        return;

    ctx = (BatchDlgCtx *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return;

    ctx->owner = owner;
    ctx->ep = ep;
    ctx->on_refresh = on_refresh;
    ctx->drag_src_idx = -1;
    ctx->drag_dst_idx = -1;
    ctx->drag_msg = RegisterWindowMessageA(DRAGLISTMSGSTRING);

    DialogBoxIndirectParamW(GetModuleHandleA(NULL), tmpl, owner, BatchDlgProc, (LPARAM)ctx);
    free(ctx);
}

// lossless vfw stream helpers for AVICHAIN (1057) and AVIMIX (1058)

static BOOL copy_vfw_video_packets(PAVISTREAM pvid_in, PAVISTREAM pvid_out, LONG *io_out_pos)
{
    LONG s_start, s_len, s;
    unsigned char *pkt_buf = NULL;
    LONG pkt_cap = 0;
    BOOL ok = TRUE;

    s_start = AVIStreamStart(pvid_in);
    s_len = AVIStreamLength(pvid_in);
    if (s_len <= 0)
        return TRUE;

    for (s = s_start; s < s_start + s_len; s++)
    {
        LONG cb = 0;
        if (AVIStreamRead(pvid_in, s, 1, NULL, 0, &cb, NULL) != 0)
        {
            ok = FALSE;
            break;
        }
        if (cb <= 0)
        {
            if (AVIStreamWrite(pvid_out, (*io_out_pos)++, 1, NULL, 0, 0, NULL, NULL) != 0)
            {
                ok = FALSE;
                break;
            }
        }
        else
        {
            BOOL is_key;
            if (cb > pkt_cap)
            {
                LONG ncap = cb + 4096;
                unsigned char *nb = (unsigned char *)realloc(pkt_buf, (size_t)ncap);
                if (!nb)
                {
                    ok = FALSE;
                    break;
                }
                pkt_buf = nb;
                pkt_cap = ncap;
            }
            if (AVIStreamRead(pvid_in, s, 1, pkt_buf, cb, &cb, NULL) != 0)
            {
                ok = FALSE;
                break;
            }
            is_key = (AVIStreamFindSample(pvid_in, s, FIND_KEY | FIND_PREV) == s);
            if (AVIStreamWrite(pvid_out, (*io_out_pos)++, 1, pkt_buf, cb,
                               is_key ? AVIIF_KEYFRAME : 0, NULL, NULL) != 0)
            {
                ok = FALSE;
                break;
            }
        }
    }

    free(pkt_buf);
    return ok;
}

static BOOL copy_vfw_audio_packets(PAVISTREAM paud_in, PAVISTREAM paud_out, LONG *io_out_pos)
{
    LONG a_start, a_len, pos;
    LONG cap = 65536;
    unsigned char *abuf = (unsigned char *)malloc((size_t)cap);
    BOOL ok = TRUE;

    if (!abuf)
        return FALSE;

    a_start = AVIStreamStart(paud_in);
    a_len = AVIStreamLength(paud_in);
    pos = a_start;

    while (pos < a_start + a_len)
    {
        LONG rem = (a_start + a_len) - pos;
        LONG req = rem < 4096 ? rem : 4096;
        LONG bytes_needed = 0, bytes_read = 0, smp_read = 0;

        if (AVIStreamRead(paud_in, pos, req, NULL, 0, &bytes_needed, NULL) == 0 &&
            bytes_needed > cap)
        {
            LONG ncap = bytes_needed + 4096;
            unsigned char *nb = (unsigned char *)realloc(abuf, (size_t)ncap);
            if (!nb)
            {
                ok = FALSE;
                break;
            }
            abuf = nb;
            cap = ncap;
        }

        if (AVIStreamRead(paud_in, pos, req, abuf, cap, &bytes_read, &smp_read) != 0 ||
            smp_read <= 0)
        {
            if (AVIStreamRead(paud_in, pos, 1, abuf, cap, &bytes_read, &smp_read) != 0 ||
                smp_read <= 0)
            {
                break;
            }
        }

        if (AVIStreamWrite(paud_out, *io_out_pos, smp_read, abuf, bytes_read,
                           AVIIF_KEYFRAME, NULL, NULL) != 0)
        {
            ok = FALSE;
            break;
        }
        *io_out_pos += smp_read;
        pos += smp_read;
    }

    free(abuf);
    return ok;
}

static BOOL read_riff_wav_fallback(const char *wav_path, unsigned char **out_fmt, LONG *out_fmt_sz,
                                   unsigned char **out_pcm, DWORD *out_pcm_sz)
{
    HANDLE hf;
    DWORD rd = 0;
    unsigned char hdr[12];
    unsigned char *fmt_buf = NULL, *data_buf = NULL;
    LONG fmt_sz = 0;
    DWORD data_sz = 0;

    *out_fmt = NULL;
    *out_fmt_sz = 0;
    *out_pcm = NULL;
    *out_pcm_sz = 0;

    hf = CreateFileA(wav_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
        return FALSE;

    if (!ReadFile(hf, hdr, 12, &rd, NULL) || rd != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
    {
        CloseHandle(hf);
        return FALSE;
    }

    for (;;)
    {
        unsigned char chdr[8];
        DWORD csz;
        if (!ReadFile(hf, chdr, 8, &rd, NULL) || rd != 8)
            break;
        memcpy(&csz, chdr + 4, 4);
        if (memcmp(chdr, "fmt ", 4) == 0 && csz >= 16 && csz <= 4096)
        {
            free(fmt_buf);
            fmt_buf = (unsigned char *)calloc(1, csz < sizeof(WAVEFORMATEX) ? sizeof(WAVEFORMATEX) : csz);
            if (!fmt_buf || !ReadFile(hf, fmt_buf, csz, &rd, NULL) || rd != csz)
                break;
            fmt_sz = (LONG)(csz < sizeof(PCMWAVEFORMAT) ? sizeof(PCMWAVEFORMAT) : csz);
        }
        else if (memcmp(chdr, "data", 4) == 0 && csz > 0)
        {
            free(data_buf);
            data_buf = (unsigned char *)malloc(csz);
            if (!data_buf || !ReadFile(hf, data_buf, csz, &rd, NULL) || rd == 0)
                break;
            data_sz = rd;
            break;
        }
        else
        {
            SetFilePointer(hf, (LONG)csz, NULL, FILE_CURRENT);
        }
        if (csz & 1u)
            SetFilePointer(hf, 1, NULL, FILE_CURRENT);
    }

    CloseHandle(hf);
    if (!fmt_buf || !data_buf || data_sz == 0)
    {
        free(fmt_buf);
        free(data_buf);
        return FALSE;
    }

    *out_fmt = fmt_buf;
    *out_fmt_sz = fmt_sz;
    *out_pcm = data_buf;
    *out_pcm_sz = data_sz;
    return TRUE;
}

static BOOL avichain_concat_files(HWND hdlg, char files[][MAX_PATH], int count,
                                  const char *out_path, const AviExInfoTags *tags)
{
    PAVIFILE pf0 = NULL, pout = NULL;
    PAVISTREAM pvid0 = NULL, paud0 = NULL, pvid_out = NULL, paud_out = NULL;
    AVISTREAMINFOA vsi0, asi0;
    unsigned char *vfmt0 = NULL, *afmt0 = NULL;
    LONG vfmt0_sz = 0, afmt0_sz = 0;
    LONG out_vpos = 0, out_apos = 0;
    int i;
    BOOL has_aud = FALSE, ok = FALSE;

    if (count <= 0 || !out_path || !out_path[0])
        return FALSE;

    AVIFileInit();

    if (AVIFileOpenA(&pf0, files[0], OF_READ | OF_SHARE_DENY_WRITE, NULL) != 0 || !pf0 ||
        AVIFileGetStream(pf0, &pvid0, streamtypeVIDEO, 0) != 0 || !pvid0)
    {
        aviutl_msgbox_w(hdlg, i18n_get_wstr(1038), L"AviUtl", MB_OK | MB_ICONERROR);
        goto cleanup;
    }

    ZeroMemory(&vsi0, sizeof(vsi0));
    AVIStreamInfoA(pvid0, &vsi0, sizeof(vsi0));
    AVIStreamFormatSize(pvid0, AVIStreamStart(pvid0), &vfmt0_sz);
    if (vfmt0_sz <= 0)
    {
        aviutl_msgbox_w(hdlg, i18n_get_wstr(1038), L"AviUtl", MB_OK | MB_ICONERROR);
        goto cleanup;
    }
    vfmt0 = (unsigned char *)calloc(1, (size_t)vfmt0_sz);
    if (!vfmt0 || AVIStreamReadFormat(pvid0, AVIStreamStart(pvid0), vfmt0, &vfmt0_sz) != 0)
    {
        aviutl_msgbox_w(hdlg, i18n_get_wstr(1038), L"AviUtl", MB_OK | MB_ICONERROR);
        goto cleanup;
    }

    if (AVIFileGetStream(pf0, &paud0, streamtypeAUDIO, 0) == 0 && paud0)
    {
        ZeroMemory(&asi0, sizeof(asi0));
        AVIStreamInfoA(paud0, &asi0, sizeof(asi0));
        AVIStreamFormatSize(paud0, AVIStreamStart(paud0), &afmt0_sz);
        if (afmt0_sz > 0)
        {
            afmt0 = (unsigned char *)calloc(1, (size_t)afmt0_sz);
            if (afmt0 && AVIStreamReadFormat(paud0, AVIStreamStart(paud0), afmt0, &afmt0_sz) == 0)
                has_aud = TRUE;
        }
    }

    // pre-validate remaining input files before creating output file
    for (i = 1; i < count; i++)
    {
        PAVIFILE pfi = NULL;
        PAVISTREAM pvi = NULL;
        BITMAPINFOHEADER bih0, bihi;
        LONG bsz = (LONG)sizeof(bihi);

        if (AVIFileOpenA(&pfi, files[i], OF_READ | OF_SHARE_DENY_WRITE, NULL) != 0 || !pfi ||
            AVIFileGetStream(pfi, &pvi, streamtypeVIDEO, 0) != 0 || !pvi)
        {
            if (pvi)
                AVIStreamRelease(pvi);
            if (pfi)
                AVIFileRelease(pfi);
            aviutl_msgbox_w(hdlg, i18n_get_wstr(1038), L"AviUtl", MB_OK | MB_ICONERROR);
            goto cleanup;
        }
        ZeroMemory(&bih0, sizeof(bih0));
        ZeroMemory(&bihi, sizeof(bihi));
        memcpy(&bih0, vfmt0, vfmt0_sz < (LONG)sizeof(bih0) ? (size_t)vfmt0_sz : sizeof(bih0));
        AVIStreamReadFormat(pvi, AVIStreamStart(pvi), &bihi, &bsz);
        AVIStreamRelease(pvi);
        AVIFileRelease(pfi);

        if (bihi.biWidth != bih0.biWidth || abs(bihi.biHeight) != abs(bih0.biHeight) ||
            bihi.biCompression != bih0.biCompression)
        {
            aviutl_msgbox_w(hdlg, i18n_get_wstr(1003), L"AviUtl", MB_OK | MB_ICONERROR);
            goto cleanup;
        }
    }

    DeleteFileA(out_path);
    if (AVIFileOpenA(&pout, out_path, OF_CREATE | OF_WRITE, NULL) != 0 || !pout)
    {
        aviutl_msgbox_w(hdlg, i18n_get_wstr(1002), L"AviUtl", MB_OK | MB_ICONERROR);
        goto cleanup;
    }

    vsi0.dwStart = 0;
    vsi0.dwLength = 0;
    if (AVIFileCreateStreamA(pout, &pvid_out, &vsi0) != 0 || !pvid_out ||
        AVIStreamSetFormat(pvid_out, 0, vfmt0, vfmt0_sz) != 0)
    {
        aviutl_msgbox_w(hdlg, i18n_get_wstr(1002), L"AviUtl", MB_OK | MB_ICONERROR);
        goto cleanup;
    }

    if (has_aud)
    {
        asi0.dwStart = 0;
        asi0.dwLength = 0;
        if (AVIFileCreateStreamA(pout, &paud_out, &asi0) == 0 && paud_out)
        {
            if (AVIStreamSetFormat(paud_out, 0, afmt0, afmt0_sz) != 0)
            {
                AVIStreamRelease(paud_out);
                paud_out = NULL;
            }
        }
    }

    for (i = 0; i < count; i++)
    {
        PAVIFILE pfi = (i == 0) ? pf0 : NULL;
        PAVISTREAM pvi = (i == 0) ? pvid0 : NULL;
        PAVISTREAM pai = (i == 0) ? paud0 : NULL;
        BOOL step_ok;

        if (i > 0)
        {
            if (AVIFileOpenA(&pfi, files[i], OF_READ | OF_SHARE_DENY_WRITE, NULL) != 0 || !pfi ||
                AVIFileGetStream(pfi, &pvi, streamtypeVIDEO, 0) != 0 || !pvi)
            {
                if (pvi)
                    AVIStreamRelease(pvi);
                if (pfi)
                    AVIFileRelease(pfi);
                aviutl_msgbox_w(hdlg, i18n_get_wstr(1001), L"AviUtl", MB_OK | MB_ICONERROR);
                goto cleanup;
            }
            if (paud_out)
                AVIFileGetStream(pfi, &pai, streamtypeAUDIO, 0);
        }

        step_ok = copy_vfw_video_packets(pvi, pvid_out, &out_vpos);
        if (step_ok && paud_out && pai)
            step_ok = copy_vfw_audio_packets(pai, paud_out, &out_apos);

        if (i > 0)
        {
            if (pai)
                AVIStreamRelease(pai);
            if (pvi)
                AVIStreamRelease(pvi);
            if (pfi)
                AVIFileRelease(pfi);
        }

        if (!step_ok)
        {
            aviutl_msgbox_w(hdlg, i18n_get_wstr(1001), L"AviUtl", MB_OK | MB_ICONERROR);
            goto cleanup;
        }
    }

    ok = TRUE;

cleanup:
    if (paud_out)
        AVIStreamRelease(paud_out);
    if (pvid_out)
        AVIStreamRelease(pvid_out);
    if (pout)
        AVIFileRelease(pout);
    if (paud0)
        AVIStreamRelease(paud0);
    if (pvid0)
        AVIStreamRelease(pvid0);
    if (pf0)
        AVIFileRelease(pf0);
    AVIFileExit();
    free(vfmt0);
    free(afmt0);

    if (ok && tags)
    {
        video_export_write_riff_info_tags(out_path, tags);
        dbg_info("AVICHAIN concatenated %d AVI files -> '%s' (%ld frames)",
                 count, out_path, (long)out_vpos);
    }
    return ok;
}

typedef struct AviChainDlgCtx
{
    HWND owner;
    char files[AVICHAIN_MAX_FILES][MAX_PATH];
    int count;
} AviChainDlgCtx;

static void refresh_avichain_listbox(HWND hdlg, AviChainDlgCtx *ctx, int new_sel)
{
    HWND hLb = GetDlgItem(hdlg, 32600);
    HDC hdc;
    HFONT hFont;
    HGDIOBJ hOld = NULL;
    int i, top_idx, max_px = 0;

    if (!hLb || !ctx)
        return;

    top_idx = (int)SendMessageW(hLb, LB_GETTOPINDEX, 0, 0);
    SendMessageW(hLb, LB_RESETCONTENT, 0, 0);

    hdc = GetDC(hLb);
    hFont = (HFONT)SendMessageW(hLb, WM_GETFONT, 0, 0);
    if (hdc && hFont)
        hOld = SelectObject(hdc, hFont);

    for (i = 0; i < ctx->count; i++)
    {
        wchar_t wpath[MAX_PATH];
        SIZE sz = {0};
        sjis_to_wide_at(ctx->files[i], wpath, MAX_PATH);
        SendMessageW(hLb, LB_ADDSTRING, 0, (LPARAM)wpath);
        if (hdc && GetTextExtentPoint32W(hdc, wpath, lstrlenW(wpath), &sz))
        {
            if (sz.cx + 16 > max_px)
                max_px = sz.cx + 16;
        }
    }

    if (hdc)
    {
        if (hOld)
            SelectObject(hdc, hOld);
        ReleaseDC(hLb, hdc);
    }

    SendMessageW(hLb, LB_SETHORIZONTALEXTENT, (WPARAM)max_px, 0);
    if (top_idx >= 0)
        SendMessageW(hLb, LB_SETTOPINDEX, (WPARAM)top_idx, 0);
    if (ctx->count > 0)
    {
        if (new_sel < 0)
            new_sel = 0;
        if (new_sel >= ctx->count)
            new_sel = ctx->count - 1;
        SendMessageW(hLb, LB_SETCURSEL, (WPARAM)new_sel, 0);
    }
}

static BOOL avichain_push_file(HWND hdlg, AviChainDlgCtx *ctx, const char *fpath)
{
    if (!ctx || !fpath || !fpath[0])
        return FALSE;
    if (ctx->count >= AVICHAIN_MAX_FILES)
    {
        show_max_items_warning(hdlg, 1024, AVICHAIN_MAX_FILES);
        return FALSE;
    }
    if (GetFullPathNameA(fpath, MAX_PATH, ctx->files[ctx->count], NULL) == 0)
        lstrcpynA(ctx->files[ctx->count], fpath, MAX_PATH);
    ctx->count++;
    return TRUE;
}

static int avichain_push_sequential(HWND hdlg, AviChainDlgCtx *ctx, const char *first_path)
{
    char cur[MAX_PATH];
    const char *base, *dot;
    int added = 0;

    if (!avichain_push_file(hdlg, ctx, first_path))
        return 0;
    added++;

    lstrcpynA(cur, ctx->files[ctx->count - 1], MAX_PATH);
    base = path_basename_dbcs_at(cur);
    dot = strrchr(base, '.');
    if (!dot)
        dot = cur + strlen(cur);

    if (dot > base && dot[-1] >= '0' && dot[-1] <= '9')
    {
        const char *d_end = dot;
        const char *d_beg = dot;
        char prefix[MAX_PATH], suffix[64];
        size_t pre_len, width;
        unsigned long val;

        while (d_beg > base && d_beg[-1] >= '0' && d_beg[-1] <= '9')
            d_beg--;

        pre_len = (size_t)(d_beg - cur);
        width = (size_t)(d_end - d_beg);
        if (pre_len < sizeof(prefix) && width <= 10)
        {
            memcpy(prefix, cur, pre_len);
            prefix[pre_len] = '\0';
            lstrcpynA(suffix, d_end, sizeof(suffix));
            val = strtoul(d_beg, NULL, 10);

            while (ctx->count < AVICHAIN_MAX_FILES)
            {
                char next_path[MAX_PATH];
                val++;
                _snprintf(next_path, sizeof(next_path), "%s%0*lu%s",
                          prefix, (int)width, val, suffix);
                next_path[sizeof(next_path) - 1] = '\0';
                if (GetFileAttributesA(next_path) == INVALID_FILE_ATTRIBUTES)
                    break;
                lstrcpynA(ctx->files[ctx->count++], next_path, MAX_PATH);
                added++;
            }
        }
    }
    return added;
}

static INT_PTR CALLBACK AviChainDlgProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    AviChainDlgCtx *ctx = (AviChainDlgCtx *)GetWindowLongPtrW(hdlg, GWLP_USERDATA);

    if (msg == WM_INITDIALOG)
    {
        ctx = (AviChainDlgCtx *)lp;
        SetWindowLongPtrW(hdlg, GWLP_USERDATA, (LONG_PTR)ctx);
        center_dlg_over_owner_at(hdlg, ctx->owner);
        DragAcceptFiles(hdlg, TRUE);
        SendDlgItemMessageW(hdlg, 32104, EM_SETLIMITTEXT, MAX_PATH - 1, 0);
        SendDlgItemMessageW(hdlg, 32100, EM_SETLIMITTEXT, 259, 0);
        SendDlgItemMessageW(hdlg, 32101, EM_SETLIMITTEXT, 259, 0);
        SendDlgItemMessageW(hdlg, 32102, EM_SETLIMITTEXT, 259, 0);
        return TRUE;
    }

    if (msg == WM_DROPFILES && ctx)
    {
        HDROP hDrop = (HDROP)wp;
        UINT nfiles = DragQueryFileA(hDrop, 0xFFFFFFFFu, NULL, 0);
        UINT i;
        BOOL added = FALSE;
        for (i = 0; i < nfiles; i++)
        {
            char fpath[MAX_PATH] = {0};
            if (DragQueryFileA(hDrop, i, fpath, MAX_PATH) > 0)
            {
                if (avichain_push_file(hdlg, ctx, fpath))
                    added = TRUE;
                else
                    break;
            }
        }
        DragFinish(hDrop);
        if (added)
            refresh_avichain_listbox(hdlg, ctx, ctx->count - 1);
        return TRUE;
    }

    if (msg == WM_COMMAND && ctx)
    {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        if (id == 32703 && code == BN_CLICKED) // add
        {
            char *fbuf = (char *)calloc(1, 32768);
            if (fbuf)
            {
                OPENFILENAMEA ofn;
                static const char flt[] = "AVI File (*.avi)\0*.avi\0All Files (*.*)\0*.*\0";
                ZeroMemory(&ofn, sizeof(ofn));
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hdlg;
                ofn.lpstrFilter = flt;
                ofn.nFilterIndex = 1;
                ofn.lpstrFile = fbuf;
                ofn.nMaxFile = 32768;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT |
                            OFN_EXPLORER | OFN_NOCHANGEDIR;
                if (GetOpenFileNameA(&ofn))
                {
                    const char *p = fbuf + strlen(fbuf) + 1;
                    BOOL added = FALSE;
                    if (*p == '\0')
                    {
                        added = avichain_push_file(hdlg, ctx, fbuf);
                    }
                    else
                    {
                        while (*p)
                        {
                            char full[MAX_PATH];
                            _snprintf(full, sizeof(full), "%s\\%s", fbuf, p);
                            full[sizeof(full) - 1] = '\0';
                            if (avichain_push_file(hdlg, ctx, full))
                                added = TRUE;
                            else
                                break;
                            p += strlen(p) + 1;
                        }
                    }
                    if (added)
                        refresh_avichain_listbox(hdlg, ctx, ctx->count - 1);
                }
                free(fbuf);
            }
            return TRUE;
        }

        if (id == 32707 && code == BN_CLICKED) // add Sequential Numbered Files
        {
            char file[MAX_PATH] = {0};
            OPENFILENAMEA ofn;
            static const char flt[] = "AVI File (*.avi)\0*.avi\0All Files (*.*)\0*.*\0";
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hdlg;
            ofn.lpstrFilter = flt;
            ofn.nFilterIndex = 1;
            ofn.lpstrFile = file;
            ofn.nMaxFile = sizeof(file);
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn))
            {
                if (avichain_push_sequential(hdlg, ctx, file) > 0)
                    refresh_avichain_listbox(hdlg, ctx, ctx->count - 1);
            }
            return TRUE;
        }

        if (id == 32704 && code == BN_CLICKED) // delete
        {
            int sel = (int)SendDlgItemMessageW(hdlg, 32600, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < ctx->count)
            {
                int k;
                for (k = sel; k + 1 < ctx->count; k++)
                    memcpy(ctx->files[k], ctx->files[k + 1], MAX_PATH);
                ctx->count--;
                refresh_avichain_listbox(hdlg, ctx, sel);
            }
            return TRUE;
        }

        if (id == 32700 && code == BN_CLICKED) // delete All
        {
            if (ctx->count > 0 &&
                aviutl_msgbox_w(hdlg, i18n_get_wstr(1037), L"AviUtl",
                                MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                ctx->count = 0;
                refresh_avichain_listbox(hdlg, ctx, 0);
            }
            return TRUE;
        }

        if (id == 32304 && code == BN_CLICKED) // output Browse
        {
            wchar_t wcur[MAX_PATH] = {0}, wnew[MAX_PATH];
            char file[MAX_PATH] = {0};
            OPENFILENAMEA ofn;
            static const char flt[] = "AVI File (*.avi)\0*.avi\0All Files (*.*)\0*.*\0";

            GetDlgItemTextW(hdlg, 32104, wcur, MAX_PATH);
            wide_to_sjis_at(wcur, file, sizeof(file));

            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hdlg;
            ofn.lpstrFilter = flt;
            ofn.nFilterIndex = 1;
            ofn.lpstrFile = file;
            ofn.nMaxFile = sizeof(file);
            ofn.lpstrDefExt = "avi";
            ofn.Flags = get_save_ofn_flags_at();
            if (GetSaveFileNameA(&ofn))
            {
                sjis_to_wide_at(file, wnew, MAX_PATH);
                SetDlgItemTextW(hdlg, 32104, wnew);
            }
            return TRUE;
        }

        if (id == IDOK)
        {
            wchar_t wout[MAX_PATH] = {0}, wt[260] = {0}, wc[260] = {0}, wcp[260] = {0};
            char out_path[MAX_PATH] = {0};
            AviExInfoTags tags;
            int k;

            GetDlgItemTextW(hdlg, 32104, wout, MAX_PATH);
            wide_to_sjis_at(wout, out_path, sizeof(out_path));
            if (ctx->count <= 0 || !out_path[0])
                return TRUE;

            for (k = 0; k < ctx->count; k++)
            {
                if (paths_equal_canon(out_path, ctx->files[k]))
                {
                    aviutl_msgbox_w(hdlg, i18n_get_wstr(1023), L"AviUtl", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
            }

            ZeroMemory(&tags, sizeof(tags));
            GetDlgItemTextW(hdlg, 32100, wt, 260);
            GetDlgItemTextW(hdlg, 32101, wc, 260);
            GetDlgItemTextW(hdlg, 32102, wcp, 260);
            wide_to_sjis_at(wt, tags.title, sizeof(tags.title));
            wide_to_sjis_at(wc, tags.comment, sizeof(tags.comment));
            wide_to_sjis_at(wcp, tags.copyright, sizeof(tags.copyright));

            if (avichain_concat_files(hdlg, ctx->files, ctx->count, out_path, &tags))
            {
                EndDialog(hdlg, IDOK);
            }
            return TRUE;
        }

        if (id == IDCANCEL)
        {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL avi_tools_run_avichain_dialog(HWND owner)
{
    const DLGTEMPLATE *tmpl = i18n_load_dialog_template("AVICHAIN");
    AviChainDlgCtx *ctx;
    INT_PTR res;

    if (!tmpl)
        return FALSE;

    ctx = (AviChainDlgCtx *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return FALSE;
    ctx->owner = owner;

    res = DialogBoxIndirectParamW(GetModuleHandleA(NULL), tmpl, owner,
                                  AviChainDlgProc, (LPARAM)ctx);
    free(ctx);
    return (res == IDOK);
}

// 0x0040b9c0, AVIMIX (1058) video and audio stream multiplexer

static BOOL avimix_mux_files(HWND hdlg, const char *vid_path, const char *aud_path,
                             const char *out_path, const AviExInfoTags *tags)
{
    PAVIFILE pf_vid = NULL, pf_aud = NULL, pf_out = NULL;
    PAVISTREAM pvid_in = NULL, paud_in = NULL, pvid_out = NULL, paud_out = NULL;
    AVISTREAMINFOA vsi, asi;
    unsigned char *vfmt = NULL, *afmt = NULL, *wav_fmt = NULL, *wav_pcm = NULL;
    LONG vfmt_sz = 0, afmt_sz = 0, wav_fmt_sz = 0;
    DWORD wav_pcm_sz = 0;
    LONG out_vpos = 0, out_apos = 0;
    BOOL ok = FALSE;

    if (!vid_path || !vid_path[0] || !out_path || !out_path[0])
        return FALSE;

    AVIFileInit();

    if (AVIFileOpenA(&pf_vid, vid_path, OF_READ | OF_SHARE_DENY_WRITE, NULL) != 0 || !pf_vid ||
        AVIFileGetStream(pf_vid, &pvid_in, streamtypeVIDEO, 0) != 0 || !pvid_in)
    {
        aviutl_msgbox_w(hdlg, i18n_get_wstr(1038), L"AviUtl", MB_OK | MB_ICONERROR);
        goto cleanup;
    }

    ZeroMemory(&vsi, sizeof(vsi));
    AVIStreamInfoA(pvid_in, &vsi, sizeof(vsi));
    AVIStreamFormatSize(pvid_in, AVIStreamStart(pvid_in), &vfmt_sz);
    if (vfmt_sz <= 0)
    {
        aviutl_msgbox_w(hdlg, i18n_get_wstr(1038), L"AviUtl", MB_OK | MB_ICONERROR);
        goto cleanup;
    }
    vfmt = (unsigned char *)calloc(1, (size_t)vfmt_sz);
    if (!vfmt || AVIStreamReadFormat(pvid_in, AVIStreamStart(pvid_in), vfmt, &vfmt_sz) != 0)
    {
        aviutl_msgbox_w(hdlg, i18n_get_wstr(1038), L"AviUtl", MB_OK | MB_ICONERROR);
        goto cleanup;
    }

    if (aud_path && aud_path[0])
    {
        if (AVIFileOpenA(&pf_aud, aud_path, OF_READ | OF_SHARE_DENY_WRITE, NULL) == 0 && pf_aud &&
            AVIFileGetStream(pf_aud, &paud_in, streamtypeAUDIO, 0) == 0 && paud_in)
        {
            ZeroMemory(&asi, sizeof(asi));
            AVIStreamInfoA(paud_in, &asi, sizeof(asi));
            AVIStreamFormatSize(paud_in, AVIStreamStart(paud_in), &afmt_sz);
            if (afmt_sz > 0)
            {
                afmt = (unsigned char *)calloc(1, (size_t)afmt_sz);
                if (!afmt || AVIStreamReadFormat(paud_in, AVIStreamStart(paud_in), afmt, &afmt_sz) != 0)
                {
                    aviutl_msgbox_w(hdlg, i18n_get_wstr(1038), L"AviUtl", MB_OK | MB_ICONERROR);
                    goto cleanup;
                }
            }
        }
        else if (!read_riff_wav_fallback(aud_path, &wav_fmt, &wav_fmt_sz, &wav_pcm, &wav_pcm_sz))
        {
            aviutl_msgbox_w(hdlg, i18n_get_wstr(1038), L"AviUtl", MB_OK | MB_ICONERROR);
            goto cleanup;
        }
    }

    DeleteFileA(out_path);
    if (AVIFileOpenA(&pf_out, out_path, OF_CREATE | OF_WRITE, NULL) != 0 || !pf_out)
    {
        aviutl_msgbox_w(hdlg, i18n_get_wstr(1002), L"AviUtl", MB_OK | MB_ICONERROR);
        goto cleanup;
    }

    vsi.dwStart = 0;
    vsi.dwLength = 0;
    if (AVIFileCreateStreamA(pf_out, &pvid_out, &vsi) != 0 || !pvid_out ||
        AVIStreamSetFormat(pvid_out, 0, vfmt, vfmt_sz) != 0)
    {
        aviutl_msgbox_w(hdlg, i18n_get_wstr(1002), L"AviUtl", MB_OK | MB_ICONERROR);
        goto cleanup;
    }

    if (paud_in && afmt && afmt_sz > 0)
    {
        asi.dwStart = 0;
        asi.dwLength = 0;
        if (AVIFileCreateStreamA(pf_out, &paud_out, &asi) != 0 || !paud_out ||
            AVIStreamSetFormat(paud_out, 0, afmt, afmt_sz) != 0)
        {
            aviutl_msgbox_w(hdlg, i18n_get_wstr(1002), L"AviUtl", MB_OK | MB_ICONERROR);
            goto cleanup;
        }
    }
    else if (wav_fmt && wav_pcm && wav_pcm_sz > 0)
    {
        const WAVEFORMATEX *wfx = (const WAVEFORMATEX *)wav_fmt;
        DWORD blk = wfx->nBlockAlign > 0 ? wfx->nBlockAlign : 1;
        DWORD rate = wfx->nAvgBytesPerSec > 0 ? wfx->nAvgBytesPerSec : (wfx->nSamplesPerSec * blk);
        ZeroMemory(&asi, sizeof(asi));
        asi.fccType = streamtypeAUDIO;
        asi.dwScale = blk;
        asi.dwRate = rate;
        asi.dwSampleSize = blk;
        asi.dwQuality = (DWORD)-1;
        if (AVIFileCreateStreamA(pf_out, &paud_out, &asi) != 0 || !paud_out ||
            AVIStreamSetFormat(paud_out, 0, wav_fmt, wav_fmt_sz) != 0)
        {
            aviutl_msgbox_w(hdlg, i18n_get_wstr(1002), L"AviUtl", MB_OK | MB_ICONERROR);
            goto cleanup;
        }
    }

    if (!copy_vfw_video_packets(pvid_in, pvid_out, &out_vpos))
    {
        aviutl_msgbox_w(hdlg, i18n_get_wstr(1001), L"AviUtl", MB_OK | MB_ICONERROR);
        goto cleanup;
    }

    if (paud_out)
    {
        if (paud_in)
        {
            if (!copy_vfw_audio_packets(paud_in, paud_out, &out_apos))
            {
                aviutl_msgbox_w(hdlg, i18n_get_wstr(1001), L"AviUtl", MB_OK | MB_ICONERROR);
                goto cleanup;
            }
        }
        else if (wav_fmt && wav_pcm && wav_pcm_sz > 0)
        {
            const WAVEFORMATEX *wfx = (const WAVEFORMATEX *)wav_fmt;
            LONG blk = wfx->nBlockAlign > 0 ? (LONG)wfx->nBlockAlign : 1;
            LONG nblocks = (LONG)(wav_pcm_sz / (DWORD)blk);
            if (nblocks > 0 &&
                AVIStreamWrite(paud_out, 0, nblocks, wav_pcm, (LONG)wav_pcm_sz,
                               AVIIF_KEYFRAME, NULL, NULL) != 0)
            {
                aviutl_msgbox_w(hdlg, i18n_get_wstr(1001), L"AviUtl", MB_OK | MB_ICONERROR);
                goto cleanup;
            }
            out_apos = nblocks;
        }
    }

    ok = TRUE;

cleanup:
    if (paud_out)
        AVIStreamRelease(paud_out);
    if (pvid_out)
        AVIStreamRelease(pvid_out);
    if (pf_out)
        AVIFileRelease(pf_out);
    if (paud_in)
        AVIStreamRelease(paud_in);
    if (pf_aud)
        AVIFileRelease(pf_aud);
    if (pvid_in)
        AVIStreamRelease(pvid_in);
    if (pf_vid)
        AVIFileRelease(pf_vid);
    AVIFileExit();
    free(vfmt);
    free(afmt);
    free(wav_fmt);
    free(wav_pcm);

    if (ok && tags)
    {
        video_export_write_riff_info_tags(out_path, tags);
        dbg_info("AVIMIX muxed '%s' + '%s' -> '%s' (%ld video frames, %ld audio blocks)",
                 vid_path, (aud_path && aud_path[0]) ? aud_path : "(none)",
                 out_path, (long)out_vpos, (long)out_apos);
    }
    return ok;
}

static INT_PTR CALLBACK AviMixDlgProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND owner = (HWND)GetWindowLongPtrW(hdlg, GWLP_USERDATA);

    if (msg == WM_INITDIALOG)
    {
        owner = (HWND)lp;
        SetWindowLongPtrW(hdlg, GWLP_USERDATA, (LONG_PTR)owner);
        center_dlg_over_owner_at(hdlg, owner);
        DragAcceptFiles(hdlg, TRUE);
        SendDlgItemMessageW(hdlg, 32105, EM_SETLIMITTEXT, MAX_PATH - 1, 0);
        SendDlgItemMessageW(hdlg, 32106, EM_SETLIMITTEXT, MAX_PATH - 1, 0);
        SendDlgItemMessageW(hdlg, 32104, EM_SETLIMITTEXT, MAX_PATH - 1, 0);
        SendDlgItemMessageW(hdlg, 32100, EM_SETLIMITTEXT, 259, 0);
        SendDlgItemMessageW(hdlg, 32101, EM_SETLIMITTEXT, 259, 0);
        SendDlgItemMessageW(hdlg, 32102, EM_SETLIMITTEXT, 259, 0);
        return TRUE;
    }

    if (msg == WM_DROPFILES)
    {
        HDROP hDrop = (HDROP)wp;
        UINT nfiles = DragQueryFileA(hDrop, 0xFFFFFFFFu, NULL, 0);
        UINT i;
        for (i = 0; i < nfiles; i++)
        {
            char fpath[MAX_PATH] = {0};
            wchar_t wpath[MAX_PATH];
            if (DragQueryFileA(hDrop, i, fpath, MAX_PATH) > 0)
            {
                sjis_to_wide_at(fpath, wpath, MAX_PATH);
                if (lstrcmpiA(path_ext_dbcs_at(fpath), ".wav") == 0)
                {
                    SetDlgItemTextW(hdlg, 32106, wpath);
                }
                else
                {
                    wchar_t wv[MAX_PATH] = {0};
                    GetDlgItemTextW(hdlg, 32105, wv, MAX_PATH);
                    if (!wv[0])
                        SetDlgItemTextW(hdlg, 32105, wpath);
                    else
                        SetDlgItemTextW(hdlg, 32106, wpath);
                }
            }
        }
        DragFinish(hDrop);
        return TRUE;
    }

    if (msg == WM_COMMAND)
    {
        int id = LOWORD(wp);
        int code = HIWORD(wp);

        if ((id == 32305 || id == 32306 || id == 32304) && code == BN_CLICKED)
        {
            int edit_id = (id == 32305) ? 32105 : ((id == 32306) ? 32106 : 32104);
            wchar_t wcur[MAX_PATH] = {0}, wnew[MAX_PATH];
            char file[MAX_PATH] = {0};
            OPENFILENAMEA ofn;
            static const char flt_avi[] = "AVI File (*.avi)\0*.avi\0All Files (*.*)\0*.*\0";
            static const char flt_aud[] =
                "Audio / AVI (*.avi;*.wav)\0*.avi;*.wav\0"
                "Wave File (*.wav)\0*.wav\0"
                "AVI File (*.avi)\0*.avi\0"
                "All Files (*.*)\0*.*\0";

            GetDlgItemTextW(hdlg, edit_id, wcur, MAX_PATH);
            wide_to_sjis_at(wcur, file, sizeof(file));

            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hdlg;
            ofn.lpstrFilter = (id == 32306) ? flt_aud : flt_avi;
            ofn.nFilterIndex = 1;
            ofn.lpstrFile = file;
            ofn.nMaxFile = sizeof(file);
            if (id == 32304)
            {
                ofn.lpstrDefExt = "avi";
                ofn.Flags = get_save_ofn_flags_at();
                if (GetSaveFileNameA(&ofn))
                {
                    sjis_to_wide_at(file, wnew, MAX_PATH);
                    SetDlgItemTextW(hdlg, edit_id, wnew);
                }
            }
            else
            {
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
                if (GetOpenFileNameA(&ofn))
                {
                    sjis_to_wide_at(file, wnew, MAX_PATH);
                    SetDlgItemTextW(hdlg, edit_id, wnew);
                }
            }
            return TRUE;
        }

        if (id == IDOK)
        {
            wchar_t wvid[MAX_PATH] = {0}, waud[MAX_PATH] = {0}, wout[MAX_PATH] = {0};
            wchar_t wt[260] = {0}, wc[260] = {0}, wcp[260] = {0};
            char vid_path[MAX_PATH] = {0}, aud_path[MAX_PATH] = {0}, out_path[MAX_PATH] = {0};
            AviExInfoTags tags;

            GetDlgItemTextW(hdlg, 32105, wvid, MAX_PATH);
            GetDlgItemTextW(hdlg, 32106, waud, MAX_PATH);
            GetDlgItemTextW(hdlg, 32104, wout, MAX_PATH);
            wide_to_sjis_at(wvid, vid_path, sizeof(vid_path));
            wide_to_sjis_at(waud, aud_path, sizeof(aud_path));
            wide_to_sjis_at(wout, out_path, sizeof(out_path));

            if (!vid_path[0] || !out_path[0])
                return TRUE;

            if (paths_equal_canon(out_path, vid_path) ||
                (aud_path[0] && paths_equal_canon(out_path, aud_path)))
            {
                aviutl_msgbox_w(hdlg, i18n_get_wstr(1023), L"AviUtl", MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            ZeroMemory(&tags, sizeof(tags));
            GetDlgItemTextW(hdlg, 32100, wt, 260);
            GetDlgItemTextW(hdlg, 32101, wc, 260);
            GetDlgItemTextW(hdlg, 32102, wcp, 260);
            wide_to_sjis_at(wt, tags.title, sizeof(tags.title));
            wide_to_sjis_at(wc, tags.comment, sizeof(tags.comment));
            wide_to_sjis_at(wcp, tags.copyright, sizeof(tags.copyright));

            if (avimix_mux_files(hdlg, vid_path, aud_path, out_path, &tags))
            {
                EndDialog(hdlg, IDOK);
            }
            return TRUE;
        }

        if (id == IDCANCEL)
        {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL avi_tools_run_avimix_dialog(HWND owner)
{
    const DLGTEMPLATE *tmpl = i18n_load_dialog_template("AVIMIX");
    if (!tmpl)
        return FALSE;
    return DialogBoxIndirectParamW(GetModuleHandleA(NULL), tmpl, owner,
                                   AviMixDlgProc, (LPARAM)owner) == IDOK;
}

