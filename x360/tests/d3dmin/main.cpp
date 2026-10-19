// Minimal D3D9 control test for the hardware-only GPU freeze.
// The same source is built twice: with the stock XDK compiler (d3dmin_stock.vcxproj, toolset 2010-01)
// and with the RXDK clang toolset (d3dmin_clang.vcxproj). Kept C++03-compatible for the stock compiler.

#include <xtl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

extern "C" ULONG DbgPrint(const char* format, ...);

#if defined(__clang__)
	#define TOOLCHAIN_NAME "RXDK clang"
	// picolibc has the standard names only; the VS2010-era XDK CRT has only the underscore ones
	#define _vsnprintf vsnprintf
	#define _snprintf snprintf
#else
	#define TOOLCHAIN_NAME "stock XDK cl.exe"
#endif

// Experiment 1+2 combined (a single define, since MSBuild /p: properties cannot carry ';')
#if defined(D3DMIN_EXP12)
	#define D3DMIN_OFFICIAL_STARTUP
	#define D3DMIN_PLAIN_MEMFUNCS
#endif

#if defined(D3DMIN_OFFICIAL_STARTUP)
// Experiment 1: entry point = the XDK's own mainCRTStartup (xapilib xapi0.o, XapiInitProcess) instead of
// RXDK's _start. It needs the MS SEH handler, which the RXDK runtime does not provide; a stub that keeps
// searching is enough unless an exception actually happens.
extern "C" EXCEPTION_DISPOSITION __C_specific_handler(struct _EXCEPTION_RECORD*, void*, struct _CONTEXT*, struct _DISPATCHER_CONTEXT*)
{
	return ExceptionContinueSearch;
}
#endif

#if defined(D3DMIN_PLAIN_MEMFUNCS)
// Experiment 2: replace picolibc's memcpy/memmove/memset (VMX, called by D3D into GPU memory) with plain
// byte loops. optnone keeps clang from vectorizing them or turning the loops back into mem* calls.
extern "C" __attribute__((optnone)) void* memcpy(void* dst, const void* src, size_t n)
{
	unsigned char* d = (unsigned char*)dst;
	const unsigned char* s = (const unsigned char*)src;
	while (n--)
		*d++ = *s++;
	return dst;
}

extern "C" __attribute__((optnone)) void* memmove(void* dst, const void* src, size_t n)
{
	unsigned char* d = (unsigned char*)dst;
	const unsigned char* s = (const unsigned char*)src;
	if (d < s)
	{
		while (n--)
			*d++ = *s++;
	}
	else
	{
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}

extern "C" __attribute__((optnone)) void* memset(void* dst, int value, size_t n)
{
	unsigned char* d = (unsigned char*)dst;
	while (n--)
		*d++ = (unsigned char)value;
	return dst;
}
#endif

static HANDLE gLogFile = INVALID_HANDLE_VALUE;
static DWORD gStartTicks = 0;

// Every line is flushed, so the log survives a console freeze right after it
static void logLine(const char* format, ...)
{
	char text[400];
	va_list args;
	va_start(args, format);
	_vsnprintf(text, sizeof(text) - 1, format, args);
	va_end(args);
	text[sizeof(text) - 1] = 0;

	char line[480];
	_snprintf(line, sizeof(line) - 1, "[%6u ms] %s\r\n", (unsigned)(GetTickCount() - gStartTicks), text);
	line[sizeof(line) - 1] = 0;

	DbgPrint("D3DMIN: %s", line);
	if (gLogFile != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		WriteFile(gLogFile, line, (DWORD)strlen(line), &written, NULL);
		FlushFileBuffers(gLogFile);
	}
}

static volatile LONG gVBlankCount = 0;

// Runs at DPC level: integer work only
static void WINAPI onVerticalBlank(D3DVBLANKDATA*)
{
	gVBlankCount = gVBlankCount + 1;
}

static bool waitForFence(DWORD fence, DWORD timeoutMs, DWORD* elapsedMs)
{
	const DWORD start = GetTickCount();
	while (D3DDevice_IsFencePending(fence))
	{
		*elapsedMs = GetTickCount() - start;
		if (*elapsedMs >= timeoutMs)
			return false;
		Sleep(1);
	}
	*elapsedMs = GetTickCount() - start;
	return true;
}

static void runTest()
{
	logLine("Direct3DCreate9");
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!d3d)
	{
		logLine("Direct3DCreate9 FAILED");
		return;
	}

	D3DPRESENT_PARAMETERS params;
	ZeroMemory(&params, sizeof(params));
	params.BackBufferWidth        = 1280;
	params.BackBufferHeight       = 720;
	params.BackBufferFormat       = D3DFMT_A8R8G8B8;
	params.BackBufferCount        = 1;
	params.EnableAutoDepthStencil = TRUE;
	params.AutoDepthStencilFormat = D3DFMT_D24S8;
	params.SwapEffect             = D3DSWAPEFFECT_DISCARD;
	params.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;

	logLine("CreateDevice");
	IDirect3DDevice9* device = NULL;
	const HRESULT result = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device);
	logLine("CreateDevice -> 0x%08X, device %p", (unsigned)result, device);
	if (FAILED(result) || !device)
		return;

	D3DRASTER_STATUS raster;
	ZeroMemory(&raster, sizeof(raster));
	D3DDevice_GetRasterStatus(device, &raster);
	logLine("[1] raster: InVBlank=%d ScanLine=%u", (int)raster.InVBlank, (unsigned)raster.ScanLine);

	logLine("[2] InsertFence (empty)...");
	DWORD fence = D3DDevice_InsertFence(device);
	logLine("[2] InsertFence returned 0x%08X, polling", (unsigned)fence);
	DWORD elapsedMs = 0;
	bool done = waitForFence(fence, 2000, &elapsedMs);
	logLine("[2] empty fence: %s after %u ms", done ? "completed" : "TIMEOUT", (unsigned)elapsedMs);
	if (!done)
		return;

	logLine("[3] Clear...");
	device->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, D3DCOLOR_XRGB(0, 160, 60), 1.0f, 0);
	fence = D3DDevice_InsertFence(device);
	done = waitForFence(fence, 2000, &elapsedMs);
	logLine("[3] clear + fence: %s after %u ms", done ? "completed" : "TIMEOUT", (unsigned)elapsedMs);
	if (!done)
		return;

	gVBlankCount = 0;
	device->SetVerticalBlankCallback(onVerticalBlank);
	Sleep(500);
	device->SetVerticalBlankCallback(NULL);
	logLine("[4] vblank callbacks in 500 ms: %d", (int)gVBlankCount);

	// 300 frames (~5 s): green, blue, yellow bands; then back to the dashboard on its own
	const D3DCOLOR colors[3] = { D3DCOLOR_XRGB(0, 160, 60), D3DCOLOR_XRGB(0, 60, 200), D3DCOLOR_XRGB(230, 200, 0) };
	for (DWORD frame = 0; frame < 300; ++frame)
	{
		const bool trace = (frame < 3) || (frame % 60 == 0);
		device->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, colors[(frame / 100) % 3], 1.0f, 0);
		if (trace)
			logLine("[5] frame %u: Present...", (unsigned)frame);
		device->Present(NULL, NULL, NULL, NULL);
		if (trace)
			logLine("[5] frame %u: Present returned", (unsigned)frame);
	}
	logLine("300 frames OK");

	device->Release();
	d3d->Release();
}

