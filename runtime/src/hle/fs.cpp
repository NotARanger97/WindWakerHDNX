#if defined(__SWITCH__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1  // newlib: fopencookie
#endif
// coreinit FS and nn_save: map Wii U volume paths onto host directories.
//   /vol/content/...  -> <game>/content/...
//   /vol/code/...     -> <game>/code/...
//   /vol/meta/...     -> <game>/meta/...
//   /vol/save/...     -> <save dir>/...
#include "../platform/filesystem.h"

#include <cstdio>
#ifndef _WIN32
#include <strings.h>
#endif
#include <mutex>
#include <string>
#include <unordered_map>

#include "../runtime.h"
#include "../write_watch.h"
#include "../mods/content.h"
#ifdef __SWITCH__
extern "C" void switch_dc_store(const void* p, size_t size);  // platform/switch/switch_host.cpp
#endif

#ifdef __SWITCH__
#define WWHD_WUA 1
#include <zarchive/zarchivereader.h>
#include <vector>
#endif

namespace {

enum FSStatus : int32_t {
    FS_OK = 0,
    FS_END = -2,
    FS_EXISTS = -5,
    FS_NOT_FOUND = -6,
    FS_NOT_FILE = -7,
    FS_NOT_DIR = -8,
    FS_ACCESS_ERROR = -10,
};

struct OpenFile {
    FILE* f;
    std::string path;  // guest path
    std::string mode;
};
struct OpenDir {
    DIR* d;
    std::string path;  // host path
    std::string gpath;
    uint32_t read = 0;  // entries returned so far
    std::vector<std::pair<std::string, int64_t>> listed;  // .wua directories: name, size (-1 = directory)
};

std::mutex g_fs_mutex;
std::unordered_map<uint32_t, OpenFile> g_files;
std::unordered_map<uint32_t, OpenDir> g_dirs;
uint32_t g_next_handle = 1;

#ifdef WWHD_WUA
// ---- the game read straight from a Cemu .wua archive (ZArchive): config::game_dir may name the
// archive file; <game_dir>/content/... then resolves inside its title folder. Files open as
// FILE* (fopencookie), so the rest of this file stays the same.
namespace wua {
std::mutex g_m;  // ZArchiveReader is not thread-safe
ZArchiveReader* g_ar = nullptr;
std::string g_root;  // e.g. "0005000010143600_v0/"
std::unordered_map<FILE*, uint64_t> g_sizes;
bool active() {
    const std::string& g = config::game_dir;
    if (g.size() < 4 || strcasecmp(g.c_str() + g.size() - 4, ".wua") != 0) return false;
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_ar) {
        g_ar = ZArchiveReader::OpenFromFile(g);
        if (!g_ar) fatal("cannot open %s", g.c_str());
        ZArchiveNodeHandle root = g_ar->LookUp("", false, true);
        ZArchiveReader::DirEntry e;
        for (uint32_t i = 0; i < g_ar->GetDirEntryCount(root); i++)
            if (g_ar->GetDirEntry(root, i, e) && e.isDirectory && e.name.find("_v") != std::string_view::npos) {
                g_root = std::string(e.name) + "/";
                break;
            }
        LOG("[fs] game archive %s, title folder %s", g.c_str(), g_root.c_str());
    }
    return true;
}
// host path below game_dir -> path inside the archive ("" if not in the archive)
std::string inner(const std::string& host) {
    if (!active()) return {};
    const std::string& g = config::game_dir;
    if (host.compare(0, g.size(), g) != 0 || host.size() <= g.size() || host[g.size()] != '/') return {};
    return g_root + host.substr(g.size() + 1);
}
struct Cookie { ZArchiveNodeHandle h; uint64_t pos, size; };
ssize_t c_read(void* p, char* buf, size_t n) {
    auto* k = (Cookie*)p;
    std::lock_guard<std::mutex> lk(g_m);
    uint64_t got = g_ar->ReadFromFile(k->h, k->pos, n, buf);
    k->pos += got;
    return (ssize_t)got;
}
int c_seek(void* p, off_t* off, int whence) {
    auto* k = (Cookie*)p;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)k->pos : (int64_t)k->size;
    int64_t np = base + *off;
    if (np < 0) return -1;
    k->pos = (uint64_t)np;
    *off = np;
    return 0;
}
int c_close(void* p) { delete (Cookie*)p; return 0; }
// lookup: 1 file, 2 directory, 0 missing
int kind(const std::string& in, uint64_t* size = nullptr) {
    std::lock_guard<std::mutex> lk(g_m);
    ZArchiveNodeHandle h = g_ar->LookUp(in);
    if (h == ZARCHIVE_INVALID_NODE) return 0;
    if (g_ar->IsFile(h)) { if (size) *size = g_ar->GetFileSize(h); return 1; }
    return 2;
}
FILE* open(const std::string& in) {
    ZArchiveNodeHandle h;
    uint64_t size;
    {
        std::lock_guard<std::mutex> lk(g_m);
        h = g_ar->LookUp(in, true, false);
        if (h == ZARCHIVE_INVALID_NODE) return nullptr;
        size = g_ar->GetFileSize(h);
    }
    cookie_io_functions_t io{c_read, nullptr, c_seek, c_close};
    FILE* f = fopencookie(new Cookie{h, 0, size}, "rb", io);
    if (f) {
        setvbuf(f, nullptr, _IOFBF, 64 << 10);
        std::lock_guard<std::mutex> lk(g_m);
        g_sizes[f] = size;
    }
    return f;
}
bool size_of(FILE* f, uint64_t& size) {
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_sizes.find(f);
    if (it == g_sizes.end()) return false;
    size = it->second;
    return true;
}
void forget(FILE* f) { std::lock_guard<std::mutex> lk(g_m); g_sizes.erase(f); }
bool list(const std::string& in, std::vector<std::pair<std::string, int64_t>>& out) {
    std::lock_guard<std::mutex> lk(g_m);
    ZArchiveNodeHandle h = g_ar->LookUp(in, false, true);
    if (h == ZARCHIVE_INVALID_NODE) return false;
    ZArchiveReader::DirEntry e;
    for (uint32_t i = 0; i < g_ar->GetDirEntryCount(h); i++)
        if (g_ar->GetDirEntry(h, i, e)) {
            int64_t sz = -1;
            if (e.isFile) {
                ZArchiveNodeHandle fh = g_ar->LookUp(in + "/" + std::string(e.name), true, false);
                sz = fh == ZARCHIVE_INVALID_NODE ? 0 : (int64_t)g_ar->GetFileSize(fh);
            }
            out.emplace_back(std::string(e.name), sz);
        }
    return true;
}
}  // namespace wua
#endif

