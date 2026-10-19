/*
*	Xbox 360 port of Sonic 3 A.I.R.
*	SDL2-lite: audio output through XAudio2.
*
*	SDL's callback model on top of an XAudio2 source voice: a feeder thread keeps a few buffers queued on the
*	voice, filling each one by calling the application's audio callback (rmxmedia's AudioManager, which runs the
*	whole mixing, including the sound chip emulation and OGG decoding, inside that callback).
*	 - 16-bit PCM in the native byte order, which is what XAudio2 on the Xbox 360 expects (big endian)
*	 - The feeder thread runs on hardware thread 3, the XAudio2 engine on hardware thread 5, both away from the
*	   main thread (hardware thread 0) and the parallel rendering workers (2 and 4)
*	 - SDL_LockAudioDevice uses the same lock as the callback invocation
*/

#include "SDL_x360_internal.h"
#include <xaudio2.h>

#include <stdlib.h>
#include <string.h>


namespace
{
	const SDL_AudioDeviceID DEVICE_ID = 1;
	const int NUM_BUFFERS = 3;
	const DWORD FEEDER_PROCESSOR = 3;

	class VoiceCallback : public IXAudio2VoiceCallback
	{
	public:
		HANDLE mBufferEndEvent = nullptr;

		STDMETHOD_(void, OnVoiceProcessingPassStart)(UINT32 bytesRequired) {}
		STDMETHOD_(void, OnVoiceProcessingPassEnd)() {}
		STDMETHOD_(void, OnStreamEnd)() {}
		STDMETHOD_(void, OnBufferStart)(void* bufferContext) {}
		STDMETHOD_(void, OnBufferEnd)(void* bufferContext)  { SetEvent(mBufferEndEvent); }
		STDMETHOD_(void, OnLoopEnd)(void* bufferContext) {}
		STDMETHOD_(void, OnVoiceError)(void* bufferContext, HRESULT error) {}
	};

	struct AudioDevice
	{
		bool mOpen = false;
		volatile bool mPaused = true;
		volatile bool mShutdown = false;

		SDL_AudioSpec mSpec;
		IXAudio2* mXAudio2 = nullptr;
		IXAudio2MasteringVoice* mMasteringVoice = nullptr;
		IXAudio2SourceVoice* mSourceVoice = nullptr;
		VoiceCallback mVoiceCallback;

		CRITICAL_SECTION mLock;
		bool mLockInitialized = false;
		HANDLE mFeederThread = nullptr;

		Uint8* mBuffers[NUM_BUFFERS] = { nullptr };
		int mNextBuffer = 0;
		Uint32 mBufferBytes = 0;
	};
	AudioDevice gDevice;

	void releaseDevice()
	{
		if (nullptr != gDevice.mSourceVoice)
		{
			gDevice.mSourceVoice->Stop(0);
			gDevice.mSourceVoice->DestroyVoice();
			gDevice.mSourceVoice = nullptr;
		}
		if (nullptr != gDevice.mMasteringVoice)
		{
			gDevice.mMasteringVoice->DestroyVoice();
			gDevice.mMasteringVoice = nullptr;
		}
		if (nullptr != gDevice.mXAudio2)
		{
			gDevice.mXAudio2->Release();
			gDevice.mXAudio2 = nullptr;
		}
		for (int i = 0; i < NUM_BUFFERS; ++i)
		{
			free(gDevice.mBuffers[i]);
			gDevice.mBuffers[i] = nullptr;
		}
		if (nullptr != gDevice.mVoiceCallback.mBufferEndEvent)
		{
			CloseHandle(gDevice.mVoiceCallback.mBufferEndEvent);
			gDevice.mVoiceCallback.mBufferEndEvent = nullptr;
		}
		gDevice.mOpen = false;
	}

	// Debugging aid: if "game:\dump_audio.txt" exists, the first 20 seconds of audio output are also written to "game:\audio_dump.wav"
	struct AudioDump
	{
		FILE* mFile = nullptr;
		Uint32 mBytesWritten = 0;
		Uint32 mMaxBytes = 0;
		bool mChecked = false;

		static void writeLittleEndian(Uint8* destination, Uint32 value, int bytes)
		{
			for (int i = 0; i < bytes; ++i)
				destination[i] = (Uint8)(value >> (i * 8));
		}

