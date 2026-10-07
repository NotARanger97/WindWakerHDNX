#include <mutex>
// In-game settings overlay: the Dear ImGui user interface (see overlay.h). The renderers draw the
// resulting ImDrawData (gfx/overlay_metal.mm, gfx/vulkan/overlay.cpp); the hosts feed input and apply
// changes on their main thread (hostui.h).
#include "overlay.h"

#include <algorithm>
#include <cstdarg>
#include <functional>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "hostui.h"
#include "controls_view.h"
#include "text_entry.h"
#include "../aspect.h"
#include "../crashrec.h"
#include "../game_languages.h"
#include "../gfx/renderer.h"
#ifdef WWHD_HAS_VULKAN
#include "../gfx/vulkan/settings.h"
#endif
#include "../input.h"
#include "../input_map.h"
#include "../mods/climb.h"
#include "../mods/mods.h"
#include "../mods/manager.h"
#include "../mods/packages.h"
#include "../platform/keycodes.h"
#include "../rumble.h"
#include "../runtime.h"
#include "../savestate.h"

namespace interp {
int mode();  // 0 off, 1 frame interpolation, 2 true 60
void set_mode(int m);
bool paced_interpolation();  // 60 fps frame interpolation keeps the game's speed (skips in-between frames)
void set_paced_interpolation(bool on);
float paced_drawn_share();  // share of in-between frames drawn lately (paced and on), -1 otherwise
}
namespace gx2 { uint64_t flips_presented(); }

namespace overlay {
namespace {

using clock = std::chrono::steady_clock;
double now_s() { return std::chrono::duration<double>(clock::now().time_since_epoch()).count(); }

std::atomic<bool> g_open{false};
std::atomic<bool> g_perf{false};
std::atomic<float> g_density{1.0f};
std::atomic<bool> g_wait_release{false};  // just closed: the game sees no buttons until all are released
std::atomic<double> g_last_frame{0};      // frame() ran (alive(): the game's text prompt can show)
const bool g_no_host = getenv("WWHD_NO_HOST_INPUT") != nullptr;  // test runs ignore the user's input
// ... except keys a test posts itself (WWHD_TEST_POST_KEYS, gfx/input.mm; the hidden test window never
// has the user's keyboard)
const bool g_no_host_keys = g_no_host && !getenv("WWHD_TEST_POST_KEYS");
bool g_pad_b_used = false;  // B answered a dialog this frame: it does not also close the menu

// input events from the host's main thread, replayed into ImGui on the render thread
struct Event {
    enum Kind { Key, MousePos, MouseButton, Wheel, Focus } kind;
    int code = 0;
    bool down = false;
    float x = 0, y = 0;
    int mods = 0;
};
std::mutex g_mu;
std::vector<Event> g_events;
std::atomic<int> g_capture_key{-2};  // remap: key pressed while capturing (-2 none, -1 cancel, -3 clear)
std::atomic<bool> g_capturing_keys{false};
// keys held while the overlay is open (the Controls tab lights them; the game never sees them)
std::atomic<bool> g_held_keys[256];

void push(const Event& e) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_events.size() < 4096) g_events.push_back(e);
}

ImGuiKey imgui_key(int code) {
    switch (code) {
    case kVK_ANSI_A: return ImGuiKey_A; case kVK_ANSI_B: return ImGuiKey_B; case kVK_ANSI_C: return ImGuiKey_C;
    case kVK_ANSI_D: return ImGuiKey_D; case kVK_ANSI_E: return ImGuiKey_E; case kVK_ANSI_F: return ImGuiKey_F;
    case kVK_ANSI_G: return ImGuiKey_G; case kVK_ANSI_H: return ImGuiKey_H; case kVK_ANSI_I: return ImGuiKey_I;
    case kVK_ANSI_J: return ImGuiKey_J; case kVK_ANSI_K: return ImGuiKey_K; case kVK_ANSI_L: return ImGuiKey_L;
    case kVK_ANSI_M: return ImGuiKey_M; case kVK_ANSI_N: return ImGuiKey_N; case kVK_ANSI_O: return ImGuiKey_O;
    case kVK_ANSI_P: return ImGuiKey_P; case kVK_ANSI_Q: return ImGuiKey_Q; case kVK_ANSI_R: return ImGuiKey_R;
    case kVK_ANSI_S: return ImGuiKey_S; case kVK_ANSI_T: return ImGuiKey_T; case kVK_ANSI_U: return ImGuiKey_U;
    case kVK_ANSI_V: return ImGuiKey_V; case kVK_ANSI_W: return ImGuiKey_W; case kVK_ANSI_X: return ImGuiKey_X;
    case kVK_ANSI_Y: return ImGuiKey_Y; case kVK_ANSI_Z: return ImGuiKey_Z;
    case kVK_ANSI_0: return ImGuiKey_0; case kVK_ANSI_1: return ImGuiKey_1; case kVK_ANSI_2: return ImGuiKey_2;
    case kVK_ANSI_3: return ImGuiKey_3; case kVK_ANSI_4: return ImGuiKey_4; case kVK_ANSI_5: return ImGuiKey_5;
    case kVK_ANSI_6: return ImGuiKey_6; case kVK_ANSI_7: return ImGuiKey_7; case kVK_ANSI_8: return ImGuiKey_8;
    case kVK_ANSI_9: return ImGuiKey_9;
    case kVK_Return: case kVK_ANSI_KeypadEnter: return ImGuiKey_Enter;
    case kVK_Tab: return ImGuiKey_Tab;
    case kVK_Space: return ImGuiKey_Space;
    case kVK_Delete: return ImGuiKey_Backspace;
    case kVK_ForwardDelete: return ImGuiKey_Delete;
    case kVK_Escape: return ImGuiKey_Escape;
    case kVK_LeftArrow: return ImGuiKey_LeftArrow; case kVK_RightArrow: return ImGuiKey_RightArrow;
    case kVK_UpArrow: return ImGuiKey_UpArrow; case kVK_DownArrow: return ImGuiKey_DownArrow;
    case kVK_Home: return ImGuiKey_Home; case kVK_End: return ImGuiKey_End;
    case kVK_PageUp: return ImGuiKey_PageUp; case kVK_PageDown: return ImGuiKey_PageDown;
    case kVK_Shift: return ImGuiKey_LeftShift; case kVK_RightShift: return ImGuiKey_RightShift;
    case kVK_Control: return ImGuiKey_LeftCtrl; case kVK_RightControl: return ImGuiKey_RightCtrl;
    case kVK_Option: return ImGuiKey_LeftAlt; case kVK_RightOption: return ImGuiKey_RightAlt;
    case kVK_Command: return ImGuiKey_LeftSuper; case kVK_RightCommand: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
    }
}

// ---------------------------------------------------------------- UI state (render thread)
enum Tab { kSaves, kGraphics, kDisplay, kMods, kControls, kAbout, kTabs };
const char* const kTabNames[kTabs] = {"Saves", "Graphics", "Display", "Mods", "Controls", "Language / About"};
const char* const kTabIds[kTabs] = {"saves", "graphics", "display", "mods", "controls", "about"};

struct Ui {
    bool init = false;
    int tab = kSaves, select_tab = -1;  // select_tab: switch to this tab next frame (shoulder buttons, tests)
    double last_time = 0;
    // controller
    float values[input_map::kPadCount] = {};
    float prev[input_map::kPadCount] = {};
    double options_since = -1;
    bool options_latched = false;
    // remap capture: action, column (0, 1 keys; 2 controller)
    int cap_action = -1, cap_col = 0;
    bool cap_pad_released = false;
    double cap_started = 0;
    std::string cap_note;
    // save state slots, refreshed twice a second while the Saves tab is shown
    ss::SlotInfo slots[ss::kSlots + 1];
    crashrec::AutoInfo autos[crashrec::kAutoSlots + 1];
    double slots_time = -1;
    // performance
    double fps_t0 = 0, fps = 0;
    uint64_t fps_n0 = 0;
    float frame_ms[120] = {};
    int frame_i = 0;
    double last_present = 0;
    // language (applies on the next start)
    int language = -1;
    int language_at_start = -1;  // the language this start runs with (game_lang: the one on the disc)
    bool linearized = false;
    bool just_opened = false;
    bool list_view = false;  // Controls: the table instead of the drawing
    int hover_action = -1;   // Controls: input under the cursor (status line)
};
Ui U;

bool controller_down(int p) { return U.values[p] > 0.5f; }
bool controller_pressed(int p) { return U.values[p] > 0.5f && U.prev[p] <= 0.5f; }