std::string host_path_exact(const std::string& guest) {
    std::string p = guest;
    auto map = [&](const char* prefix, const std::string& root) -> bool {
        size_t n = strlen(prefix);
        if (p.compare(0, n, prefix) == 0) {
            p = root + p.substr(n);
            return true;
        }
        return false;
    };
    if (map("/vol/content", config::game_dir + "/content")) return p;
    if (map("/vol/code", config::game_dir + "/code")) return p;
    if (map("/vol/meta", config::game_dir + "/meta")) return p;
    if (map("/vol/save", config::save_dir)) return p;
    if (!p.empty() && p[0] != '/') return config::game_dir + "/content/" + p;  // relative to cwd (/vol/content)
    return config::game_dir + p;
}

#ifndef _WIN32
// Wii U volumes are case-insensitive (the game asks for Common/Audiores, the disc folder is AudioRes);
// case-sensitive host file systems (Linux, case-sensitive APFS) need the path resolved one component
// at a time. Exact matches cost one stat(); resolved directories are cached. Components that don't
// exist yet (new save files) keep the guest's spelling.
std::string resolve_case(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) == 0) return p;
    static std::mutex mu;
    static std::unordered_map<std::string, std::string> dirs;  // lower-cased dir path -> host dir path
    std::lock_guard<std::mutex> lk(mu);
    auto lower = [](std::string s) { for (char& ch : s) ch = (char)tolower((unsigned char)ch); return s; };
    std::string cur = p.compare(0, 1, "/") == 0 ? "/" : "";
    size_t pos = cur.size();
    while (pos <= p.size()) {
        size_t e = p.find('/', pos);
        if (e == std::string::npos) e = p.size();
        std::string comp = p.substr(pos, e - pos);
        pos = e + 1;
        if (comp.empty()) { if (e == p.size()) break; continue; }
        std::string base = cur.empty() ? "" : (cur == "/" ? "/" : cur + "/");
        std::string cand = base + comp;
        std::string key = lower(cand);
        if (auto it = dirs.find(key); it != dirs.end()) { cur = it->second; continue; }
        if (stat(cand.c_str(), &st) != 0) {
            std::string found;
            if (DIR* d = opendir(cur.empty() ? "." : cur.c_str())) {
                while (dirent* de = readdir(d))
                    if (!strcasecmp(de->d_name, comp.c_str())) { found = de->d_name; break; }
                closedir(d);
            }
            if (found.empty()) return cand + (e < p.size() ? p.substr(e) : "");  // not there: keep the rest as asked
            cand = base + found;
        }
        if (e < p.size()) dirs[key] = cand;  // only directories are cached
        cur = cand;
    }
    return cur;
}
#endif

