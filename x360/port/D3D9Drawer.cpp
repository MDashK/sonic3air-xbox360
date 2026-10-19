/*
*	Xbox 360 port of Sonic 3 A.I.R.
*
*	D3D9 drawer: an Oxygen DrawerInterface implementation on the Xbox 360 GPU, in place of the software drawer.
*	The game screen itself still comes from Oxygen's software renderer (uploaded as a texture every frame),
*	but everything that gets drawn with it - the upscaled game screen, menus, sprites, text, overlays - is
*	drawn by the GPU at the full 1280x720 window resolution. Modelled after Oxygen's OpenGLDrawer.
*
*	 - Uses the D3D9 device that SDL2-lite creates (SDL_X360_GetD3DDevice), and presents by itself
*	 - Textures from the engine use D3DFMT_LIN_A8B8G8R8, which is exactly the engine's 0xAABBGGRR color layout
*	 - Palette sprites are 8-bit textures (D3DFMT_LIN_L8), with the palette looked up in the pixel shader
*	 - Text uses one glyph atlas per font
*	 - Render targets are EDRAM surfaces placed after the back buffer, resolved into a texture when done
*	 - The CPU and GPU are not synchronized automatically on the 360: textures that get rewritten are
*	   double-buffered, and any texture is only modified or released after BlockUntilNotBusy
*/

#include "oxygen/pch.h"
#include "oxygen/application/Configuration.h"
#include "oxygen/drawing/Drawer.h"
#include "oxygen/drawing/DrawerInterface.h"
#include "oxygen/drawing/DrawerTexture.h"
#include "oxygen/drawing/DrawCollection.h"
#include "oxygen/drawing/DrawCommand.h"
#include "oxygen/rendering/parts/palette/Palette.h"
#include "oxygen/rendering/sprite/ComponentSprite.h"
#include "oxygen/rendering/sprite/PaletteSprite.h"
#include "oxygen/resources/PaletteCollection.h"
#include "oxygen/resources/SpriteCollection.h"

#include "SDL_x360.h"

// After all the engine headers: <xtl.h> defines Windows macros (min, max, ERROR, IGNORE) that break them
#include <xtl.h>
#undef min
#undef max

#include "shaders/drawer_vs.h"
#include "shaders/drawer_color_ps.h"
#include "shaders/drawer_textured_ps.h"
#include "shaders/drawer_indexed_ps.h"
#include "shaders/drawer_upscale_ps.h"

#include <iostream>
#include <unordered_map>

extern "C" ULONG DbgPrint(const char* format, ...);


namespace d3d9drawer
{
	IDirect3DDevice9* gDevice = nullptr;

	// EDRAM layout: the device's 1280x720 back buffer starts at tile 0, render targets get placed behind it,
	// so switching between them never destroys the content of the other ones
	const DWORD EDRAM_TILES_TOTAL = GPU_EDRAM_SIZE / (GPU_EDRAM_TILE_WIDTH_1X * GPU_EDRAM_TILE_HEIGHT_1X * 4);
	const DWORD EDRAM_TILES_BACK_BUFFER = (1280 / GPU_EDRAM_TILE_WIDTH_1X) * (720 / GPU_EDRAM_TILE_HEIGHT_1X);
	DWORD gNextEdramTile = EDRAM_TILES_BACK_BUFFER;

	DWORD allocateEdramTiles(int width, int height)
	{
		const DWORD tiles = (DWORD)((width + GPU_EDRAM_TILE_WIDTH_1X - 1) / GPU_EDRAM_TILE_WIDTH_1X) * (DWORD)((height + GPU_EDRAM_TILE_HEIGHT_1X - 1) / GPU_EDRAM_TILE_HEIGHT_1X);
		if (gNextEdramTile + tiles > EDRAM_TILES_TOTAL)
		{
			// Out of EDRAM (render targets got recreated often): start over behind the back buffer
			DbgPrint("D3D9Drawer: EDRAM exhausted, reusing render target space\n");
			gNextEdramTile = EDRAM_TILES_BACK_BUFFER;
		}
		const DWORD base = gNextEdramTile;
		gNextEdramTile += tiles;
		return base;
	}

	void releaseTexture(IDirect3DTexture9*& texture)
	{
		if (nullptr != texture)
		{
			texture->BlockUntilNotBusy();
			texture->Release();
			texture = nullptr;
		}
	}


	// Texture with CPU-written content, optionally double-buffered for content that changes often
	class UploadTexture
	{
	public:
		explicit UploadTexture(int numBuffers = 1) : mNumBuffers(numBuffers) {}
		~UploadTexture()  { release(); }

		void release()
		{
			for (int i = 0; i < 2; ++i)
				releaseTexture(mTextures[i]);
			mWidth = 0;
			mHeight = 0;
		}

		inline IDirect3DTexture9* get() const  { return mTextures[mCurrent]; }
		inline bool isValid() const  { return (nullptr != mTextures[mCurrent]); }

		void upload(const void* data, int width, int height, int bytesPerPixel, int sourcePitch, D3DFORMAT format)
		{
			if (width <= 0 || height <= 0 || nullptr == data)
				return;
			if (width != mWidth || height != mHeight || format != mFormat)
			{
				release();
				for (int i = 0; i < mNumBuffers; ++i)
				{
					if (FAILED(gDevice->CreateTexture(width, height, 1, 0, format, 0, &mTextures[i], nullptr)))
					{
						DbgPrint("D3D9Drawer: CreateTexture %dx%d failed\n", width, height);
						release();
						return;
					}
				}
				mWidth = width;
				mHeight = height;
				mFormat = format;
			}

			mCurrent = (mCurrent + 1) % mNumBuffers;
			IDirect3DTexture9* texture = mTextures[mCurrent];
			texture->BlockUntilNotBusy();

			D3DLOCKED_RECT lockedRect;
			if (FAILED(texture->LockRect(0, &lockedRect, nullptr, 0)))
				return;
			const uint8* source = static_cast<const uint8*>(data);
			uint8* destination = static_cast<uint8*>(lockedRect.pBits);
			const size_t rowBytes = (size_t)width * bytesPerPixel;
			for (int y = 0; y < height; ++y)
			{
				memcpy(destination, source, rowBytes);
				source += sourcePitch;
				destination += lockedRect.Pitch;
			}
			texture->UnlockRect(0);
		}

