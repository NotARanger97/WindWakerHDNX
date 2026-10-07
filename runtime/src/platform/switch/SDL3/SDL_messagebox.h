// Switch SDL shim: message boxes go to the log (sdmc:/switch/wwhd/log.txt); there is no dialog.
#pragma once
#include "SDL.h"
#define SDL_MESSAGEBOX_ERROR 0x00000010u
#define SDL_MESSAGEBOX_WARNING 0x00000020u
#define SDL_MESSAGEBOX_INFORMATION 0x00000040u
typedef Uint32 SDL_MessageBoxFlags;
bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char* title, const char* message, SDL_Window* window);