std::string host_path(const std::string& guest) {
#if defined(_WIN32) || defined(__SWITCH__)
    return host_path_exact(guest);  // Windows file systems are case-insensitive
#else
    return resolve_case(host_path_exact(guest));
#endif
}

// Optional content mods affect read-only content access only.
std::string read_path(const std::string& guest,const std::string& mode="rb") {
    auto override=mods::content::replacement(guest,mode);
    if(!override.empty())if(const char* trace=std::getenv("WWHD_TEST_CONTENT_TRACE");trace&&std::string(trace)=="1")LOG("[content-mod] read %s -> %s",guest.c_str(),override.c_str());
    return override.empty()?host_path(guest):override;
}

void make_parent_dirs(const std::string& path) {
    std::error_code ec; std::filesystem::create_directories(std::filesystem::path(path).parent_path(),ec);
}

void fill_stat(uint32_t out, const struct stat& st) {
    memset(mem::ptr(out), 0, 0x64);
    bool dir = S_ISDIR(st.st_mode);
    st32(out + 0x00, dir ? 0x80000000u : 0x01000000u);
    st32(out + 0x04, 0x666);
    st32(out + 0x10, dir ? 0 : (uint32_t)st.st_size);
    st32(out + 0x14, dir ? 0 : (uint32_t)st.st_size);
}

int32_t open_file(const std::string& gpath, const std::string& mode, uint32_t out_handle) {
    std::string hp = read_path(gpath,mode);
    if (mode.find_first_of("wa") != std::string::npos) make_parent_dirs(hp);
    std::string m = mode;
    if (m.find('b') == std::string::npos) m += "b";
    FILE* f = nullptr;
#ifdef WWHD_WUA
    if (std::string in = wua::inner(hp); !in.empty()) {
        if (mode.find_first_of("wa+") != std::string::npos) return FS_ACCESS_ERROR;
        f = wua::open(in);
    } else
#endif
    f = fopen(hp.c_str(), m.c_str());
    TRACE("[fs] open %s (%s) -> %s", gpath.c_str(), mode.c_str(), f ? "ok" : "not found");
    if (!f) return FS_NOT_FOUND;
    std::lock_guard<std::mutex> lk(g_fs_mutex);
    uint32_t h = g_next_handle++;
    g_files[h] = {f, gpath, m};
    st32(out_handle, h);
    return FS_OK;
}

FILE* file(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_fs_mutex);
    auto it = g_files.find(h);
    return it == g_files.end() ? nullptr : it->second.f;
}

