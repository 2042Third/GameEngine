#include "Include/Common.slang"

#include "Include/Common.slang"
#include "Include/Dither.slang"
#include "Include/Grain.slang"

// Morphological anti-aliasing, pass 3 of 3: each pixel mixes, in linear light, with the neighbors across its edges by the
// weights of pass 2 (its own top and left edges' r and b; the g of the pixel below and the a of the pixel right of it, the
// shares their edges give this pixel), and the result takes the film grain and is dithered into the display target. A pixel
// without weights keeps the tone mapping's value exactly (before the grain).

Texture2D<float4> u_Color : register(t0); // The tone mapping's display-encoded image (10 bits)
Texture2D<float4> u_Weights : register(t1);

// RenderData::MorphologicalParameters (Renderer/PostProcessData.h).
struct MorphologicalParameters
{
	float Threshold;
	float DepthThreshold;
	float DitherLevels; // 2^bits - 1 of the target this pass writes; 0: no dither
	float FilmGrain;      // The blend's grain (Include/Grain.glsl), added after the blend; 0 = off
	uint GrainFrame;      // The frame's number: the grain changes every frame
	float Padding0;
	float Padding1;
	float Padding2;
};
[[vk::push_constant]] ConstantBuffer<MorphologicalParameters> u_Parameters;

static float2 v_UV;
static float4 o_Color;

void st_Main()
{
	int2 size = TextureSize(u_Color, 0);
	int2 pixel = int2(st_FragCoord.xy);
	float3 color = u_Color.Load(int3(pixel, 0)).rgb;
	float4 own = u_Weights.Load(int3(pixel, 0));
	float up = own.r;
	float left = own.b;
	float down = pixel.y + 1 < size.y ? u_Weights.Load(int3(pixel + int2(0, 1), 0)).g : 0.0;
	float right = pixel.x + 1 < size.x ? u_Weights.Load(int3(pixel + int2(1, 0), 0)).a : 0.0;
	float total = up + down + left + right;
	if (total > 0.0)
	{
		// Shares above a whole pixel (a pixel in a corner of two runs) are scaled down to one.
		float scale = total > 1.0 ? 1.0 / total : 1.0;
		float3 mixed = SRGBToLinear(color) * (1.0 - total * scale);
		mixed += SRGBToLinear(u_Color.Load(int3(max(pixel - int2(0, 1), int2(0)), 0)).rgb) * (up * scale);
		mixed += SRGBToLinear(u_Color.Load(int3(min(pixel + int2(0, 1), size - 1), 0)).rgb) * (down * scale);
		mixed += SRGBToLinear(u_Color.Load(int3(max(pixel - int2(1, 0), int2(0)), 0)).rgb) * (left * scale);
		mixed += SRGBToLinear(u_Color.Load(int3(min(pixel + int2(1, 0), size - 1), 0)).rgb) * (right * scale);
		color = LinearToSRGB(mixed);
	}
	color = AddFilmGrain(color, uint2(pixel), u_Parameters.FilmGrain, u_Parameters.GrainFrame);
	o_Color = float4(DitherToCodes(color, uint2(pixel), u_Parameters.DitherLevels), 1.0);
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float2 v_UV : TEXCOORD0;
};
struct ST_Output
{
	[[vk::location(0)]] float4 o_Color : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	v_UV = input.v_UV;
	st_Main();
	ST_Output output;
	output.o_Color = o_Color;
	return output;
}