void post_changed(std::function<void()> fn) {
    hostui::post([fn] {
        fn();
        hostui::graphics_changed();
    });
}

// ---------------------------------------------------------------- style
void setup_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 10;
    s.ChildRounding = 6;
    s.FrameRounding = 5;
    s.PopupRounding = 6;
    s.GrabRounding = 5;
    s.TabRounding = 6;
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(9, 5);
    s.ItemSpacing = ImVec2(10, 7);
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 1;  // unchecked boxes and radio buttons stay visible on the dark background
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    ImVec4* c = s.Colors;
    // deep sea blue with a teal accent
    c[ImGuiCol_WindowBg] = ImVec4(0.03f, 0.07f, 0.12f, 0.975f);  // sRGB targets blend in linear light: keep it dense
    c[ImGuiCol_ChildBg] = ImVec4(0.06f, 0.13f, 0.20f, 0.35f);
    c[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.11f, 0.18f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.30f, 0.62f, 0.70f, 0.45f);
    c[ImGuiCol_TitleBg] = ImVec4(0.05f, 0.20f, 0.30f, 1.0f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.06f, 0.27f, 0.38f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.22f, 0.32f, 0.85f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.34f, 0.46f, 0.90f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.17f, 0.42f, 0.54f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.12f, 0.36f, 0.48f, 0.90f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.50f, 0.62f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.60f, 0.70f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.12f, 0.36f, 0.48f, 0.70f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.50f, 0.62f, 0.85f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.60f, 0.70f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.08f, 0.24f, 0.34f, 0.90f);
    c[ImGuiCol_TabHovered] = ImVec4(0.20f, 0.52f, 0.64f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.16f, 0.44f, 0.56f, 1.0f);
    c[ImGuiCol_CheckMark] = ImVec4(0.55f, 0.95f, 0.85f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.80f, 0.80f, 1.0f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.55f, 0.95f, 0.90f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.62f, 0.70f, 0.35f);
    c[ImGuiCol_NavCursor] = ImVec4(1.0f, 0.85f, 0.35f, 1.0f);  // controller cursor: gold, easy to follow
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.08f, 0.24f, 0.34f, 1.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
}

// a font with arrows and accents if the system has one; Dear ImGui's own scalable font otherwise
void setup_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    static const char* const candidates[] = {
#if defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
#elif defined(_WIN32)
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
#endif
    };
    for (const char* path : candidates) {
        if (FILE* f = fopen(path, "rb")) {
            fclose(f);
            if (io.Fonts->AddFontFromFileTTF(path, 17.0f)) {
                LOG("[overlay] font %s", path);
                return;
            }
        }
    }
    ImFontConfig cfg;
    cfg.SizePixels = 17.0f;
    io.Fonts->AddFontDefaultVector(&cfg);
}

void init_context() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing written next to the game
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "wwhd";
    io.ConfigNavCaptureKeyboard = true;
    setup_style();
    setup_fonts();
    ImGui::GetStyle().FontSizeBase = 17.0f;
    U.init = true;
}

// ---------------------------------------------------------------- test switch
struct TestSwitch {
    bool open = false, perf = false;
    int tab = -1;
    uint64_t at = 1;
    bool done = false;
};
TestSwitch parse_test() {
    TestSwitch t;
    const char* e = getenv("WWHD_TEST_OVERLAY");
    if (!e || !*e) { t.done = true; return t; }
    std::string s = e;
    if (size_t at = s.find('@'); at != std::string::npos) {
        t.at = strtoull(s.c_str() + at + 1, nullptr, 10);
        s.resize(at);
    }
    std::string what = s, tab;
    if (size_t c = s.find(':'); c != std::string::npos) { what = s.substr(0, c); tab = s.substr(c + 1); }
    t.open = what == "open";
    t.perf = what == "perf";
    for (int i = 0; i < kTabs; i++)
        if (tab == kTabIds[i]) t.tab = i;
    return t;
}

// ---------------------------------------------------------------- controller
// debug: WWHD_TEST_PAD=320-400:B+LeftStickRight:0.8+LeftStickUp:0.4 holds host controller inputs
// (controls.json names, value 1 unless given) during TV frames 320..400. They reach only the
// Controls tab's live display and the game's text prompt (not the menu navigation, capture or the game).
void test_pad(float* v) {
    struct Hold { uint64_t from, to; int pad; float value; };
    static const std::vector<Hold> holds = [] {
        std::vector<Hold> out;
        const char* e = getenv("WWHD_TEST_PAD");
        while (e && *e) {
            unsigned long long a, b;
            int n;
            if (sscanf(e, "%llu-%llu:%n", &a, &b, &n) != 2) break;
            e += n;
            for (;;) {
                size_t len = strcspn(e, "+,");
                std::string item(e, len), id = item;
                float value = 1;
                if (size_t c = item.find(':'); c != std::string::npos) { id = item.substr(0, c); value = (float)atof(item.c_str() + c + 1); }
                int p = input_map::pad_from_id(id);
                if (p > 0) out.push_back({a, b, p, value});
                else LOG("[overlay] WWHD_TEST_PAD: unknown controller input %s", id.c_str());
                e += len;
                if (*e != '+') break;
                e++;
            }
            if (*e != ',') break;
            e++;
        }
        return out;
    }();
    const uint64_t f = render::frame_count();
    for (const Hold& h : holds)
        if (f >= h.from && f <= h.to) v[h.pad] = std::max(v[h.pad], h.value);
}

void read_controller() {
    std::copy(std::begin(U.values), std::end(U.values), std::begin(U.prev));
    if (g_no_host) {
        std::fill(std::begin(U.values), std::end(U.values), 0.0f);
        if (g_wait_release.load()) g_wait_release = false;
        return;
    }
    input::host_controller_values(U.values);
    using namespace input_map;
    const double t = now_s();
    // Home: toggles; Select / Minus (View / Share): held half a second opens, a press closes
    if (controller_pressed(kPadHome)) set_open(!is_open());
    if (controller_down(kPadOptions)) {
        if (!U.options_latched) {
            if (is_open()) {
                set_open(false);
                U.options_latched = true;
            } else {
                if (U.options_since < 0) U.options_since = t;
                if (t - U.options_since >= 0.5) {
                    set_open(true);
                    U.options_latched = true;
                }
            }
        }
    } else {
        U.options_since = -1;
        U.options_latched = false;
    }
    if (g_wait_release.load()) {
        bool any = false;
        for (int p = 1; p < kPadCount; p++) any |= U.values[p] > 0.3f;
        if (!any) g_wait_release = false;
    }
}

void feed_gamepad(ImGuiIO& io, bool enabled) {
    using namespace input_map;
    auto key = [&](ImGuiKey k, int p) { io.AddKeyAnalogEvent(k, enabled && U.values[p] > 0.5f, enabled ? U.values[p] : 0.0f); };
    key(ImGuiKey_GamepadFaceDown, kPadA);
    key(ImGuiKey_GamepadFaceRight, kPadB);
    key(ImGuiKey_GamepadFaceLeft, kPadX);
    key(ImGuiKey_GamepadFaceUp, kPadY);
    key(ImGuiKey_GamepadDpadUp, kPadDUp);
    key(ImGuiKey_GamepadDpadDown, kPadDDown);
    key(ImGuiKey_GamepadDpadLeft, kPadDLeft);
    key(ImGuiKey_GamepadDpadRight, kPadDRight);
    key(ImGuiKey_GamepadLStickUp, kPadLSUp);
    key(ImGuiKey_GamepadLStickDown, kPadLSDown);
    key(ImGuiKey_GamepadLStickLeft, kPadLSLeft);
    key(ImGuiKey_GamepadLStickRight, kPadLSRight);
    key(ImGuiKey_GamepadRStickUp, kPadRSUp);
    key(ImGuiKey_GamepadRStickDown, kPadRSDown);
    key(ImGuiKey_GamepadL2, kPadLT);
    key(ImGuiKey_GamepadR2, kPadRT);
    key(ImGuiKey_GamepadStart, kPadMenu);
}

