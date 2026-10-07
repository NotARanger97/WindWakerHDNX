// Wind Waker HD recompiled: entry point.
#if defined(__SWITCH__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#elif !defined(_WIN32)
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#endif
#include <ctime>
#include <filesystem>
#include "platform/host.h"
#ifdef _WIN32
#include <timeapi.h>
#endif

#include <cstring>
#include <string>
#include <thread>

#include "gfx/renderer.h"
#include "mods/cemu_pack.h"
#include "gx2/gx2.h"
#include "recomp_table.h"
#include "crash_addr.h"
#include "crashrec.h"
#include "input.h"
#include "mods/manager.h"
#include "mods/packages.h"
#include "runtime.h"
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // main() becomes SDL_main, called by SDLActivity
namespace interp { void set_mode(int); }
#endif

#ifdef WWHD_HAS_VULKAN
namespace gfxvk {
int renderer_smoke_test();
namespace vk { void warm_up_shader_cache(); }
void warm_up_pipelines();
}
#endif
#ifdef WWHD_HAS_METAL
int gfx_headstart_warm();  // gfx/shader_headstart.mm
#endif

void mem_setup_heaps(uint32_t data_end);
void trace_dump(FILE* f, unsigned last);
void mem_init_data_imports(uint32_t alloc_slot, uint32_t alloc_ex_slot, uint32_t free_slot);