	private:
		const int mNumBuffers;
		IDirect3DTexture9* mTextures[2] = { nullptr, nullptr };
		int mCurrent = 0;
		int mWidth = 0;
		int mHeight = 0;
		D3DFORMAT mFormat = D3DFMT_UNKNOWN;
	};


	class D3D9DrawerTexture final : public DrawerTextureImplementation
	{
	public:
		explicit D3D9DrawerTexture(DrawerTexture& owner) : DrawerTextureImplementation(owner), mUploaded(2) {}

		~D3D9DrawerTexture()
		{
			releaseRenderTarget();
		}

		void updateFromBitmap(const Bitmap& bitmap) override
		{
			if (bitmap.empty())
				return;
			mUploaded.upload(bitmap.getData(), bitmap.getWidth(), bitmap.getHeight(), 4, bitmap.getWidth() * 4, D3DFMT_LIN_A8B8G8R8);
			mUseRenderTarget = false;
		}

		void setupAsRenderTarget(const Vec2i& size) override
		{
			mUseRenderTarget = true;
			if (nullptr != mRenderTargetSurface && size == mRenderTargetSize)
				return;

			releaseRenderTarget();
			if (size.x <= 0 || size.y <= 0)
				return;

			D3DSURFACE_PARAMETERS parameters;
			memset(&parameters, 0, sizeof(parameters));
			parameters.Base = allocateEdramTiles(size.x, size.y);
			if (FAILED(gDevice->CreateRenderTarget(size.x, size.y, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &mRenderTargetSurface, &parameters)) ||
				FAILED(gDevice->CreateTexture(size.x, size.y, 1, 0, D3DFMT_A8R8G8B8, 0, &mRenderTargetTexture, nullptr)))
			{
				DbgPrint("D3D9Drawer: creating render target %dx%d failed\n", size.x, size.y);
				releaseRenderTarget();
				return;
			}
			mRenderTargetSize = size;
		}

		void writeContentToBitmap(Bitmap& outBitmap) override
		{
			// Reading back a resolved (tiled) render target is not supported; the uploaded content is available on the CPU anyways
			if (!mUseRenderTarget && !mOwner.getBitmap().empty())
				outBitmap = mOwner.getBitmap();
			else
				outBitmap.create(std::max(mOwner.getSize().x, 1), std::max(mOwner.getSize().y, 1), 0xff000000);
		}

		void refreshImplementation(bool setupRenderTarget, const Vec2i& size) override
		{
			if (setupRenderTarget)
			{
				setupAsRenderTarget(size);
			}
			else if (!mOwner.accessBitmap().empty())
			{
				updateFromBitmap(mOwner.accessBitmap());
			}
		}

		IDirect3DTexture9* getTexture() const
		{
			return mUseRenderTarget ? mRenderTargetTexture : mUploaded.get();
		}

		inline IDirect3DSurface9* getRenderTargetSurface() const  { return mRenderTargetSurface; }
		inline IDirect3DTexture9* getRenderTargetTexture() const  { return mRenderTargetTexture; }

	private:
		void releaseRenderTarget()
		{
			if (nullptr != mRenderTargetSurface)
			{
				mRenderTargetSurface->Release();
				mRenderTargetSurface = nullptr;
			}
			releaseTexture(mRenderTargetTexture);
			mRenderTargetSize.set(0, 0);
		}

	private:
		UploadTexture mUploaded;
		IDirect3DSurface9* mRenderTargetSurface = nullptr;
		IDirect3DTexture9* mRenderTargetTexture = nullptr;
		Vec2i mRenderTargetSize;
		bool mUseRenderTarget = false;
	};


	// Glyph atlas for one font, filled on demand with the glyph bitmaps (including effects like outlines and shadows)
	class FontAtlas
	{
	public:
		struct Glyph
		{
			int mPage = -1;
			Vec2f mUV0;
			Vec2f mUV1;
			Vec2i mSize;
			int mBorderLeft = 0;
			int mBorderTop = 0;
		};

		static const int PAGE_SIZE = 512;

	public:
		explicit FontAtlas(Font& font) : mFont(font), mLastChangeCounter(font.getChangeCounter()) {}
		~FontAtlas()  { clear(); }

		IDirect3DTexture9* getPageTexture(int page) const  { return mPages[page].mTexture; }

