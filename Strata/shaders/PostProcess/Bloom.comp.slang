#include "Include/Common.slang"

#include "Include/Common.slang"
#include "Include/Exposure.slang"

// Bloom (Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare"): the exposed scene color
// is downsampled through a mip chain with a 13-tap filter, then each level is upsampled with a 3x3 tent filter and
// added to the next larger one. The first downsample also applies the threshold and weights its samples by
// inverse luminance (Karis average), which keeps single bright pixels from flickering.

Texture2D<float4> u_Source : register(t0);
SamplerState u_LinearClamp : register(s0);
[vk::image_format("rgba16f")] RWTexture2D<float4> o_Destination : register(u0);
StructuredBuffer<float> u_Exposure : register(t1);

#define ST_BLOOM_PREFILTER  0u
#define ST_BLOOM_DOWNSAMPLE 1u
#define ST_BLOOM_UPSAMPLE   2u

struct BloomParameters
{
	float Threshold; // On the exposed color; 0 lets every pixel scatter
	float Knee;      // Width of the soft transition around the threshold
	uint Mode;
	float InversePreExposure; // 1 / the pre-exposure the scene color carries
	float UpsampleWeight;     // The weight of the smaller level an upsample adds: the deepest level's share, else 1
	float Padding0;
	float Padding1;
	float Padding2;
};
[[vk::push_constant]] ConstantBuffer<BloomParameters> u_Parameters;

float3 SampleSource(float2 uv)
{
	return u_Source.SampleLevel(u_LinearClamp, uv, 0.0).rgb;
}

float KarisWeight(float3 color, float weight)
{
	return weight / (1.0 + Luminance(color));
}

float3 Downsample(float2 uv, float2 texel, bool karisAverage)
{
	// a . b . c
	// . j . k .
	// d . e . f
	// . l . m .
	// g . h . i
	float3 a = SampleSource(uv + texel * float2(-2.0, -2.0));
	float3 b = SampleSource(uv + texel * float2(0.0, -2.0));
	float3 c = SampleSource(uv + texel * float2(2.0, -2.0));
	float3 d = SampleSource(uv + texel * float2(-2.0, 0.0));
	float3 e = SampleSource(uv);
	float3 f = SampleSource(uv + texel * float2(2.0, 0.0));
	float3 g = SampleSource(uv + texel * float2(-2.0, 2.0));
	float3 h = SampleSource(uv + texel * float2(0.0, 2.0));
	float3 i = SampleSource(uv + texel * float2(2.0, 2.0));
	float3 j = SampleSource(uv + texel * float2(-1.0, -1.0));
	float3 k = SampleSource(uv + texel * float2(1.0, -1.0));
	float3 l = SampleSource(uv + texel * float2(-1.0, 1.0));
	float3 m = SampleSource(uv + texel * float2(1.0, 1.0));

	// Five overlapping 2x2 boxes: the center one weighs 0.5, the corner ones 0.125 each.
	float3 boxes[5] = { (j + k + l + m) * 0.25, (a + b + d + e) * 0.25, (b + c + e + f) * 0.25, (d + e + g + h) * 0.25, (e + f + h + i) * 0.25 };
	float weights[5] = { 0.5, 0.125, 0.125, 0.125, 0.125 };

	float3 result = float3(0.0);
	float weightSum = 0.0;
	for (int box = 0; box < 5; box++)
	{
		float weight = karisAverage ? KarisWeight(boxes[box], weights[box]) : weights[box];
		result += boxes[box] * weight;
		weightSum += weight;
	}
	return result / weightSum;
}

float3 Upsample(float2 uv, float2 texel)
{
	// 3x3 tent filter: 1 2 1 / 2 4 2 / 1 2 1
	float3 result = SampleSource(uv) * 4.0;
	result += (SampleSource(uv + float2(0.0, -texel.y)) + SampleSource(uv + float2(-texel.x, 0.0))
		+ SampleSource(uv + float2(texel.x, 0.0)) + SampleSource(uv + float2(0.0, texel.y))) * 2.0;
	result += SampleSource(uv + float2(-texel.x, -texel.y)) + SampleSource(uv + float2(texel.x, -texel.y))
		+ SampleSource(uv + float2(-texel.x, texel.y)) + SampleSource(uv + float2(texel.x, texel.y));
	return result / 16.0;
}

float3 ApplyThreshold(float3 color)
{
	if (u_Parameters.Threshold <= 0.0)
		return color;
	// Soft knee: a quadratic ramp around the threshold instead of a hard cut.
	float brightness = max(color.r, max(color.g, color.b));
	float knee = max(u_Parameters.Knee, 1e-5);
	float soft = clamp(brightness - u_Parameters.Threshold + knee, 0.0, 2.0 * knee);
	soft = soft * soft / (4.0 * knee);
	return color * (max(soft, brightness - u_Parameters.Threshold) / max(brightness, 1e-5));
}

void st_Main()
{
	int2 pixel = int2(st_GlobalInvocationID.xy);
	int2 size = ImageSize(o_Destination);
	if (pixel.x >= size.x || pixel.y >= size.y)
		return;

	float2 uv = (float2(pixel) + 0.5) / float2(size);
	float2 texel = 1.0 / float2(TextureSize(u_Source, 0));
	float3 color;
	if (u_Parameters.Mode == ST_BLOOM_UPSAMPLE)
	{
		color = o_Destination[pixel].rgb + u_Parameters.UpsampleWeight * Upsample(uv, texel);
	}
	else if (u_Parameters.Mode == ST_BLOOM_PREFILTER)
	{
		color = ApplyThreshold(Downsample(uv, texel, true) * (u_Exposure[ST_EXPOSURE_MULTIPLIER] * u_Parameters.InversePreExposure));
		// Non-finite scene colors (a broken material) must not spread over the whole screen.
		if (any(isnan(color)) || any(isinf(color)))
			color = float3(0.0);
		color = min(color, float3(65000.0));
	}
	else
	{
		color = Downsample(uv, texel, false);
	}
	o_Destination[pixel] = float4(color, 1.0);
}

// --- Entry point ---
[numthreads(8, 8, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