		void writeHeader()
		{
			const Uint32 blockAlign = (Uint32)gDevice.mSpec.channels * 2;
			Uint8 header[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ' };
			writeLittleEndian(&header[4], 36 + mBytesWritten, 4);
			writeLittleEndian(&header[16], 16, 4);
			writeLittleEndian(&header[20], 1, 2);
			writeLittleEndian(&header[22], gDevice.mSpec.channels, 2);
			writeLittleEndian(&header[24], (Uint32)gDevice.mSpec.freq, 4);
			writeLittleEndian(&header[28], (Uint32)gDevice.mSpec.freq * blockAlign, 4);
			writeLittleEndian(&header[32], blockAlign, 2);
			writeLittleEndian(&header[34], 16, 2);
			memcpy(&header[36], "data", 4);
			writeLittleEndian(&header[40], mBytesWritten, 4);
			fseek(mFile, 0, SEEK_SET);
			fwrite(header, 1, sizeof(header), mFile);
			fseek(mFile, 0, SEEK_END);
		}

		void addSamples(const Uint8* data, Uint32 bytes)
		{
			if (!mChecked)
			{
				mChecked = true;
				FILE* trigger = fopen("game:\\dump_audio.txt", "rb");
				if (nullptr == trigger)
					return;
				fclose(trigger);
				mFile = fopen("game:\\audio_dump.wav", "wb");
				mMaxBytes = (Uint32)gDevice.mSpec.freq * gDevice.mSpec.channels * 2 * 20;
				if (nullptr != mFile)
					writeHeader();
			}
			if (nullptr == mFile)
				return;

			// WAV data is little endian, the output samples are big endian
			Uint8 swapped[8192];
			Uint32 offset = 0;
			while (offset < bytes && mBytesWritten < mMaxBytes)
			{
				const Uint32 chunk = (bytes - offset < sizeof(swapped)) ? (bytes - offset) : (Uint32)sizeof(swapped);
				for (Uint32 i = 0; i + 1 < chunk; i += 2)
				{
					swapped[i] = data[offset + i + 1];
					swapped[i + 1] = data[offset + i];
				}
				fwrite(swapped, 1, chunk, mFile);
				mBytesWritten += chunk;
				offset += chunk;
			}
			if (mBytesWritten >= mMaxBytes)
			{
				writeHeader();
				fclose(mFile);
				mFile = nullptr;
				DbgPrint("SDL2-lite: audio dump written\n");
			}
		}
	};
	AudioDump gAudioDump;

