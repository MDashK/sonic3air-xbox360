// Xbox 360 port of Sonic 3 A.I.R.: shaders of the D3D9 drawer (x360/port/D3D9Drawer.cpp).
// Compiled offline with the XDK's fxc.exe into drawer_*.h (see build_shaders.cmd).
//
// All textures that come from the engine use D3DFMT_LIN_A8B8G8R8, which matches the engine's 0xAABBGGRR colors,
// so the sampled .rgba values need no swizzling. Vertex positions are in pixels of the current render target.

float4 gTransform : register(c0);		// Pixel to clip space: xy = offset, zw = scale

struct VertexInput
{
	float2 position : POSITION;
	float2 uv       : TEXCOORD0;
	float4 color    : COLOR0;
};

struct VertexOutput
{
	float4 position : POSITION;
	float2 uv       : TEXCOORD0;
	float4 color    : COLOR0;
};

VertexOutput mainVS(VertexInput input)
{
	VertexOutput output;
	output.position = float4(gTransform.xy + input.position * gTransform.zw, 0.0f, 1.0f);
	output.uv = input.uv;
	output.color = input.color;
	return output;
}


sampler2D gTexture : register(s0);
sampler2D gPalette : register(s1);
float4 gTextureSizeAndScale : register(c0);		// Only for upscaling: xy = texture size in pixels, zw = output pixels per texture pixel

// Plain color
float4 colorPS(float2 uv : TEXCOORD0, float4 color : COLOR0) : COLOR0
{
	return color;
}

// Texture with tint color; fully transparent pixels are discarded
float4 texturedPS(float2 uv : TEXCOORD0, float4 color : COLOR0) : COLOR0
{
	const float4 result = tex2D(gTexture, uv) * color;
	clip(result.a - 0.002f);
	return result;
}

// 8-bit palette sprite (D3DFMT_LIN_L8) with a palette texture of 256 x 1 colors
float4 indexedPS(float2 uv : TEXCOORD0, float4 color : COLOR0) : COLOR0
{
	const float index = tex2D(gTexture, uv).r;
	const float4 result = tex2D(gPalette, float2(index * (255.0f / 256.0f) + (0.5f / 256.0f), 0.5f)) * color;
	clip(result.a - 0.002f);
	return result;
}

// "Sharp bilinear" upscaling of the game screen: sharp pixel cores, only the borders between pixels get interpolated
float4 upscalePS(float2 uv : TEXCOORD0, float4 color : COLOR0) : COLOR0
{
	const float2 textureSize = gTextureSizeAndScale.xy;
	const float2 scale = gTextureSizeAndScale.zw;
	const float2 texel = uv * textureSize;
	const float2 texelFloor = floor(texel);
	const float2 centerDistance = (texel - texelFloor) - 0.5f;
	const float2 region = max(0.5f - 0.5f / scale, 0.0f);
	const float2 fraction = (centerDistance - clamp(centerDistance, -region, region)) * max(scale, 1.0f) + 0.5f;
	return float4(tex2D(gTexture, (texelFloor + fraction) / textureSize).rgb, 1.0f);
}