// Minimal crash report: registers only, no stack walk (reading stack memory inside the filter can fault
// again and freeze the console). The exception then goes on to the dashboard's handler.
// For an access violation, info[0] is 0 = read / 1 = write and info[1] is the faulting address.
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* info)
{
	const EXCEPTION_RECORD* record = info->ExceptionRecord;
	const CONTEXT* context = info->ContextRecord;
	logLine("!!! CRASH: exception 0x%08X at 0x%08X", (unsigned)record->ExceptionCode, (unsigned)(UINT_PTR)record->ExceptionAddress);
	logLine("!!!   info[0]=0x%08X info[1]=0x%08X (params %u)", (unsigned)record->ExceptionInformation[0], (unsigned)record->ExceptionInformation[1], (unsigned)record->NumberParameters);
	logLine("!!!   IAR 0x%08X  LR 0x%08X  SP 0x%08X  CTR 0x%08X", (unsigned)context->Iar, (unsigned)context->Lr, (unsigned)context->Gpr1, (unsigned)context->Ctr);
	return EXCEPTION_CONTINUE_SEARCH;
}

int main()
{
	gStartTicks = GetTickCount();
	gLogFile = CreateFileA("game:\\d3dmin.log", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	SetUnhandledExceptionFilter(crashFilter);
	logLine("=== d3dmin v6 (%s) ===", TOOLCHAIN_NAME);
#if defined(D3DMIN_OFFICIAL_STARTUP)
	logLine("Experiment 1: entry = XDK mainCRTStartup");
#endif
#if defined(D3DMIN_PLAIN_MEMFUNCS)
	logLine("Experiment 2: plain byte-loop memcpy/memmove/memset");
#endif
#if defined(D3DMIN_EXP3)
	logLine("Experiment 3: this code was compiled by MS cl.exe, linked by the RXDK clang pipeline");
#endif

	// Hypothesis: RXDK's _start forces XEnableSmallPagesOverride(1), the official startup does not.
	// With small pages, D3D's physical allocations (ring buffer, write-backs) land in a different
	// physical window than with the stock CRT, and the GPU never consumes the ring buffer.
	logLine("XGetSmallPagesOverride() at main: %d", (int)XGetSmallPagesOverride());
#if defined(D3DMIN_SMALL_PAGES_OFF)
	XEnableSmallPagesOverride(FALSE);
	logLine("XEnableSmallPagesOverride(FALSE) -> now %d", (int)XGetSmallPagesOverride());
#endif

	// Where a GPU-style physical allocation lands (D3DPhysicalAlloc uses the same call)
	void* physical = XPhysicalAlloc(64 * 1024, MAXULONG_PTR, 4096, PAGE_READWRITE | PAGE_WRITECOMBINE);
	logLine("XPhysicalAlloc(64 KB, write-combine) -> %p", physical);
	if (physical)
		XPhysicalFree(physical);

	runTest();

	logLine("Done, returning to the dashboard");
	if (gLogFile != INVALID_HANDLE_VALUE)
		CloseHandle(gLogFile);
	XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
	return 0;
}