		const Glyph* getGlyph(const Font::TypeInfo& typeInfo)
		{
			if (mLastChangeCounter != mFont.getChangeCounter())
			{
				clear();
				mLastChangeCounter = mFont.getChangeCounter();
			}

			const auto it = mGlyphs.find(typeInfo.mUnicode);
			if (it != mGlyphs.end())
				return (it->second.mPage >= 0) ? &it->second : nullptr;

			Glyph& glyph = mGlyphs[typeInfo.mUnicode];
			const Font::CharacterInfo& characterInfo = mFont.applyEffects(typeInfo);
			const Bitmap& bitmap = characterInfo.mCachedBitmap;
			if (bitmap.empty() || bitmap.getWidth() > PAGE_SIZE - 2 || bitmap.getHeight() > PAGE_SIZE - 2)
				return nullptr;

			// Simple shelf packing, with a 1 pixel gap between glyphs
			if (mPages.empty() || mShelfX + bitmap.getWidth() + 1 > PAGE_SIZE)
			{
				mShelfX = 0;
				mShelfY += mShelfHeight + 1;
				mShelfHeight = 0;
			}
			if (mPages.empty() || mShelfY + bitmap.getHeight() + 1 > PAGE_SIZE)
			{
				Page& page = vectorAdd(mPages);
				if (FAILED(gDevice->CreateTexture(PAGE_SIZE, PAGE_SIZE, 1, 0, D3DFMT_LIN_A8B8G8R8, 0, &page.mTexture, nullptr)))
				{
					mPages.pop_back();
					return nullptr;
				}
				D3DLOCKED_RECT lockedRect;
				if (SUCCEEDED(page.mTexture->LockRect(0, &lockedRect, nullptr, 0)))
				{
					memset(lockedRect.pBits, 0, (size_t)lockedRect.Pitch * PAGE_SIZE);
					page.mTexture->UnlockRect(0);
				}
				mShelfX = 0;
				mShelfY = 0;
				mShelfHeight = 0;
			}

			Page& page = mPages.back();
			page.mTexture->BlockUntilNotBusy();
			RECT rect = { mShelfX, mShelfY, mShelfX + bitmap.getWidth(), mShelfY + bitmap.getHeight() };
			D3DLOCKED_RECT lockedRect;
			if (FAILED(page.mTexture->LockRect(0, &lockedRect, &rect, 0)))
				return nullptr;
			for (int y = 0; y < bitmap.getHeight(); ++y)
			{
				memcpy(static_cast<uint8*>(lockedRect.pBits) + (size_t)y * lockedRect.Pitch, bitmap.getPixelPointer(0, y), (size_t)bitmap.getWidth() * 4);
			}
			page.mTexture->UnlockRect(0);

			glyph.mPage = (int)mPages.size() - 1;
			glyph.mUV0.set((float)mShelfX / (float)PAGE_SIZE, (float)mShelfY / (float)PAGE_SIZE);
			glyph.mUV1.set((float)(mShelfX + bitmap.getWidth()) / (float)PAGE_SIZE, (float)(mShelfY + bitmap.getHeight()) / (float)PAGE_SIZE);
			glyph.mSize = bitmap.getSize();
			glyph.mBorderLeft = characterInfo.mBorderLeft;
			glyph.mBorderTop = characterInfo.mBorderTop;

			mShelfX += bitmap.getWidth() + 1;
			mShelfHeight = std::max(mShelfHeight, bitmap.getHeight());
			return &glyph;
		}

	private:
		struct Page
		{
			IDirect3DTexture9* mTexture = nullptr;
		};

		void clear()
		{
			for (Page& page : mPages)
				releaseTexture(page.mTexture);
			mPages.clear();
			mGlyphs.clear();
			mShelfX = 0;
			mShelfY = 0;
			mShelfHeight = 0;
		}

	private:
		Font& mFont;
		uint32 mLastChangeCounter = 0;
		std::vector<Page> mPages;
		std::unordered_map<uint32, Glyph> mGlyphs;
		int mShelfX = 0;
		int mShelfY = 0;
		int mShelfHeight = 0;
	};


	struct Vertex
	{
		float x, y;
		float u, v;
		float r, g, b, a;

		inline void set(float x_, float y_, float u_, float v_, const Color& color)
		{
			x = x_;  y = y_;  u = u_;  v = v_;
			r = color.r;  g = color.g;  b = color.b;  a = color.a;
		}
	};


