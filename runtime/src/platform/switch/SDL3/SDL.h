// Minimal SDL3 API on libnx (Nintendo Switch): exactly the subset the SDL host of this port uses
// (Vulkan window, events, one gamepad, one audio output stream). Implemented in
// platform/switch/sdl_switch.cpp. Not a general SDL replacement.
#pragma once
#include <cstddef>
#include <cstdint>

#define SDLCALL

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef int16_t Sint16;
typedef int32_t Sint32;
typedef uint32_t Uint32;
typedef uint64_t Uint64;

typedef struct SDL_Window SDL_Window;
typedef Uint32 SDL_WindowID;
typedef Uint32 SDL_JoystickID;
typedef struct SDL_Gamepad SDL_Gamepad;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef Uint32 SDL_AudioDeviceID;
typedef Uint64 SDL_WindowFlags;
typedef Uint32 SDL_InitFlags;
typedef Uint16 SDL_Keymod;
typedef Uint64 SDL_TouchID;

#define SDL_INIT_AUDIO 0x00000010u
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_INIT_GAMEPAD 0x00002000u
#define SDL_WINDOW_RESIZABLE 0x0000000000000020ull
#define SDL_WINDOW_VULKAN 0x0000000010000000ull
#define SDL_WINDOW_FULLSCREEN 0x0000000000000001ull
#define SDL_WINDOW_HIDDEN 0x0000000000000008ull
#define SDL_HINT_MAC_BACKGROUND_APP "SDL_MAC_BACKGROUND_APP"
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID)0xFFFFFFFFu)
#define SDL_BUTTON_LEFT 1
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3

#define SDL_KMOD_SHIFT 0x0003u
#define SDL_KMOD_CTRL 0x00C0u
#define SDL_KMOD_ALT 0x0300u
#define SDL_KMOD_GUI 0x0C00u

typedef enum SDL_AudioFormat { SDL_AUDIO_S16 = 0x8010u } SDL_AudioFormat;
typedef struct SDL_AudioSpec { SDL_AudioFormat format; int channels; int freq; } SDL_AudioSpec;
typedef void(SDLCALL* SDL_AudioStreamCallback)(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount);

typedef enum SDL_Scancode {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D, SDL_SCANCODE_E, SDL_SCANCODE_F, SDL_SCANCODE_G,
    SDL_SCANCODE_H, SDL_SCANCODE_I, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_M, SDL_SCANCODE_N,
    SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T, SDL_SCANCODE_U,
    SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X, SDL_SCANCODE_Y, SDL_SCANCODE_Z,
    SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7,
    SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0,
    SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE = 41, SDL_SCANCODE_BACKSPACE = 42, SDL_SCANCODE_TAB = 43,
    SDL_SCANCODE_SPACE = 44, SDL_SCANCODE_MINUS = 45, SDL_SCANCODE_EQUALS = 46, SDL_SCANCODE_LEFTBRACKET = 47,
    SDL_SCANCODE_RIGHTBRACKET = 48, SDL_SCANCODE_BACKSLASH = 49, SDL_SCANCODE_SEMICOLON = 51,
    SDL_SCANCODE_APOSTROPHE = 52, SDL_SCANCODE_GRAVE = 53, SDL_SCANCODE_COMMA = 54, SDL_SCANCODE_PERIOD = 55,
    SDL_SCANCODE_SLASH = 56, SDL_SCANCODE_CAPSLOCK = 57,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_F6,
    SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
    SDL_SCANCODE_HOME = 74, SDL_SCANCODE_PAGEUP = 75, SDL_SCANCODE_DELETE = 76, SDL_SCANCODE_END = 77,
    SDL_SCANCODE_PAGEDOWN = 78, SDL_SCANCODE_RIGHT = 79, SDL_SCANCODE_LEFT = 80, SDL_SCANCODE_DOWN = 81,
    SDL_SCANCODE_UP = 82, SDL_SCANCODE_NUMLOCKCLEAR = 83, SDL_SCANCODE_KP_DIVIDE = 84, SDL_SCANCODE_KP_MULTIPLY = 85,
    SDL_SCANCODE_KP_MINUS = 86, SDL_SCANCODE_KP_PLUS = 87, SDL_SCANCODE_KP_ENTER = 88,
    SDL_SCANCODE_KP_1 = 89, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5,
    SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9, SDL_SCANCODE_KP_0,
    SDL_SCANCODE_KP_PERIOD = 99, SDL_SCANCODE_NONUSBACKSLASH = 100, SDL_SCANCODE_KP_EQUALS = 103,
    SDL_SCANCODE_F13 = 104, SDL_SCANCODE_F14, SDL_SCANCODE_F15, SDL_SCANCODE_F16,
    SDL_SCANCODE_KP_DECIMAL = 220,
    SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LALT, SDL_SCANCODE_LGUI, SDL_SCANCODE_RCTRL,
    SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RALT, SDL_SCANCODE_RGUI,
    SDL_SCANCODE_COUNT = 512,
} SDL_Scancode;

