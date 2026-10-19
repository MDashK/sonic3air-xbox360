/*
*	Xbox 360 port of Sonic 3 A.I.R.
*
*	SDL2-lite: there is no SDL2 port for the Xbox 360 XDK. Instead of a full port, this uses SDL2's
*	own public headers (framework/external/sdl/SDL2/include, the same SDL version upstream builds
*	against) for all types, constants and prototypes, and x360/sdl2lite/src implements only the
*	subset of functions S3AIR uses, on top of the XDK (threads, timers, files, input, audio).
*	Anything not implemented shows up as an undefined symbol at link time.
*
*	This wrapper is found first through the include path (x360/sdl2lite/include) whenever code
*	does #include <SDL.h>.
*/

#pragma once

// Static "library", plain C calling convention: no dllimport/dllexport or __cdecl decorations
#define DECLSPEC
#define SDLCALL

// The Xbox 360 toolchain defines _WIN32, which would make SDL's headers pick the Windows
// configuration (SDL_config_windows.h, SDL_CreateThread routed through _beginthreadex, ...).
// Hide it while SDL's headers are parsed, so they see a generic platform (SDL_config_minimal.h).
#pragma push_macro("_WIN32")
#pragma push_macro("WIN32")
#undef _WIN32
#undef WIN32

#include <SDL2/include/SDL.h>

#pragma pop_macro("WIN32")
#pragma pop_macro("_WIN32")
