/*
*	Xbox 360 port of Sonic 3 A.I.R.
*
*	Process-level platform glue, without touching upstream's main():
*	 - std::cout / std::cerr go to game:\s3air.log (written by a background thread, as writes to the console's
*	   storage are slow) and to the kernel debug output (DbgPrint: xenia.log, or a devkit's debugger)
*	 - when the game exits, go back to the dashboard: with the RXDK runtime, returning from main would
*	   reboot the console
*	Both are set up by a static constructor, i.e. before main() runs.
*/

#include "rmxbase.h"

// After the rmx headers: <xtl.h> defines Windows macros (min, max, ERROR, IGNORE) that break them
#include <xtl.h>

#include <cstdlib>
#include <iostream>
#include <streambuf>
#include <string>

extern "C" ULONG DbgPrint(const char* format, ...);


namespace
{
	// Log output into game:\s3air.log
	//  -> Writing to the console's storage is slow (especially to USB, where a write plus flush can take many
	//     milliseconds), so the logging thread only appends to a buffer, and a background thread writes it to the file
	class LogStreamBuf : public std::streambuf
	{
	public:
		explicit LogStreamBuf(const char* filename)
		{
			InitializeCriticalSection(&mBufferLock);
			InitializeCriticalSection(&mFileLock);
			mFile = CreateFileA(filename, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			mWakeUpEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			HANDLE thread = CreateThread(nullptr, 64 * 1024, &LogStreamBuf::writerThreadFunction, this, CREATE_SUSPENDED, nullptr);
			if (nullptr != thread)
			{
				XSetThreadProcessor(thread, 5);		// Hardware thread 5: the main thread runs on 0, the parallel rendering workers on 2 and 4
				ResumeThread(thread);
			}
		}

		// Writes everything that's still buffered, from the calling thread (used when exiting)
		void flushNow()
		{
			writePending();
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
			EnterCriticalSection(&mBufferLock);
			mPending.append(text, (size_t)count);
			LeaveCriticalSection(&mBufferLock);

			// The kernel debug output gets complete lines right away
			bool completedLine = false;
			for (std::streamsize i = 0; i < count; ++i)
			{
				if (text[i] == '\n')
				{
					DbgPrint("S3AIR: %s\n", mLine.c_str());
					mLine.clear();
					completedLine = true;
				}
				else if (text[i] != '\r')
				{
					mLine += text[i];
				}
			}
			if (completedLine)
				SetEvent(mWakeUpEvent);
			return count;
		}

	private:
		static DWORD WINAPI writerThreadFunction(LPVOID parameter)
		{
			LogStreamBuf& self = *static_cast<LogStreamBuf*>(parameter);
			while (true)
			{
				WaitForSingleObject(self.mWakeUpEvent, 1000);
				self.writePending();
			}
			return 0;
		}

		void writePending()
		{
			std::string text;
			EnterCriticalSection(&mBufferLock);
			text.swap(mPending);
			LeaveCriticalSection(&mBufferLock);
			if (text.empty() || mFile == INVALID_HANDLE_VALUE)
				return;

			EnterCriticalSection(&mFileLock);
			DWORD written = 0;
			WriteFile(mFile, text.data(), (DWORD)text.size(), &written, nullptr);
			FlushFileBuffers(mFile);		// Keeps the log useful after a crash; only the background thread waits for it
			LeaveCriticalSection(&mFileLock);
		}

	private:
		HANDLE mFile = INVALID_HANDLE_VALUE;
		HANDLE mWakeUpEvent = nullptr;
		CRITICAL_SECTION mBufferLock;
		CRITICAL_SECTION mFileLock;
		std::string mPending;
		std::string mLine;		// Only for DbgPrint, written by the logging threads
	};

	LogStreamBuf* gLogBuffer = nullptr;

	void returnToDashboard()
	{
		std::cout << "[X360] Exiting, returning to the dashboard" << std::endl;
		if (nullptr != gLogBuffer)
			gLogBuffer->flushNow();
		XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
	}

	struct X360ProcessSetup
	{
		X360ProcessSetup()
		{
			static LogStreamBuf logBuffer("game:\\s3air.log");
			gLogBuffer = &logBuffer;
			std::cout.rdbuf(&logBuffer);
			std::cerr.rdbuf(&logBuffer);
			std::cout << "[X360] Sonic 3 A.I.R. for Xbox 360 starting" << std::endl;
			atexit(returnToDashboard);
		}
	};

	X360ProcessSetup gX360ProcessSetup;
}
