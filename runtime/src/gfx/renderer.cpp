// Renderer selection, start-up with fallback, and restart (see renderer.h).
#include "renderer.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "runtime.h"
#ifdef WWHD_SDL_HOST
#include <SDL3/SDL_messagebox.h>
#endif

#ifdef WWHD_HAS_METAL
// AppKit host (gfx/display.mm): persistent settings, start-up message on the TV window
namespace gfx {
bool host_setting(const char* key, std::string& value);
void set_host_setting(const char* key, const std::string& value);
void show_startup_notice(const std::string& title, const std::string& text);
}  // namespace gfx
#endif
// decompiler output flavour (gx2/decompiler_glue.cpp): MSL for Metal, GLSL/SPIR-V for Vulkan
void select_decompiler_api(render::Api api);

namespace render {

const Backend* g_backend = nullptr;

namespace {
Api g_requested = Api::Metal;
std::string g_reason;            // why the requested renderer did not start
std::vector<std::string> g_args; // restart arguments (without --renderer)
std::atomic<bool> g_shut{false};
std::atomic<bool> g_pref_changed{false};

bool parse(const char* s, Api& out) {
    if (!s) return false;
    if (!strcasecmp(s, "metal")) { out = Api::Metal; return true; }
    if (!strcasecmp(s, "vulkan") || !strcasecmp(s, "vk") || !strcasecmp(s, "moltenvk")) { out = Api::Vulkan; return true; }
    return false;
}

const Backend* backend_for(Api a) {
#ifdef WWHD_HAS_METAL
    if (a == Api::Metal) return &metal_backend();
#endif
#ifdef WWHD_HAS_VULKAN
    if (a == Api::Vulkan) return &vulkan_backend();
#endif
    return nullptr;
}

Api default_api() {
#ifdef WWHD_HAS_METAL
    return Api::Metal;
#else
    return Api::Vulkan;
#endif
}
}  // namespace

const char* api_name(Api a) { return a == Api::Vulkan ? "Vulkan" : "Metal"; }
const char* api_key(Api a) { return a == Api::Vulkan ? "vulkan" : "metal"; }
bool compiled(Api a) { return backend_for(a) != nullptr; }
bool can_choose() { return compiled(Api::Metal) && compiled(Api::Vulkan); }
Api active() { return g_backend ? g_backend->api : g_requested; }
Api requested() { return g_requested; }
std::string fallback_reason() { return g_reason; }
uint64_t frame_count() { return g_backend ? g_backend->frame_count() : 0; }

Api preferred() {
    Api a = default_api();
#ifdef WWHD_HAS_METAL
    std::string v;
    if (gfx::host_setting("renderer", v)) parse(v.c_str(), a);
#endif
    return compiled(a) ? a : default_api();
}

bool restart_pending() { return g_pref_changed && preferred() != active(); }

void set_preferred(Api a) {
    g_pref_changed = true;
#ifdef WWHD_HAS_METAL
    gfx::set_host_setting("renderer", api_key(a));
#endif
    LOG("[gfx] renderer for the next start: %s", api_name(a));
}

void choose(int argc, char** argv) {
    Api a = preferred();
    const char* why = "default";
#ifdef WWHD_HAS_METAL
    std::string saved;
    if (gfx::host_setting("renderer", saved)) why = "saved setting";
#endif
    if (parse(getenv("WWHD_RENDERER_RUNTIME"), a)) why = "WWHD_RENDERER_RUNTIME";
    for (int i = 1; i < argc; i++) {
        const char* v = nullptr;
        if (!strncmp(argv[i], "--renderer=", 11)) v = argv[i] + 11;
        else if (!strcmp(argv[i], "--renderer") && i + 1 < argc) v = argv[i + 1];
        if (v) {
            if (parse(v, a)) why = "command line";
            else LOG("[gfx] unknown renderer '%s' (metal or vulkan)", v);
        }
    }
    if (!compiled(a)) {
        g_reason = std::string(api_name(a)) + " is not built into this executable";
        LOG("[gfx] %s; using %s", g_reason.c_str(), api_name(default_api()));
        a = default_api();
    }
    g_requested = a;
    LOG("[gfx] renderer: %s (%s)", api_name(a), why);
    set_restart_args(argc, argv);
}

void set_restart_args(int argc, char** argv) {
    g_args.clear();
    for (int i = 0; i < argc; i++) {
        if (i > 0 && !strncmp(argv[i], "--renderer=", 11)) continue;
        if (i > 0 && !strcmp(argv[i], "--renderer")) { i++; continue; }
        g_args.push_back(argv[i]);
    }
}

void init() {
    const Backend* b = backend_for(g_requested);
    select_decompiler_api(b->api);
    g_backend = b;
    try {
        b->init();
        LOG("[gfx] %s renderer started", api_name(b->api));
        return;
    } catch (const std::exception& e) {
        g_reason = e.what();
    } catch (...) {
        g_reason = "unknown error";
    }
    const Backend* fallback = nullptr;
#ifdef WWHD_HAS_METAL
    if (b->api != Api::Metal) fallback = &metal_backend();
#endif
    if (!fallback) {
#ifdef WWHD_SDL_HOST
        // a player starts the game from the launcher, without a terminal: say why in a message box
        // (old graphics driver, no Vulkan) and end without a crash report
        LOG("FATAL: %s renderer could not start: %s", api_name(b->api), g_reason.c_str());
        const std::string text = std::string("The ") + api_name(b->api) + " renderer could not start.\n\n" + g_reason;
        const char* hidden = getenv("WWHD_HIDDEN_WINDOWS");  // test runs: nothing pops up
        if (!hidden || !*hidden || !strcmp(hidden, "0"))
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Wind Waker HD", text.c_str(), nullptr);
        fflush(stderr);
        _Exit(1);
#else
        fatal("%s renderer could not start: %s", api_name(b->api), g_reason.c_str());
#endif
    }
    LOG("[gfx] %s renderer could not start: %s -- falling back to %s", api_name(b->api), g_reason.c_str(),
        api_name(fallback->api));
    select_decompiler_api(fallback->api);
    g_backend = fallback;
    fallback->init();  // Metal: a failure here is fatal (no Metal device)
    LOG("[gfx] %s renderer started (fallback)", api_name(fallback->api));
#ifdef WWHD_HAS_METAL
    gfx::show_startup_notice(std::string(api_name(b->api)) + " could not start",
                             "The game is running with " + std::string(api_name(fallback->api)) + " instead.\n\n" + g_reason +
                                 "\n\nGraphics > Renderer selects the renderer for the next start.");
#endif
}

void run_main_loop() { g_backend->run_main_loop(); }

void shutdown() {
    if (g_shut.exchange(true) || !g_backend || !g_backend->shutdown) return;
    g_backend->shutdown();
}

bool restart() {
#if defined(_WIN32) || defined(__SWITCH__)
    return false;  // Switch: Vulkan is the only renderer
#else
    std::string exe;
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) exe = buf;
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) exe.assign(buf, (size_t)n);
#endif
    if (exe.empty() || g_args.empty()) return false;
    LOG("[gfx] restarting with the %s renderer", api_name(preferred()));
    shutdown();
    std::vector<char*> av;
    for (auto& s : g_args) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);
    // the environment's WWHD_RENDERER_RUNTIME would override the new choice
    unsetenv("WWHD_RENDERER_RUNTIME");
    fflush(stdout);
    fflush(stderr);
    execv(exe.c_str(), av.data());
    LOG("[gfx] restart failed: %s", strerror(errno));
    return false;
#endif
}

}  // namespace render
