/*
*	Xbox 360 port of Sonic 3 A.I.R.
*	SDL2-lite: threads, mutexes and condition variables on top of the XDK's Win32 subset.
*/

#include "SDL_x360_internal.h"

#include <new>


struct SDL_Thread
{
	HANDLE mHandle = nullptr;
	SDL_ThreadFunction mFunction = nullptr;
	void* mData = nullptr;
	int mStatus = 0;
};

struct SDL_mutex
{
	CRITICAL_SECTION mCriticalSection;
};

// Same algorithm as SDL2's generic condition variable (src/thread/generic/SDL_syscond.c):
// the XDK has semaphores but no condition variables
struct SDL_cond
{
	CRITICAL_SECTION mLock;
	int mWaiting = 0;
	int mSignals = 0;
	HANDLE mWaitSemaphore = nullptr;
	HANDLE mWaitDoneSemaphore = nullptr;
};


namespace
{
	// Default stack for SDL_CreateThread; the XDK's own default (64 KB) is too small for engine threads
	const SIZE_T DEFAULT_THREAD_STACK_SIZE = 1024 * 1024;

	DWORD WINAPI threadEntry(LPVOID parameter)
	{
		SDL_Thread* thread = static_cast<SDL_Thread*>(parameter);
		thread->mStatus = thread->mFunction(thread->mData);
		return 0;
	}

	SDL_Thread* createThread(SDL_ThreadFunction function, const char* name, size_t stackSize, void* data)
	{
		SDL_Thread* thread = new (std::nothrow) SDL_Thread();
		if (nullptr == thread)
		{
			SDL_OutOfMemory();
			return nullptr;
		}
		thread->mFunction = function;
		thread->mData = data;
		thread->mHandle = CreateThread(nullptr, (stackSize > 0) ? stackSize : DEFAULT_THREAD_STACK_SIZE, threadEntry, thread, CREATE_SUSPENDED, nullptr);
		if (nullptr == thread->mHandle)
		{
			sdl2lite::setError("CreateThread failed for thread '%s' (error %u)", name ? name : "", (unsigned)GetLastError());
			delete thread;
			return nullptr;
		}

		// On the Xbox 360, a new thread runs on the same hardware thread as its creator unless told otherwise;
		// spread the application's threads (e.g. rmxmedia's audio job workers) over hardware threads 5, 3 and 1,
		// away from the main thread (0) and the parallel rendering workers (2 and 4)
		static const DWORD PROCESSORS[] = { 5, 3, 1 };
		static LONG nextProcessor = 0;
		const LONG index = (InterlockedIncrement(&nextProcessor) - 1) % 3;
		XSetThreadProcessor(thread->mHandle, PROCESSORS[index]);
		ResumeThread(thread->mHandle);
		DbgPrint("SDL2-lite: thread '%s' started on hardware thread %u\n", name ? name : "", (unsigned)PROCESSORS[index]);
		return thread;
	}
}


