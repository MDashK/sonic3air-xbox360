/*
*	Xbox 360 port of Sonic 3 A.I.R.
*	SDL2-lite: window, display, window surface (presented through D3D9), OpenGL, message boxes.
*
*	The Xbox 360 has no windows and no OpenGL. The "window" remembers its size; its window surface is
*	a system memory ABGR8888 buffer that Oxygen's software renderer draws into, and
*	SDL_UpdateWindowSurface copies it into a linear A8B8G8R8 texture and draws it as a quad onto the
*	1280x720 back buffer, keeping the frame's aspect ratio. A uint32 0xAABBGGRR is exactly the 360's
*	native D3DFMT_LIN_A8B8G8R8 layout, so no pixel conversion is needed.
*	OpenGL functions fail, so the engine uses its software renderer.
*/

#include "SDL_x360_internal.h"

#include <iostream>
#include <new>
#include <stdlib.h>
#include <string.h>

#include "../shaders/present_vs.h"
#include "../shaders/present_ps.h"


struct SDL_Window
{
	int mWidth = 0;
	int mHeight = 0;
	Uint32 mFlags = 0;
	SDL_Surface* mSurface = nullptr;
};


namespace
{
	const int BACK_BUFFER_WIDTH = 1280;
	const int BACK_BUFFER_HEIGHT = 720;

	SDL_PixelFormat gSurfacePixelFormat;

	void initSurfacePixelFormat()
	{
		static bool initialized = false;
		if (initialized)
			return;
		memset(&gSurfacePixelFormat, 0, sizeof(gSurfacePixelFormat));
		gSurfacePixelFormat.format = SDL_PIXELFORMAT_ABGR8888;
		gSurfacePixelFormat.BitsPerPixel = 32;
		gSurfacePixelFormat.BytesPerPixel = 4;
		gSurfacePixelFormat.Rmask = 0x000000ff;
		gSurfacePixelFormat.Gmask = 0x0000ff00;
		gSurfacePixelFormat.Bmask = 0x00ff0000;
		gSurfacePixelFormat.Amask = 0xff000000;
		gSurfacePixelFormat.Rshift = 0;
		gSurfacePixelFormat.Gshift = 8;
		gSurfacePixelFormat.Bshift = 16;
		gSurfacePixelFormat.Ashift = 24;
		gSurfacePixelFormat.refcount = 1;
		initialized = true;
	}

	SDL_Surface* createWindowSurface(int width, int height)
	{
		initSurfacePixelFormat();
		SDL_Surface* surface = static_cast<SDL_Surface*>(calloc(1, sizeof(SDL_Surface)));
		if (nullptr == surface)
			return nullptr;
		surface->format = &gSurfacePixelFormat;
		surface->w = width;
		surface->h = height;
		surface->pitch = width * 4;
		surface->pixels = calloc((size_t)width * (size_t)height, 4);
		surface->refcount = 1;
		surface->clip_rect.w = width;
		surface->clip_rect.h = height;
		if (nullptr == surface->pixels)
		{
			free(surface);
			return nullptr;
		}
		return surface;
	}

	void destroyWindowSurface(SDL_Surface*& surface)
	{
		if (nullptr != surface)
		{
			free(surface->pixels);
			free(surface);
			surface = nullptr;
		}
	}


	// Debugging aid: if "game:\dump_frames.txt" exists, every 300th presented frame is saved as "game:\frame_NNNNN.bmp"
	//  -> Works in Xenia and on the console, where there is no other way to get a screenshot of the window surface
	bool shouldDumpFrames()
	{
		static int state = -1;
		if (state < 0)
		{
			FILE* file = fopen("game:\\dump_frames.txt", "rb");
			state = (nullptr != file) ? 1 : 0;
			if (nullptr != file)
				fclose(file);
		}
		return (state == 1);
	}

	void writeLittleEndian(uint8_t* destination, uint32_t value, int bytes)
	{
		for (int i = 0; i < bytes; ++i)
			destination[i] = (uint8_t)(value >> (i * 8));
	}