	struct Internal
	{
	public:
		Internal()
		{
			gDevice = static_cast<IDirect3DDevice9*>(SDL_X360_GetD3DDevice());
			if (nullptr == gDevice)
			{
				RMX_LOG_INFO("D3D9 drawer: no D3D9 device available");
				return;
			}

			const D3DVERTEXELEMENT9 vertexElements[] =
			{
				{ 0,  0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
				{ 0,  8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
				{ 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
				D3DDECL_END()
			};
			if (FAILED(gDevice->CreateVertexDeclaration(vertexElements, &mVertexDeclaration)) ||
				FAILED(gDevice->CreateVertexShader(g_drawerVS, &mVertexShader)) ||
				FAILED(gDevice->CreatePixelShader(g_drawerColorPS, &mColorShader)) ||
				FAILED(gDevice->CreatePixelShader(g_drawerTexturedPS, &mTexturedShader)) ||
				FAILED(gDevice->CreatePixelShader(g_drawerIndexedPS, &mIndexedShader)) ||
				FAILED(gDevice->CreatePixelShader(g_drawerUpscalePS, &mUpscaleShader)))
			{
				RMX_LOG_INFO("D3D9 drawer: creating the shaders failed");
				return;
			}
			if (FAILED(gDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &mBackBuffer)))
			{
				RMX_LOG_INFO("D3D9 drawer: GetBackBuffer failed");
				return;
			}

			// Only "60 FPS" (V-Sync on) and "30 FPS" (V-Sync + FPS cap) are offered on Xbox 360
			Configuration& config = Configuration::instance();
			if (config.mFrameSync != Configuration::FrameSyncType::VSYNC_ON && config.mFrameSync != Configuration::FrameSyncType::VSYNC_FRAMECAP)
				config.mFrameSync = Configuration::FrameSyncType::VSYNC_ON;

			RMX_LOG_INFO("D3D9 drawer ready");
			mSetupSuccessful = true;
		}

		~Internal()
		{
			if (nullptr != gDevice)
				gDevice->BlockUntilIdle();
			mComponentSpriteTextures.clear();
			mPaletteSpriteTextures.clear();
			mPaletteTextures.clear();
			for (auto& pair : mFontAtlases)
				delete pair.second;
			mFontAtlases.clear();
			if (nullptr != mBackBuffer)
				mBackBuffer->Release();
		}

		void applyStaticRenderStates()
		{
			gDevice->SetRenderState(D3DRS_HALFPIXELOFFSET, TRUE);	// Pixel centers at (0.5, 0.5), like in OpenGL
			gDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			gDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
			gDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
			gDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
			gDevice->SetVertexDeclaration(mVertexDeclaration);
			gDevice->SetVertexShader(mVertexShader);
			applyBlendMode(mBlendMode, true);
			applyScissor();
		}

		void beginFrameIfNeeded()
		{
			if (mFrameStarted)
				return;
			mFrameStarted = true;
			applyStaticRenderStates();
			setWindowRenderTarget(getWindowRect());
		}

		Recti getWindowRect() const
		{
			int width = 1280;
			int height = 720;
			if (nullptr != mOutputWindow)
				SDL_GetWindowSize(mOutputWindow, &width, &height);
			return Recti(0, 0, width, height);
		}

		void setViewport(const Recti& viewport)
		{
			mCurrentViewport = viewport;
			D3DVIEWPORT9 d3dViewport;
			d3dViewport.X = (DWORD)std::max(viewport.x, 0);
			d3dViewport.Y = (DWORD)std::max(viewport.y, 0);
			d3dViewport.Width = (DWORD)std::max(viewport.width, 1);
			d3dViewport.Height = (DWORD)std::max(viewport.height, 1);
			d3dViewport.MinZ = 0.0f;
			d3dViewport.MaxZ = 1.0f;
			gDevice->SetViewport(&d3dViewport);

			// Pixel coordinates relative to the viewport, y pointing down
			const float transform[4] = { -1.0f, 1.0f, 2.0f / (float)d3dViewport.Width, -2.0f / (float)d3dViewport.Height };
			gDevice->SetVertexShaderConstantF(0, transform, 1);
		}

		void finishRenderTarget()
		{
			if (nullptr != mActiveRenderTarget)
			{
				gDevice->Resolve(D3DRESOLVE_RENDERTARGET0, nullptr, mActiveRenderTarget->getRenderTargetTexture(), nullptr, 0, 0, nullptr, 0.0f, 0, nullptr);
				mActiveRenderTarget = nullptr;
			}
		}

		void setWindowRenderTarget(const Recti& viewport)
		{
			finishRenderTarget();
			gDevice->SetRenderTarget(0, mBackBuffer);
			if (!mWindowCleared)
			{
				gDevice->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
				mWindowCleared = true;
			}
			setViewport(viewport);
			applyScissor();
		}

		void setTextureRenderTarget(DrawerTexture& drawerTexture, const Recti& viewport)
		{
			finishRenderTarget();
			D3D9DrawerTexture* texture = drawerTexture.getImplementation<D3D9DrawerTexture>();
			if (nullptr == texture || nullptr == texture->getRenderTargetSurface())
			{
				// Shouldn't happen; keep drawing into the window then
				gDevice->SetRenderTarget(0, mBackBuffer);
				setViewport(viewport);
				return;
			}
			gDevice->SetRenderTarget(0, texture->getRenderTargetSurface());
			mActiveRenderTarget = texture;
			setViewport(viewport);
			applyScissor();
		}

		void applyBlendMode(BlendMode blendMode, bool force = false)
		{
			if (blendMode == mBlendMode && !force)
				return;
			mBlendMode = blendMode;

			D3DBLENDOP op = D3DBLENDOP_ADD;
			D3DBLEND source = D3DBLEND_ONE;
			D3DBLEND destination = D3DBLEND_ZERO;
			bool enable = true;
			switch (blendMode)
			{
				case BlendMode::OPAQUE:			enable = false;  break;
				case BlendMode::ALPHA:
				case BlendMode::ONE_BIT:		source = D3DBLEND_SRCALPHA;  destination = D3DBLEND_INVSRCALPHA;  break;
				case BlendMode::ADDITIVE:		source = D3DBLEND_SRCALPHA;  destination = D3DBLEND_ONE;  break;
				case BlendMode::SUBTRACTIVE:	op = D3DBLENDOP_REVSUBTRACT;  source = D3DBLEND_SRCALPHA;  destination = D3DBLEND_ONE;  break;
				case BlendMode::MULTIPLICATIVE:	source = D3DBLEND_DESTCOLOR;  destination = D3DBLEND_ZERO;  break;
				case BlendMode::MINIMUM:		op = D3DBLENDOP_MIN;  break;
				case BlendMode::MAXIMUM:		op = D3DBLENDOP_MAX;  break;
			}
			gDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, enable ? TRUE : FALSE);
			gDevice->SetRenderState(D3DRS_BLENDOP, op);
			gDevice->SetRenderState(D3DRS_SRCBLEND, source);
			gDevice->SetRenderState(D3DRS_DESTBLEND, destination);
			gDevice->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);		// Same blending for alpha, as with OpenGL's glBlendFunc
		}

		void applyScissor()
		{
			if (mScissorStack.empty())
			{
				gDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
				mInvalidScissorRegion = false;
				return;
			}
			const Recti& rect = mScissorStack.back();
			mInvalidScissorRegion = (rect.width <= 0 || rect.height <= 0);
			if (mInvalidScissorRegion)
				return;
			RECT scissorRect = { rect.x, rect.y, rect.x + rect.width, rect.y + rect.height };
			gDevice->SetScissorRect(&scissorRect);
			gDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
		}

