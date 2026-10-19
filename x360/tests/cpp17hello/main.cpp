// Phase 0 toolchain smoke test for the Sonic 3 A.I.R. Xbox 360 port.
// Checks that the RXDK-360 clang toolset can build C++17 code that uses the
// same language/library features as S3AIR, together with the XDK APIs
// (D3D9, XInput, file I/O on game:\).

#include <xtl.h>

#include <cerrno>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>


// Kernel debug print: shows up in xenia.log and in a devkit's debug output
extern "C" ULONG DbgPrint(const char* format, ...);

// The log goes through the XDK file API: RXDK's fopen uses "\??\game:" paths, which Xenia does not resolve
static HANDLE gLogFile = INVALID_HANDLE_VALUE;
static int gFailures = 0;

static void logLine(const char* format, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	DbgPrint("S3AIR: %s\n", buffer);
	if (gLogFile != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		WriteFile(gLogFile, buffer, (DWORD)strlen(buffer), &written, nullptr);
		WriteFile(gLogFile, "\r\n", 2, &written, nullptr);
	}
}

static void check(bool condition, std::string_view name)
{
	if (!condition)
		++gFailures;
	logLine("[%s] %.*s", condition ? " OK " : "FAIL", (int)name.size(), name.data());
}


// --- C++17 feature tests ---

enum class Zone : uint8_t { AIZ = 0, HCZ, MGZ, CNZ };

struct Vec2 { int x; int y; };

template<typename... Args>
constexpr int sumAll(Args... args) { return (0 + ... + args); }

template<typename T>
constexpr bool isBigType() { if constexpr (sizeof(T) > 4) return true; else return false; }

static std::optional<int> findZone(std::string_view name)
{
	static const std::map<std::string_view, int> zones = { { "AIZ", 0 }, { "HCZ", 1 }, { "MGZ", 2 } };
	const auto it = zones.find(name);
	if (it == zones.end())
		return std::nullopt;
	return it->second;
}

struct GlobalInit
{
	int value;
	GlobalInit() : value(1234) {}
};
static GlobalInit gGlobalInit;		// Tests static constructors

thread_local int tThreadLocalValue = 42;

class Base
{
public:
	virtual ~Base() = default;
	virtual int get() const = 0;
};

class Derived final : public Base
{
public:
	int get() const override { return 7; }
};


static void runLanguageTests()
{
	logLine("--- C++ tests (__cplusplus = %ld) ---", (long)__cplusplus);
	check(__cplusplus >= 201703L, "C++17 or newer");

	check(gGlobalInit.value == 1234, "static constructors ran");
	// Informational only: thread_local is not supported by the toolchain yet, and S3AIR only uses it once (jsoncpp)
	logLine("[info] thread_local initial value: %s", (tThreadLocalValue == 42) ? "works" : "NOT supported");

	// Endianness: the Xbox 360 CPU is big-endian
	const uint32_t probe = 0x11223344;
	const bool bigEndian = (*reinterpret_cast<const uint8_t*>(&probe) == 0x11);
	check(bigEndian, "host is big-endian");
	check(sizeof(void*) == 4, "32-bit pointers");

	// string_view, optional, map
	check(findZone("HCZ").value_or(-1) == 1, "std::optional + std::string_view map lookup");
	check(!findZone("XYZ").has_value(), "std::nullopt");

	// enum class
	Zone zone = Zone::MGZ;
	check(static_cast<int>(zone) == 2, "enum class with underlying type");

	// Range-based for + vector + structured bindings
	std::vector<Vec2> points = { { 1, 2 }, { 3, 4 }, { 5, 6 } };
	int total = 0;
	for (const auto& [x, y] : points)
		total += x * y;
	check(total == 44, "range-for + structured bindings");

	// Variadic templates, fold expressions, constexpr, if constexpr
	static_assert(sumAll(1, 2, 3, 4) == 10, "fold expression");
	check(isBigType<uint64_t>() && !isBigType<uint16_t>(), "if constexpr");

	// std::function + lambdas with captures
	int counter = 0;
	std::function<void(int)> add = [&counter](int value) { counter += value; };
	add(5); add(6);
	check(counter == 11, "std::function + lambda capture");

	// unordered_map + std::string
	std::unordered_map<std::string, int> scores;
	scores["sonic"] = 100;
	scores["tails"] = 50;
	check(scores.size() == 2 && scores["sonic"] == 100, "std::unordered_map<std::string>");

	// unique_ptr + virtual functions
	std::unique_ptr<Base> object = std::make_unique<Derived>();
	check(object->get() == 7, "std::unique_ptr + virtual dispatch");

	// Tuple
	auto [a, b, c] = std::make_tuple(1, 2.5f, std::string("air"));
	check(a == 1 && b == 2.5f && c == "air", "std::tuple + structured bindings");

	// Atomics
	std::atomic<int> atomicValue(0);
	atomicValue.fetch_add(3);
	check(atomicValue.load() == 3, "std::atomic");

	// Exceptions
	bool caught = false;
	try
	{
		throw std::runtime_error("test");
	}
	catch (const std::exception& e)
	{
		caught = (std::strcmp(e.what(), "test") == 0);
	}
	check(caught, "exceptions (throw/catch)");

	// Float formatting
	char floatText[32];
	snprintf(floatText, sizeof(floatText), "%.3f", 3.14159f);
	check(std::string_view(floatText) == "3.142", "printf float formatting");

	// Big heap allocation (S3AIR needs tens of MB)
	const size_t bigSize = 64 * 1024 * 1024;
	uint8_t* big = static_cast<uint8_t*>(malloc(bigSize));
	if (big)
	{
		memset(big, 0xAB, bigSize);
		check(big[bigSize - 1] == 0xAB, "malloc 64 MB");
		free(big);
	}
	else
	{
		check(false, "malloc 64 MB");
	}
}