int32_t stat_path(const std::string& gpath, uint32_t out) {
    struct stat st;
    const std::string path = read_path(gpath);  // a mod's replacement, else the game's file
#ifdef WWHD_WUA
    if (std::string in = wua::inner(path); !in.empty()) {
        uint64_t size = 0;
        int k = wua::kind(in, &size);
        if (!k) return FS_NOT_FOUND;
        memset(&st, 0, sizeof st);
        st.st_mode = k == 2 ? S_IFDIR : S_IFREG;
        st.st_size = (off_t)size;
        fill_stat(out, st);
        return FS_OK;
    }
#endif
    if (stat(path.c_str(), &st) != 0) return FS_NOT_FOUND;
    fill_stat(out, st);
    return FS_OK;
}

int32_t open_dir(const std::string& gpath, uint32_t out_handle) {
#ifdef WWHD_WUA
    if (std::string in = wua::inner(host_path(gpath)); !in.empty()) {
        OpenDir od{nullptr, host_path(gpath), gpath};
        if (!wua::list(in, od.listed)) return FS_NOT_FOUND;
        std::lock_guard<std::mutex> lk(g_fs_mutex);
        uint32_t h = g_next_handle++;
        g_dirs[h] = std::move(od);
        st32(out_handle, h);
        return FS_OK;
    }
#endif
    DIR* d = opendir(host_path(gpath).c_str());
    TRACE("[fs] opendir %s -> %s", gpath.c_str(), d ? "ok" : "not found");
    if (!d) return FS_NOT_FOUND;
    std::lock_guard<std::mutex> lk(g_fs_mutex);
    uint32_t h = g_next_handle++;
    g_dirs[h] = {d, host_path(gpath), gpath};
    st32(out_handle, h);
    return FS_OK;
}

}  // namespace

// ---------------------------------------------------------------- FS
HLE(coreinit, FSInit) {}
HLE(coreinit, FSShutdown) {}
HLE(coreinit, FSAddClient) { ret(c, 0); }
HLE(coreinit, FSDelClient) { ret(c, 0); }
HLE(coreinit, FSInitCmdBlock) { memset(mem::ptr(arg(c, 0)), 0, 0xA80); }
HLE(coreinit, FSSetCmdPriority) { ret(c, 0); }
HLE(coreinit, FSSetStateChangeNotification) {}
HLE(coreinit, FSGetVolumeState) { ret(c, 1); }  // FS_VOLSTATE_READY
HLE(coreinit, FSGetLastError) { ret(c, 0); }
HLE(coreinit, FSGetLastErrorCodeForViewer) { ret(c, 0); }
HLE(coreinit, FSGetCwd) { mem::write_cstr(arg(c, 2), "/vol/content", arg(c, 3)); ret(c, FS_OK); }

HLE(coreinit, FSOpenFile) {
    ret(c, open_file(mem::read_cstr(arg(c, 2)), mem::read_cstr(arg(c, 3)), arg(c, 4)));
}

HLE(coreinit, FSCloseFile) {
    std::lock_guard<std::mutex> lk(g_fs_mutex);
    auto it = g_files.find(arg(c, 2));
    if (it != g_files.end()) {
#ifdef WWHD_WUA
        wua::forget(it->second.f);
#endif
        fclose(it->second.f);
        g_files.erase(it);
    }
    ret(c, FS_OK);
}

HLE(coreinit, FSReadFile) {
    uint32_t dst = arg(c, 2), size = arg(c, 3), count = arg(c, 4);
    FILE* f = file(arg(c, 5));
    if (!f || size == 0) { ret(c, 0); return; }
    size_t n;
    {
        BlockingScope b;  // the calling thread waits for the disc; others on its core run
        // the kernel writes guest memory here: write-protected texture pages would fail the read
        // (EFAULT) instead of faulting into the write tracker (write_watch.h)
        wwatch::HostWrite w(dst, (uint32_t)std::min<uint64_t>((uint64_t)size * count, 0x100000000ull - dst));
        n = fread(mem::ptr(dst), 1, (size_t)size * count, f);
    }
#ifdef __SWITCH__
    // The Wii U reads files by DMA; here the CPU wrote them: clean the cache for the GPU's
    // in-place reads of MEM2 and announce the pages to the texture watch.
    switch_dc_store(mem::ptr(dst), n);
#endif
    ret(c, (uint32_t)(n / size));
}

