/*
*	Xbox 360 port of Sonic 3 A.I.R.
*
*	Runtime functions missing from the RXDK-360 clang runtime (release be16a94), linked into every
*	X360 application through x360runtime.lib (x360/build/x360runtime.vcxproj).
*/

#include <math.h>
#include <new>
#include <stdlib.h>


/* ----- Float math -----
   RXDK's libc excludes picolibc's float cores sf_exp.c / sf_log.c / sf_pow.c / sf_log2.c
   (tools/build_libc.py LIBM_EXCLUDE) and only keeps a libm/math replacement for some of them,
   so these four are undefined. The double versions exist; precision is more than enough. */
extern "C"
{
	float expf(float x)				{ return (float)exp((double)x); }
	float logf(float x)				{ return (float)log((double)x); }
	float powf(float x, float y)	{ return (float)pow((double)x, (double)y); }
	float log2f(float x)			{ return (float)log2((double)x); }
}


/* ----- Non-throwing operator new / delete -----
   The runtime provides the throwing and aligned forms, but not the std::nothrow_t overloads. */
void* operator new(size_t size, const std::nothrow_t&) noexcept
{
	return malloc(size ? size : 1);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept
{
	return malloc(size ? size : 1);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept
{
	free(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept
{
	free(pointer);
}
