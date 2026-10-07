// The SDL3 subset of SDL.h in this directory, on libnx: one full-screen Vulkan window (nwindow +
// VK_NN_vi_surface), the Switch controllers as one gamepad, and audout for sound.
#define VK_USE_PLATFORM_VI_NN 1
#include <switch.h>

#include "SDL3/SDL_vulkan.h"
#include "SDL3/SDL_messagebox.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <malloc.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" void switch_set_helper_thread(int core, int priority);  // switch_host.cpp

struct SDL_Window {
    SDL_WindowID id;
    int w, h;
    std::string title;
};
struct SDL_Gamepad {
    SDL_JoystickID id;
};
struct SDL_AudioStream {
    SDL_AudioStreamCallback callback;
    void* userdata;
    float gain = 1.0f;
    std::vector<int16_t> staging;  // filled by the callback through SDL_PutAudioStreamData
    std::thread* thread = nullptr;
    std::atomic<bool> run{false};
};

namespace {
const char* g_error = "";
std::mutex g_events_m;
std::deque<SDL_Event> g_events;
std::vector<SDL_Window*> g_windows;
PadState g_pad;
bool g_pad_init = false;
u64 g_buttons = 0;
HidAnalogStickState g_lstick{}, g_rstick{};
bool g_announced_pad = false;
u64 g_vib_stop = 0;  // system tick at which the running rumble ends (0: none; rumble section below)
void send_vibration(float low, float high);

void init_pad() {
    if (g_pad_init) return;
    g_pad_init = true;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
}
}  // namespace

bool SDL_Init(SDL_InitFlags flags) { return SDL_InitSubSystem(flags); }
bool SDL_InitSubSystem(SDL_InitFlags flags) {
    if (flags & SDL_INIT_GAMEPAD) init_pad();
    return true;
}
const char* SDL_GetError(void) { return g_error; }
void SDL_free(void* p) { free(p); }
bool SDL_AddEventWatch(SDL_EventFilter, void*) { return true; }
bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags, const char* title, const char* message, SDL_Window*) {
    fprintf(stderr, "[%s] %s\n", title ? title : "message", message ? message : "");
    return true;
}
SDL_Window** SDL_GetWindows(int* count) {
    auto** list = (SDL_Window**)malloc(sizeof(SDL_Window*) * (g_windows.size() + 1));
    std::copy(g_windows.begin(), g_windows.end(), list);
    list[g_windows.size()] = nullptr;
    if (count) *count = int(g_windows.size());
    return list;
}
bool SDL_SetTextInputArea(SDL_Window*, const SDL_Rect*, int) { return true; }
const char* SDL_GetScancodeName(SDL_Scancode) { return ""; }
void SDL_Delay(Uint32 ms) { svcSleepThread((s64)ms * 1000000); }
Uint64 SDL_GetTicks(void) { return armTicksToNs(armGetSystemTick()) / 1000000; }
SDL_Surface* SDL_CreateSurface(int, int, SDL_PixelFormat) { return nullptr; }
void SDL_DestroySurface(SDL_Surface*) {}
bool SDL_SetWindowIcon(SDL_Window*, SDL_Surface*) { return false; }
void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window*, const SDL_DialogFileFilter*,
                            int, const char*, bool) { if (callback) callback(userdata, nullptr, -1); }
void SDL_ShowOpenFolderDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window*, const char*, bool) {
    if (callback) callback(userdata, nullptr, -1);
}

SDL_Window* SDL_CreateWindow(const char* title, int, int, SDL_WindowFlags) {
    auto* w = new SDL_Window{(SDL_WindowID)(g_windows.size() + 1), 1280, 720, title ? title : ""};
    g_windows.push_back(w);
    return w;
}
SDL_WindowID SDL_GetWindowID(SDL_Window* window) { return window ? window->id : 0; }
bool SDL_GetWindowSizeInPixels(SDL_Window* window, int* w, int* h) {
    if (!window) return false;
    *w = window->w;
    *h = window->h;
    return true;
}
bool SDL_SetWindowTitle(SDL_Window* window, const char* title) {
    if (window) window->title = title;
    return true;
}
const char* SDL_GetWindowTitle(SDL_Window* window) { return window ? window->title.c_str() : ""; }
SDL_Window* SDL_GetKeyboardFocus(void) { return g_windows.empty() ? nullptr : g_windows[0]; }
bool SDL_SetWindowRelativeMouseMode(SDL_Window*, bool) { return false; }
bool SDL_StartTextInput(SDL_Window*) {
    g_error = "no keyboard (use the software keyboard)";
    return false;
}
bool SDL_StopTextInput(SDL_Window*) { return true; }
// one full-screen picture: always shown, always "full screen"
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* window) { return window ? SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN : 0; }
bool SDL_SetWindowFullscreen(SDL_Window*, bool) { return true; }
bool SDL_ShowWindow(SDL_Window*) { return true; }
bool SDL_HideWindow(SDL_Window*) { return true; }
bool SDL_GetWindowSize(SDL_Window* window, int* w, int* h) { return SDL_GetWindowSizeInPixels(window, w, h); }
float SDL_GetWindowPixelDensity(SDL_Window*) { return 1.0f; }
SDL_Window* SDL_GetWindowFromID(SDL_WindowID id) {
    for (auto* w : g_windows)
        if (w->id == id) return w;
    return nullptr;
}
bool SDL_SetHint(const char*, const char*) { return true; }