typedef enum SDL_GamepadButton {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_COUNT
} SDL_GamepadButton;
typedef enum SDL_GamepadAxis {
    SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_GAMEPAD_AXIS_COUNT
} SDL_GamepadAxis;

typedef enum SDL_EventType {
    SDL_EVENT_FIRST = 0,
    SDL_EVENT_QUIT = 0x100, SDL_EVENT_TERMINATING = 0x101,
    SDL_EVENT_WINDOW_SHOWN = 0x202, SDL_EVENT_WINDOW_MINIMIZED = 0x209, SDL_EVENT_WINDOW_RESTORED = 0x20B,
    SDL_EVENT_WINDOW_FOCUS_LOST = 0x20F, SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED = 0x207,
    SDL_EVENT_WINDOW_CLOSE_REQUESTED = 0x210,
    SDL_EVENT_WINDOW_ENTER_FULLSCREEN = 0x21A, SDL_EVENT_WINDOW_LEAVE_FULLSCREEN = 0x21B,  /* never sent */
    SDL_EVENT_KEY_DOWN = 0x300, SDL_EVENT_KEY_UP, SDL_EVENT_TEXT_EDITING = 0x302, SDL_EVENT_TEXT_INPUT = 0x303,
    SDL_EVENT_MOUSE_MOTION = 0x400, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP, SDL_EVENT_MOUSE_WHEEL,
    SDL_EVENT_GAMEPAD_ADDED = 0x653, SDL_EVENT_GAMEPAD_REMOVED = 0x654,
} SDL_EventType;
typedef enum SDL_MouseWheelDirection { SDL_MOUSEWHEEL_NORMAL, SDL_MOUSEWHEEL_FLIPPED } SDL_MouseWheelDirection;

typedef struct SDL_CommonEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; } SDL_CommonEvent;
typedef struct SDL_WindowEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; int data1, data2; } SDL_WindowEvent;
typedef struct SDL_KeyboardEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; SDL_Scancode scancode; SDL_Keymod mod; bool down, repeat; } SDL_KeyboardEvent;
typedef struct SDL_TextInputEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; const char* text; } SDL_TextInputEvent;
typedef struct SDL_TextEditingEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; const char* text; Sint32 start, length; } SDL_TextEditingEvent;
typedef struct SDL_GamepadDeviceEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_JoystickID which; } SDL_GamepadDeviceEvent;
typedef struct SDL_MouseMotionEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; float x, y, xrel, yrel; } SDL_MouseMotionEvent;
typedef struct SDL_MouseButtonEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; Uint8 button; bool down; float x, y; } SDL_MouseButtonEvent;
typedef struct SDL_MouseWheelEvent { Uint32 type; Uint32 reserved; Uint64 timestamp; SDL_WindowID windowID; float x, y; SDL_MouseWheelDirection direction; } SDL_MouseWheelEvent;
typedef union SDL_Event {
    Uint32 type;
    SDL_CommonEvent common;
    SDL_TextEditingEvent edit;
    SDL_WindowEvent window;
    SDL_KeyboardEvent key;
    SDL_TextInputEvent text;
    SDL_GamepadDeviceEvent gdevice;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    Uint8 padding[64];
} SDL_Event;