extern "C"
{

/* ----- Threads ----- */

SDL_Thread* SDL_CreateThread(SDL_ThreadFunction fn, const char* name, void* data)
{
	return createThread(fn, name, 0, data);
}

SDL_Thread* SDL_CreateThreadWithStackSize(SDL_ThreadFunction fn, const char* name, const size_t stacksize, void* data)
{
	return createThread(fn, name, stacksize, data);
}

void SDL_WaitThread(SDL_Thread* thread, int* status)
{
	if (nullptr == thread)
		return;
	WaitForSingleObject(thread->mHandle, INFINITE);
	CloseHandle(thread->mHandle);
	if (nullptr != status)
		*status = thread->mStatus;
	delete thread;
}

void SDL_DetachThread(SDL_Thread* thread)
{
	// The thread keeps running; only the handle and bookkeeping are released (the struct leaks
	// intentionally, as the running thread still reads its function and data from it)
	if (nullptr != thread)
		CloseHandle(thread->mHandle);
}

SDL_threadID SDL_ThreadID(void)
{
	return (SDL_threadID)GetCurrentThreadId();
}


/* ----- Mutexes ----- */

SDL_mutex* SDL_CreateMutex(void)
{
	SDL_mutex* mutex = new (std::nothrow) SDL_mutex();
	if (nullptr == mutex)
	{
		SDL_OutOfMemory();
		return nullptr;
	}
	InitializeCriticalSection(&mutex->mCriticalSection);
	return mutex;
}

void SDL_DestroyMutex(SDL_mutex* mutex)
{
	if (nullptr == mutex)
		return;
	DeleteCriticalSection(&mutex->mCriticalSection);
	delete mutex;
}

int SDL_LockMutex(SDL_mutex* mutex)
{
	if (nullptr == mutex)
		return 0;
	EnterCriticalSection(&mutex->mCriticalSection);
	return 0;
}

int SDL_TryLockMutex(SDL_mutex* mutex)
{
	if (nullptr == mutex)
		return 0;
	return TryEnterCriticalSection(&mutex->mCriticalSection) ? 0 : SDL_MUTEX_TIMEDOUT;
}

int SDL_UnlockMutex(SDL_mutex* mutex)
{
	if (nullptr == mutex)
		return 0;
	LeaveCriticalSection(&mutex->mCriticalSection);
	return 0;
}


/* ----- Condition variables ----- */

SDL_cond* SDL_CreateCond(void)
{
	SDL_cond* cond = new (std::nothrow) SDL_cond();
	if (nullptr == cond)
	{
		SDL_OutOfMemory();
		return nullptr;
	}
	InitializeCriticalSection(&cond->mLock);
	cond->mWaitSemaphore = CreateSemaphore(nullptr, 0, 0x7fffffff, nullptr);
	cond->mWaitDoneSemaphore = CreateSemaphore(nullptr, 0, 0x7fffffff, nullptr);
	if (nullptr == cond->mWaitSemaphore || nullptr == cond->mWaitDoneSemaphore)
	{
		sdl2lite::setError("CreateSemaphore failed (error %u)", (unsigned)GetLastError());
		SDL_DestroyCond(cond);
		return nullptr;
	}
	return cond;
}

void SDL_DestroyCond(SDL_cond* cond)
{
	if (nullptr == cond)
		return;
	if (nullptr != cond->mWaitSemaphore)
		CloseHandle(cond->mWaitSemaphore);
	if (nullptr != cond->mWaitDoneSemaphore)
		CloseHandle(cond->mWaitDoneSemaphore);
	DeleteCriticalSection(&cond->mLock);
	delete cond;
}

int SDL_CondSignal(SDL_cond* cond)
{
	if (nullptr == cond)
		return SDL_InvalidParamError("cond");

	EnterCriticalSection(&cond->mLock);
	if (cond->mWaiting > cond->mSignals)
	{
		++cond->mSignals;
		ReleaseSemaphore(cond->mWaitSemaphore, 1, nullptr);
		LeaveCriticalSection(&cond->mLock);
		WaitForSingleObject(cond->mWaitDoneSemaphore, INFINITE);
	}
	else
	{
		LeaveCriticalSection(&cond->mLock);
	}
	return 0;
}

int SDL_CondBroadcast(SDL_cond* cond)
{
	if (nullptr == cond)
		return SDL_InvalidParamError("cond");

	EnterCriticalSection(&cond->mLock);
	if (cond->mWaiting > cond->mSignals)
	{
		const int numWaiting = cond->mWaiting - cond->mSignals;
		cond->mSignals = cond->mWaiting;
		ReleaseSemaphore(cond->mWaitSemaphore, numWaiting, nullptr);
		LeaveCriticalSection(&cond->mLock);
		for (int i = 0; i < numWaiting; ++i)
			WaitForSingleObject(cond->mWaitDoneSemaphore, INFINITE);
	}
	else
	{
		LeaveCriticalSection(&cond->mLock);
	}
	return 0;
}

int SDL_CondWaitTimeout(SDL_cond* cond, SDL_mutex* mutex, Uint32 ms)
{
	if (nullptr == cond)
		return SDL_InvalidParamError("cond");

	EnterCriticalSection(&cond->mLock);
	++cond->mWaiting;
	LeaveCriticalSection(&cond->mLock);

	SDL_UnlockMutex(mutex);
	const DWORD result = WaitForSingleObject(cond->mWaitSemaphore, (ms == SDL_MUTEX_MAXWAIT) ? INFINITE : ms);

	EnterCriticalSection(&cond->mLock);
	if (cond->mSignals > 0)
	{
		// A signal arrived after the timeout: consume it, so the semaphore count stays in sync
		if (result != WAIT_OBJECT_0)
			WaitForSingleObject(cond->mWaitSemaphore, INFINITE);
		ReleaseSemaphore(cond->mWaitDoneSemaphore, 1, nullptr);
		--cond->mSignals;
	}
	--cond->mWaiting;
	LeaveCriticalSection(&cond->mLock);

	SDL_LockMutex(mutex);
	return (result == WAIT_OBJECT_0) ? 0 : SDL_MUTEX_TIMEDOUT;
}

int SDL_CondWait(SDL_cond* cond, SDL_mutex* mutex)
{
	return SDL_CondWaitTimeout(cond, mutex, SDL_MUTEX_MAXWAIT);
}

}