	void dumpFrame(const SDL_Surface& surface, unsigned frameNumber)
	{
		char filename[64];
		snprintf(filename, sizeof(filename), "game:\\frame_%05u.bmp", frameNumber);
		FILE* file = fopen(filename, "wb");
		if (nullptr == file)
			return;

		// 32-bit BMP, bottom-up rows, pixel bytes in B, G, R, A order
		const uint32_t imageSize = (uint32_t)surface.w * (uint32_t)surface.h * 4;
		uint8_t header[54] = { 'B', 'M' };
		writeLittleEndian(&header[2], 54 + imageSize, 4);	// File size
		writeLittleEndian(&header[10], 54, 4);				// Offset of the pixel data
		writeLittleEndian(&header[14], 40, 4);				// Size of the info header
		writeLittleEndian(&header[18], (uint32_t)surface.w, 4);
		writeLittleEndian(&header[22], (uint32_t)surface.h, 4);
		writeLittleEndian(&header[26], 1, 2);				// Planes
		writeLittleEndian(&header[28], 32, 2);				// Bits per pixel
		writeLittleEndian(&header[34], imageSize, 4);
		fwrite(header, 1, sizeof(header), file);

		uint8_t* row = static_cast<uint8_t*>(malloc((size_t)surface.w * 4));
		if (nullptr != row)
		{
			for (int y = surface.h - 1; y >= 0; --y)
			{
				const uint32_t* source = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(surface.pixels) + (size_t)y * surface.pitch);
				for (int x = 0; x < surface.w; ++x)
				{
					const uint32_t color = source[x];	// 0xAABBGGRR
					row[x * 4 + 0] = (uint8_t)(color >> 16);
					row[x * 4 + 1] = (uint8_t)(color >> 8);
					row[x * 4 + 2] = (uint8_t)(color);
					row[x * 4 + 3] = (uint8_t)(color >> 24);
				}
				fwrite(row, 1, (size_t)surface.w * 4, file);
			}
			free(row);
		}
		fclose(file);
		DbgPrint("SDL2-lite: saved %s\n", filename);
	}


	// D3D9 presentation of the window surface
	class Presenter
	{
	public:
		bool present(const SDL_Surface& surface)
		{
			if (!mInitialized && !initialize())
				return false;

			LARGE_INTEGER timeStart;
			QueryPerformanceCounter(&timeStart);

			IDirect3DTexture9* texture = getTexture(surface.w, surface.h);
			if (nullptr == texture)
				return false;

			// The CPU and GPU are not synchronized automatically on the 360: wait until the GPU is done
			// with this texture (used two frames ago, as there are two of them) before overwriting it
			texture->BlockUntilNotBusy();

			D3DLOCKED_RECT lockedRect;
			if (FAILED(texture->LockRect(0, &lockedRect, nullptr, 0)))
				return false;
			const uint8_t* source = static_cast<const uint8_t*>(surface.pixels);
			uint8_t* destination = static_cast<uint8_t*>(lockedRect.pBits);
			const size_t rowBytes = (size_t)surface.w * 4;
			for (int y = 0; y < surface.h; ++y)
			{
				memcpy(destination, source, rowBytes);
				source += surface.pitch;
				destination += lockedRect.Pitch;
			}
			texture->UnlockRect(0);

			// Fit the frame into the 16:9 back buffer, keeping its aspect ratio
			float halfWidth = 1.0f;
			float halfHeight = 1.0f;
			const float frameAspect = (float)surface.w / (float)surface.h;
			const float backBufferAspect = (float)BACK_BUFFER_WIDTH / (float)BACK_BUFFER_HEIGHT;
			if (frameAspect > backBufferAspect)
				halfHeight = backBufferAspect / frameAspect;
			else
				halfWidth = frameAspect / backBufferAspect;

			struct Vertex { float x, y, u, v; };
			const Vertex quad[4] =
			{
				{ -halfWidth,  halfHeight, 0.0f, 0.0f },
				{  halfWidth,  halfHeight, 1.0f, 0.0f },
				{  halfWidth, -halfHeight, 1.0f, 1.0f },
				{ -halfWidth, -halfHeight, 0.0f, 1.0f },
			};

			mDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
			mDevice->SetVertexDeclaration(mVertexDeclaration);
			mDevice->SetVertexShader(mVertexShader);
			mDevice->SetPixelShader(mPixelShader);
			const float textureSizeAndScale[4] = { (float)surface.w, (float)surface.h, halfWidth * BACK_BUFFER_WIDTH / (float)surface.w, halfHeight * BACK_BUFFER_HEIGHT / (float)surface.h };
			mDevice->SetPixelShaderConstantF(0, textureSizeAndScale, 1);
			mDevice->SetTexture(0, texture);
			mDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
			mDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
			mDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			mDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			mDevice->DrawPrimitiveUP(D3DPT_QUADLIST, 1, quad, sizeof(Vertex));
			mDevice->SetTexture(0, nullptr);
			mDevice->Present(nullptr, nullptr, nullptr, nullptr);

			LARGE_INTEGER timeEnd;
			LARGE_INTEGER frequency;
			QueryPerformanceCounter(&timeEnd);
			QueryPerformanceFrequency(&frequency);
			mPresentSeconds += (double)(timeEnd.QuadPart - timeStart.QuadPart) / (double)frequency.QuadPart;

			// Occasional status in the debug output (xenia.log / devkit) and the log file, to tell "nothing presented"
			// from "black frames", and with the average time spent here (texture copy, GPU wait and present)
			++mFramesPresented;
			if (mFramesPresented == 1 || (mFramesPresented % 600) == 0)
			{
				const uint32_t centerPixel = static_cast<const uint32_t*>(surface.pixels)[(surface.h / 2) * (surface.pitch / 4) + surface.w / 2];
				const double framesMeasured = (mFramesPresented == 1) ? 1.0 : 600.0;
				char text[192];
				snprintf(text, sizeof(text), "SDL2-lite: presented frame %u (%dx%d), center pixel 0x%08X, avg %.2f ms per present", (unsigned)mFramesPresented, surface.w, surface.h, (unsigned)centerPixel, mPresentSeconds * 1000.0 / framesMeasured);
				DbgPrint("%s\n", text);
				std::cout << text << std::endl;		// Goes into game:\s3air.log (see x360/port/x360_platform.cpp)
				mPresentSeconds = 0.0;
			}
			if ((mFramesPresented % 300) == 0 && mFramesPresented <= 300 * 40 && shouldDumpFrames())		// At most 40 dumps (each one is width * height * 4 bytes)
			{
				dumpFrame(surface, (unsigned)mFramesPresented);
			}

			mCurrentTexture = 1 - mCurrentTexture;
			return true;
		}

		// The device is shared with the port's D3D9 drawer (x360/port/D3D9Drawer.cpp), which then does its own presentation
		IDirect3DDevice9* getDevice()
		{
			if (!mInitialized)
				initialize();
			return mReady ? mDevice : nullptr;
		}

	private:
		bool initialize()
		{
			mInitialized = true;	// Only try once

			mD3D = Direct3DCreate9(D3D_SDK_VERSION);
			if (nullptr == mD3D)
			{
				sdl2lite::setError("Direct3DCreate9 failed");
				return false;
			}

			D3DPRESENT_PARAMETERS params;
			ZeroMemory(&params, sizeof(params));
			params.BackBufferWidth        = BACK_BUFFER_WIDTH;
			params.BackBufferHeight       = BACK_BUFFER_HEIGHT;
			params.BackBufferFormat       = D3DFMT_A8R8G8B8;
			params.BackBufferCount        = 1;
			params.EnableAutoDepthStencil = FALSE;
			params.SwapEffect             = D3DSWAPEFFECT_DISCARD;
			params.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;
			if (FAILED(mD3D->CreateDevice(0, D3DDEVTYPE_HAL, nullptr, D3DCREATE_HARDWARE_VERTEXPROCESSING, &params, &mDevice)))
			{
				sdl2lite::setError("IDirect3D9::CreateDevice failed");
				return false;
			}

			const D3DVERTEXELEMENT9 vertexElements[] =
			{
				{ 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
				{ 0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
				D3DDECL_END()
			};
			if (FAILED(mDevice->CreateVertexDeclaration(vertexElements, &mVertexDeclaration)) ||
				FAILED(mDevice->CreateVertexShader(g_presentVertexShader, &mVertexShader)) ||
				FAILED(mDevice->CreatePixelShader(g_presentPixelShader, &mPixelShader)))
			{
				sdl2lite::setError("Creating the presentation shaders failed");
				return false;
			}

			mReady = true;
			DbgPrint("SDL2-lite: D3D9 presenter ready (back buffer %dx%d)\n", BACK_BUFFER_WIDTH, BACK_BUFFER_HEIGHT);
			return true;
		}

		IDirect3DTexture9* getTexture(int width, int height)
		{
			if (!mReady)
				return nullptr;
			if (width != mTextureWidth || height != mTextureHeight)
			{
				for (int i = 0; i < 2; ++i)
				{
					if (nullptr != mTextures[i])
					{
						mTextures[i]->BlockUntilNotBusy();
						mTextures[i]->Release();
						mTextures[i] = nullptr;
					}
				}
				for (int i = 0; i < 2; ++i)
				{
					if (FAILED(mDevice->CreateTexture(width, height, 1, 0, D3DFMT_LIN_A8B8G8R8, 0, &mTextures[i], nullptr)))
					{
						sdl2lite::setError("CreateTexture %dx%d failed", width, height);
						return nullptr;
					}
				}
				mTextureWidth = width;
				mTextureHeight = height;
			}
			return mTextures[mCurrentTexture];
		}

	private:
		bool mInitialized = false;
		bool mReady = false;
		IDirect3D9* mD3D = nullptr;
		IDirect3DDevice9* mDevice = nullptr;
		IDirect3DVertexDeclaration9* mVertexDeclaration = nullptr;
		IDirect3DVertexShader9* mVertexShader = nullptr;
		IDirect3DPixelShader9* mPixelShader = nullptr;
		IDirect3DTexture9* mTextures[2] = { nullptr, nullptr };
		int mCurrentTexture = 0;
		uint32_t mFramesPresented = 0;
		double mPresentSeconds = 0.0;
		int mTextureWidth = 0;
		int mTextureHeight = 0;
	};

	Presenter gPresenter;
}


extern "C"
{

/* ----- Window ----- */

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags)
{
	SDL_Window* window = new (std::nothrow) SDL_Window();
	if (nullptr == window)
	{
		SDL_OutOfMemory();
		return nullptr;
	}
	window->mWidth = w;
	window->mHeight = h;
	window->mFlags = flags & ~SDL_WINDOW_OPENGL;
	return window;
}

void SDL_DestroyWindow(SDL_Window* window)
{
	if (nullptr != window)
	{
		destroyWindowSurface(window->mSurface);
		delete window;
	}
}

void SDL_GetWindowSize(SDL_Window* window, int* w, int* h)
{
	if (nullptr != w)
		*w = (nullptr != window) ? window->mWidth : 0;
	if (nullptr != h)
		*h = (nullptr != window) ? window->mHeight : 0;
}

void SDL_SetWindowSize(SDL_Window* window, int w, int h)
{
	if (nullptr != window && (w != window->mWidth || h != window->mHeight))
	{
		window->mWidth = w;
		window->mHeight = h;
		destroyWindowSurface(window->mSurface);		// Recreated with the new size on the next SDL_GetWindowSurface
	}
}

void SDL_SetWindowPosition(SDL_Window* window, int x, int y)  {}
void SDL_SetWindowBordered(SDL_Window* window, SDL_bool bordered)  {}
void SDL_SetWindowResizable(SDL_Window* window, SDL_bool resizable)  {}
void SDL_SetWindowIcon(SDL_Window* window, SDL_Surface* icon)  {}

int SDL_SetWindowFullscreen(SDL_Window* window, Uint32 flags)
{
	return 0;	// Always "fullscreen"
}

int SDL_GetWindowDisplayIndex(SDL_Window* window)
{
	return 0;
}

Uint32 SDL_GetWindowID(SDL_Window* window)
{
	return 1;
}

SDL_Surface* SDL_GetWindowSurface(SDL_Window* window)
{
	if (nullptr == window)
	{
		SDL_InvalidParamError("window");
		return nullptr;
	}
	if (nullptr == window->mSurface)
	{
		window->mSurface = createWindowSurface(window->mWidth, window->mHeight);
		if (nullptr == window->mSurface)
			SDL_OutOfMemory();
	}
	return window->mSurface;
}

int SDL_UpdateWindowSurface(SDL_Window* window)
{
	if (nullptr == window || nullptr == window->mSurface)
		return SDL_InvalidParamError("window");
	return gPresenter.present(*window->mSurface) ? 0 : -1;
}

extern "C" void* SDL_X360_GetD3DDevice(void)
{
	return gPresenter.getDevice();
}


/* ----- Display ----- */

int SDL_GetDesktopDisplayMode(int displayIndex, SDL_DisplayMode* mode)
{
	if (nullptr == mode)
		return SDL_InvalidParamError("mode");
	mode->format = SDL_PIXELFORMAT_ARGB8888;
	mode->w = BACK_BUFFER_WIDTH;
	mode->h = BACK_BUFFER_HEIGHT;
	mode->refresh_rate = 60;
	mode->driverdata = nullptr;
	return 0;
}

int SDL_GetDisplayBounds(int displayIndex, SDL_Rect* rect)
{
	if (nullptr == rect)
		return SDL_InvalidParamError("rect");
	rect->x = 0;
	rect->y = 0;
	rect->w = BACK_BUFFER_WIDTH;
	rect->h = BACK_BUFFER_HEIGHT;
	return 0;
}

void SDL_DisableScreenSaver(void)
{
	XEnableScreenSaver(FALSE);
}


/* ----- Mouse cursor ----- */

int SDL_ShowCursor(int toggle)
{
	return SDL_DISABLE;
}

void SDL_WarpMouseInWindow(SDL_Window* window, int x, int y)  {}


/* ----- Surfaces ----- */

SDL_Surface* SDL_CreateRGBSurfaceFrom(void* pixels, int width, int height, int depth, int pitch, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
	SDL_Surface* surface = static_cast<SDL_Surface*>(calloc(1, sizeof(SDL_Surface)));
	if (nullptr == surface)
	{
		SDL_OutOfMemory();
		return nullptr;
	}
	surface->flags = SDL_PREALLOC;
	surface->w = width;
	surface->h = height;
	surface->pitch = pitch;
	surface->pixels = pixels;
	surface->refcount = 1;
	return surface;
}

void SDL_FreeSurface(SDL_Surface* surface)
{
	free(surface);	// Only surfaces from SDL_CreateRGBSurfaceFrom (window surfaces are owned by their window)
}

int SDL_LockSurface(SDL_Surface* surface)
{
	return 0;	// System memory: always accessible
}

void SDL_UnlockSurface(SDL_Surface* surface)  {}


/* ----- OpenGL: not available, so the engine falls back to software rendering ----- */

int SDL_GL_SetAttribute(SDL_GLattr attr, int value)
{
	return SDL_SetError("OpenGL is not available on Xbox 360");
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window* window)
{
	SDL_SetError("OpenGL is not available on Xbox 360");
	return nullptr;
}

int SDL_GL_SetSwapInterval(int interval)
{
	return SDL_SetError("OpenGL is not available on Xbox 360");
}


/* ----- Misc ----- */

SDL_bool SDL_SetHint(const char* name, const char* value)
{
	return SDL_TRUE;
}

int SDL_OpenURL(const char* url)
{
	return SDL_Unsupported();
}

int SDL_ShowMessageBox(const SDL_MessageBoxData* messageboxdata, int* buttonid)
{
	// No dialogs: log the message and answer with the default button
	if (nullptr == messageboxdata)
		return SDL_InvalidParamError("messageboxdata");
	DbgPrint("SDL message box [%s]: %s\n", messageboxdata->title ? messageboxdata->title : "", messageboxdata->message ? messageboxdata->message : "");

	// Nobody can answer on the console: prefer the "escape" choice (cancel / ignore / continue), as the
	// "return" choice is often "retry", which would loop forever (e.g. "Retry script compilation?")
	int chosen = -1;
	for (int pass = 0; pass < 2 && chosen < 0; ++pass)
	{
		const Uint32 wantedFlag = (pass == 0) ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
		for (int i = 0; i < messageboxdata->numbuttons; ++i)
		{
			if (messageboxdata->buttons[i].flags & wantedFlag)
			{
				chosen = messageboxdata->buttons[i].buttonid;
				break;
			}
		}
	}
	if (chosen < 0 && messageboxdata->numbuttons > 0)
		chosen = messageboxdata->buttons[0].buttonid;
	if (nullptr != buttonid)
		*buttonid = chosen;
	return 0;
}

}