// ---------------------------------------------------------------- widgets
void help(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", text);
}
void heading(const char* text) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.95f, 0.85f, 1.0f));
    ImGui::SeparatorText(text);
    ImGui::PopStyleColor();
}
void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.78f, 0.84f, 1.0f));
    ImGui::TextWrappedV(fmt, ap);
    ImGui::PopStyleColor();
    va_end(ap);
}
void warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.80f, 0.35f, 1.0f));
    ImGui::TextWrappedV(fmt, ap);
    ImGui::PopStyleColor();
    va_end(ap);
}
// a radio button that runs `set` on the main thread when chosen
bool radio(const char* label, bool active, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::RadioButton(label, active) && !active;
    ImGui::EndDisabled();
    return clicked;
}
bool check(const char* label, bool value, bool* out, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    bool v = value;
    bool changed = ImGui::Checkbox(label, &v);
    ImGui::EndDisabled();
    *out = v;
    return changed;
}

// ---------------------------------------------------------------- tabs
void refresh_slots(bool force) {
    double t = now_s();
    if (!force && U.slots_time >= 0 && t - U.slots_time < 0.5) return;
    U.slots_time = t;
    for (int i = 1; i <= ss::kSlots; i++) U.slots[i] = ss::slot_info(i);
    for (int i = 1; i <= crashrec::kAutoSlots; i++) U.autos[i] = crashrec::auto_info(i);
}