bool SDL_Init(SDL_InitFlags flags);
bool SDL_InitSubSystem(SDL_InitFlags flags);
const char* SDL_GetError(void);
void SDL_free(void* p);
void SDL_Delay(Uint32 ms);
Uint64 SDL_GetTicks(void);
/* window icons and file dialogs do not exist on the Switch: no surface is created, dialogs cancel */
typedef enum SDL_PixelFormat { SDL_PIXELFORMAT_BGRA32 = 0x16862004u } SDL_PixelFormat;
typedef struct SDL_Surface { Uint32 flags; SDL_PixelFormat format; int w, h, pitch; void* pixels; } SDL_Surface;
SDL_Surface* SDL_CreateSurface(int width, int height, SDL_PixelFormat format);
void SDL_DestroySurface(SDL_Surface* surface);
bool SDL_SetWindowIcon(SDL_Window* window, SDL_Surface* icon);
typedef struct SDL_DialogFileFilter { const char* name; const char* pattern; } SDL_DialogFileFilter;
typedef void(SDLCALL* SDL_DialogFileCallback)(void* userdata, const char* const* filelist, int filter);
void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window,
                            const SDL_DialogFileFilter* filters, int nfilters, const char* default_location, bool allow_many);
void SDL_ShowOpenFolderDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window,
                              const char* default_location, bool allow_many);

SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags);
SDL_WindowID SDL_GetWindowID(SDL_Window* window);
bool SDL_GetWindowSizeInPixels(SDL_Window* window, int* w, int* h);
bool SDL_SetWindowTitle(SDL_Window* window, const char* title);
const char* SDL_GetWindowTitle(SDL_Window* window);
SDL_Window* SDL_GetKeyboardFocus(void);
bool SDL_SetWindowRelativeMouseMode(SDL_Window* window, bool enabled);
bool SDL_StartTextInput(SDL_Window* window);
bool SDL_StopTextInput(SDL_Window* window);
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* window);
bool SDL_SetWindowFullscreen(SDL_Window* window, bool fullscreen);
bool SDL_ShowWindow(SDL_Window* window);
bool SDL_HideWindow(SDL_Window* window);
bool SDL_GetWindowSize(SDL_Window* window, int* w, int* h);
float SDL_GetWindowPixelDensity(SDL_Window* window);
SDL_Window* SDL_GetWindowFromID(SDL_WindowID id);
bool SDL_SetHint(const char* name, const char* value);

bool SDL_PollEvent(SDL_Event* event);
typedef bool(SDLCALL* SDL_EventFilter)(void* userdata, SDL_Event* event);
bool SDL_AddEventWatch(SDL_EventFilter filter, void* userdata);  /* no system events to watch: ignored */
SDL_Window** SDL_GetWindows(int* count);  /* SDL_free the result */
typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
bool SDL_SetTextInputArea(SDL_Window* window, const SDL_Rect* rect, int cursor);
const char* SDL_GetScancodeName(SDL_Scancode scancode);
bool SDL_PushEvent(SDL_Event* event);

SDL_JoystickID* SDL_GetGamepads(int* count);
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID id);
void SDL_CloseGamepad(SDL_Gamepad* gamepad);
bool SDL_GamepadConnected(SDL_Gamepad* gamepad);
bool SDL_GetGamepadButton(SDL_Gamepad* gamepad, SDL_GamepadButton button);
Sint16 SDL_GetGamepadAxis(SDL_Gamepad* gamepad, SDL_GamepadAxis axis);
bool SDL_RumbleGamepad(SDL_Gamepad* gamepad, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, Uint32 duration_ms);

SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec* spec, SDL_AudioStreamCallback callback, void* userdata);
bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* buf, int len);
bool SDL_SetAudioStreamGain(SDL_AudioStream* stream, float gain);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* stream);
void SDL_DestroyAudioStream(SDL_AudioStream* stream);