HLE(coreinit, FSWriteFile) {
    uint32_t src = arg(c, 2), size = arg(c, 3), count = arg(c, 4);
    FILE* f = file(arg(c, 5));
    if (!f || size == 0) { ret(c, 0); return; }
    size_t n;
    {
        BlockingScope b;
        n = fwrite(mem::ptr(src), 1, (size_t)size * count, f);
        fflush(f);
    }
    ret(c, (uint32_t)(n / size));
}

HLE(coreinit, FSSetPosFile) {
    FILE* f = file(arg(c, 2));
    ret(c, f && host::file_seek(f,arg(c,3),SEEK_SET) == 0 ? FS_OK : FS_ACCESS_ERROR);
}

HLE(coreinit, FSGetStat) { ret(c, stat_path(mem::read_cstr(arg(c, 2)), arg(c, 3))); }

HLE(coreinit, FSGetStatFile) {
    FILE* f = file(arg(c, 2));
    struct stat st;
#ifdef WWHD_WUA
    if (uint64_t size; f && wua::size_of(f, size)) {
        memset(&st, 0, sizeof st);
        st.st_mode = S_IFREG;
        st.st_size = (off_t)size;
        fill_stat(arg(c, 3), st);
        ret(c, FS_OK);
        return;
    }
#endif
    if (!f || fstat(fileno(f), &st) != 0) { ret(c, FS_NOT_FOUND); return; }
    fill_stat(arg(c, 3), st);
    ret(c, FS_OK);
}

HLE(coreinit, FSOpenDir) { ret(c, open_dir(mem::read_cstr(arg(c, 2)), arg(c, 3))); }