void tab_saves() {
    refresh_slots(false);
    std::string msg = ss::last_message();
    if (!msg.empty()) note("%s", msg.c_str());
    heading("Save states");
    note("A save state keeps the whole running game. Shift+F1..F5 save in game, F2..F5 load (F1 opens this menu).");
    if (ImGui::BeginTable("slots", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Saved");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        for (int i = 1; i <= ss::kSlots; i++) {
            const ss::SlotInfo& s = U.slots[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Slot %d", i);
            ImGui::TableNextColumn();
            if (!s.used) ImGui::TextDisabled("empty");
            else {
                std::string d = s.when + (s.area.empty() ? "" : "  -  " + s.area);
                if (!s.compatible) d += "  (incompatible)";
                ImGui::TextUnformatted(d.c_str());
            }
            ImGui::TableNextColumn();
            if (ImGui::Button("Save")) {
                ss::request_save(i);
                U.slots_time = -1;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!s.used || !s.compatible);
            if (ImGui::Button("Load")) {
                ss::request_load(i);
                set_open(false);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    heading("Crash Recovery");
    bool on;
    if (check("Crash Recovery (automatic state every few minutes)", crashrec::enabled(), &on)) crashrec::set_enabled(on);
    help("Saves the game into automatic states in the background and records the controller input since the latest "
         "one. After a crash, the crash log says how to load it. Saving freezes the game for a moment.");
    for (int i = 1; i <= crashrec::kAutoSlots; i++) {
        const crashrec::AutoInfo& a = U.autos[i];
        ImGui::PushID(100 + i);
        ImGui::BeginDisabled(!a.used);
        if (ImGui::Button("Load")) {
            crashrec::request_load(i);
            set_open(false);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (a.used) ImGui::Text("Automatic state %d:  %s%s%s", i, a.when.c_str(), a.area.empty() ? "" : "  -  ", a.area.c_str());
        else ImGui::TextDisabled("Automatic state %d:  empty", i);
        ImGui::PopID();
    }
}

void tab_graphics() {
    if (render::can_choose()) {
        heading("Renderer (takes effect after a restart)");
        for (render::Api a : {render::Api::Metal, render::Api::Vulkan}) {
            std::string label = a == render::Api::Vulkan ? "Vulkan (MoltenVK)" : "Metal";
            if (a == render::active()) label += "  - in use";
            if (radio(label.c_str(), render::preferred() == a, render::compiled(a)))
                post_changed([a] { render::set_preferred(a); });
            ImGui::SameLine();
        }
        ImGui::NewLine();
        if (!render::fallback_reason().empty()) note("This start: %s", render::fallback_reason().c_str());
        if (render::restart_pending()) {
            note("The game uses %s after a restart. Progress since your last save is lost.", render::api_name(render::preferred()));
            if (ImGui::Button("Restart now")) hostui::post([] { render::restart(); });
        }
    }
    heading("Frame rate");
    int m = interp::mode();
    if (radio("30 fps (original)", m == 0)) post_changed([] { interp::set_mode(0); });
    ImGui::SameLine();
    if (radio("60 fps: frame interpolation", m == 1)) post_changed([] { interp::set_mode(1); });
    ImGui::SameLine();
    if (radio("True 60 (experimental)", m == 2)) post_changed([] { interp::set_mode(2); });
    help("True 60 runs the game logic at 60 steps per second");
    if (m == 1) {
        bool paced;
        if (check("Keep game speed", interp::paced_interpolation(), &paced, !getenv("WWHD_INTERP_PACED")))
            post_changed([paced] { interp::set_paced_interpolation(paced); });
        help("When the computer cannot draw 60 frames a second, skip in-between frames instead of\n"
             "slowing the whole game down. The performance overlay shows how many are drawn.");
    }

    heading("Internal resolution");
    static const float scales[] = {1.0f, 1.5f, 2.0f, 3.0f};
    static const char* const names[] = {"1x  (1280x720)", "1.5x  (1920x1080)", "2x  (2560x1440)", "3x  (3840x2160)"};
    float cur = hostui::res_scale();
    for (int i = 0; i < 4; i++) {
        if (i) ImGui::SameLine();
        float s = scales[i];
        if (radio(names[i], std::fabs(cur - s) < 0.01f)) post_changed([s] { hostui::set_res_scale(s); });
    }

    heading("Aspect ratio");
    int am = aspect::mode();
    for (int i = aspect::kOriginal; i <= aspect::k32x9; i++) {
        if (i) ImGui::SameLine();
        if (radio(aspect::mode_name(i), am == i)) post_changed([i] { aspect::set_mode(i); });
    }

    heading("Effects");
    bool v;
    const bool ao_ok = render::feature_available(render::kFeatureAO);
    static const char* const ao[] = {"AO: original", "AO: centre fix", "AO: centre + noise fix"};
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        if (radio(ao[i], render::ao_mode() == i, ao_ok)) post_changed([i] { render::set_ao_mode(i); });
    }
    if (check("Full-size occlusion depth", render::ao_hires(), &v, render::feature_available(render::kFeatureAOHires)))
        post_changed([v] { render::set_ao_hires(v); });
    ImGui::SameLine(0, 30);
    if (check("16x anisotropic filtering", render::aniso(), &v, render::feature_available(render::kFeatureAniso)))
        post_changed([v] { render::set_aniso(v); });
    ImGui::SameLine(0, 30);
    if (check("Edge smoothing (FXAA)", render::fxaa(), &v, render::feature_available(render::kFeatureFXAA)))
        post_changed([v] { render::set_fxaa(v); });
    heading("Presentation (Vulkan)");
#ifdef WWHD_HAS_VULKAN
    {
        const bool vk = render::vulkan(), env = gfxvk::present_mode_from_env();
        static const char* const names[] = {"Vsync (smooth)", "Low latency", "Off (may tear)"};
        static const char* const tips[] = {"FIFO: every frame waits for the display's refresh; no tearing (default)",
                                           "MAILBOX: the newest finished frame is shown at the next refresh; no tearing, less delay",
                                           "IMMEDIATE: frames are shown at once; lowest delay, may tear"};
        // the mode in use: the setting, or vsync when this driver does not offer it
        const int in_use = gfxvk::present_mode_offered(gfxvk::present_mode()) ? gfxvk::present_mode() : gfxvk::kPresentFifo;
        for (int i = 0; i < gfxvk::kPresentModes; i++) {
            if (i) ImGui::SameLine();
            const bool offered = gfxvk::present_mode_offered(i);
            if (radio(names[i], in_use == i, vk && !env && offered)) post_changed([i] { gfxvk::set_present_mode(i); });
            if (vk && !offered) help("not offered by this driver");
            else help(tips[i]);
        }
        if (!vk) note("Used by the Vulkan renderer only (Metal always presents with vsync).");
        else if (env) note("WWHD_VK_PRESENT_MODE=%s is set for this start and takes precedence.", getenv("WWHD_VK_PRESENT_MODE"));
        else {
            std::string missing;
            for (int i = 1; i < gfxvk::kPresentModes; i++)
                if (!gfxvk::present_mode_offered(i)) missing += std::string(missing.empty() ? "" : ", ") + names[i];
            if (!missing.empty()) note("Not offered by this driver: %s.", missing.c_str());
        }
    }
#else
    ImGui::BeginDisabled();
    radio("Vsync (smooth)", true);
    ImGui::EndDisabled();
    note("This build has no Vulkan renderer.");
#endif
    heading("Overlay");
    if (check("Performance overlay (FPS, frame time)", perf_shown(), &v)) set_perf_shown(v);
}

void tab_display() {
    bool v;
    heading("Window");
    if (check("Full screen", hostui::fullscreen(), &v)) hostui::post([v] { hostui::set_fullscreen(v); });
#ifndef __ANDROID__  // always full screen there
    help(!strcmp(hostui::name(), "AppKit") ? "The TV window (Cmd+F); remembered for the next start"
                                           : "The TV window (F11 or Alt+Enter); remembered for the next start");
#endif
    heading("Picture scaling");
    static const char* const f[] = {"Smooth", "Sharp", "Integer scale (pixel exact)"};
    const bool fok = hostui::scale_filter_available();
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        if (radio(f[i], hostui::scale_filter() == i, fok)) hostui::post([i] { hostui::set_scale_filter(i); });
    }
    heading("GamePad screen");
    // the modes of display_modes.h; a host offers those it can show (no "Separate window" without one,
    // as on Android)
    const int mode = hostui::drc_mode();
    if (hostui::drc_modes() >= 4) {
        static const char* const modes[] = {"Separate window", "Picture-in-picture", "Automatic picture-in-picture", "Off",
                                            "GamePad only"};
        bool first = true;
        for (int i = 0; i < hostui::drc_modes() && i < 5; i++) {
            if (!hostui::drc_mode_offered(i)) continue;
            if (!first) ImGui::SameLine();
            first = false;
            if (radio(modes[i], mode == i)) hostui::post([i] { hostui::set_drc_mode(i); });
        }
        if (mode == 2) note("The overlay appears for a few seconds when the GamePad picture changes a lot (a menu opens).");
        if (mode == 4) note("Only the GamePad picture. In the game, Minus switches to Off-TV Play (the game on the GamePad).");
    }
    if (check("Show GamePad screen", hostui::drc_shown(), &v,
              hostui::drc_available() && !(hostui::drc_modes() >= 4 && (mode == 3 || mode == 4))))
        hostui::post([v] { hostui::show_drc(v); });
    if (hostui::drc_modes() >= 4 && (mode == 1 || mode == 2)) {
        // the overlay's corner, size and opacity (also in the Display menu on macOS)
        static const char* const corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Picture-in-picture corner:");
        for (int i = 0; i < 4; i++) {
            ImGui::SameLine();
            if (radio(corners[i], hostui::pip_corner() == i)) hostui::post([i] { hostui::set_pip_corner(i); });
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Size (of the TV picture's width):");
        for (int s : {20, 25, 33, 40}) {
            ImGui::SameLine();
            char l[16];
            snprintf(l, sizeof l, "%d%%##size", s);
            if (radio(l, std::fabs(hostui::pip_size() * 100 - s) < 0.5f)) hostui::post([s] { hostui::set_pip_size(s / 100.0f); });
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Opacity:");
        for (int o : {100, 85, 70, 50}) {
            ImGui::SameLine();
            char l[16];
            snprintf(l, sizeof l, "%d%%##opacity", o);
            if (radio(l, std::fabs(hostui::pip_opacity() * 100 - o) < 0.5f)) hostui::post([o] { hostui::set_pip_opacity(o / 100.0f); });
        }
        note("A click (or touch) on the GamePad picture touches the GamePad screen.");
    }
}

// The one-time native code confirmation (packages.h confirm_native): asked before enable() for each
// native package the player has not confirmed; Cancel (also B) leaves everything disabled.
struct NativeConfirm {
    std::string id, name;                                // the package the player is enabling
    std::vector<std::pair<std::string, std::string>> native;  // what needs confirming (it, dependencies)
    bool open_now = false;
};
void native_confirm_dialog(NativeConfirm& c, std::string& error) {
    using namespace mods::packages;
    const char* title = "Native code##native_confirm";
    if (c.open_now) { ImGui::OpenPopup(title); c.open_now = false; }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    bool answered = c.native.empty(), accept = false;
    if (!answered) {
        std::string names;
        for (size_t i = 0; i < c.native.size(); i++)
            names += (i == 0 ? "" : i + 1 == c.native.size() ? " and " : ", ") + c.native[i].second;
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
        ImGui::TextWrapped("%s %s native code. It runs with the game's full permissions and can do anything a program "
                           "on your computer can. Only enable mods from sources you trust.",
                           names.c_str(), c.native.size() == 1 ? "contains" : "contain");
        if (c.native.size() > 1 || c.native[0].first != c.id) note("Enabling %s also enables these packages.", c.name.c_str());
        note("You won't be asked again for this version of the mod.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        accept = ImGui::Button("Enable", ImVec2(120, 0));
        ImGui::SameLine();
        answered = ImGui::Button("Cancel", ImVec2(120, 0)) || accept;
        ImGui::SetItemDefaultFocus();  // keyboard and controller start on Cancel
        if (controller_pressed(input_map::kPadB)) { answered = true; accept = false; g_pad_b_used = true; }
    }
    if (accept) {
        bool ok = true;
        for (const auto& [id, name] : c.native) ok = ok && confirm_native(id, error);
        if (ok) enable(c.id, true, error);
    }
    if (answered) {
        c = NativeConfirm{};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void package_controls() {
    using namespace mods::packages;
    static std::string error;
    static NativeConfirm confirm;
    // debug: WWHD_TEST_MOD_ENABLE=<package id> ticks that package's checkbox once in test runs (the
    // confirmation then shows for unconfirmed native code)
    static const char* test_enable = g_no_host ? getenv("WWHD_TEST_MOD_ENABLE") : nullptr;
    static char source[1024] = {}, new_profile[65] = {};
    static std::mutex picker_mutex;
    static std::string picked;
    {
        std::lock_guard guard(picker_mutex);
        if (!picked.empty()) { snprintf(source, sizeof source, "%s", picked.c_str()); picked.clear(); }
    }
    heading("Profiles");
    auto current = current_profile();
    if (ImGui::BeginCombo("Active profile", current.c_str())) {
        for (const auto& name : profiles())
            if (ImGui::Selectable(name.c_str(), name == current)) select_profile(name, error);
        ImGui::EndCombo();
    }
    ImGui::InputText("New profile", new_profile, sizeof new_profile);
    ImGui::SameLine();
    if (ImGui::Button("Clone current") && create_profile(new_profile, error)) new_profile[0] = 0;
    static std::string delete_choice;
    auto saved_profiles = profiles();
    if (delete_choice == current || std::find(saved_profiles.begin(), saved_profiles.end(), delete_choice) == saved_profiles.end()) delete_choice.clear();
    if (delete_choice.empty()) for (const auto& name : saved_profiles) if (name != current) { delete_choice = name; break; }
    if (ImGui::BeginCombo("Delete profile", delete_choice.empty() ? "No inactive profile" : delete_choice.c_str())) {
        for (const auto& name : saved_profiles) if (name != current && ImGui::Selectable(name.c_str(), name == delete_choice)) delete_choice = name;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(delete_choice.empty());
    if (ImGui::Button("Delete selected")) { if (delete_profile(delete_choice, error)) delete_choice.clear(); }
    ImGui::EndDisabled();
    note("Your active profile is protected from deletion.");
    heading("Installed packages");
    note("Install a local package, a content/ mod folder or ZIP, or a replacement .pack file.");
    if (ImGui::Button("Choose package…")) hostui::choose_mod_source(false, [](std::string path) {
        std::lock_guard guard(picker_mutex); picked = std::move(path);
    });
    ImGui::SameLine();
    if (ImGui::Button("Choose folder…")) hostui::choose_mod_source(true, [](std::string path) {
        std::lock_guard guard(picker_mutex); picked = std::move(path);
    });
    ImGui::SetNextItemWidth(-140);
    ImGui::InputText("Package path", source, sizeof source);
    if (ImGui::Button("Install package")) install(source, error);
    ImGui::SameLine();
    if (ImGui::Button("Refresh packages")) refresh(error);
    auto path = directory();
    if (!path.empty()) note("Mods folder: %s", path.c_str());
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    auto installed = list();
    if (installed.empty()) note("No external packages installed.");
    for (const auto& mod : installed) {
        ImGui::PushID(mod.id.c_str());
        bool on = mod.enabled;
        bool toggled = ImGui::Checkbox("##package_enabled", &on);
        if (test_enable && mod.id == test_enable) { toggled = on = true; test_enable = nullptr; }
        if (toggled) {
            auto native = on ? unconfirmed_native(mod.id) : decltype(unconfirmed_native(mod.id)){};
            if (native.empty()) enable(mod.id, on, error);
            else confirm = {mod.id, mod.name, std::move(native), true};
        }
        ImGui::SameLine();
        if (installed.size() == 1) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        bool expanded = ImGui::TreeNode("details", "%s · %s", mod.name.c_str(), mod.version.c_str());
        if (expanded) {
            note("%s · %s", mod.kind == "native" ? "Native mod" : mod.kind == "cemu" ? "Cemu graphics / shader pack" : mod.kind == "content" ? "Model / texture / UI replacement" : "Built-in settings preset",
                 mod.pending_restart ? "Restart required" : mod.active ? "Active" : mod.enabled ? "Waiting for game update" : "Disabled");
            if (mod.kind == "native" && mod.compatible)
                note(mod.native_confirmed ? "Runs native code with the game's permissions (you confirmed this version)."
                                          : "Runs native code with the game's permissions. Enabling it asks you to confirm first.");
            if (!mod.author.empty()) note("By %s", mod.author.c_str());
            ImGui::TextWrapped("%s", mod.description.c_str());
            if (!mod.reason.empty()) ImGui::TextWrapped("%s", mod.reason.c_str());
            if (!mod.status.empty()) ImGui::TextWrapped("%s", mod.status.c_str());
            for (const auto& dependency : mod.dependencies) note("Requires %s", dependency.c_str());
            for (const auto& conflict : mod.conflicts) note("Conflicts with %s", conflict.c_str());
            for (const auto& option : mod.options) {
                ImGui::PushID(option.id.c_str());
                if (option.type == "bool") {
                    bool value = option.value.boolean;
                    if (ImGui::Checkbox(option.name.c_str(), &value)) configure(mod.id, option.id, value, error);
                } else if (option.type == "number") {
                    double value = option.value.number;
                    if (ImGui::SliderScalar(option.name.c_str(), ImGuiDataType_Double, &value,
                                            &option.minimum, &option.maximum, "%.3f"))
                        configure(mod.id, option.id, value, error);
                } else if (option.type == "enum") {
                    if (ImGui::BeginCombo(option.name.c_str(), option.value.text.c_str())) {
                        for (const auto& choice : option.choices)
                            if (ImGui::Selectable(choice.c_str(), choice == option.value.text))
                                configure(mod.id, option.id, choice, error);
                        ImGui::EndCombo();
                    }
                } else {
                    char value[1025]; snprintf(value, sizeof value, "%s", option.value.text.c_str());
                    if (ImGui::InputText(option.name.c_str(), value, sizeof value, ImGuiInputTextFlags_EnterReturnsTrue))
                        configure(mod.id, option.id, std::string(value), error);
                }
                if (!option.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", option.description.c_str());
                ImGui::PopID();
            }
            if(mod.restart_required) note("Changes apply on the next game start. Active files stay loaded until exit.");
            ImGui::BeginDisabled(mod.enabled || mod.active);
            if (ImGui::Button("Remove package")) remove(mod.id, error);
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    native_confirm_dialog(confirm, error);
}

void tab_mods() {
    bool v;
    heading("Mod manager");
    note("Built-in mods are part of this recomp build. Your choices are saved; all start off by default.");
    static ImGuiTextFilter search;
    search.Draw("Search mods", 260);
    static bool only_enabled = false;
    ImGui::SameLine();
    ImGui::Checkbox("Enabled only", &only_enabled);
    unsigned enabled = 0;
    for (const auto& entry : mods::manager::entries()) if (entry.enabled()) ++enabled;
    ImGui::Text("%u of %zu enabled", enabled, mods::manager::entries().size());
    ImGui::SameLine();
    if (ImGui::Button("Disable all mods")) hostui::post([] { mods::packages::disable_all(); });
    static std::string selected = "direct-camera";
    if (ImGui::BeginTable("mod_catalogue", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Mods", ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        unsigned shown = 0;
        for (const auto& entry : mods::manager::entries()) {
            bool on = entry.enabled();
            if (only_enabled && !on) continue;
            std::string searchable = std::string(entry.name)+" "+entry.category+" "+entry.description;
            if (!search.PassFilter(searchable.c_str())) continue;
            ++shown;
            ImGui::PushID(entry.id);
            if (ImGui::Checkbox("##enabled", &on)) {
                std::string id = entry.id;
                hostui::post([id, on] { mods::manager::set_enabled(id, on); });
            }
            ImGui::SameLine();
            if (ImGui::Selectable(entry.name, selected == entry.id)) selected = entry.id;
            ImGui::PopID();
        }
        if (!shown) note("No mods match your filter.");
        ImGui::TableNextColumn();
        if (const auto* entry = mods::manager::find(selected)) {
            ImGui::TextUnformatted(entry->name);
            note("%s · Built in · %s", entry->category, entry->enabled() ? "Enabled" : "Disabled");
            ImGui::TextWrapped("%s", entry->description);
            if (const char* override = std::getenv(entry->startup_env))
                note("%s=%s overrides the saved choice at startup.", entry->startup_env, override);
            if (entry->restart_required) note("Restart the game after changing this mod.");
            if (selected == "direct-camera") {
                float speed = mods::camera_speed();
                if (ImGui::SliderFloat("Camera speed", &speed, .5f, 2.f, "%.2fx"))
                    hostui::post([speed] { mods::set_camera_speed(speed); hostui::set("mod.direct-camera.speed", std::to_string(speed)); mods::packages::remember_option("direct-camera.speed", speed); });
            } else if (selected == "mouse-camera") {
                float sensitivity = mods::mouse_sensitivity();
                if (ImGui::SliderFloat("Sensitivity", &sensitivity, .08f, .3f, "%.3f"))
                    hostui::post([sensitivity] { mods::set_mouse_sensitivity(sensitivity); hostui::set("mod.mouse-camera.sensitivity", std::to_string(sensitivity)); mods::packages::remember_option("mouse-camera.sensitivity", sensitivity); });
            }
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    package_controls();
    ImGui::Separator();
    heading("Cheats (save in game to keep them)");
    if (ImGui::Button("Give all items")) mods::request_cheat(mods::kCheatItems);
    ImGui::SameLine();
    if (ImGui::Button("Master Sword + Mirror Shield")) mods::request_cheat(mods::kCheatSword);
    ImGui::SameLine();
    if (ImGui::Button("20 hearts, double magic, 5000 rupees")) mods::request_cheat(mods::kCheatStats);
    static const std::pair<const char*, int> inf[] = {{"Infinite health", mods::kInfHealth}, {"Infinite magic", mods::kInfMagic},
                                                      {"Infinite arrows and bombs", mods::kInfAmmo}};
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine(0, 24);
        int bit = inf[i].second;
        if (check(inf[i].first, mods::infinite(bit), &v)) hostui::post([bit, v] { mods::set_infinite(bit, v); });
    }
    heading("Story cheats (can break story events; use a spare save file)");
    if (ImGui::Button("All songs")) mods::request_cheat(mods::kCheatSongs);
    ImGui::SameLine();
    if (ImGui::Button("All Triforce shards")) mods::request_cheat(mods::kCheatTriforce);
    ImGui::SameLine();
    if (ImGui::Button("Map, compass, boss key")) mods::request_cheat(mods::kCheatDungeon);
    ImGui::SameLine();
    if (ImGui::Button("Small key")) mods::request_cheat(mods::kCheatKey);
}

void start_capture(int a, int col) {
    U.cap_action = a;
    U.cap_col = col;
    U.cap_pad_released = false;
    U.cap_started = now_s();
    U.cap_note.clear();
    g_capture_key = -2;
    g_capturing_keys = col != kColPad;
}

void clear_binding(int a, int col) {
    input_map::Mapping m = input_map::current();
    if (col == kColPad) m.pad[a] = input_map::kPadNone;
    else m.keys[a][col] = input_map::kNoKey;
    input_map::set_current(m);
}

void apply_capture() {
    if (U.cap_action < 0) return;
    using namespace input_map;
    input_map::Mapping m = input_map::current();
    const int a = U.cap_action, colm = U.cap_col;
    int code = g_capture_key.exchange(-2);
    bool done = false;
    if (code == -1) done = true;  // Esc: cancel
    else if (code == -3) {         // Backspace / Delete: clear
        if (colm == kColPad) m.pad[a] = kPadNone;
        else m.keys[a][colm == kColAny ? 0 : colm] = kNoKey;
        input_map::set_current(m);
        done = true;
    } else if (code >= 0 && colm != kColPad) {
        if (const char* r = input_map::reserved_key(code)) U.cap_note = std::string("That key is used for ") + r;
        else {
            m.keys[a][colm == kColAny ? 0 : colm] = code;
            input_map::set_current(m);
            done = true;
        }
    }
    if (!done) {
        // controller: wait for all inputs to be released, then take the first one pressed (controller
        // slots); for a key slot, B cancels
        int pressed = -1;
        bool any = false;
        for (int p = 1; p < kPadCount; p++) {
            any |= U.values[p] > 0.3f;
            if (U.values[p] > 0.6f && pressed < 0) pressed = p;
        }
        if (!U.cap_pad_released) U.cap_pad_released = !any;
        else if (pressed > 0) {
            if (colm == kColPad || colm == kColAny) {
                m.pad[a] = pressed;
                input_map::set_current(m);
            }
            done = true;  // (key slots: any controller input cancels)
        }
    }
    if (!done && now_s() - U.cap_started > 8) done = true;  // nothing pressed: give up
    if (done) {
        U.cap_action = -1;
        g_capturing_keys = false;
        g_wait_release = true;  // the controller input just assigned does not also act in the menu
    }
}

std::string summary(const input_map::Mapping& m, int a) {
    std::string out;
    for (int k : m.keys[a])
        if (k != input_map::kNoKey) out += (out.empty() ? "" : "  -  ") + input_map::key_label(k);
    if (m.pad[a] != input_map::kPadNone) out += (out.empty() ? "controller " : "  -  controller ") + std::string(input_map::pad_label(m.pad[a]));
    return out.empty() ? "not bound" : out;
}

// the table (List view)
void controls_list(input_map::Mapping& m, float h) {
    if (ImGui::BeginTable("map", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame,
                          ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Wii U input", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("Key 2");
        ImGui::TableSetupColumn("Controller");
        ImGui::TableHeadersRow();
        for (int a = 0; a < input_map::kActionCount; a++) {
            ImGui::PushID(a);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const bool conflict = input_map::has_conflict(m, a);
            if (conflict) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.62f, 0.20f, 1.0f));
            ImGui::TextUnformatted(input_map::action_label(a));
            if (conflict) ImGui::PopStyleColor();
            for (int col = 0; col < 3; col++) {
                ImGui::TableNextColumn();
                std::string label;
                if (U.cap_action == a && (U.cap_col == col || (U.cap_col == kColAny && col != kColKey1))) label = "...";
                else if (col < 2) label = m.keys[a][col] == input_map::kNoKey ? "-" : input_map::key_label(m.keys[a][col]);
                else label = m.pad[a] == input_map::kPadNone ? "-" : input_map::pad_label(m.pad[a]);
                label += "##c" + std::to_string(col);
                if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0)) && U.cap_action < 0) start_capture(a, col);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
                    (ImGui::IsItemFocused() && U.cap_action < 0 &&
                     (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))))
                    clear_binding(a, col);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void tab_controls() {
    bool v;
    input_map::Mapping m = input_map::current();
    // top: which controller the keyboard and controllers act as (the drawing follows), view switch
    const bool pro = input::pro_controller();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Keyboard and controllers act as");
    ImGui::SameLine();
    // the choice is saved (issue #26: every start went back to the GamePad and showed its screen)
    if (radio("Wii U GamePad", !pro))
        hostui::post([] { hostui::set_pro_controller(false); hostui::set("proController", "0"); });
    ImGui::SameLine();
    if (radio("Wii U Pro Controller", pro))
        hostui::post([] { hostui::set_pro_controller(true); hostui::set("proController", "1"); });
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize("List view").x -
                    ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::Checkbox("List view", &U.list_view);
    help("A plain table of all inputs instead of the controller drawing");
    // status line: capture prompt > note > hovered input > duplicates > help
    std::string status;
    ImVec4 sc(0.70f, 0.78f, 0.84f, 1.0f);
    if (U.cap_action >= 0) {
        sc = ImVec4(1.0f, 0.85f, 0.35f, 1.0f);
        status = std::string(input_map::action_label(U.cap_action)) + ": " +
                 (U.cap_col == kColPad   ? "press a controller button or stick direction (Esc cancels)"
                  : U.cap_col == kColAny ? "press a key or a controller button (Esc cancels, Backspace clears)"
                                         : "press a key (Esc cancels, Backspace clears)");
    } else if (!U.cap_note.empty()) {
        status = U.cap_note;
    } else if (U.hover_action >= 0) {
        status = std::string(input_map::action_label(U.hover_action)) + "  -  " + summary(m, U.hover_action) +
                 ".  A / Enter / click: rebind; X / Delete / right-click: clear.";
    } else if (int n = input_map::conflict_count(m)) {
        sc = ImVec4(1.0f, 0.62f, 0.20f, 1.0f);
        status = std::to_string(n) + " input(s) are bound to more than one action (orange).";
    } else {
        status = "Select a button on the controller or a chip to rebind it. Changes are saved to controls.json.";
    }
    const float bottom = ImGui::GetFrameHeightWithSpacing() * 2 + 4;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float h = std::max(160.0f, avail.y - bottom);
    U.hover_action = -1;
    if (U.list_view) {
        controls_list(m, h);
    } else {
        ControlsView cv;
        cv.m = m;
        cv.pro = pro;
        cv.cap_action = U.cap_action;
        cv.cap_col = U.cap_col;
        static float live[input_map::kPadCount];
        std::copy(std::begin(U.values), std::end(U.values), live);
        test_pad(live);
        cv.pad = live;
        cv.t = now_s();
        // live display: pressed inputs and stick deflection through the current mapping, from the raw
        // host state (as the Controls window's poll; camera inversion left out)
        static bool keys[256];
        for (int i = 0; i < 256; i++) keys[i] = g_held_keys[i].load(std::memory_order_relaxed);
        cv.keys = keys;
        for (int a = 0; a < input_map::kActionCount; a++) {
            float x = 0;
            for (int key : m.keys[a])
                if (key != input_map::kNoKey && keys[key]) x = 1;
            if (int p = m.pad[a]; p != input_map::kPadNone) x = std::max(x, live[p]);
            cv.act[a] = x;
        }
        input_map::Mapping mm = m;
        mm.invert_camera_y = false;
        input::PadState ks = input_map::keyboard_state(mm, keys), cs = input_map::controller_state(mm, live);
        const bool kl = ks.lx || ks.ly, kr = ks.rx || ks.ry;
        cv.stick[0][0] = kl ? ks.lx : cs.lx;
        cv.stick[0][1] = kl ? ks.ly : cs.ly;
        cv.stick[1][0] = kr ? ks.rx : cs.rx;
        cv.stick[1][1] = kr ? ks.ry : cs.ry;
        draw_controls(cv, avail.x, h);
        U.hover_action = cv.hover_action;
        if (cv.start_action >= 0) start_capture(cv.start_action, cv.start_col);
        if (cv.clear_action >= 0) clear_binding(cv.clear_action, cv.clear_col);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, sc);
    ImGui::TextUnformatted(status.c_str());
    ImGui::PopStyleColor();
    float dz = m.deadzone * 100.0f;
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("Stick dead zone", &dz, 0.0f, 90.0f, "%.0f%%")) {
        m.deadzone = std::clamp(dz / 100.0f, 0.0f, 0.9f);
        input_map::set_current(m);
    }
    ImGui::SameLine(0, 24);
    if (check("Invert camera up/down", m.invert_camera_y, &v)) {
        m.invert_camera_y = v;
        input_map::set_current(m);
    }
    ImGui::SameLine(0, 24);
    // issue #35: a way to keep the controller motors still (saved; WWHD_RUMBLE=0 starts with it off)
    if (check("Rumble", rumble::enabled(), &v, input::has_rumble())) {
        rumble::set_enabled(v);
        hostui::post([v] { hostui::set("rumble", v ? "1" : "0"); });
    }
    help(input::has_rumble() ? "Controller vibration when the game asks for it. Off keeps the motors still."
                             : "Controller vibration: this host does not drive controller motors yet.");
    ImGui::SameLine(0, 24);
    if (ImGui::Button("Reset to defaults")) input_map::set_current(input_map::Mapping::defaults());
}

int saved_language() {
    std::string v;
    if (hostui::get("language", v) && !v.empty()) {
        int l = atoi(v.c_str());
        if (l >= 0 && l < game_lang::kLanguages) return l;
    }
    return 1;
}

// "English, French and Spanish"
std::string language_list(const std::vector<int>& langs) {
    std::string s;
    for (size_t k = 0; k < langs.size(); k++)
        s += std::string(k == 0 ? "" : k + 1 == langs.size() ? " and " : ", ") + game_lang::name(langs[k]);
    return s;
}

void tab_about() {
    const char* env = getenv("WWHD_LANGUAGE");
    const bool env_set = env && *env;
    char* end = nullptr;
    const long env_value = env_set ? strtol(env, &end, 10) : 1;
    const int env_language = env_set && !*end && env_value >= 0 && env_value < game_lang::kLanguages ? (int)env_value : 1;
    if (U.language < 0) {
        U.language = saved_language();
        U.language_at_start = game_lang::started();  // the game reads it early in the boot
        if (U.language_at_start < 0) U.language_at_start = game_lang::usable(env_set ? env_language : U.language);
    }
    const std::vector<int>& avail = game_lang::available();
    heading("Console language (applies on the next start)");
    if (avail.empty())
        note("The game picks its text language from the console language. Its language packs (content/Common/Pack) "
             "were not found, so every language is offered; the game shows only those its disc carries.");
    else
        note("The game picks its text language from the console language. This game (%s) contains %s; the other "
             "languages need a disc that carries them.", game_lang::region().c_str(), language_list(avail).c_str());
    ImGui::BeginDisabled(env_set);
    for (int i : {1, 2, 5, 3, 4, 8, 9, 10, 7, 0, 6, 11}) {
        if (i != 1 && i != 3 && i != 9 && i != 0) ImGui::SameLine();  // rows of three
        if (radio(game_lang::name(i), U.language == i, game_lang::is_available(i))) {
            U.language = i;
            hostui::post([i] { hostui::set("language", std::to_string(i)); });
        }
    }
    ImGui::EndDisabled();
    if (env_set) {
        note("WWHD_LANGUAGE=%s is set for this start and takes precedence.", env);
        if (!game_lang::is_available(env_language))
            warn("%s is not on this disc: the game runs in %s.", game_lang::name(env_language),
                 game_lang::name(game_lang::usable(env_language)));
    } else if (!game_lang::is_available(U.language)) {
        warn("%s is saved but is not on this disc: the game runs in %s. Choose one of the languages above.",
             game_lang::name(U.language), game_lang::name(game_lang::usable(U.language)));
    } else if (U.language != U.language_at_start) {
        warn("%s is saved. Restart the game to apply it: the game reads the console language only when it starts.",
             game_lang::name(U.language));
    }
    heading("About");
    ImGui::Text("The Legend of Zelda: The Wind Waker HD - native port (%s host, %s renderer)", hostui::name(),
                render::api_name(render::active()));
#ifdef __APPLE__
    note("Settings overlay: F1 (Fn+F1 on most Mac keyboards), Cmd+, or Settings... in the app menu, or hold Select / "
         "press Home on a controller. Esc, F1 or B closes. "
#else
    note("Settings overlay: F1, or hold Select / press Home on a controller. Esc, F1 or B closes. "
#endif
         "L / R switch tabs on a controller. The game keeps running and sees no input while this menu is open.");
    note("When the game asks for text (your name), a text window appears over the game: type on the keyboard "
         "(Enter = OK, Esc = Cancel), click the on-screen keys, or use a controller: D-pad / left stick choose a key, "
         "A types it, B deletes, X adds a space, Y is Shift, L / R switch between letters, accents and symbols, "
         "Start confirms.");
    note("Built with Dear ImGui %s (MIT License, Omar Cornut and contributors).", IMGUI_VERSION);
}

#ifdef __SWITCH__
extern "C" uint64_t switch_thread_cpu_ns(int which);  // 0 game main thread, 1 render, 2 record
#endif
void perf_window(bool menu_open) {
    const double t = now_s();
    if (U.fps_t0 == 0) { U.fps_t0 = t; U.fps_n0 = gx2::flips_presented(); }
#ifdef __SWITCH__
    // CPU time per frame of the two threads that bound the frame rate (refreshed with the fps)
    static uint64_t cpu0[2] = {};
    static float cpu_ms[2] = {};
#endif
    if (t - U.fps_t0 >= 0.5) {
        uint64_t n = gx2::flips_presented();
        U.fps = (double)(n - U.fps_n0) / (t - U.fps_t0);
#ifdef __SWITCH__
        for (int i = 0; i < 2; ++i) {
            const uint64_t c = switch_thread_cpu_ns(i);
            if (cpu0[i] && n > U.fps_n0 && c >= cpu0[i]) cpu_ms[i] = float((c - cpu0[i]) / 1e6 / double(n - U.fps_n0));
            cpu0[i] = c;
        }
#endif
        U.fps_t0 = t;
        U.fps_n0 = n;
    }
    float worst = 0, sum = 0;
    for (float f : U.frame_ms) { worst = std::max(worst, f); sum += f; }
#ifdef __SWITCH__
    // bottom left: the hearts and magic meter use the top left
    ImGui::SetNextWindowPos(ImVec2(10, ImGui::GetIO().DisplaySize.y - 10), ImGuiCond_Always, ImVec2(0, 1));
#else
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
#endif
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##perf", nullptr, fl)) {
        ImGui::Text("%.0f fps   %.1f ms (worst %.1f)", U.fps, sum / 120.0f, worst);
        ImGui::PlotLines("##ft", U.frame_ms, 120, U.frame_i, nullptr, 0.0f, 50.0f, ImVec2(220, 36));
#ifdef __SWITCH__
        ImGui::Text("game %.1f ms   render %.1f ms", cpu_ms[0], cpu_ms[1]);
#endif
        ImGui::TextDisabled("%s  %gx  %s", render::api_name(render::active()), hostui::res_scale(),
                            interp::mode() == 2 ? "true 60" : interp::mode() == 1 ? "60 fps" : "30 fps");
        if (float share = interp::paced_drawn_share(); share >= 0)
            ImGui::TextDisabled("60 fps frames drawn: %.0f%%", share * 100.0f);
    }
    ImGui::End();
    (void)menu_open;
}

void settings_window() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ds = io.DisplaySize;
    // the Controls drawing wants room: that tab gets (almost) the whole picture
    const ImVec2 size = U.tab == kControls ? ImVec2(std::min(1240.0f, ds.x * 0.98f), std::min(720.0f, ds.y * 0.97f))
                                           : ImVec2(std::min(860.0f, ds.x * 0.94f), std::min(620.0f, ds.y * 0.92f));
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoSavedSettings;
    bool open = true;
    if (U.just_opened) {
        ImGui::SetNextWindowFocus();  // keyboard / controller navigation starts in the menu
        U.just_opened = false;
    }
    if (ImGui::Begin("Wind Waker HD  -  Settings", &open, fl)) {
        // L / R on a controller switch tabs
        if (U.cap_action < 0 && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
            if (controller_pressed(input_map::kPadLB)) U.select_tab = (U.tab + kTabs - 1) % kTabs;
            if (controller_pressed(input_map::kPadRB)) U.select_tab = (U.tab + 1) % kTabs;
        }
        if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
            for (int i = 0; i < kTabs; i++) {
                ImGuiTabItemFlags f = U.select_tab == i ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(kTabNames[i], nullptr, f)) {
                    if (U.tab != i) U.cap_action = -1, g_capturing_keys = false, U.cap_note.clear();
                    U.tab = i;
                    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                    switch (i) {
                    case kSaves: tab_saves(); break;
                    case kGraphics: tab_graphics(); break;
                    case kDisplay: tab_display(); break;
                    case kMods: tab_mods(); break;
                    case kControls: tab_controls(); break;
                    default: tab_about(); break;
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            U.select_tab = -1;
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    // B (not while choosing an input or in a list) or the close button closes the menu
    if (!open) set_open(false);
    if (U.cap_action < 0 && controller_pressed(input_map::kPadB) && !g_pad_b_used && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
        set_open(false);
    g_pad_b_used = false;
}

}  // namespace

// ---------------------------------------------------------------- API
bool is_open() { return g_open.load(std::memory_order_relaxed); }
void set_open(bool open) {
    if (g_open.exchange(open) == open) return;
    for (auto& k : g_held_keys) k = false;
    input::release_keys();  // keys held now belong to the menu (opening) or are not stuck in the game (closing)
    if (!open) g_wait_release = true;
    if (open) {
        mods::mouse_release();
        U.just_opened = true;
        U.slots_time = -1;
    }
    LOG("[overlay] %s", open ? "opened" : "closed");
}
bool blocks_input() { return captures() || g_wait_release.load(std::memory_order_relaxed); }
bool captures() { return is_open() || text_entry::active(); }
bool alive() { return now_s() - g_last_frame.load(std::memory_order_relaxed) < 1.0; }
bool perf_shown() { return g_perf.load(std::memory_order_relaxed); }
void set_perf_shown(bool on) {
    g_perf = on;
    hostui::post([on] { hostui::set("perfOverlay", on ? "1" : "0"); });
}

bool key(int code, bool down, bool repeat, int mods) {
    if (g_no_host_keys) return false;
#ifdef __APPLE__
    // Cmd+, (the macOS settings shortcut; the AppKit host gets it as the app menu's Settings... item)
    if (code == kVK_ANSI_Comma && (mods & kSuper) && !g_capturing_keys.load()) {
        if (down && !repeat) set_open(!is_open());
        return true;
    }
#endif
    // F1 (no modifiers; Shift+F1 saves slot 1) toggles; Esc closes
    if (code == kVK_F1 && !(mods & (kShift | kCtrl | kAlt | kSuper))) {
        if (down && !repeat && !g_capturing_keys.load()) set_open(!is_open());
        if (!g_capturing_keys.load()) return true;
    }
    if (!is_open()) {
        // the game's text prompt (text_entry.h) has the keyboard while it shows
        if (!text_entry::active()) return false;
        text_entry::key(code, down, repeat);
        return true;
    }
    if (code >= 0 && code < 256 && !repeat) g_held_keys[code] = down;
    if (g_capturing_keys.load()) {
        if (down && !repeat) {
            if (code == kVK_Escape) g_capture_key = -1;
            else if (code == kVK_Delete || code == kVK_ForwardDelete) g_capture_key = -3;
            else g_capture_key = code;
        }
        return true;
    }
    if (code == kVK_Escape && down && !repeat) {
        set_open(false);
        return true;
    }
    push({Event::Key, code, down, 0, 0, mods});
    return true;
}
bool mouse_move(float nx, float ny) {
    if (!captures() || g_no_host) return false;
    push({Event::MousePos, 0, false, nx, ny, 0});
    return true;
}
bool mouse_button(int button, bool down) {
    if (!captures() || g_no_host) return false;
    push({Event::MouseButton, button, down, 0, 0, 0});
    return true;
}
bool mouse_wheel(float dx, float dy) {
    if (!captures() || g_no_host) return false;
    push({Event::Wheel, 0, false, dx, dy, 0});
    return true;
}
void set_density(float d) { g_density = d > 0.5f ? d : 1.0f; }

ImDrawData* frame(float pw, float ph, void (*renderer_init)()) {
    static TestSwitch test = parse_test();
    static bool prefs_read = false;
    if (!prefs_read) {
        prefs_read = true;
        std::string v;
        if (hostui::get("perfOverlay", v)) g_perf = v == "1";
        // the saved controller choice (WWHD_PRO_CONTROLLER wins); it also hides or shows the GamePad screen
        if (!getenv("WWHD_PRO_CONTROLLER") && hostui::get("proController", v))
            hostui::post([pro = v == "1"] { hostui::set_pro_controller(pro); });
        // the saved rumble choice (WWHD_RUMBLE wins)
        if (!rumble::env_override() && hostui::get("rumble", v)) rumble::set_enabled(v != "0");
        if (const char* e = getenv("WWHD_PERF_OVERLAY")) g_perf = atoi(e) != 0;
    }
    if (!test.done && render::frame_count() + 1 >= test.at) {
        test.done = true;
        if (test.tab >= 0) U.select_tab = U.tab = test.tab;
        if (test.open) set_open(true);
        if (test.perf) g_perf = true;
        LOG("[overlay] test switch: %s", getenv("WWHD_TEST_OVERLAY"));
    }
#ifdef WWHD_HAS_VULKAN
    // debug: WWHD_TEST_PRESENT_MODE=mailbox@330 picks Graphics > Presentation at TV frame 330, as the
    // radio button does (the swapchains are recreated)
    static const char* tpm = getenv("WWHD_TEST_PRESENT_MODE");
    if (tpm) {
        const char* at = strchr(tpm, '@');
        if (render::frame_count() + 1 >= (at ? strtoull(at + 1, nullptr, 10) : 1)) {
            std::string m(tpm, at ? at - tpm : strlen(tpm));
            int mode = m == "mailbox" ? gfxvk::kPresentMailbox : m == "immediate" ? gfxvk::kPresentImmediate : gfxvk::kPresentFifo;
            LOG("[overlay] test: presentation %s", m.c_str());
            post_changed([mode] { gfxvk::set_present_mode(mode); });
            tpm = nullptr;
        }
    }
#endif
    const double t = now_s();
    if (U.last_present > 0) {
        U.frame_ms[U.frame_i] = (float)((t - U.last_present) * 1000.0);
        U.frame_i = (U.frame_i + 1) % 120;
    }
    U.last_present = t;
    g_last_frame = t;
    read_controller();
    // the game's text prompt shows unless the menu is open over it (the menu has the input then)
    const bool open = is_open(), perf = perf_shown(), text = !open && text_entry::active();
    U.linearized = false;
    if (!open && !perf && !text) {
        if (U.init) {  // forget events and pressed keys while nothing is shown
            std::lock_guard<std::mutex> lk(g_mu);
            g_events.clear();
        }
        return nullptr;
    }
    if (pw < 16 || ph < 16) return nullptr;
    if (!U.init) {
        init_context();
        if (renderer_init) renderer_init();
    }
    ImGuiIO& io = ImGui::GetIO();
    // UI scale: HiDPI backing scale, and larger on large windows (a TV across the room)
    const float scale = std::max({1.0f, g_density.load(), ph / 820.0f});
    io.DisplaySize = ImVec2(pw / scale, ph / scale);
    io.DisplayFramebufferScale = ImVec2(scale, scale);
    io.DeltaTime = U.last_time > 0 ? (float)std::clamp(t - U.last_time, 1e-4, 0.25) : 1.0f / 60.0f;
    U.last_time = t;
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        events.swap(g_events);
    }
    for (const Event& e : events) {
        switch (e.kind) {
        case Event::Key:
            io.AddKeyEvent(ImGuiMod_Shift, e.mods & kShift);
            io.AddKeyEvent(ImGuiMod_Ctrl, e.mods & kCtrl);
            io.AddKeyEvent(ImGuiMod_Alt, e.mods & kAlt);
            io.AddKeyEvent(ImGuiMod_Super, e.mods & kSuper);
            if (ImGuiKey k = imgui_key(e.code); k != ImGuiKey_None) io.AddKeyEvent(k, e.down);
            break;
        case Event::MousePos: io.AddMousePosEvent(e.x * io.DisplaySize.x, e.y * io.DisplaySize.y); break;
        case Event::MouseButton: io.AddMouseButtonEvent(e.code, e.down); break;
        case Event::Wheel: io.AddMouseWheelEvent(e.x, e.y); break;
        default: break;
        }
    }
    apply_capture();
    feed_gamepad(io, open && U.cap_action < 0);  // the text prompt reads the controller itself
    ImGui::NewFrame();
    if (open) settings_window();
    if (text) {
        float pad[input_map::kPadCount];
        std::copy(std::begin(U.values), std::end(U.values), pad);
        if (g_no_host) test_pad(pad);  // WWHD_TEST_PAD drives the on-screen keyboard in test runs
        if (text_entry::draw(pad)) {
            // answered: the game sees no buttons until the one that confirmed is released
            for (auto& k : g_held_keys) k = false;
            g_wait_release = true;
        }
    }
    if (perf) perf_window(open);
    ImGui::Render();
    return ImGui::GetDrawData();
}

void linearize_colors(ImDrawData* d) {
    if (!d || U.linearized) return;
    U.linearized = true;
    static uint8_t table[256];
    static bool made = false;
    if (!made) {
        for (int i = 0; i < 256; i++) {
            float c = i / 255.0f;
            c = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
            table[i] = (uint8_t)std::lround(c * 255.0f);
        }
        made = true;
    }
    for (ImDrawList* l : d->CmdLists)
        for (ImDrawVert& v : l->VtxBuffer) {
            uint32_t c = v.col;
            v.col = (c & 0xFF000000u) | (uint32_t)table[(c >> 16) & 0xFF] << 16 | (uint32_t)table[(c >> 8) & 0xFF] << 8 |
                    table[c & 0xFF];
        }
}

}  // namespace overlay