		void setSampler(DWORD stage, SamplingMode samplingMode, TextureWrapMode wrapMode)
		{
			const D3DTEXTUREFILTERTYPE filter = (samplingMode == SamplingMode::BILINEAR) ? D3DTEXF_LINEAR : D3DTEXF_POINT;
			const D3DTEXTUREADDRESS address = (wrapMode == TextureWrapMode::REPEAT) ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
			gDevice->SetSamplerState(stage, D3DSAMP_MINFILTER, filter);
			gDevice->SetSamplerState(stage, D3DSAMP_MAGFILTER, filter);
			gDevice->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			gDevice->SetSamplerState(stage, D3DSAMP_ADDRESSU, address);
			gDevice->SetSamplerState(stage, D3DSAMP_ADDRESSV, address);
		}

		inline bool mayRenderAnything() const  { return !mInvalidScissorRegion; }

		void drawPrimitives(D3DPRIMITIVETYPE primitiveType, const Vertex* vertices, size_t numVertices)
		{
			// Split into chunks, to keep the command buffer allocations of DrawPrimitiveUP small
			const size_t verticesPerPrimitive = (primitiveType == D3DPT_QUADLIST) ? 4 : 3;
			const size_t maxVerticesPerCall = 256 * verticesPerPrimitive;
			while (numVertices >= verticesPerPrimitive)
			{
				const size_t count = std::min(numVertices - numVertices % verticesPerPrimitive, maxVerticesPerCall);
				gDevice->DrawPrimitiveUP(primitiveType, (UINT)(count / verticesPerPrimitive), vertices, sizeof(Vertex));
				vertices += count;
				numVertices -= count;
			}
		}

		void drawQuad(const Recti& rect, const Color& color, const Vec2f& uv0 = Vec2f(0.0f, 0.0f), const Vec2f& uv1 = Vec2f(1.0f, 1.0f))
		{
			const float x0 = (float)rect.x;
			const float y0 = (float)rect.y;
			const float x1 = (float)(rect.x + rect.width);
			const float y1 = (float)(rect.y + rect.height);
			Vertex quad[4];
			quad[0].set(x0, y0, uv0.x, uv0.y, color);
			quad[1].set(x1, y0, uv1.x, uv0.y, color);
			quad[2].set(x1, y1, uv1.x, uv1.y, color);
			quad[3].set(x0, y1, uv0.x, uv1.y, color);
			drawPrimitives(D3DPT_QUADLIST, quad, 4);
		}

		void drawColoredRect(const Recti& rect, const Color& color)
		{
			gDevice->SetPixelShader(mColorShader);
			drawQuad(rect, color);
		}

		void drawTexturedRect(const Recti& rect, IDirect3DTexture9* texture, const Color& color, const Vec2f& uv0 = Vec2f(0.0f, 0.0f), const Vec2f& uv1 = Vec2f(1.0f, 1.0f))
		{
			if (nullptr == texture)
				return;
			gDevice->SetPixelShader(mTexturedShader);
			gDevice->SetTexture(0, texture);
			setSampler(0, mSamplingMode, mWrapMode);
			drawQuad(rect, color, uv0, uv1);
		}

		void drawIndexedRect(const Recti& rect, IDirect3DTexture9* indexTexture, IDirect3DTexture9* paletteTexture, const Color& color)
		{
			if (nullptr == indexTexture || nullptr == paletteTexture)
				return;
			gDevice->SetPixelShader(mIndexedShader);
			gDevice->SetTexture(0, indexTexture);
			gDevice->SetTexture(1, paletteTexture);
			setSampler(0, SamplingMode::POINT, TextureWrapMode::CLAMP);
			setSampler(1, SamplingMode::POINT, TextureWrapMode::CLAMP);
			drawQuad(rect, color);
			gDevice->SetTexture(1, nullptr);
		}

		void drawUpscaledRect(const Recti& rect, DrawerTexture& drawerTexture)
		{
			D3D9DrawerTexture* texture = drawerTexture.getImplementation<D3D9DrawerTexture>();
			if (nullptr == texture || nullptr == texture->getTexture() || drawerTexture.getWidth() <= 0 || drawerTexture.getHeight() <= 0 || rect.width <= 0 || rect.height <= 0)
				return;
			const float textureSizeAndScale[4] =
			{
				(float)drawerTexture.getWidth(), (float)drawerTexture.getHeight(),
				(float)rect.width / (float)drawerTexture.getWidth(), (float)rect.height / (float)drawerTexture.getHeight()
			};
			gDevice->SetPixelShader(mUpscaleShader);
			gDevice->SetPixelShaderConstantF(0, textureSizeAndScale, 1);
			gDevice->SetTexture(0, texture->getTexture());
			setSampler(0, SamplingMode::BILINEAR, TextureWrapMode::CLAMP);

			const BlendMode oldBlendMode = mBlendMode;
			applyBlendMode(BlendMode::OPAQUE);
			drawQuad(rect, Color::WHITE);
			applyBlendMode(oldBlendMode);
		}

		IDirect3DTexture9* getComponentSpriteTexture(const SpriteCollection::Item& item)
		{
			SpriteTexture& entry = mComponentSpriteTextures[item.mKey];
			if (entry.mChangeCounter != item.mChangeCounter || !entry.mTexture.isValid())
			{
				const Bitmap& bitmap = static_cast<ComponentSprite*>(item.mSprite)->getBitmap();
				entry.mTexture.upload(bitmap.getData(), bitmap.getWidth(), bitmap.getHeight(), 4, bitmap.getWidth() * 4, D3DFMT_LIN_A8B8G8R8);
				entry.mChangeCounter = item.mChangeCounter;
			}
			return entry.mTexture.get();
		}