	DWORD WINAPI feederThreadFunction(LPVOID)
	{
		while (!gDevice.mShutdown)
		{
			if (!gDevice.mPaused)
			{
				XAUDIO2_VOICE_STATE state;
				gDevice.mSourceVoice->GetState(&state);
				for (UINT32 queued = state.BuffersQueued; queued < (UINT32)NUM_BUFFERS; ++queued)
				{
					Uint8* buffer = gDevice.mBuffers[gDevice.mNextBuffer];
					gDevice.mNextBuffer = (gDevice.mNextBuffer + 1) % NUM_BUFFERS;

					// Let the application fill the buffer
					EnterCriticalSection(&gDevice.mLock);
					gDevice.mSpec.callback(gDevice.mSpec.userdata, buffer, (int)gDevice.mBufferBytes);
					LeaveCriticalSection(&gDevice.mLock);
					gAudioDump.addSamples(buffer, gDevice.mBufferBytes);

					XAUDIO2_BUFFER xaudioBuffer;
					memset(&xaudioBuffer, 0, sizeof(xaudioBuffer));
					xaudioBuffer.AudioBytes = gDevice.mBufferBytes;
					xaudioBuffer.pAudioData = buffer;
					gDevice.mSourceVoice->SubmitSourceBuffer(&xaudioBuffer);
				}
			}

			// Wake up whenever a buffer finished playing (or regularly, e.g. while paused)
			WaitForSingleObject(gDevice.mVoiceCallback.mBufferEndEvent, 20);
		}
		return 0;
	}
}


extern "C"
{

int SDL_GetNumAudioDevices(int iscapture)
{
	return iscapture ? 0 : 1;
}

const char* SDL_GetAudioDeviceName(int index, int iscapture)
{
	if (iscapture || index != 0)
	{
		SDL_SetError("Invalid audio device index");
		return nullptr;
	}
	return "XAudio2";
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired, SDL_AudioSpec* obtained, int allowed_changes)
{
	if (iscapture)
	{
		SDL_SetError("Audio capture is not supported on Xbox 360");
		return 0;
	}
	if (nullptr == desired || nullptr == desired->callback)
	{
		SDL_SetError("SDL_OpenAudioDevice: a callback is required");
		return 0;
	}
	if (gDevice.mOpen)
	{
		SDL_SetError("SDL_OpenAudioDevice: the audio device is already open");
		return 0;
	}

	if (!gDevice.mLockInitialized)
	{
		InitializeCriticalSection(&gDevice.mLock);
		gDevice.mLockInitialized = true;
	}

	// Output format: 16-bit signed PCM in native (big endian) byte order
	gDevice.mSpec = *desired;
	gDevice.mSpec.format = AUDIO_S16MSB;
	gDevice.mSpec.channels = (desired->channels >= 2) ? 2 : 1;
	gDevice.mSpec.freq = (desired->freq > 0) ? desired->freq : 48000;
	gDevice.mSpec.samples = (desired->samples > 0) ? desired->samples : 1024;
	gDevice.mSpec.silence = 0;
	gDevice.mBufferBytes = (Uint32)gDevice.mSpec.samples * gDevice.mSpec.channels * 2;
	gDevice.mSpec.size = gDevice.mBufferBytes;

	if (FAILED(XAudio2Create(&gDevice.mXAudio2, 0, XboxThread5)))
	{
		SDL_SetError("XAudio2Create failed");
		releaseDevice();
		return 0;
	}
	if (FAILED(gDevice.mXAudio2->CreateMasteringVoice(&gDevice.mMasteringVoice)))
	{
		SDL_SetError("IXAudio2::CreateMasteringVoice failed");
		releaseDevice();
		return 0;
	}

	WAVEFORMATEX waveFormat;
	memset(&waveFormat, 0, sizeof(waveFormat));
	waveFormat.wFormatTag = WAVE_FORMAT_PCM;
	waveFormat.nChannels = gDevice.mSpec.channels;
	waveFormat.nSamplesPerSec = gDevice.mSpec.freq;
	waveFormat.wBitsPerSample = 16;
	waveFormat.nBlockAlign = (WORD)(waveFormat.nChannels * 2);
	waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;

	gDevice.mVoiceCallback.mBufferEndEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (FAILED(gDevice.mXAudio2->CreateSourceVoice(&gDevice.mSourceVoice, &waveFormat, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &gDevice.mVoiceCallback)))
	{
		SDL_SetError("IXAudio2::CreateSourceVoice failed");
		releaseDevice();
		return 0;
	}

	for (int i = 0; i < NUM_BUFFERS; ++i)
	{
		gDevice.mBuffers[i] = static_cast<Uint8*>(calloc(1, gDevice.mBufferBytes));
		if (nullptr == gDevice.mBuffers[i])
		{
			SDL_SetError("Out of memory for audio buffers");
			releaseDevice();
			return 0;
		}
	}

	// Like SDL, the device starts paused
	gDevice.mPaused = true;
	gDevice.mShutdown = false;
	gDevice.mNextBuffer = 0;
	gDevice.mOpen = true;

	gDevice.mFeederThread = CreateThread(nullptr, 256 * 1024, feederThreadFunction, nullptr, CREATE_SUSPENDED, nullptr);
	if (nullptr == gDevice.mFeederThread)
	{
		SDL_SetError("Creating the audio thread failed");
		releaseDevice();
		return 0;
	}
	XSetThreadProcessor(gDevice.mFeederThread, FEEDER_PROCESSOR);
	SetThreadPriority(gDevice.mFeederThread, THREAD_PRIORITY_HIGHEST);
	ResumeThread(gDevice.mFeederThread);

	if (nullptr != obtained)
		*obtained = gDevice.mSpec;

	DbgPrint("SDL2-lite: XAudio2 output opened (%d Hz, %d channels, %d samples per buffer)\n", gDevice.mSpec.freq, gDevice.mSpec.channels, gDevice.mSpec.samples);
	return DEVICE_ID;
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev)
{
	if (dev != DEVICE_ID || !gDevice.mOpen)
		return;

	gDevice.mShutdown = true;
	SetEvent(gDevice.mVoiceCallback.mBufferEndEvent);
	if (nullptr != gDevice.mFeederThread)
	{
		WaitForSingleObject(gDevice.mFeederThread, INFINITE);
		CloseHandle(gDevice.mFeederThread);
		gDevice.mFeederThread = nullptr;
	}
	releaseDevice();
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on)
{
	if (dev != DEVICE_ID || !gDevice.mOpen)
		return;

	const bool pause = (pause_on != 0);
	if (pause == gDevice.mPaused)
		return;

	gDevice.mPaused = pause;
	if (pause)
	{
		gDevice.mSourceVoice->Stop(0);
	}
	else
	{
		gDevice.mSourceVoice->Start(0);
		SetEvent(gDevice.mVoiceCallback.mBufferEndEvent);		// Fill the buffers right away
	}
}

SDL_AudioStatus SDL_GetAudioStatus(void)
{
	if (!gDevice.mOpen)
		return SDL_AUDIO_STOPPED;
	return gDevice.mPaused ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING;
}

void SDL_LockAudioDevice(SDL_AudioDeviceID dev)
{
	if (dev == DEVICE_ID && gDevice.mLockInitialized)
		EnterCriticalSection(&gDevice.mLock);
}

void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev)
{
	if (dev == DEVICE_ID && gDevice.mLockInitialized)
		LeaveCriticalSection(&gDevice.mLock);
}

SDL_AudioSpec* SDL_LoadWAV_RW(SDL_RWops* src, int freesrc, SDL_AudioSpec* spec, Uint8** audio_buf, Uint32* audio_len)
{
	if (freesrc && nullptr != src)
		SDL_RWclose(src);
	SDL_SetError("SDL_LoadWAV is not implemented yet on Xbox 360 (SDL2-lite)");
	return nullptr;
}

void SDL_FreeWAV(Uint8* audio_buf)
{
}

int SDL_BuildAudioCVT(SDL_AudioCVT* cvt, SDL_AudioFormat src_format, Uint8 src_channels, int src_rate, SDL_AudioFormat dst_format, Uint8 dst_channels, int dst_rate)
{
	return SDL_SetError("SDL_BuildAudioCVT is not implemented yet on Xbox 360 (SDL2-lite)");
}

int SDL_ConvertAudio(SDL_AudioCVT* cvt)
{
	return SDL_SetError("SDL_ConvertAudio is not implemented yet on Xbox 360 (SDL2-lite)");
}

}