HLE(coreinit, FSReadDir) {
    uint32_t out = arg(c, 3);
    std::lock_guard<std::mutex> lk(g_fs_mutex);
    auto it = g_dirs.find(arg(c, 2));
    if (it == g_dirs.end()) { ret(c, FS_NOT_DIR); return; }
    if (!it->second.d) {  // .wua directory
        if (it->second.read >= it->second.listed.size()) { ret(c, FS_END); return; }
        auto& [name, size] = it->second.listed[it->second.read++];
        struct stat st;
        memset(&st, 0, sizeof st);
        st.st_mode = size < 0 ? S_IFDIR : S_IFREG;
        st.st_size = size < 0 ? 0 : (off_t)size;
        fill_stat(out, st);
        mem::write_cstr(out + 0x64, name.c_str(), 256);
        ret(c, FS_OK);
        return;
    }
    struct dirent* de;
    while ((de = readdir(it->second.d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        struct stat st;
        auto path=it->second.path + "/" + de->d_name;
        auto replacement=mods::content::replacement(it->second.gpath + "/" + de->d_name);
        if(!replacement.empty())path=std::move(replacement);
        if (stat(path.c_str(), &st) != 0) continue;
        fill_stat(out, st);
        mem::write_cstr(out + 0x64, de->d_name, 256);
        it->second.read++;
        ret(c, FS_OK);
        return;
    }
    ret(c, FS_END);
}

HLE(coreinit, FSCloseDir) {
    std::lock_guard<std::mutex> lk(g_fs_mutex);
    auto it = g_dirs.find(arg(c, 2));
    if (it != g_dirs.end()) {
        if (it->second.d) closedir(it->second.d);
        g_dirs.erase(it);
    }
    ret(c, FS_OK);
}

// ---------------------------------------------------------------- nn_save
// Saves live in <save dir>/<account slot or "common">/<path>
static std::string save_path(uint32_t slot, const std::string& p) {
    std::string base = slot == 0xFF ? "/vol/save/common" : "/vol/save/user";
    return base + (p.empty() || p[0] == '/' ? "" : "/") + p;
}

HLE(nn_save, SAVEInit) { ret(c, 0); }
HLE(nn_save, SAVEShutdown) {}
HLE(nn_save, SAVEInitSaveDir) {
    make_parent_dirs(host_path(save_path(arg(c, 0), "x")));
    ret(c, 0);
}
HLE(nn_save, SAVEFlushQuota) { ret(c, 0); }
HLE(nn_save, SAVEOpenFile) {
    ret(c, open_file(save_path(arg(c, 2), mem::read_cstr(arg(c, 3))), mem::read_cstr(arg(c, 4)), arg(c, 5)));
}
HLE(nn_save, SAVEGetStat) { ret(c, stat_path(save_path(arg(c, 2), mem::read_cstr(arg(c, 3))), arg(c, 4))); }
HLE(nn_save, SAVEOpenDir) {
    std::string gp = save_path(arg(c, 2), mem::read_cstr(arg(c, 3)));
    make_parent_dirs(host_path(gp) + "/");
    ret(c, open_dir(gp, arg(c, 4)));
}

// ---------------------------------------------------------------- save states: open files
#include <algorithm>
#include <vector>

#include "../savestate.h"
void fs_ss_save(ss::Writer& w) {
    std::lock_guard<std::mutex> lk(g_fs_mutex);
    w.u32(g_next_handle);
    std::vector<uint32_t> keys;
    for (auto& [h, f] : g_files) keys.push_back(h);
    std::sort(keys.begin(), keys.end());
    w.u32((uint32_t)keys.size());
    for (uint32_t h : keys) {
        OpenFile& f = g_files[h];
        w.u32(h);
        w.str(f.path);
        w.str(f.mode);
        w.u64((uint64_t)host::file_tell(f.f));
    }
    keys.clear();
    for (auto& [h, d] : g_dirs) keys.push_back(h);
    std::sort(keys.begin(), keys.end());
    w.u32((uint32_t)keys.size());
    for (uint32_t h : keys) {
        w.u32(h);
        w.str(g_dirs[h].gpath);
        w.u32(g_dirs[h].read);
    }
}

void fs_ss_load(ss::Reader& r) {
    std::lock_guard<std::mutex> lk(g_fs_mutex);
    uint32_t next = r.u32();
    for (auto& [h, f] : g_files) fclose(f.f);
    g_files.clear();
    for (auto& [h, d] : g_dirs) closedir(d.d);
    g_dirs.clear();
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        uint32_t h = r.u32();
        std::string gp = r.str(), mode = r.str();
        uint64_t pos = r.u64();
        // reopening must not truncate or create: writers continue in update mode
        std::string m = mode.find_first_of("wa+") != std::string::npos ? "r+b" : "rb";
        FILE* f = fopen(read_path(gp,m).c_str(), m.c_str());
        if (!f) {
            LOG("[savestate] cannot reopen %s", gp.c_str());
            continue;
        }
        host::file_seek(f,pos,SEEK_SET);
        g_files[h] = {f, gp, mode};
    }
    n = r.u32();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        uint32_t h = r.u32();
        std::string gp = r.str();
        uint32_t read = r.u32();
        DIR* d = opendir(host_path(gp).c_str());
        if (!d) continue;
        OpenDir od{d, host_path(gp), gp, 0};
        while (od.read < read) {
            struct dirent* de = readdir(d);
            if (!de) break;
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            od.read++;
        }
        g_dirs[h] = od;
    }
    g_next_handle = std::max(g_next_handle, next);
}

#ifdef WWHD_WUA
// whole file below game_dir, read from the .wua archive (core.cpp's RPX loader); false if not archived
bool wua_read_file(const std::string& host, std::vector<uint8_t>& out) {
    std::string in = wua::inner(host);
    if (in.empty()) return false;
    FILE* f = wua::open(in);
    if (!f) return false;
    uint64_t size = 0;
    wua::size_of(f, size);
    out.resize(size);
    size_t n = fread(out.data(), 1, size, f);
    wua::forget(f);
    fclose(f);
    return n == size;
}
#endif
