/*
*	Xbox 360 port of Sonic 3 A.I.R.
*	SDL2-lite extension: run a job on several of the Xenon's hardware threads at once (SDL_X360_ParallelFor).
*
*	The Xenon has 3 cores with 2 hardware threads each. The game's main thread runs on hardware thread 0,
*	so the workers are pinned to the other cores first (hardware threads 2 and 4), as two threads on the
*	same core share its execution units.
*/

#include "SDL_x360_internal.h"
#include "SDL_x360.h"


namespace
{
	const int MAX_WORKERS = 3;
	const DWORD WORKER_PROCESSORS[MAX_WORKERS] = { 2, 4, 3 };	// Hardware threads: other cores first
	const int NUM_WORKERS = 2;									// Workers actually used (plus the calling thread)

	struct Worker
	{
		HANDLE mThread = nullptr;
		HANDLE mStartEvent = nullptr;
		int mIndex = 0;
	};

	struct Pool
	{
		bool mInitialized = false;
		Worker mWorkers[MAX_WORKERS];
		HANDLE mDoneEvent = nullptr;
		volatile LONG mRemaining = 0;

		// Current job
		void (*mJob)(int index, void* userData) = nullptr;
		void* mUserData = nullptr;
		int mCount = 0;
	};
	Pool gPool;

	DWORD WINAPI workerThreadFunction(LPVOID parameter)
	{
		Worker& worker = *static_cast<Worker*>(parameter);
		while (true)
		{
			WaitForSingleObject(worker.mStartEvent, INFINITE);
			const int jobIndex = worker.mIndex + 1;		// Index 0 is run by the calling thread
			if (jobIndex < gPool.mCount)
				gPool.mJob(jobIndex, gPool.mUserData);
			if (InterlockedDecrement(&gPool.mRemaining) == 0)
				SetEvent(gPool.mDoneEvent);
		}
		return 0;
	}

	bool initializePool()
	{
		if (gPool.mInitialized)
			return true;
		gPool.mInitialized = true;

		gPool.mDoneEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		for (int i = 0; i < NUM_WORKERS; ++i)
		{
			Worker& worker = gPool.mWorkers[i];
			worker.mIndex = i;
			worker.mStartEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			worker.mThread = CreateThread(nullptr, 256 * 1024, workerThreadFunction, &worker, CREATE_SUSPENDED, nullptr);
			if (nullptr == worker.mThread || nullptr == worker.mStartEvent)
			{
				DbgPrint("SDL2-lite: creating parallel worker %d failed\n", i);
				return false;
			}
			XSetThreadProcessor(worker.mThread, WORKER_PROCESSORS[i]);
			SetThreadPriority(worker.mThread, THREAD_PRIORITY_ABOVE_NORMAL);
			ResumeThread(worker.mThread);
		}
		DbgPrint("SDL2-lite: %d parallel workers ready\n", NUM_WORKERS);
		return true;
	}
}


extern "C" int SDL_X360_GetParallelThreadCount(void)
{
	return NUM_WORKERS + 1;
}

extern "C" void SDL_X360_ParallelFor(int count, void (*job)(int index, void* userData), void* userData)
{
	if (count <= 0)
		return;
	if (count == 1 || !initializePool())
	{
		for (int i = 0; i < count; ++i)
			job(i, userData);
		return;
	}

	// Indices beyond the number of threads are not distributed; callers are expected to use SDL_X360_GetParallelThreadCount
	const int numWorkersUsed = (count - 1 < NUM_WORKERS) ? (count - 1) : NUM_WORKERS;
	gPool.mJob = job;
	gPool.mUserData = userData;
	gPool.mCount = numWorkersUsed + 1;
	gPool.mRemaining = numWorkersUsed;
	MemoryBarrier();
	for (int i = 0; i < numWorkersUsed; ++i)
		SetEvent(gPool.mWorkers[i].mStartEvent);

	job(0, userData);

	WaitForSingleObject(gPool.mDoneEvent, INFINITE);
	MemoryBarrier();

	// Any remaining indices (only if count exceeds the thread count) run here
	for (int i = numWorkersUsed + 1; i < count; ++i)
		job(i, userData);
}
