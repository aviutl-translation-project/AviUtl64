#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filter.h"
#include "editp.h"
#include "plugin_loader.h"
#include "darken_theme.h"

EXFUNC *exfunc_get_table(void);

typedef FILTER_DLL *(__stdcall *GetFilterTableFn)(void);
typedef FILTER_DLL **(__stdcall *GetFilterTableListFn)(void);

static int populate_from_exports(HMODULE mod, PluginReport *rep, BOOL yuy2_exports)
{
    GetFilterTableFn get1 = NULL;
    GetFilterTableListFn getN = NULL;
    int count = 0;

    if (yuy2_exports)
    {
        getN = (GetFilterTableListFn)GetProcAddress(mod, "GetFilterTableListYUY2");
        if (!getN)
            getN = (GetFilterTableListFn)GetProcAddress(mod, "_GetFilterTableListYUY2@0");
        get1 = (GetFilterTableFn)GetProcAddress(mod, "GetFilterTableYUY2");
        if (!get1)
            get1 = (GetFilterTableFn)GetProcAddress(mod, "_GetFilterTableYUY2@0");
    }
    else
    {
        getN = (GetFilterTableListFn)GetProcAddress(mod, "GetFilterTableList");
        if (!getN)
            getN = (GetFilterTableListFn)GetProcAddress(mod, "_GetFilterTableList@0");
        get1 = (GetFilterTableFn)GetProcAddress(mod, "GetFilterTable");
        if (!get1)
            get1 = (GetFilterTableFn)GetProcAddress(mod, "_GetFilterTable@0");
    }

    if (getN)
    {
        FILTER_DLL **list = getN();
        if (list)
        {
            while (list[count] && count < PLUGIN_MAX_FILTERS)
            {
                rep->filters[count] = list[count];
                rep->is_yuy2[count] = yuy2_exports;
                count++;
            }
        }
    }
    else if (get1)
    {
        FILTER_DLL *t = get1();
        if (t)
        {
            rep->filters[0] = t;
            rep->is_yuy2[0] = yuy2_exports;
            count = 1;
        }
    }
    return count;
}

