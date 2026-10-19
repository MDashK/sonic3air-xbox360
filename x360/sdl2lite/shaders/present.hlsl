// Xbox 360 presentation shaders for SDL2-lite (SDL_UpdateWindowSurface): draw the software-rendered
// frame (a linear A8B8G8R8 texture) as a textured quad. Compiled offline with the XDK's fxc.exe into
// present_vs.h / present_ps.h (see build_shaders.cmd), which are embedded in SDL_x360_video.cpp.

struct VertexInput
{
	float2 position : POSITION;		// Normalized device coordinates
	float2 uv       : TEXCOORD0;
};

struct VertexOutput
{
	float4 position : POSITION;
	float2 uv       : TEXCOORD0;
};

VertexOutput mainVS(VertexInput input)
{
	VertexOutput output;
	output.position = float4(input.position, 0.0f, 1.0f);
	output.uv = input.uv;
	return output;
}

sampler2D gFrameTexture : register(s0);
float4 gTextureSizeAndScale : register(c0);		// xy = texture size in pixels, zw = scale factor (output pixels per texture pixel)

// "Sharp bilinear" upscaling: each source pixel keeps a flat, sharp core, and only the edges between pixels
// get interpolated across about one output pixel. This avoids both the blur of plain bilinear filtering and
// the uneven pixel sizes of point sampling at non-integer scale factors (like 720 / 224).
float4 mainPS(float2 uv : TEXCOORD0) : COLOR0
{
	const float2 textureSize = gTextureSizeAndScale.xy;
	const float2 scale = gTextureSizeAndScale.zw;
	const float2 texel = uv * textureSize;
	const float2 texelFloor = floor(texel);
	const float2 centerDistance = (texel - texelFloor) - 0.5f;
	const float2 region = 0.5f - 0.5f / scale;
	const float2 fraction = (centerDistance - clamp(centerDistance, -region, region)) * scale + 0.5f;
	return float4(tex2D(gFrameTexture, (texelFloor + fraction) / textureSize).rgb, 1.0f);
}