#if defined(__SWITCH__)
// platform/switch/switch_host.cpp: libnx exception handler (crash log on the SD card)
void switch_platform_init();
std::string switch_game_dir();  // platform/switch/switch_host.cpp
static void install_crash_handler() {}
#elif !defined(_WIN32)
// Memory crashes (SIGSEGV/SIGBUS/...): the report goes to the terminal and to
// captures/crash-<time>.log (registers, guest return chain, host backtrace, crash recovery's
// automatic state, the last log lines). Only write() and preformatted text after the crash.
static int g_crash_fd = -1;
static void crash_out(int fd, const char* s, size_t n) {
    if (write(2, s, n) < 0) {}
    if (fd >= 0 && write(fd, s, n) < 0) {}
}
static void crash_log_only(int fd, const char* s, size_t n) {
    if (fd >= 0 && write(fd, s, n) < 0) {}
}
static void crash_handler(int sig, siginfo_t* si, void* uctx) {
    uintptr_t a = (uintptr_t)si->si_addr;
    uintptr_t base = (uintptr_t)PPC_MEM_BASE;
    char path[96];
    {
        mkdir("captures", 0755);
        time_t t = time(nullptr);
        struct tm tmv;
        localtime_r(&t, &tmv);
        strftime(path, sizeof path, "captures/crash-%Y%m%d-%H%M%S.log", &tmv);
        g_crash_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    const int fd = g_crash_fd;
    char buf[256];
    int n;
    if (a >= base && a < base + 0x100000000ull)
        n = snprintf(buf, sizeof buf, "\nCRASH: signal %d at guest address %08X\n", sig, (unsigned)(a - base));
    else
        n = snprintf(buf, sizeof buf, "\nCRASH: signal %d at host address %p\n", sig, si->si_addr);
    crash_out(fd, buf, n);
    // the faulting instruction and the module holding it (a driver, an overlay's layer, the game)
    char where[384], mpath[512] = "", line[1024];
    if (uintptr_t pc = crash_addr::context_pc(uctx)) {
        crash_addr::describe(where, sizeof where, pc, mpath, sizeof mpath);
        n = crash_addr::fit(snprintf(line, sizeof line, "  host pc %p%s\n", (void*)pc, where), sizeof line);
        crash_out(fd, line, n);
        if (mpath[0]) {
            n = crash_addr::fit(snprintf(line, sizeof line, "  module: %s\n", mpath), sizeof line);
            crash_out(fd, line, n);
        }
    }
    // a host fault address inside a module (a write to read-only data, a jump into a data section)
    if (!(a >= base && a < base + 0x100000000ull) && crash_addr::describe(where, sizeof where, a)) {
        n = crash_addr::fit(snprintf(line, sizeof line, "  fault address %p%s\n", si->si_addr, where), sizeof line);
        crash_out(fd, line, n);
    }
    Cpu* c = threads::current();
    if (c) {
        n = snprintf(buf, sizeof buf, "  guest lr=%08X ctr=%08X cr=%08X\n", c->lr, c->ctr, ppc_mfcr(c));
        crash_out(fd, buf, n);
        for (int i = 0; i < 32; i += 8) {
            n = snprintf(buf, sizeof buf, "  r%-2d %08X %08X %08X %08X %08X %08X %08X %08X\n", i, c->r[i], c->r[i + 1],
                         c->r[i + 2], c->r[i + 3], c->r[i + 4], c->r[i + 5], c->r[i + 6], c->r[i + 7]);
            crash_out(fd, buf, n);
        }
        // guest return chain (back-chain words on the guest stack; names: build/names.tsv)
        crash_out(fd, "  guest call chain:", 19);
        uint32_t sp = c->r[1];
        for (int i = 0; i < 24 && sp >= 0x10000000u && sp < 0xF0000000u; i++) {
            uint32_t prev = ld32(sp);
            if (!prev || prev <= sp || prev - sp > 0x100000u) break;
            n = snprintf(buf, sizeof buf, " %08X", ld32(prev + 4));
            crash_out(fd, buf, n);
            sp = prev;
        }
        crash_out(fd, "\n", 1);
    }
    crash_addr::host_backtrace(fd, crash_out, uctx);
    crashrec::crash_note(fd, crash_out);
    if (fd >= 0) {
        crash_log_only(fd, "\n--- last log lines ---\n", 24);
        log_ring_write(fd, crash_log_only);
        close(fd);
        n = snprintf(buf, sizeof buf, "[crash] wrote %s\n", path);
        crash_out(-1, buf, n);
    }
    if (g_ppc_trace) {
        FILE* f = fopen("trace_dump.txt", "w");
        if (f) { trace_dump(f, 3000); fclose(f); if (write(2, "[trace] wrote trace_dump.txt\n", 29) < 0) {} }
    }
    input::stop_rumble_now();  // controllers keep their last motor level after the process (issue #35)
    _exit(128 + sig);
}

static void install_crash_handler() {
    static char altstack[1 << 16];
    stack_t ss{};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    crash_addr::prime();
}

#else
static void win_crash_out(int fd, const char* s, size_t n) {
    fwrite(s, 1, n, stderr);
    if (fd >= 0) _write(fd, s, (unsigned)n);
}
static void win_crash_log_only(int fd, const char* s, size_t n) { if (fd >= 0) _write(fd, s, (unsigned)n); }
static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ex) {
    auto code=ex->ExceptionRecord->ExceptionCode;
    std::error_code ec; std::filesystem::create_directories("captures",ec);
    char path[96]; time_t t=time(nullptr); struct tm tmv; localtime_s(&tmv,&t);
    strftime(path,sizeof path,"captures/crash-%Y%m%d-%H%M%S.log",&tmv);
    int fd=_open(path,_O_WRONLY|_O_CREAT|_O_TRUNC|_O_BINARY,_S_IREAD|_S_IWRITE);
    char buf[256]; int n;
    // the module holding the faulting instruction (issue #41: a driver or an overlay's Vulkan layer)
    char where[384], mpath[512]="", line[1024];
    using crash_addr::fit;
    crash_addr::describe(where,sizeof where,(uintptr_t)ex->ExceptionRecord->ExceptionAddress,mpath,sizeof mpath);
    n=fit(snprintf(line,sizeof line,"CRASH: Windows exception %08lX at %p%s\n",code,ex->ExceptionRecord->ExceptionAddress,where),sizeof line); win_crash_out(fd,line,n);
    if(mpath[0]){n=fit(snprintf(line,sizeof line,"  module: %s\n",mpath),sizeof line); win_crash_out(fd,line,n);}
    // access violations (and in-page errors): read / write / execute, and of which address
    const EXCEPTION_RECORD* er=ex->ExceptionRecord;
    if((code==EXCEPTION_ACCESS_VIOLATION||code==EXCEPTION_IN_PAGE_ERROR)&&er->NumberParameters>=2){
        const ULONG_PTR kind=er->ExceptionInformation[0], target=er->ExceptionInformation[1];
        const char* what=kind==0?"read":kind==1?"write":kind==8?"execute (DEP)":"access";
        const uintptr_t gbase=(uintptr_t)PPC_MEM_BASE;
        n=fit(snprintf(line,sizeof line,"  %s: %s of address %p",code==EXCEPTION_ACCESS_VIOLATION?"access violation":"in-page error",
                       what,(void*)target),sizeof line);
        if(target>=gbase&&target<gbase+0x100000000ull) n+=fit(snprintf(line+n,sizeof line-n," (guest address %08X)",(unsigned)(target-gbase)),sizeof line-n);
        else if(crash_addr::describe(where,sizeof where,target)) n+=fit(snprintf(line+n,sizeof line-n,"%s",where),sizeof line-n);
        if(n>(int)sizeof line-2) n=(int)sizeof line-2;
        line[n++]='\n'; win_crash_out(fd,line,n);
    }
    if(Cpu* c=threads::current()){n=snprintf(buf,sizeof buf,"guest lr=%08X ctr=%08X\n",c->lr,c->ctr); win_crash_out(fd,buf,n);}
    crash_addr::host_backtrace(fd,win_crash_out,ex->ContextRecord);
    crashrec::crash_note(fd,win_crash_out);
    if(fd>=0){win_crash_log_only(fd,"\n--- last log lines ---\n",24); log_ring_write(fd,win_crash_log_only); _close(fd); fprintf(stderr,"[crash] wrote %s\n",path);}
    if(g_ppc_trace) { FILE* f=fopen("trace_dump.txt","w"); if(f){trace_dump(f,3000);fclose(f);} }
    input::stop_rumble_now();  // controllers keep their last motor level after the process (issue #35)
    return EXCEPTION_EXECUTE_HANDLER;
}
static void install_crash_handler() { SetUnhandledExceptionFilter(crash_handler); crash_addr::prime(); }
#endif
static void init_data_imports() {
    uint32_t alloc = 0, alloc_ex = 0, free_ = 0;
    for (unsigned i = 0; i < g_recomp_import_count; i++) {
        const RecompImport& im = g_recomp_imports[i];
        if (im.is_func) continue;
        std::string n = im.name;
        if (n == "MEMAllocFromDefaultHeap") alloc = im.addr;
        else if (n == "MEMAllocFromDefaultHeapEx") alloc_ex = im.addr;
        else if (n == "MEMFreeToDefaultHeap") free_ = im.addr;
        else if (n == "__gh_FOPEN_MAX") st32(im.addr, 20);
        else if (n == "environ") st32(im.addr, im.addr + 0x10);  // empty environment list
    }
    mem_init_data_imports(alloc, alloc_ex, free_);
}

