// Phase 2 runtime test for the Sonic 3 A.I.R. Xbox 360 port.
// Links zlib, rmxbase, rmxmedia, lemonscript and SDL2-lite, runs a few checks of each, then runs
// upstream's own lemonscript test (Oxygen/lemonscript/source/test/main.cpp, compiled here with
// main renamed to lemon_test_main), which compiles script/mainscript.lemon and executes it,
// including the script unit tests (script/unittests.lemon).
// All console output goes to game:\libtest.log (written through immediately) and to DbgPrint.

// rmx headers first: <xtl.h> defines Windows macros (min, max, ERROR, IGNORE) that break them
#include "rmxmedia.h"
#include <xtl.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <streambuf>
#include <string>

#undef main		// The project defines main=lemon_test_main for upstream's test main.cpp

extern "C" ULONG DbgPrint(const char* format, ...);
int lemon_test_main(int argc, char** argv);


namespace
{
	// std::streambuf that writes every byte straight to the log file and sends complete lines to
	// DbgPrint, so nothing is lost if the console freezes
	class LogStreamBuf : public std::streambuf
	{
	public:
		explicit LogStreamBuf(const char* filename)
		{
			mFile = CreateFileA(filename, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		}

	protected:
		int overflow(int ch) override
		{
			if (ch != EOF)
			{
				const char c = (char)ch;
				xsputn(&c, 1);
			}
			return ch;
		}

		std::streamsize xsputn(const char* text, std::streamsize count) override
		{
			if (mFile != INVALID_HANDLE_VALUE)
			{
				DWORD written = 0;
				WriteFile(mFile, text, (DWORD)count, &written, nullptr);
				FlushFileBuffers(mFile);
			}
			for (std::streamsize i = 0; i < count; ++i)
			{
				if (text[i] == '\n')
				{
					DbgPrint("LIBTEST: %s\n", mLine.c_str());
					mLine.clear();
				}
				else if (text[i] != '\r')
				{
					mLine += text[i];
				}
			}
			return count;
		}

	private:
		HANDLE mFile = INVALID_HANDLE_VALUE;
		std::string mLine;
	};

	int gFailures = 0;

	struct ThreadData
	{
		SDL_mutex* mMutex = nullptr;
		SDL_cond* mCond = nullptr;
		std::atomic<int> mCounter { 0 };
		bool mReady = false;
	};

	// A plain function: clang in MS-compatibility mode won't convert a lambda to SDL's extern "C" SDL_ThreadFunction
	int threadFunction(void* param)
	{
		ThreadData& data = *static_cast<ThreadData*>(param);
		for (int i = 0; i < 10000; ++i)
			data.mCounter.fetch_add(1);
		SDL_LockMutex(data.mMutex);
		data.mReady = true;
		SDL_CondSignal(data.mCond);
		SDL_UnlockMutex(data.mMutex);
		return 42;
	}

	void check(bool condition, const char* name)
	{
		if (!condition)
			++gFailures;
		std::cout << (condition ? "[ OK ] " : "[FAIL] ") << name << "\r\n";
	}

