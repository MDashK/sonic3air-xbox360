/*
*	Xbox 360 port of Sonic 3 A.I.R.
*	SDL2-lite extensions that only exist on the Xbox 360.
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Returns the IDirect3DDevice9* that SDL2-lite uses for presenting the window surface (created on first use,
// with a 1280x720 back buffer), or null if it couldn't be created. Declared as void* so that this header doesn't
// need the XDK headers.
void* SDL_X360_GetD3DDevice(void);

// Number of hardware threads SDL_X360_ParallelFor runs jobs on at the same time (including the calling thread)
int SDL_X360_GetParallelThreadCount(void);

// Runs job(index, userData) for every index in [0, count), in parallel on the Xenon's hardware threads, and returns
// when all of them are done. Index 0 runs on the calling thread. Not reentrant: only call it from one thread.
void SDL_X360_ParallelFor(int count, void (*job)(int index, void* userData), void* userData);

#ifdef __cplusplus
}
#endif