// Portable mode (portable.txt next to the executable, see host::portable_user_dir): the macOS paths
// that do not go through host::config_dir() get their existing overrides pointed into the folder.
static void apply_portable_mode() {
    if (!host::portable()) return;
    const std::string u = host::portable_user_dir();
    std::error_code ec;
    std::filesystem::create_directories(u, ec);
    auto set = [](const char* k, const std::string& v) {
        if (getenv(k)) return;  // an explicit override wins
#ifdef _WIN32
        _putenv_s(k, v.c_str());
#else
        setenv(k, v.c_str(), 0);
#endif
    };
    set("WWHD_STATE_DIR", u + "/states");
#ifdef __APPLE__
    set("WWHD_DISPLAY_SETTINGS", u + "/display.plist");
    set("WWHD_SHADER_CACHE", u + "/shaders.bin");
#endif
}

int main(int argc, char** argv) {
    apply_portable_mode();
#ifdef _WIN32
    // Windows sleeps in steps of the system timer (15.6 ms by default): sleep_for(1 ms) took
    // 15.7 ms, the 3 ms AX frame loop ran in bursts and vsync waits alternated 15.7 / 31.5 ms.
    // 1 ms resolution for the whole process; Windows restores it when the process exits.
    timeBeginPeriod(1);
#endif
#ifdef __ANDROID__
    // Everything lives in the app's external files folder (Android/data/<package>/files), which a
    // computer can reach over USB: game/ (the extracted game: code, content, meta), save/, and the
    // settings and shader caches in config/. Relative paths (captures/) resolve there too.
    if (const char* dir = SDL_GetAndroidExternalStoragePath()) {
        if (chdir(dir) != 0) fprintf(stderr, "cannot enter %s\n", dir);
        setenv("XDG_CONFIG_HOME", (std::string(dir) + "/config").c_str(), 1);
        // Phones: the Vulkan renderer's validated opt-in CPU paths (docs/vulkan.md, "Opt-in CPU
        // experiments") and draw batching are on; measured on a Galaxy S25 Ultra, Outset Island,
        // they took the render thread from about 40 to 30 ms per frame. env.txt can turn any off.
        for (const char* name : {"WWHD_VK_REUSE_UNIFORM_SNAPSHOTS", "WWHD_VK_REUSE_FEEDBACK_IMAGES",
                                 "WWHD_VK_SKIP_REDUNDANT_BINDS", "WWHD_VK_DESCRIPTOR_RANKS",
                                 "WWHD_VK_PIPELINE_LOOKASIDE", "WWHD_VK_SHADER_ADDRESS_MEMO",
                                 "WWHD_VK_FETCH_MEMO", "WWHD_VK_SPECIALIZE_INDICES",
                                 "WWHD_VK_SHADER_STATE_MEMO", "WWHD_VK_SKIP_VERTEX_BINDS",
                                 "WWHD_VK_SAMPLER_MEMO", "WWHD_VK_SPARSE_HASH_MEMO",
                                 "WWHD_VK_SHADER_KEY_DIRTY", "WWHD_VK_REUSE_VERTEX_SNAPSHOTS",
                                 "WWHD_VK_VERTEX_HISTORY_REUSE"})
            setenv(name, "1", 0);
        setenv("WWHD_VK_DRAW_BATCH", "2048", 0);
        // 60 fps (frame interpolation) is on unless chosen otherwise (settings.ini, read when the
        // renderer starts); platform/perf_hint.cpp pauses it where the phone cannot keep up
        if (!getenv("WWHD_INTERP") && !getenv("WWHD_TRUE60")) interp::set_mode(1);
        // env.txt there: one NAME=value per line (the WWHD_ options of the README); # comments
        if (FILE* f = fopen("env.txt", "r")) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                char* eq = strchr(line, '=');
                if (line[0] == '#' || !eq || eq == line) continue;
                *eq = 0;
                setenv(line, eq + 1, 1);
                LOG("[boot] env.txt: %s=%s", line, eq + 1);
            }
            fclose(f);
        }
    }