	void runLibraryChecks()
	{
		std::cout << "--- rmxbase ---\r\n";

		// Endianness helpers and string formatting
		check(swapBytes32(0x11223344) == 0x44332211, "swapBytes32");
		String text;
		text << "value=" << 1234 << " hex=" << rmx::hexString(0xABCD, 4);
		check(text == "value=1234 hex=0xabcd" || text == "value=1234 hex=0xABCD", *(String("String formatting: ") + text));

		// zlib round trip through rmx
		std::vector<uint8> original(64 * 1024);
		for (size_t i = 0; i < original.size(); ++i)
			original[i] = (uint8)((i * 7) ^ (i >> 5));
		std::vector<uint8> compressed, decompressed;
		const bool encoded = ZlibDeflate::encode(compressed, original.data(), original.size());
		const bool decoded = encoded && ZlibDeflate::decode(decompressed, compressed.data(), compressed.size());
		check(decoded && decompressed == original, "zlib deflate/inflate round trip");

		// JSON parsing
		const Json::Value json = rmx::JsonHelper::loadFromString("{ \"zone\": \"AIZ\", \"act\": 1, \"rings\": [10, 20, 30] }");
		check(json["zone"].asString() == "AIZ" && json["act"].asInt() == 1 && json["rings"][2].asInt() == 30, "JSON parsing");

		// File system: relative path (cwd is game:\) through FTX::FileSystem
		std::vector<uint8> script;
		const bool readOk = FTX::FileSystem->readFile(L"script/mainscript.lemon", script);
		check(readOk && script.size() > 100, "FTX::FileSystem readFile script/mainscript.lemon");

		std::cout << "--- SDL2-lite ---\r\n";

		// Threads, mutex, condition variable
		ThreadData data;
		data.mMutex = SDL_CreateMutex();
		data.mCond = SDL_CreateCond();
		SDL_Thread* thread = SDL_CreateThread(threadFunction, "libtest", &data);
		SDL_LockMutex(data.mMutex);
		const Uint32 waitStart = SDL_GetTicks();
		while (!data.mReady && SDL_GetTicks() - waitStart < 3000)
			SDL_CondWaitTimeout(data.mCond, data.mMutex, 100);
		const bool ready = data.mReady;
		SDL_UnlockMutex(data.mMutex);
		int status = 0;
		SDL_WaitThread(thread, &status);
		check(nullptr != thread && ready && status == 42 && data.mCounter.load() == 10000, "SDL thread + mutex + condition variable");
		SDL_DestroyCond(data.mCond);
		SDL_DestroyMutex(data.mMutex);

		// Timer
		const Uint32 before = SDL_GetTicks();
		SDL_Delay(50);
		const Uint32 elapsed = SDL_GetTicks() - before;
		check(elapsed >= 40 && elapsed < 500, "SDL_Delay / SDL_GetTicks");

		// RWops
		SDL_RWops* rw = SDL_RWFromFile("game:\\script\\mainscript.lemon", "rb");
		const Sint64 rwSize = (nullptr != rw) ? SDL_RWsize(rw) : -1;
		if (nullptr != rw)
			SDL_RWclose(rw);
		check(rwSize == (Sint64)script.size(), "SDL_RWFromFile + SDL_RWsize");
	}
}


// Returns to the dashboard if the test does not finish in time (known issue: the lemonscript runtime
// loops forever on big-endian until the opcode parameter accesses are fixed), so the console never
// needs a hard reboot
int watchdogFunction(void*)
{
	const Uint32 TIMEOUT_MS = 30000;
	SDL_Delay(TIMEOUT_MS);
	std::cout << "\r\n=== WATCHDOG: test did not finish within " << (TIMEOUT_MS / 1000) << " s, returning to the dashboard ===\r\n";
	XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
	return 0;
}


int main()
{
	static LogStreamBuf logBuffer("game:\\libtest.log");
	std::cout.rdbuf(&logBuffer);
	std::cerr.rdbuf(&logBuffer);

	std::cout << "=== S3AIR X360 phase 2 library test ===\r\n";
	SDL_DetachThread(SDL_CreateThread(watchdogFunction, "watchdog", nullptr));
	INIT_RMX;
	runLibraryChecks();
	std::cout << "=== " << gFailures << " library check failure(s) ===\r\n\r\n";

	std::cout << "=== Upstream lemonscript test (compile + run script/mainscript.lemon) ===\r\n";
	char programName[] = "game:\\default.xex";
	char* argv[] = { programName, nullptr };
	const int result = lemon_test_main(1, argv);
	std::cout << "\r\n=== lemon_test_main returned " << result << " ===\r\n";

	// Back to the dashboard (returning from main would reboot the console with the RXDK runtime)
	XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
	return 0;
}