		IDirect3DTexture9* getPaletteSpriteTexture(const SpriteCollection::Item& item)
		{
			SpriteTexture& entry = mPaletteSpriteTextures[item.mKey];
			if (entry.mChangeCounter != item.mChangeCounter || !entry.mTexture.isValid())
			{
				const PaletteBitmap& bitmap = static_cast<PaletteSprite*>(item.mSprite)->getBitmap();
				entry.mTexture.upload(bitmap.getData(), bitmap.getWidth(), bitmap.getHeight(), 1, bitmap.getWidth(), D3DFMT_LIN_L8);
				entry.mChangeCounter = item.mChangeCounter;
			}
			return entry.mTexture.get();
		}

		IDirect3DTexture9* getPaletteTexture(const PaletteBase& palette)
		{
			PaletteTexture& entry = mPaletteTextures[palette.getKey()];
			if (entry.mChangeCounter != palette.getChangeCounter() || !entry.mTexture.isValid())
			{
				uint32 colors[256] = { 0 };
				memcpy(colors, palette.getRawColors(), std::min<size_t>(palette.getSize(), 256) * sizeof(uint32));
				entry.mTexture.upload(colors, 256, 1, 4, 256 * 4, D3DFMT_LIN_A8B8G8R8);
				entry.mChangeCounter = palette.getChangeCounter();
			}
			return entry.mTexture.get();
		}

		void printText(Font& font, const StringReader& text, const Recti& rect, const DrawerPrintOptions& printOptions)
		{
			FontAtlas*& atlas = mFontAtlases[&font];
			if (nullptr == atlas)
				atlas = new FontAtlas(font);

			const Vec2i position = font.alignText(rect, text, printOptions.mAlignment);
			mTypeInfos.clear();
			font.getTypeInfos(mTypeInfos, position, text, printOptions.mSpacing);
			if (mTypeInfos.empty())
				return;

			// Like in the OpenGL drawer, text always switches to alpha blending
			applyBlendMode(BlendMode::ALPHA);
			gDevice->SetPixelShader(mTexturedShader);
			setSampler(0, SamplingMode::POINT, TextureWrapMode::CLAMP);

			// Batch the glyph quads per atlas page
			int currentPage = -1;
			mVertexBuffer.clear();
			for (const Font::TypeInfo& typeInfo : mTypeInfos)
			{
				if (nullptr == typeInfo.mBitmap)
					continue;
				const FontAtlas::Glyph* glyph = atlas->getGlyph(typeInfo);
				if (nullptr == glyph)
					continue;

				if (glyph->mPage != currentPage)
				{
					flushTextVertices(*atlas, currentPage);
					currentPage = glyph->mPage;
				}

				const float x0 = (float)(typeInfo.mPosition.x - glyph->mBorderLeft);
				const float y0 = (float)(typeInfo.mPosition.y - glyph->mBorderTop);
				const float x1 = x0 + (float)glyph->mSize.x;
				const float y1 = y0 + (float)glyph->mSize.y;
				const size_t index = mVertexBuffer.size();
				mVertexBuffer.resize(index + 4);
				mVertexBuffer[index + 0].set(x0, y0, glyph->mUV0.x, glyph->mUV0.y, printOptions.mTintColor);
				mVertexBuffer[index + 1].set(x1, y0, glyph->mUV1.x, glyph->mUV0.y, printOptions.mTintColor);
				mVertexBuffer[index + 2].set(x1, y1, glyph->mUV1.x, glyph->mUV1.y, printOptions.mTintColor);
				mVertexBuffer[index + 3].set(x0, y1, glyph->mUV0.x, glyph->mUV1.y, printOptions.mTintColor);
			}
			flushTextVertices(*atlas, currentPage);
		}

		void flushTextVertices(FontAtlas& atlas, int page)
		{
			if (page >= 0 && !mVertexBuffer.empty())
			{
				gDevice->SetTexture(0, atlas.getPageTexture(page));
				drawPrimitives(D3DPT_QUADLIST, &mVertexBuffer[0], mVertexBuffer.size());
			}
			mVertexBuffer.clear();
		}

	public:
		struct SpriteTexture
		{
			UploadTexture mTexture;
			uint32 mChangeCounter = 0xffffffff;
		};
		struct PaletteTexture
		{
			PaletteTexture() : mTexture(2) {}
			UploadTexture mTexture;
			uint32 mChangeCounter = 0xffffffff;
		};

		bool mSetupSuccessful = false;
		SDL_Window* mOutputWindow = nullptr;

		IDirect3DVertexDeclaration9* mVertexDeclaration = nullptr;
		IDirect3DVertexShader9* mVertexShader = nullptr;
		IDirect3DPixelShader9* mColorShader = nullptr;
		IDirect3DPixelShader9* mTexturedShader = nullptr;
		IDirect3DPixelShader9* mIndexedShader = nullptr;
		IDirect3DPixelShader9* mUpscaleShader = nullptr;
		IDirect3DSurface9* mBackBuffer = nullptr;

		bool mFrameStarted = false;
		bool mWindowCleared = false;
		D3D9DrawerTexture* mActiveRenderTarget = nullptr;
		Recti mCurrentViewport;

		BlendMode mBlendMode = BlendMode::OPAQUE;
		SamplingMode mSamplingMode = SamplingMode::POINT;
		TextureWrapMode mWrapMode = TextureWrapMode::CLAMP;
		std::vector<Recti> mScissorStack;
		bool mInvalidScissorRegion = false;

		std::unordered_map<uint64, SpriteTexture> mComponentSpriteTextures;
		std::unordered_map<uint64, SpriteTexture> mPaletteSpriteTextures;
		std::unordered_map<uint64, PaletteTexture> mPaletteTextures;
		std::unordered_map<Font*, FontAtlas*> mFontAtlases;

		std::vector<Font::TypeInfo> mTypeInfos;
		std::vector<Vertex> mVertexBuffer;

		DWORD mPresentInterval = 0xffffffff;	// Not set yet

		// Diagnostics
		uint32 mFramesPresented = 0;
		double mPresentSeconds = 0.0;
	};
}