#endif
    bool warm_shaders = false;
#ifdef __SWITCH__
    switch_platform_init();  // log file, working directory, crash handler
    config::game_dir = switch_game_dir();  // WWHD_GAME, a .wua, or sdmc:/switch/wwhd/game
    config::save_dir = "sdmc:/switch/wwhd/save";
    fprintf(stderr, "[switch] game: %s\n", config::game_dir.c_str());
#endif
#ifdef WWHD_HAS_VULKAN
    bool renderer_smoke = false;
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) config::game_dir = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) config::save_dir = argv[++i];
        else if (!strcmp(argv[i], "--trace")) g_trace_hle = true;
        else if (!strcmp(argv[i], "--warm-shaders")) warm_shaders = true;
#ifdef WWHD_HAS_VULKAN
        else if (!strcmp(argv[i], "--renderer-smoke")) renderer_smoke = true;
#endif
    }
    install_crash_handler();
    // test aid: WWHD_TEST_HOST_CRASH=1 crashes inside a system library (strlen of a bad pointer), so
    // the crash log's module names can be checked (CTest crash_log_module, runtime/tools/crash_log_test.cmake)
    if (getenv("WWHD_TEST_HOST_CRASH")) {
        LOG("[boot] WWHD_TEST_HOST_CRASH: crashing on purpose in the C library");
        size_t (*volatile len)(const char*) = strlen;
#if !defined(_WIN32) && !defined(__SWITCH__)
        // the C library's own strlen: zig links its own copy into the executable (Linux releases)
        if (void* f = dlsym(RTLD_DEFAULT, "strlen")) len = (size_t (*)(const char*))f;
#endif
        LOG("%zu", len((const char*)(uintptr_t)16));
    }
    // Metal or Vulkan: --renderer=, WWHD_RENDERER_RUNTIME, Graphics > Renderer (gfx/renderer.h)
    render::choose(argc, argv);
