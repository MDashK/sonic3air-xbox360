/*
*	Xbox 360 port of Sonic 3 A.I.R.
*
*	Toolchain compatibility shims, force-included into every translation unit of the X360 build
*	(see x360/build/x360.common.props). Keeps toolchain workarounds out of the upstream sources.
*
*	The RXDK-360 clang toolchain in XDK-headers mode defines _WIN32 and _MSC_VER and enables the
*	MS "secure" CRT (__STDC_WANT_SECURE_LIB__), while the actual C/C++ runtime is picolibc + libc++.
*	Code (upstream and libc++) that takes the MSVC paths then expects a few MS CRT functions that
*	picolibc lacks or RXDK only provides with a different signature.
*/

#pragma once

#if defined(_XBOX) && defined(__clang__) && defined(_WIN32) && defined(__cplusplus)

#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <time.h>
#include <wchar.h>

// libc++'s <ctime> only imports ::strftime into std if it is already declared when <ctime> is parsed,
// which the XDK-headers include order does not guarantee (librmx uses std::strftime)
namespace std
{
	using ::strftime;
}

// libc++'s <fstream> uses these when _WIN32 is defined (it assumes an MSVC-like CRT)
inline long long _ftelli64(FILE* file)
{
	return (long long)ftell(file);
}

inline int _fseeki64(FILE* file, long long offset, int origin)
{
	return fseek(file, (long)offset, origin);
}

// MS secure CRT forms used by librmx (rmxbase/memory/String.cpp) when __STDC_WANT_SECURE_LIB__ is set.
// RXDK's rxdk_msvcrt_compat.h only provides the underscore / array-reference variants.
// Callers pass the buffer size for both size arguments, so "count" only matters as an upper bound.
inline int vsnprintf_s(char* buffer, size_t bufferSize, size_t count, const char* format, va_list args)
{
	const size_t limit = (count < bufferSize) ? count + 1 : bufferSize;
	return vsnprintf(buffer, limit, format, args);
}

inline int _vsnwprintf_s(wchar_t* buffer, size_t bufferSize, size_t count, const wchar_t* format, va_list args)
{
	const size_t limit = (count < bufferSize) ? count + 1 : bufferSize;
	return vswprintf(buffer, limit, format, args);
}

#endif