int plugin_inspect_auf(const char *path, PluginReport *rep)
{
    HMODULE mod = NULL;
    char abs_path[MAX_PATH];
    BOOL prefer_yuy2;
    int count = 0;

    if (!rep)
    {
        return -1;
    }
    ZeroMemory(rep, sizeof(*rep));

    if (!path || !path[0])
    {
        lstrcpynA(rep->err, "empty plugin path", sizeof(rep->err));
        return -1;
    }

    // use altered search path so companion dlls next to .auf (like lua51.dll) load
    if (GetFullPathNameA(path, MAX_PATH, abs_path, NULL) > 0)
    {
        mod = LoadLibraryExA(abs_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (!mod)
    {
        mod = LoadLibraryA(path);
    }

    if (!mod)
    {
        DWORD e = GetLastError();
        rep->last_error = e;
        if (e == ERROR_BAD_EXE_FORMAT)
        {
            wsprintfA(rep->err, "bitness mismatch (err 193)");
        }
        else
        {
            wsprintfA(rep->err, "LoadLibrary failed (err %lu)", (unsigned long)e);
        }
        return -1;
    }
    rep->module = mod;

    // 0x0042e920: prefer GetFilterTableYUY2/ListYUY2 when [system] yuy2mode=1, with fallback in both modes
    prefer_yuy2 = editp_is_yuy2_mode();
    count = populate_from_exports(mod, rep, prefer_yuy2);
    if (count <= 0)
    {
        count = populate_from_exports(mod, rep, !prefer_yuy2);
    }
    if (count <= 0)
    {
        wsprintfA(rep->err, "missing GetFilterTable/GetFilterTableList export");
        FreeLibrary(mod);
        rep->module = NULL;
        return -1;
    }

    darken_theme_hook_module(mod);
    rep->count = count;
    return count;
}

void plugin_unload(PluginReport *rep)
{
    if (rep && rep->module)
    {
        FreeLibrary((HMODULE)rep->module);
        rep->module = NULL;
    }
}

void plugin_describe(const FILTER_DLL *f, char *out, int out_sz)
{
    int n = 0;
    if (!f || !out || out_sz <= 0)
    {
        return;
    }
    n += wsprintfA(out + n, "name        : %s\r\n", f->name ? f->name : "(null)");
    n += wsprintfA(out + n, "flag        : 0x%08x\r\n", (unsigned)f->flag);
    n += wsprintfA(out + n, "trackbars   : %d\r\n", f->track_n);
    n += wsprintfA(out + n, "checkboxes  : %d\r\n", f->check_n);
    n += wsprintfA(out + n, "win size    : %d x %d\r\n", f->x, f->y);
    if (f->information)
    {
        n += wsprintfA(out + n, "info        : %s\r\n", f->information);
    }
    n += wsprintfA(out + n, "func_proc   : %s\r\n", f->func_proc ? "yes" : "-");
    n += wsprintfA(out + n, "func_init   : %s\r\n", f->func_init ? "yes" : "-");
    n += wsprintfA(out + n, "func_WndProc: %s\r\n", f->func_WndProc ? "yes" : "-");
    (void)out_sz;
}

BOOL plugin_setup_runtime_filter(FILTER *fp_out, FILTER_DLL *src,
                                 HINSTANCE hinst, const char *path,
                                 HWND filter_hwnd)
{
    int i;
    if (!fp_out || !src)
    {
        return FALSE;
    }

    ZeroMemory(fp_out, sizeof(*fp_out));

    fp_out->flag = src->flag;
    if (src->flag & (FILTER_FLAG_ALWAYS_ACTIVE | FILTER_FLAG_INTERLACE_FILTER))
    {
        fp_out->flag |= FILTER_FLAG_ACTIVE;
    }
    fp_out->x = src->x;
    fp_out->y = src->y;
    fp_out->name = src->name;
    fp_out->track_n = src->track_n;
    fp_out->track_name = src->track_name;
    fp_out->track_default = src->track_default;
    fp_out->track_s = src->track_s;
    fp_out->track_e = src->track_e;
    fp_out->check_n = src->check_n;
    fp_out->check_name = src->check_name;
    fp_out->check_default = src->check_default;
    fp_out->func_proc = src->func_proc;
    fp_out->func_init = src->func_init;
    fp_out->func_exit = src->func_exit;
    fp_out->func_update = src->func_update;
    fp_out->func_WndProc = src->func_WndProc;
    fp_out->ex_data_size = src->ex_data_size;
    fp_out->information = src->information;
    fp_out->func_save_start = src->func_save_start;
    fp_out->func_save_end = src->func_save_end;
    fp_out->func_is_saveframe = src->func_is_saveframe;
    fp_out->func_project_load = src->func_project_load;
    fp_out->func_project_save = src->func_project_save;
    fp_out->func_modify_title = src->func_modify_title;
    fp_out->ex_data_def = src->ex_data_def;

    if (src->track_n > 0)
    {
        fp_out->track = (int *)calloc((size_t)src->track_n, sizeof(int));
        if (!fp_out->track)
        {
            return FALSE;
        }
        for (i = 0; i < src->track_n; i++)
        {
            fp_out->track[i] = src->track_default ? src->track_default[i] : 0;
        }
    }

    if (src->check_n > 0)
    {
        fp_out->check = (int *)calloc((size_t)src->check_n, sizeof(int));
        if (!fp_out->check)
        {
            plugin_free_runtime_filter(fp_out);
            return FALSE;
        }
        for (i = 0; i < src->check_n; i++)
        {
            fp_out->check[i] = (src->check_default && src->check_default[i] > 0)
                                   ? src->check_default[i]
                                   : 0;
        }
    }

    if ((src->flag & FILTER_FLAG_EX_DATA) && src->ex_data_size > 0)
    {
        if (src->ex_data_ptr)
        {
            fp_out->ex_data_ptr = src->ex_data_ptr;
            if (src->ex_data_def)
            {
                memcpy(fp_out->ex_data_ptr, src->ex_data_def, (size_t)src->ex_data_size);
            }
        }
        else
        {
            fp_out->ex_data_ptr = calloc(1, (size_t)src->ex_data_size);
            if (!fp_out->ex_data_ptr)
            {
                plugin_free_runtime_filter(fp_out);
                return FALSE;
            }
            if (src->ex_data_def)
            {
                memcpy(fp_out->ex_data_ptr, src->ex_data_def, (size_t)src->ex_data_size);
            }
        }
    }
    else
    {
        fp_out->ex_data_ptr = src->ex_data_ptr;
    }

    fp_out->exfunc = exfunc_get_table();
    fp_out->hwnd = filter_hwnd;
    fp_out->dll_hinst = hinst;
    if (path && path[0])
    {
        // allocate extra zeroed tail for aviutl_exedit_sdk FilterPlugin::path (char[260] + HMENU menu1, menu2)
        char *dup = (char *)calloc(1, MAX_PATH + 32);
        if (dup)
        {
            if (GetFullPathNameA(path, MAX_PATH, dup, NULL) == 0)
            {
                lstrcpynA(dup, path, MAX_PATH);
            }
            fp_out->dll_path = dup;
        }
    }

    // mirror runtime pointers back into the plugin's static FILTER_DLL table
    src->exfunc = fp_out->exfunc;
    src->hwnd = fp_out->hwnd;
    src->dll_hinst = fp_out->dll_hinst;
    src->dll_path = fp_out->dll_path;
    src->track = fp_out->track;
    src->check = fp_out->check;
    if (fp_out->ex_data_ptr)
    {
        src->ex_data_ptr = fp_out->ex_data_ptr;
    }

    return TRUE;
}

void plugin_free_runtime_filter(FILTER *fp)
{
    if (!fp)
    {
        return;
    }
    if (fp->track)
    {
        free(fp->track);
        fp->track = NULL;
    }
    if (fp->check)
    {
        free(fp->check);
        fp->check = NULL;
    }
    if (fp->dll_path)
    {
        free(fp->dll_path);
        fp->dll_path = NULL;
    }
}

static FILTER g_standalone_fps[PLUGIN_MAX_FILTERS];
static int g_standalone_fp_count = 0;

BOOL plugin_init_filter(FILTER_DLL *f, HINSTANCE hinst, const char *path,
                        HWND hwnd_parent, char *err, int err_sz)
{
    FILTER *fp_runtime;

    if (!f)
    {
        if (err)
        {
            lstrcpynA(err, "NULL FILTER_DLL", err_sz);
        }
        return FALSE;
    }

    if (g_standalone_fp_count >= PLUGIN_MAX_FILTERS)
    {
        if (err)
        {
            lstrcpynA(err, "standalone filter slot limit reached", err_sz);
        }
        return FALSE;
    }

    fp_runtime = &g_standalone_fps[g_standalone_fp_count++];
    if (!plugin_setup_runtime_filter(fp_runtime, f, hinst, path, hwnd_parent))
    {
        if (err)
        {
            lstrcpynA(err, "plugin_setup_runtime_filter failed", err_sz);
        }
        return FALSE;
    }

    if (fp_runtime->func_init)
    {
        if (!fp_runtime->func_init(fp_runtime))
        {
            if (err)
            {
                lstrcpynA(err, "func_init returned FALSE", err_sz);
            }
            return FALSE;
        }
    }

    return TRUE;
}