#ifdef WWHD_HAS_VULKAN
    if(renderer_smoke) {
        // GPU self-test of the Vulkan renderer (no game files): always Vulkan, no fallback
        int result = 1;
        host::with_autorelease_pool([&] {
            render::g_backend = &render::vulkan_backend();
            try {
                render::g_backend->init();
            } catch (const std::exception& e) {
                fprintf(stderr, "[renderer smoke] FAIL: Vulkan could not start: %s\n", e.what());
                return;
            }
            result = gfxvk::renderer_smoke_test();
        });
        return result;
    }
#endif
    mods::manager::load_saved();  // player choices, before the game starts
    mods::cemu::set_vulkan(render::requested()==render::Api::Vulkan);
    mods::packages::initialize();
    mem::init();
    auto valid_mod_memory = [](uint32_t address, size_t size) {
        if (size > 1024 * 1024) return false;
        uint64_t end = uint64_t(address) + size;
        return (address >= mem::kMem2Start && end <= mem::kMem2End) ||
               (address >= mem::kMem1 && end <= uint64_t(mem::kMem1) + mem::kMem1Size) ||
               (address >= mem::kFgBucket && end <= uint64_t(mem::kFgBucket) + mem::kFgBucketSize);
    };
    // Store noncapturing callbacks: native mods operate on guest data, on the game thread.
    static auto valid_memory = valid_mod_memory;
    mods::packages::set_memory_access(
        [](uint32_t a, void* out, size_t n) -> int {
            if (!out || !valid_memory(a, n)) return 0;
            memcpy(out, mem::ptr(a), n); return 1;
        },
        [](uint32_t a, const void* in, size_t n) -> int {
            if (!in || !valid_memory(a, n)) return 0;
            memcpy(mem::ptr(a), in, n); return 1;
        });

    LoadedModule m{};
    std::string rpx = config::game_dir + "/code/cking.rpx";
    if (!load_rpx(rpx, m)) fatal("cannot load %s", rpx.c_str());
    if (m.entry != g_recomp_entry_point) fatal("%s does not match the recompiled code", rpx.c_str());
    LOG("[boot] loaded %s: entry %08X sda %08X sda2 %08X data end %08X", rpx.c_str(), m.entry, m.sda_base, m.sda2_base,
        m.data_end);

    dispatch::init();
    init_data_imports();
    mem_setup_heaps(m.data_end);
    threads::init(m);

    uint32_t argv_arr = mem::runtime_alloc(16);
    uint32_t arg0 = mem::runtime_alloc(16);
    mem::write_cstr(arg0, "cking.rpx", 16);
    st32(argv_arr, arg0);
    // the game runs on its own threads; the process main thread belongs to the window system
    render::init();
    mods::cemu::set_vulkan(render::active()==render::Api::Vulkan);
#ifdef WWHD_HAS_VULKAN
    if (render::active() == render::Api::Vulkan) {
        gfxvk::vk::warm_up_shader_cache();
        gfxvk::warm_up_pipelines();
    }
#endif
    if (warm_shaders) {
        // compile the shader head start once (fills the macOS Metal shader cache), then quit
#ifdef WWHD_HAS_METAL
        if (render::active() == render::Api::Metal) return gfx_headstart_warm();
#endif
        fprintf(stderr, "--warm-shaders fills the Metal shader cache; the %s renderer compiles shaders on first use.\n",
                render::api_name(render::active()));
        return 1;
    }
    static LoadedModule mod = m;
    static uint32_t args = argv_arr;
    std::thread([] {
        threads::run_main(mod, 1, args);
        LOG("[boot] game main thread returned");
        std::exit(0);
    }).detach();
    render::run_main_loop();
    return 0;
}