// --- File I/O test ---

static void runFileTests()
{
	logLine("--- File tests ---");

	// Probe which path forms and APIs can open a file next to the .xex
	const char* probePaths[] = { "game:\\testdata.txt", "d:\\testdata.txt", "testdata.txt" };
	for (const char* path : probePaths)
	{
		const HANDLE handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		const DWORD lastError = (handle == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);

		errno = 0;
		FILE* probe = fopen(path, "rb");
		const int fopenErrno = errno;
		if (probe)
			fclose(probe);

		logLine("  %-22s CreateFileA: %s (error %u)   fopen: %s (errno %d)", path,
				(handle != INVALID_HANDLE_VALUE) ? "ok" : "FAIL", (unsigned)lastError, probe ? "ok" : "FAIL", fopenErrno);
	}

	// Write test
	const HANDLE writeHandle = CreateFileA("game:\\write_test.txt", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	logLine("  CreateFileA write game:\\write_test.txt: %s (error %u)", (writeHandle != INVALID_HANDLE_VALUE) ? "ok" : "FAIL",
			(unsigned)((writeHandle == INVALID_HANDLE_VALUE) ? GetLastError() : 0));
	if (writeHandle != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		WriteFile(writeHandle, "hello\r\n", 7, &written, nullptr);
		CloseHandle(writeHandle);
	}

	// Read back a file shipped next to the .xex (XDK file API, which the port will use)
	HANDLE handle = CreateFileA("game:\\testdata.txt", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	check(handle != INVALID_HANDLE_VALUE, "CreateFileA game:\\testdata.txt (read)");
	if (handle != INVALID_HANDLE_VALUE)
	{
		char text[64] = { 0 };
		DWORD bytesRead = 0;
		ReadFile(handle, text, sizeof(text) - 1, &bytesRead, nullptr);
		CloseHandle(handle);
		check(bytesRead > 0 && std::string_view(text).substr(0, 9) == "Angel Isl", "ReadFile content");
	}

	// Large binary file: the Sonic 3 & Knuckles ROM, if present
	handle = CreateFileA("game:\\Sonic_Knuckles_wSonic3.bin", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle != INVALID_HANDLE_VALUE)
	{
		const DWORD size = GetFileSize(handle, nullptr);
		std::vector<uint8_t> rom(size);
		DWORD bytesRead = 0;
		ReadFile(handle, rom.data(), size, &bytesRead, nullptr);
		CloseHandle(handle);
		logLine("ROM size: %u bytes, read %u", (unsigned)size, (unsigned)bytesRead);
		check(size == 4194304 && bytesRead == size, "ROM read completely (4 MB)");
	}
	else
	{
		logLine("(ROM not found next to the .xex, skipped)");
	}
}


// --- GPU diagnostics (hardware-only hang in the first Present) ---

static volatile LONG gVBlankCount = 0;

// Runs at DPC level: keep it to integer work only (no FP/VMX, no library calls)
static void WINAPI onVerticalBlank(D3DVBLANKDATA*)
{
	gVBlankCount = gVBlankCount + 1;
}

// Polls a fence with a timeout instead of blocking, so a GPU that never consumes commands cannot freeze the console
static bool waitForFence(DWORD fence, DWORD timeoutMs, DWORD& elapsedMs)
{
	const DWORD start = GetTickCount();
	while (D3DDevice_IsFencePending(fence))
	{
		elapsedMs = GetTickCount() - start;
		if (elapsedMs >= timeoutMs)
			return false;
		Sleep(1);
	}
	elapsedMs = GetTickCount() - start;
	return true;
}

// Returns true if the GPU processes commands, i.e. Present is safe to try
static bool runGpuDiagnostics(IDirect3DDevice9* device, D3DCOLOR clearColor)
{
	logLine("--- GPU diagnostics ---");

	// 1. Display controller: the scan line should move
	for (int i = 0; i < 5; ++i)
	{
		D3DRASTER_STATUS raster;
		ZeroMemory(&raster, sizeof(raster));
		D3DDevice_GetRasterStatus(device, &raster);
		logLine("  [1] raster: InVBlank=%d ScanLine=%u", (int)raster.InVBlank, (unsigned)raster.ScanLine);
		Sleep(7);
	}

	// 2. Fence with no other commands: does the GPU consume the ring buffer at all?
	DWORD elapsedMs = 0;
	DWORD fence = D3DDevice_InsertFence(device);
	const bool emptyFenceDone = waitForFence(fence, 2000, elapsedMs);
	logLine("  [2] empty fence 0x%08X: %s after %u ms", (unsigned)fence, emptyFenceDone ? "completed" : "TIMEOUT", (unsigned)elapsedMs);

	// 3. Clear + fence: does the GPU execute a real command?
	device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, clearColor, 1.0f, 0);
	fence = D3DDevice_InsertFence(device);
	const bool clearFenceDone = waitForFence(fence, 2000, elapsedMs);
	logLine("  [3] clear + fence 0x%08X: %s after %u ms", (unsigned)fence, clearFenceDone ? "completed" : "TIMEOUT", (unsigned)elapsedMs);

	// 4. VBlank interrupts / DPC callbacks
	gVBlankCount = 0;
	device->SetVerticalBlankCallback(onVerticalBlank);
	Sleep(500);
	device->SetVerticalBlankCallback(nullptr);
	logLine("  [4] vblank callbacks in 500 ms: %d (expected ~30)", (int)gVBlankCount);

	// 5. Busy flag
	logLine("  [5] IsBusy: %d", (int)D3DDevice_IsBusy(device));

	const bool gpuWorks = emptyFenceDone && clearFenceDone;
	logLine("  => GPU %s", gpuWorks ? "processes commands, trying Present" : "does NOT process commands, skipping Present");
	return gpuWorks;
}


// --- D3D9 + XInput test ---

static void runGraphicsLoop()
{
	logLine("--- Graphics test ---");

	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	check(d3d != nullptr, "Direct3DCreate9");
	if (!d3d)
		return;

	XVIDEO_MODE videoMode;
	ZeroMemory(&videoMode, sizeof(videoMode));
	XGetVideoMode(&videoMode);
	logLine("Video mode: %ux%u, widescreen=%d, HD=%d", (unsigned)videoMode.dwDisplayWidth, (unsigned)videoMode.dwDisplayHeight, (int)videoMode.fIsWideScreen, (int)videoMode.fIsHiDef);

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

	IDirect3DDevice9* device = nullptr;
	const HRESULT result = d3d->CreateDevice(0, D3DDEVTYPE_HAL, nullptr, D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &device);
	check(SUCCEEDED(result) && device != nullptr, "CreateDevice");
	if (!device)
		return;

	const D3DCOLOR baseColor = (gFailures == 0) ? D3DCOLOR_XRGB(0, 160, 60) : D3DCOLOR_XRGB(200, 0, 0);

	if (!runGpuDiagnostics(device, baseColor))
	{
		device->Release();
		d3d->Release();
		return;
	}

	logLine("Running. Green = all tests OK, red = failures. Press A to cycle colours, BACK to exit.");

	const D3DCOLOR colors[] = { baseColor, D3DCOLOR_XRGB(0, 60, 200), D3DCOLOR_XRGB(230, 200, 0) };
	int colorIndex = 0;
	bool previousA = false;
	uint32_t frame = 0;
	const DWORD startTicks = GetTickCount();

	while (true)
	{
		// Trace every step of the first frames, then once per second, to locate hardware-only crashes
		const bool trace = (frame < 3) || (frame % 60 == 0);
		if (trace)
			logLine("[t=%5u ms] frame %u: XInputGetState", (unsigned)(GetTickCount() - startTicks), (unsigned)frame);

		XINPUT_STATE state;
		ZeroMemory(&state, sizeof(state));
		if (XInputGetState(0, &state) == ERROR_SUCCESS)
		{
			const bool pressedA = (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
			if (pressedA && !previousA)
			{
				colorIndex = (colorIndex + 1) % 3;
				logLine("A pressed, colour %d", colorIndex);
			}
			previousA = pressedA;

			if (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK)
				break;

			// Y: deliberate crash, to check that crashFilter catches exceptions on this console
			if (state.Gamepad.wButtons & XINPUT_GAMEPAD_Y)
			{
				logLine("Y pressed: deliberate null pointer write");
				*reinterpret_cast<volatile int*>(0) = 0x5A41;
			}
		}

		if (trace)
			logLine("[t=%5u ms] frame %u: Clear", (unsigned)(GetTickCount() - startTicks), (unsigned)frame);
		device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, colors[colorIndex], 1.0f, 0);

		if (trace)
			logLine("[t=%5u ms] frame %u: Present", (unsigned)(GetTickCount() - startTicks), (unsigned)frame);
		device->Present(nullptr, nullptr, nullptr, nullptr);

		if (trace)
			logLine("[t=%5u ms] frame %u: done", (unsigned)(GetTickCount() - startTicks), (unsigned)frame);
#ifdef CRASH_TEST_FRAME
		if (frame == CRASH_TEST_FRAME)
		{
			logLine("CRASH_TEST_FRAME reached: deliberate null pointer write");
			*reinterpret_cast<volatile int*>(0) = 0x5A41;
		}
#endif
		++frame;
		if (frame == 300)
			logLine("300 frames presented");
	}

	logLine("Exiting after %u frames", (unsigned)frame);
	device->Release();
	d3d->Release();
}


// --- Crash reporting ---

static bool isStackAddress(DWORD address)
{
	return (address >= 0x70000000 && address < 0x80000000 && (address & 7) == 0);
}

// Logs the exception and a stack back-trace before the dashboard's crash handler takes over.
// Symbolize the addresses with: python -I x360\tools\addr2sym.py bin\Release\default.elf <addresses>
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* info)
{
	const EXCEPTION_RECORD* record = info->ExceptionRecord;
	const CONTEXT* context = info->ContextRecord;
	logLine("!!! CRASH: exception 0x%08X at 0x%08X (thread %u)", (unsigned)record->ExceptionCode, (unsigned)(uintptr_t)record->ExceptionAddress, (unsigned)GetCurrentThreadId());
	for (DWORD i = 0; i < record->NumberParameters && i < 4; ++i)
		logLine("!!!   info[%u] = 0x%08X", (unsigned)i, (unsigned)record->ExceptionInformation[i]);
	logLine("!!!   IAR 0x%08X  LR 0x%08X  SP 0x%08X", (unsigned)context->Iar, (unsigned)context->Lr, (unsigned)context->Gpr1);
	logLine("!!!   r3 0x%08X  r4 0x%08X  r5 0x%08X  r31 0x%08X", (unsigned)context->Gpr3, (unsigned)context->Gpr4, (unsigned)context->Gpr5, (unsigned)context->Gpr31);

	// Walk the back chain: [sp] = caller's sp, the return address is saved at caller_sp - 8.
	// MS-compiled frames store both as 32-bit words (stwu/stw), clang frames as 64-bit doublewords
	// (stdu/std), where the 32-bit value sits in the low word at +4 and the high word is 0.
	auto readSlot = [](DWORD address) -> DWORD
	{
		const DWORD high = *reinterpret_cast<const DWORD*>(address);
		return (high != 0) ? high : *reinterpret_cast<const DWORD*>(address + 4);
	};
	DWORD stackPointer = (DWORD)context->Gpr1;
	for (int depth = 0; depth < 12 && isStackAddress(stackPointer); ++depth)
	{
		const DWORD callerStackPointer = readSlot(stackPointer);
		if (!isStackAddress(callerStackPointer) || callerStackPointer <= stackPointer)
			break;
		const DWORD returnAddress = readSlot(callerStackPointer - 8);
		logLine("!!!   frame %2d: return 0x%08X  (sp 0x%08X)", depth, (unsigned)returnAddress, (unsigned)callerStackPointer);
		stackPointer = callerStackPointer;
	}

	if (gLogFile != INVALID_HANDLE_VALUE)
		FlushFileBuffers(gLogFile);
	return EXCEPTION_CONTINUE_SEARCH;
}


int main()
{
	gLogFile = CreateFileA("game:\\cpp17hello.log", GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	SetUnhandledExceptionFilter(crashFilter);
	logLine("=== S3AIR X360 toolchain smoke test (v2: crash trace) ===");
	if (gLogFile == INVALID_HANDLE_VALUE)
		logLine("Could not open game:\\cpp17hello.log for writing (error %u)", (unsigned)GetLastError());

	runLanguageTests();
	runFileTests();
	logLine("=== %d failure(s) ===", gFailures);

	runGraphicsLoop();

	if (gLogFile != INVALID_HANDLE_VALUE)
		CloseHandle(gLogFile);

	// Go back to the dashboard (Aurora/FSD on RGH); the RXDK runtime would reboot the console if main returned
	XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
	return 0;
}
