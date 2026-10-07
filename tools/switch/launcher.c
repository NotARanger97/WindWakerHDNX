/* WindWakerHDNX.exe: the Switch builder as one file (Windows).
 *
 * The program carries a zip of everything the builder needs (a private Python, zig, the Switch SDK, the
 * recompiler and tools/switch) after its own code, followed by a trailer (struct Trailer). On the first
 * start of a version it unpacks that zip to %LOCALAPPDATA%\WindWakerHDNX\<version> with a small progress
 * window; every start then runs <that folder>\python\pythonw.exe tools\switch\builder_gui.py and exits.
 * Nothing is installed elsewhere (delete that folder to remove it). Built by tools/switch/package_builder.py
 * with zig cc; unzips with miniz (MIT, tools/switch/third_party/miniz).
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "third_party/miniz/miniz.h"

#define APP L"Wind Waker HD NX"

#pragma pack(push, 1)
typedef struct {
    uint64_t zip_offset, zip_size;
    char version[32];
    char magic[8]; /* "WWHDNXP1" */
} Trailer;
#pragma pack(pop)

static HWND g_window, g_bar, g_text;
static volatile LONG g_done;   /* 1 ok, -1 failed */
static volatile LONG g_permille;
static wchar_t g_error[512];
static wchar_t g_exe[MAX_PATH], g_dir[MAX_PATH];
static Trailer g_trailer;

static void fail(const wchar_t *msg) {
    MessageBoxW(NULL, msg, APP, MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

static int make_dirs(const wchar_t *path) {
    wchar_t tmp[MAX_PATH];
    wcsncpy(tmp, path, MAX_PATH - 1);
    tmp[MAX_PATH - 1] = 0;
    for (wchar_t *p = tmp + 3; *p; ++p)
        if (*p == L'\\' || *p == L'/') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    return CreateDirectoryW(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int read_trailer(void) {
    FILE *f = _wfopen(g_exe, L"rb");
    if (!f) return 0;
    _fseeki64(f, -(int64_t)sizeof(Trailer), SEEK_END);
    size_t n = fread(&g_trailer, 1, sizeof g_trailer, f);
    fclose(f);
    return n == sizeof g_trailer && !memcmp(g_trailer.magic, "WWHDNXP1", 8);
}

/* unpacks the zip into g_dir (worker thread); .complete marks a finished unpack */
static DWORD WINAPI unpack(LPVOID unused) {
    (void)unused;
    char exe8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, g_exe, -1, exe8, sizeof exe8, NULL, NULL);
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_file_v2(&zip, exe8, 0, g_trailer.zip_offset, g_trailer.zip_size)) {
        wcscpy(g_error, L"The program file is damaged (its contents cannot be read). Download it again.");
        InterlockedExchange(&g_done, -1);
        return 0;
    }
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    uint64_t total = 0, done = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&zip, i, &st)) total += st.m_uncomp_size;
    }
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        wchar_t rel[MAX_PATH], out[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, st.m_filename, -1, rel, MAX_PATH);
        for (wchar_t *p = rel; *p; ++p) if (*p == L'/') *p = L'\\';
        _snwprintf(out, MAX_PATH, L"%s\\%s", g_dir, rel);
        out[MAX_PATH - 1] = 0;
        if (st.m_is_directory) { make_dirs(out); continue; }
        wchar_t parent[MAX_PATH];
        wcscpy(parent, out);
        wchar_t *slash = wcsrchr(parent, L'\\');
        if (slash) { *slash = 0; make_dirs(parent); }
        char out8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, out, -1, out8, sizeof out8, NULL, NULL);
        FILE *f = _wfopen(out, L"wb");
        if (!f || !mz_zip_reader_extract_to_cfile(&zip, i, f, 0)) {
            if (f) fclose(f);
            _snwprintf(g_error, 512, L"Cannot write %s (disk full or no permission?).", out);
            mz_zip_reader_end(&zip);
            InterlockedExchange(&g_done, -1);
            return 0;
        }
        fclose(f);
        done += st.m_uncomp_size;
        InterlockedExchange(&g_permille, total ? (LONG)(done * 1000 / total) : 1000);
    }
    mz_zip_reader_end(&zip);
    wchar_t mark[MAX_PATH];
    _snwprintf(mark, MAX_PATH, L"%s\\.complete", g_dir);
    FILE *m = _wfopen(mark, L"wb");
    if (m) fclose(m);
    InterlockedExchange(&g_done, 1);
    return 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_TIMER) {
        SendMessageW(g_bar, PBM_SETPOS, (WPARAM)g_permille, 0);
        if (g_done) DestroyWindow(h);
        return 0;
    }
    if (msg == WM_CLOSE) return 0; /* wait for the unpack to finish */
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, w, l);
}

static void unpack_with_window(HINSTANCE inst) {
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"WWHDNXUnpack";
    wc.hCursor = LoadCursorW(NULL, IDC_WAIT);
    RegisterClassW(&wc);
    int w = 420, hgt = 130;
    g_window = CreateWindowW(L"WWHDNXUnpack", APP, WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2,
                             w, hgt, NULL, NULL, inst, NULL);
    g_text = CreateWindowW(L"STATIC", L"Preparing the builder (first start, a few seconds)...",
                           WS_CHILD | WS_VISIBLE, 16, 14, 380, 20, g_window, NULL, inst, NULL);
    SendMessageW(g_text, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    g_bar = CreateWindowW(PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE, 16, 44, 372, 20, g_window, NULL, inst, NULL);
    SendMessageW(g_bar, PBM_SETRANGE32, 0, 1000);
    SetTimer(g_window, 1, 100, NULL);
    CreateThread(NULL, 0, unpack, NULL, 0, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    GetModuleFileNameW(NULL, g_exe, MAX_PATH);
    if (!read_trailer()) fail(L"This program file is incomplete. Download WindWakerHDNX.exe again.");
    wchar_t base[MAX_PATH], version[64];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        fail(L"Cannot find your local application data folder.");
    char v8[33];
    memcpy(v8, g_trailer.version, 32);
    v8[32] = 0;
    MultiByteToWideChar(CP_UTF8, 0, v8, -1, version, 64);
    _snwprintf(g_dir, MAX_PATH, L"%s\\WindWakerHDNX\\%s", base, version);
    g_dir[MAX_PATH - 1] = 0;
    wchar_t mark[MAX_PATH];
    _snwprintf(mark, MAX_PATH, L"%s\\.complete", g_dir);
    if (GetFileAttributesW(mark) == INVALID_FILE_ATTRIBUTES) {
        if (!make_dirs(g_dir)) fail(L"Cannot create the program's folder in your local application data.");
        unpack_with_window(inst);
        if (g_done != 1) fail(g_error);
    }
    wchar_t python[MAX_PATH], line[MAX_PATH * 3];
    _snwprintf(python, MAX_PATH, L"%s\\python\\pythonw.exe", g_dir);
    _snwprintf(line, MAX_PATH * 3, L"\"%s\" \"%s\\tools\\switch\\builder_gui.py\"", python, g_dir);
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(python, line, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi))
        fail(L"Cannot start the builder (its files may have been removed: delete the WindWakerHDNX folder in "
             L"%LOCALAPPDATA% and start again).");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
