/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2026 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#pragma once

#include "oxygen/rendering/Renderer.h"
#include "oxygen/drawing/software/Blitter.h"

class PlaneGeometry;
class SpriteGeometry;
namespace detail
{
	class PixelBlockWriter;
}


class SoftwareRenderer : public Renderer
{
friend class detail::PixelBlockWriter;

public:
	static constexpr int8 RENDERER_TYPE_ID = 0x10;

public:
	SoftwareRenderer(RenderParts& renderParts, DrawerTexture& outputTexture);

	virtual void initialize() override;
	virtual void reset() override;
	virtual void setGameResolution(const Vec2i& gameResolution) override;
	virtual void clearGameScreen() override;
	virtual void renderGameScreen(const std::vector<Geometry*>& geometries) override;
	virtual void renderDebugDraw(int debugDrawMode, const Recti& rect) override;

private:
	struct RenderContext;

	void renderGeometryRange(RenderContext& context, size_t firstIndex, size_t endIndex);
	void renderGeometry(RenderContext& context, const Geometry& geometry);
	void renderPlane(RenderContext& context, const PlaneGeometry& geometry);
	void renderSprite(RenderContext& context, const SpriteGeometry& geometry);

private:
	Vec2i mGameResolution;
	Bitmap mGameScreenCopy;

	uint8 mDepthBuffer[0x20000] = { 0 };	// 512x256 pixels

	struct BufferedPlaneData
	{
		struct PixelBlock
		{
			Vec2i mStartCoords;
			int mLinearPosition = 0;	// Linear position on screen / in content
			int mNumPixels = 0;			// Pixels of horizontal line
			uint8 mAtex = 0;
			uint8 mPaletteIndex = 0;
		};

		bool mValid = false;
		int mPlaneIndex = 0;
		int mScrollOffsets = 0;
		Recti mActiveRect;

		std::vector<uint8> mContent;
		std::vector<PixelBlock> mPrioBlocks;
		std::vector<PixelBlock> mNonPrioBlocks;
	};
	static const constexpr int MAX_BUFFER_PLANE_DATA = 8;

	// The game screen can be rendered in horizontal bands, each by its own thread (used on Xbox 360): every band
	// renders all geometries clipped to its own lines, so the bands only write disjoint parts of the output and depth buffer
	struct RenderContext
	{
		Recti mBandRect;						// Lines of the game screen covered by this context
		Recti mCurrentViewport;					// Always inside the band
		bool mFullViewport = true;
		bool mEmptyDepthBuffer = true;			// Stays true until first non-zero depth value was written
		uint16 mLastRenderQueue = 0xffff;
		uint16 mWindowPlaneScrollOffsetX = 0;
		BufferedPlaneData mBufferedPlaneData[MAX_BUFFER_PLANE_DATA];
		Blitter mBlitter;
	};
	static const constexpr int MAX_RENDER_CONTEXTS = 4;
	RenderContext mRenderContexts[MAX_RENDER_CONTEXTS];
	int mNumRenderContexts = 1;

	// State shared by the render contexts while rendering a frame
	const std::vector<Geometry*>* mGeometries = nullptr;
	bool mUsingSpriteMask = false;
	volatile bool mAbortRendering = false;
};