bool SDL_PushEvent(SDL_Event* event) {
    std::lock_guard<std::mutex> lk(g_events_m);
    g_events.push_back(*event);
    return true;
}
bool SDL_PollEvent(SDL_Event* event) {
    {
        std::lock_guard<std::mutex> lk(g_events_m);
        if (!g_events.empty()) {
            *event = g_events.front();
            g_events.pop_front();
            return true;
        }
    }
    // once per poll round: system events (home menu, exit request) and the controllers
    if (!appletMainLoop()) {
        memset(event, 0, sizeof *event);
        event->type = SDL_EVENT_QUIT;
        return true;
    }
    if (g_vib_stop && armGetSystemTick() >= g_vib_stop) {
        send_vibration(0.0f, 0.0f);
        g_vib_stop = 0;
    }
    if (g_pad_init) {
        padUpdate(&g_pad);
        g_buttons = padGetButtons(&g_pad);
        g_lstick = padGetStickPos(&g_pad, 0);
        g_rstick = padGetStickPos(&g_pad, 1);
        if (!g_announced_pad) {
            g_announced_pad = true;
            memset(event, 0, sizeof *event);
            event->type = SDL_EVENT_GAMEPAD_ADDED;
            event->gdevice.which = 1;
            return true;
        }
    }
    return false;
}

SDL_JoystickID* SDL_GetGamepads(int* count) {
    init_pad();
    auto* ids = (SDL_JoystickID*)malloc(sizeof(SDL_JoystickID) * 2);
    ids[0] = 1;
    ids[1] = 0;
    *count = 1;
    g_announced_pad = true;
    return ids;
}
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID id) { return new SDL_Gamepad{id}; }
void SDL_CloseGamepad(SDL_Gamepad* gamepad) { delete gamepad; }
bool SDL_GamepadConnected(SDL_Gamepad*) { return true; }
bool SDL_GetGamepadButton(SDL_Gamepad*, SDL_GamepadButton button) {
    // positional names: the Switch layout matches the Wii U GamePad (A right, B bottom)
    static const u64 kMap[SDL_GAMEPAD_BUTTON_COUNT] = {
        HidNpadButton_B, HidNpadButton_A, HidNpadButton_Y, HidNpadButton_X,
        HidNpadButton_Minus, 0, HidNpadButton_Plus, HidNpadButton_StickL,
        HidNpadButton_StickR, HidNpadButton_L, HidNpadButton_R,
        HidNpadButton_Up, HidNpadButton_Down, HidNpadButton_Left, HidNpadButton_Right,
    };
    if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) return false;
    return (g_buttons & kMap[button]) != 0;
}
Sint16 SDL_GetGamepadAxis(SDL_Gamepad*, SDL_GamepadAxis axis) {
    auto clamp = [](int v) { return (Sint16)std::clamp(v, -32768, 32767); };
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return clamp(g_lstick.x);
    case SDL_GAMEPAD_AXIS_LEFTY: return clamp(-g_lstick.y);  // SDL: down is positive
    case SDL_GAMEPAD_AXIS_RIGHTX: return clamp(g_rstick.x);
    case SDL_GAMEPAD_AXIS_RIGHTY: return clamp(-g_rstick.y);
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return (g_buttons & HidNpadButton_ZL) ? 32767 : 0;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return (g_buttons & HidNpadButton_ZR) ? 32767 : 0;
    default: return 0;
    }
}

