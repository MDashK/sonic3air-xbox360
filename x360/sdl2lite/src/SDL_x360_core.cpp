/*
*	Xbox 360 port of Sonic 3 A.I.R.
*	SDL2-lite: initialization, errors and timers.
*/

#include "SDL_x360_internal.h"

#include <stdarg.h>
#include <stdio.h>


namespace
{
	char gErrorText[512] = "";
	DWORD gStartTicks = 0;
	bool gTicksInitialized = false;

	void initTicks()
	{
		if (!gTicksInitialized)
		{
			gStartTicks = GetTickCount();
			gTicksInitialized = true;
		}
	}
}


namespace sdl2lite
{
	void setError(const char* format, ...)
	{
		va_list args;
		va_start(args, format);
		vsnprintf(gErrorText, sizeof(gErrorText), format, args);
		va_end(args);
	}
}


extern "C"
{

int SDL_Init(Uint32 flags)
{
	initTicks();
	return 0;
}

int SDL_InitSubSystem(Uint32 flags)
{
	initTicks();
	return 0;
}

void SDL_Quit(void)
{
}

const char* SDL_GetError(void)
{
	return gErrorText;
}

int SDL_SetError(SDL_PRINTF_FORMAT_STRING const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(gErrorText, sizeof(gErrorText), format, args);
	va_end(args);
	return -1;
}

void SDL_ClearError(void)
{
	gErrorText[0] = 0;
}

// Backs the SDL_OutOfMemory / SDL_Unsupported macros
int SDL_Error(SDL_errorcode code)
{
	switch (code)
	{
		case SDL_ENOMEM:		return SDL_SetError("Out of memory");
		case SDL_EFREAD:		return SDL_SetError("Error reading from datastream");
		case SDL_EFWRITE:		return SDL_SetError("Error writing to datastream");
		case SDL_EFSEEK:		return SDL_SetError("Error seeking in datastream");
		case SDL_UNSUPPORTED:	return SDL_SetError("That operation is not supported");
		default:				return SDL_SetError("Unknown SDL error");
	}
}

Uint32 SDL_GetTicks(void)
{
	initTicks();
	return (Uint32)(GetTickCount() - gStartTicks);
}

Uint64 SDL_GetTicks64(void)
{
	return (Uint64)SDL_GetTicks();
}

Uint64 SDL_GetPerformanceCounter(void)
{
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return (Uint64)counter.QuadPart;
}

Uint64 SDL_GetPerformanceFrequency(void)
{
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	return (Uint64)frequency.QuadPart;
}

void SDL_Delay(Uint32 ms)
{
	Sleep(ms);
}

}
