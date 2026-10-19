/*
*	Xbox 360 port of Sonic 3 A.I.R.
*	SDL2-lite: SDL_RWops for files, on top of stdio (RXDK's picolibc fopen, which resolves game:\
*	paths both on the console and in Xenia once x360/tools/fix_rxdk_ppc64.py has patched libc.a).
*/

#include "SDL_x360_internal.h"

#include <stdio.h>
#include <stdlib.h>


namespace
{
	FILE* getFile(SDL_RWops* context)
	{
		return static_cast<FILE*>(context->hidden.unknown.data1);
	}

	Sint64 SDLCALL fileSize(SDL_RWops* context)
	{
		FILE* file = getFile(context);
		const long position = ftell(file);
		if (fseek(file, 0, SEEK_END) != 0)
			return -1;
		const long size = ftell(file);
		fseek(file, position, SEEK_SET);
		return (Sint64)size;
	}

	Sint64 SDLCALL fileSeek(SDL_RWops* context, Sint64 offset, int whence)
	{
		static const int ORIGINS[] = { SEEK_SET, SEEK_CUR, SEEK_END };
		if (whence < RW_SEEK_SET || whence > RW_SEEK_END)
			return SDL_SetError("Unknown value for 'whence'");
		FILE* file = getFile(context);
		if (fseek(file, (long)offset, ORIGINS[whence]) != 0)
			return SDL_Error(SDL_EFSEEK);
		return (Sint64)ftell(file);
	}

	size_t SDLCALL fileRead(SDL_RWops* context, void* ptr, size_t size, size_t maxnum)
	{
		FILE* file = getFile(context);
		const size_t count = fread(ptr, size, maxnum, file);
		if (count == 0 && ferror(file))
			SDL_Error(SDL_EFREAD);
		return count;
	}

	size_t SDLCALL fileWrite(SDL_RWops* context, const void* ptr, size_t size, size_t num)
	{
		FILE* file = getFile(context);
		const size_t count = fwrite(ptr, size, num, file);
		if (count == 0 && ferror(file))
			SDL_Error(SDL_EFWRITE);
		return count;
	}

	int SDLCALL fileClose(SDL_RWops* context)
	{
		int result = 0;
		if (nullptr != context)
		{
			if (fclose(getFile(context)) != 0)
				result = SDL_Error(SDL_EFWRITE);
			SDL_FreeRW(context);
		}
		return result;
	}
}


extern "C"
{

SDL_RWops* SDL_AllocRW(void)
{
	SDL_RWops* context = static_cast<SDL_RWops*>(calloc(1, sizeof(SDL_RWops)));
	if (nullptr == context)
		SDL_OutOfMemory();
	else
		context->type = SDL_RWOPS_UNKNOWN;
	return context;
}

void SDL_FreeRW(SDL_RWops* area)
{
	free(area);
}

SDL_RWops* SDL_RWFromFile(const char* file, const char* mode)
{
	if (nullptr == file || nullptr == mode || file[0] == 0)
	{
		SDL_SetError("SDL_RWFromFile(): No file or no mode specified");
		return nullptr;
	}

	// stdio on this platform has no text mode translation, so "r" behaves like "rb"
	FILE* fp = fopen(file, mode);
	if (nullptr == fp)
	{
		SDL_SetError("Couldn't open %s", file);
		return nullptr;
	}

	SDL_RWops* context = SDL_AllocRW();
	if (nullptr == context)
	{
		fclose(fp);
		return nullptr;
	}
	context->size  = fileSize;
	context->seek  = fileSeek;
	context->read  = fileRead;
	context->write = fileWrite;
	context->close = fileClose;
	context->hidden.unknown.data1 = fp;
	return context;
}

Sint64 SDL_RWsize(SDL_RWops* context)
{
	return context->size(context);
}

Sint64 SDL_RWseek(SDL_RWops* context, Sint64 offset, int whence)
{
	return context->seek(context, offset, whence);
}

Sint64 SDL_RWtell(SDL_RWops* context)
{
	return context->seek(context, 0, RW_SEEK_CUR);
}

size_t SDL_RWread(SDL_RWops* context, void* ptr, size_t size, size_t maxnum)
{
	return context->read(context, ptr, size, maxnum);
}

size_t SDL_RWwrite(SDL_RWops* context, const void* ptr, size_t size, size_t num)
{
	return context->write(context, ptr, size, num);
}

int SDL_RWclose(SDL_RWops* context)
{
	return context->close(context);
}

}