// ---- rumble: HD rumble on the handheld Joy-Cons and on player 1 (Pro Controller or a Joy-Con pair),
// both motors at the frequencies of an Xbox-style rumble; SDL_PollEvent ends it after the duration
namespace {
HidVibrationDeviceHandle g_vib[4];
int g_vib_count = 0;
u32 g_vib_style = ~0u;  // player 1's controller style the handles were made for
void update_vibration_devices() {
    const u32 style = hidGetNpadStyleSet(HidNpadIdType_No1);
    if (style == g_vib_style && g_vib_count) return;
    g_vib_style = style;
    g_vib_count = 0;
    if (R_SUCCEEDED(hidInitializeVibrationDevices(g_vib, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld)))
        g_vib_count = 2;
    const HidNpadStyleTag tag = (style & HidNpadStyleTag_NpadFullKey) ? HidNpadStyleTag_NpadFullKey
                              : (style & HidNpadStyleTag_NpadJoyDual) ? HidNpadStyleTag_NpadJoyDual
                                                                      : HidNpadStyleTag_NpadFullKey;
    if (style && R_SUCCEEDED(hidInitializeVibrationDevices(g_vib + g_vib_count, 2, HidNpadIdType_No1, tag)))
        g_vib_count += 2;
}
void send_vibration(float low, float high) {
    HidVibrationValue v[4];
    for (auto& x : v) {
        x.amp_low = low;
        x.freq_low = 160.0f;
        x.amp_high = high;
        x.freq_high = 320.0f;
    }
    if (g_vib_count) hidSendVibrationValues(g_vib, v, g_vib_count);
}
}  // namespace
bool SDL_RumbleGamepad(SDL_Gamepad*, Uint16 low, Uint16 high, Uint32 ms) {
    update_vibration_devices();
    if (!g_vib_count) return false;
    send_vibration(low / 65535.0f, high / 65535.0f);
    g_vib_stop = (low || high) ? armGetSystemTick() + armNsToTicks(u64(ms ? ms : 1) * 1000000) : 0;
    return true;
}

// ---- audio: audout, 48 kHz stereo S16, a few 20 ms buffers in flight
namespace {
constexpr int kFrames = 960;
constexpr int kBuffers = 3;

void audio_thread(SDL_AudioStream* s) {
    switch_set_helper_thread(2, 0x2B);
    const size_t data = kFrames * 4, size = (data + 0xFFF) & ~size_t(0xFFF);
    AudioOutBuffer bufs[kBuffers] = {};
    for (auto& b : bufs) {
        b.buffer = memalign(0x1000, size);
        memset(b.buffer, 0, size);
        b.buffer_size = size;
        b.data_size = data;
    }
    auto fill = [&](AudioOutBuffer& b) {
        s->staging.clear();
        s->callback(s->userdata, s, (int)data, (int)data);
        auto* out = (int16_t*)b.buffer;
        size_t n = std::min(s->staging.size(), (size_t)kFrames * 2);
        for (size_t i = 0; i < n; i++)
            out[i] = (int16_t)std::clamp((int)(s->staging[i] * s->gain), -32768, 32767);
        memset(out + n, 0, (kFrames * 2 - n) * 2);
        audoutAppendAudioOutBuffer(&b);
    };
    for (auto& b : bufs) fill(b);
    while (s->run.load()) {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100000000ull)) || !released) continue;
        fill(*released);
    }
}
}  // namespace

SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID, const SDL_AudioSpec* spec, SDL_AudioStreamCallback callback, void* userdata) {
    if (!spec || spec->format != SDL_AUDIO_S16 || spec->channels != 2 || spec->freq != 48000) {
        g_error = "only 48 kHz stereo S16 output";
        return nullptr;
    }
    if (R_FAILED(audoutInitialize())) {
        g_error = "audoutInitialize failed";
        return nullptr;
    }
    auto* s = new SDL_AudioStream;
    s->callback = callback;
    s->userdata = userdata;
    s->staging.reserve(kFrames * 2);
    return s;
}
bool SDL_PutAudioStreamData(SDL_AudioStream* s, const void* buf, int len) {
    const int16_t* p = (const int16_t*)buf;
    s->staging.insert(s->staging.end(), p, p + len / 2);
    return true;
}
bool SDL_SetAudioStreamGain(SDL_AudioStream* s, float gain) {
    s->gain = gain;
    return true;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* s) {
    if (s->run.exchange(true)) return true;
    if (R_FAILED(audoutStartAudioOut())) {
        g_error = "audoutStartAudioOut failed";
        s->run = false;
        return false;
    }
    s->thread = new std::thread(audio_thread, s);  // never joined (libnx cannot detach)
    return true;
}
void SDL_DestroyAudioStream(SDL_AudioStream* s) {
    if (!s) return;
    if (s->run.exchange(false) && s->thread) s->thread->join();
    audoutStopAudioOut();
    delete s;
}

// ---- Vulkan surface
char const* const* SDL_Vulkan_GetInstanceExtensions(Uint32* count) {
    static const char* const kExt[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME};
    *count = 2;
    return kExt;
}
bool SDL_Vulkan_CreateSurface(SDL_Window* window, VkInstance instance, const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    if (!window || window != g_windows.front()) {
        g_error = "only one window can be shown";
        return false;
    }
    auto create = (PFN_vkCreateViSurfaceNN)vkGetInstanceProcAddr(instance, "vkCreateViSurfaceNN");
    if (!create) {
        g_error = "vkCreateViSurfaceNN unavailable";
        return false;
    }
    VkViSurfaceCreateInfoNN ci{VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};
    ci.window = nwindowGetDefault();
    if (create(instance, &ci, allocator, surface) != VK_SUCCESS) {
        g_error = "vkCreateViSurfaceNN failed";
        return false;
    }
    return true;
}