using namespace d3d9drawer;


class D3D9Drawer final : public DrawerInterface
{
public:
	D3D9Drawer() : mInternal(*new d3d9drawer::Internal()) {}
	~D3D9Drawer()  { delete &mInternal; }

	inline Drawer::Type getType() override  { return Drawer::Type::D3D9; }
	bool wasSetupSuccessful() override  { return mInternal.mSetupSuccessful; }

	void createTexture(DrawerTexture& outTexture) override
	{
		outTexture.setImplementation(new D3D9DrawerTexture(outTexture));
	}

	void refreshTexture(DrawerTexture& texture) override
	{
		createTexture(texture);
	}

	void setupRenderWindow(SDL_Window* window) override
	{
		mInternal.mOutputWindow = window;
	}

	void performRendering(const DrawCollection& drawCollection) override;
	void presentScreen() override;

private:
	d3d9drawer::Internal& mInternal;
};


void D3D9Drawer::performRendering(const DrawCollection& drawCollection)
{
	Internal& in = mInternal;
	in.beginFrameIfNeeded();

	for (DrawCommand* drawCommand : drawCollection.getDrawCommands())
	{
		switch (drawCommand->getType())
		{
			case DrawCommand::Type::UNDEFINED:
				break;

			case DrawCommand::Type::SET_WINDOW_RENDER_TARGET:
			{
				in.setWindowRenderTarget(drawCommand->as<SetWindowRenderTargetDrawCommand>().mViewport);
				break;
			}

			case DrawCommand::Type::SET_RENDER_TARGET:
			{
				SetRenderTargetDrawCommand& dc = drawCommand->as<SetRenderTargetDrawCommand>();
				in.setTextureRenderTarget(*dc.mTexture, dc.mViewport);
				break;
			}

			case DrawCommand::Type::RECT:
			{
				if (!in.mayRenderAnything())
					break;
				RectDrawCommand& dc = drawCommand->as<RectDrawCommand>();
				if (nullptr != dc.mTexture)
				{
					D3D9DrawerTexture* texture = dc.mTexture->getImplementation<D3D9DrawerTexture>();
					if (nullptr != texture)
						in.drawTexturedRect(dc.mRect, texture->getTexture(), dc.mColor, dc.mUV0, dc.mUV1);
				}
				else
				{
					in.drawColoredRect(dc.mRect, dc.mColor);
				}
				break;
			}

			case DrawCommand::Type::UPSCALED_RECT:
			{
				if (!in.mayRenderAnything())
					break;
				UpscaledRectDrawCommand& dc = drawCommand->as<UpscaledRectDrawCommand>();
				in.drawUpscaledRect(dc.mRect, *dc.mTexture);
				break;
			}

			case DrawCommand::Type::SPRITE:
			{
				if (!in.mayRenderAnything())
					break;
				SpriteDrawCommand& sc = drawCommand->as<SpriteDrawCommand>();
				const SpriteCollection::Item* item = SpriteCollection::instance().getSprite(sc.mSpriteKey);
				if (nullptr == item)
					break;

				SpriteBase& sprite = *item->mSprite;
				Vec2i offset = sprite.mOffset;
				Vec2i size = sprite.getSize();
				if (sc.mScale.x != 1.0f || sc.mScale.y != 1.0f)
				{
					offset.x = roundToInt((float)offset.x * sc.mScale.x);
					offset.y = roundToInt((float)offset.y * sc.mScale.y);
					size.x = roundToInt((float)size.x * sc.mScale.x);
					size.y = roundToInt((float)size.y * sc.mScale.y);
				}
				const Recti targetRect(sc.mPosition + offset, size);

				if (item->mUsesComponentSprite)
				{
					in.drawTexturedRect(targetRect, in.getComponentSpriteTexture(*item), sc.mTintColor);
				}
				else
				{
					const PaletteBase* palette = PaletteCollection::instance().getPalette(sc.mPaletteKey, 0);
					if (nullptr == palette)
						break;
					in.drawIndexedRect(targetRect, in.getPaletteSpriteTexture(*item), in.getPaletteTexture(*palette), sc.mTintColor);
				}
				break;
			}

			case DrawCommand::Type::SPRITE_RECT:
			{
				if (!in.mayRenderAnything())
					break;
				SpriteRectDrawCommand& sc = drawCommand->as<SpriteRectDrawCommand>();
				const SpriteCollection::Item* item = SpriteCollection::instance().getSprite(sc.mSpriteKey);
				if (nullptr == item || !item->mUsesComponentSprite)
					break;
				in.drawTexturedRect(sc.mRect, in.getComponentSpriteTexture(*item), sc.mTintColor);
				break;
			}

			case DrawCommand::Type::MESH:
			{
				if (!in.mayRenderAnything())
					break;
				MeshDrawCommand& dc = drawCommand->as<MeshDrawCommand>();
				if (dc.mTriangles.empty() || nullptr == dc.mTexture)
					break;
				D3D9DrawerTexture* texture = dc.mTexture->getImplementation<D3D9DrawerTexture>();
				if (nullptr == texture || nullptr == texture->getTexture())
					break;

				in.mVertexBuffer.resize(dc.mTriangles.size());
				for (size_t i = 0; i < dc.mTriangles.size(); ++i)
				{
					const DrawerMeshVertex& src = dc.mTriangles[i];
					in.mVertexBuffer[i].set(src.mPosition.x, src.mPosition.y, src.mTexcoords.x, src.mTexcoords.y, Color::WHITE);
				}
				gDevice->SetPixelShader(in.mTexturedShader);
				gDevice->SetTexture(0, texture->getTexture());
				in.setSampler(0, in.mSamplingMode, in.mWrapMode);
				in.drawPrimitives(D3DPT_TRIANGLELIST, &in.mVertexBuffer[0], in.mVertexBuffer.size());
				break;
			}

			case DrawCommand::Type::MESH_VERTEX_COLOR:
			{
				if (!in.mayRenderAnything())
					break;
				MeshVertexColorDrawCommand& dc = drawCommand->as<MeshVertexColorDrawCommand>();
				if (dc.mTriangles.empty())
					break;

				in.mVertexBuffer.resize(dc.mTriangles.size());
				for (size_t i = 0; i < dc.mTriangles.size(); ++i)
				{
					const DrawerMeshVertex_P2_C4& src = dc.mTriangles[i];
					in.mVertexBuffer[i].set(src.mPosition.x, src.mPosition.y, 0.0f, 0.0f, src.mColor);
				}
				gDevice->SetPixelShader(in.mColorShader);
				in.drawPrimitives(D3DPT_TRIANGLELIST, &in.mVertexBuffer[0], in.mVertexBuffer.size());
				break;
			}

			case DrawCommand::Type::SET_BLEND_MODE:
			{
				in.applyBlendMode(drawCommand->as<SetBlendModeDrawCommand>().mBlendMode);
				break;
			}

			case DrawCommand::Type::SET_SAMPLING_MODE:
			{
				in.mSamplingMode = drawCommand->as<SetSamplingModeDrawCommand>().mSamplingMode;
				break;
			}

			case DrawCommand::Type::SET_WRAP_MODE:
			{
				in.mWrapMode = drawCommand->as<SetWrapModeDrawCommand>().mWrapMode;
				break;
			}

			case DrawCommand::Type::PRINT_TEXT:
			{
				if (!in.mayRenderAnything())
					break;
				PrintTextDrawCommand& dc = drawCommand->as<PrintTextDrawCommand>();
				in.printText(*dc.mFont, dc.mText, dc.mRect, dc.mPrintOptions);
				break;
			}

			case DrawCommand::Type::PRINT_TEXT_W:
			{
				if (!in.mayRenderAnything())
					break;
				PrintTextWDrawCommand& dc = drawCommand->as<PrintTextWDrawCommand>();
				in.printText(*dc.mFont, dc.mText, dc.mRect, dc.mPrintOptions);
				break;
			}

			case DrawCommand::Type::PUSH_SCISSOR:
			{
				Recti scissorRect = drawCommand->as<PushScissorDrawCommand>().mRect;
				if (!in.mScissorStack.empty())
					scissorRect.intersect(in.mScissorStack.back());
				in.mScissorStack.emplace_back(scissorRect);
				in.applyScissor();
				break;
			}

			case DrawCommand::Type::POP_SCISSOR:
			{
				if (!in.mScissorStack.empty())
					in.mScissorStack.pop_back();
				in.applyScissor();
				break;
			}
		}
	}

	gDevice->SetTexture(0, nullptr);
}

void D3D9Drawer::presentScreen()
{
	Internal& in = mInternal;
	in.beginFrameIfNeeded();
	in.finishRenderTarget();
	gDevice->SetRenderTarget(0, in.mBackBuffer);


	// 60 FPS: present on every vsync, 30 FPS mode: on every second one
	const DWORD presentInterval = (Configuration::instance().mFrameSync == Configuration::FrameSyncType::VSYNC_FRAMECAP) ? D3DPRESENT_INTERVAL_TWO : D3DPRESENT_INTERVAL_ONE;
	if (presentInterval != in.mPresentInterval)
	{
		gDevice->SetRenderState(D3DRS_PRESENTINTERVAL, presentInterval);

		// "Adaptive vsync" at 60 FPS: a frame that missed its vertical blank still gets presented immediately, as long
		// as the display is within the first 20% of the screen; that avoids waiting a whole frame (which halves the frame
		// rate for that moment), at the cost of a possible tear line near the top. Not used for the 30 FPS mode.
		gDevice->SetRenderState(D3DRS_PRESENTIMMEDIATETHRESHOLD, (presentInterval == D3DPRESENT_INTERVAL_ONE) ? 20 : 0);
		in.mPresentInterval = presentInterval;
	}

	LARGE_INTEGER timeStart;
	QueryPerformanceCounter(&timeStart);
	gDevice->Present(nullptr, nullptr, nullptr, nullptr);
	LARGE_INTEGER timeEnd;
	LARGE_INTEGER frequency;
	QueryPerformanceCounter(&timeEnd);
	QueryPerformanceFrequency(&frequency);
	in.mPresentSeconds += (double)(timeEnd.QuadPart - timeStart.QuadPart) / (double)frequency.QuadPart;

	// Occasional status in the log, with the average time spent in Present (i.e. waiting for the GPU or vsync)
	++in.mFramesPresented;
	if (in.mFramesPresented == 1 || (in.mFramesPresented % 600) == 0)
	{
		// Only written with "Full Profiling", as writing to the log isn't free
		if (in.mFramesPresented == 1 || Configuration::instance().mPerformanceDisplay >= 2)
		{
			const double framesMeasured = (in.mFramesPresented == 1) ? 1.0 : 600.0;
			char text[128];
			snprintf(text, sizeof(text), "D3D9Drawer: presented frame %u, avg %.2f ms per Present", (unsigned)in.mFramesPresented, in.mPresentSeconds * 1000.0 / framesMeasured);
			DbgPrint("%s\n", text);
			std::cout << text << std::endl;		// Goes into game:\s3air.log
		}
		in.mPresentSeconds = 0.0;
	}

	in.mFrameStarted = false;
	in.mWindowCleared = false;
}


// Called by EngineMain on Xbox 360 instead of creating the software drawer
bool createX360Drawer(Drawer& drawer)
{
	return drawer.createDrawer<D3D9Drawer>();
}
