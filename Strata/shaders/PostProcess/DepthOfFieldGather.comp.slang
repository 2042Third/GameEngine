#include "Include/Common.slang"

#include "Include/Dither.slang"
#include "Include/DepthOfField.slang"

// Depth of field's gather, at half resolution: scatter as gather on a golden-angle (Vogel) disk of SampleCount samples, turned by
// a static per-pixel angle (no pattern repeats, nothing temporal). Two fields:
// - The pixel's own (o_Own): a blurred pixel (its circle of confusion at least half a texel) takes the mean of the samples within
//   its circle whose own circle covers it, so a sharp foreground never bleeds into a blurred background and near samples reach
//   a farther pixel only through the near field. A near pixel instead keeps what lies behind it (the samples farther than it),
//   which shows where the near field's coverage is partial; deep inside a near object, where nothing farther is in reach, its
//   own blur.
// - The near field (o_Near, premultiplied by its coverage): within the neighborhood's largest near circle, the samples of the near
//   field (at least as near as the pixel) whose circle covers the pixel, each weighed by the share of its light that falls on
//   the pixel (the disk's area over its circle's, at most 16), so a blurred foreground spreads half over the edge of what lies
//   behind it and fully over its own inside.

Texture2D<float4> u_Half : register(t0);
Texture2D<float4> u_Dilated : register(t2);
[vk::image_format("rgba16f")] RWTexture2D<float4> o_Near : register(u0);
[vk::image_format("rgba16f")] RWTexture2D<float4> o_Own : register(u1);

static const float c_GoldenAngle = 2.39996323;
static const float c_MaxNearShare = 16.0;

// Sample `index` of `count` on the unit disk, evenly spread (radius sqrt((i + 0.5) / count)), turned by `rotation`.
float2 VogelSample(uint index, uint count, float rotation)
{
	float radius = sqrt((float(index) + 0.5) / float(count));
	float angle = float(index) * c_GoldenAngle + rotation;
	return radius * float2(cos(angle), sin(angle));
}

float4 FetchHalf(int2 texel, float2 offset, int2 size)
{
	return u_Half.Load(int3(clamp(int2(floor(float2(texel) + 0.5 + offset)), int2(0), size - 1), 0));
}

// How much a sample whose circle has `radius` covers a pixel `distance` away: one inside, fading over the circle's last texel.
float Cover(float radius, float distance)
{
	return clamp(radius - distance + 1.0, 0.0, 1.0);
}

void st_Main()
{
	int2 size = TextureSize(u_Half, 0);
	int2 texel = int2(st_GlobalInvocationID.xy);
	if (any((texel >= size)))
		return;
	float4 center = u_Half.Load(int3(texel, 0));
	float centerCoC = center.a;
	bool centerNear = centerCoC < -c_DepthOfFieldInFocus;
	uint count = u_DepthOfField.SampleCount;
	float rotation = PixelRandom(uint2(texel), 7u) * TWO_PI;

	float3 own = center.rgb;
	float ownRadius = abs(centerCoC);
	if (ownRadius >= c_DepthOfFieldInFocus)
	{
		float3 sum = center.rgb;
		float total = 1.0;
		float3 behind = float3(0.0);
		float behindCount = 0.0;
		for (uint index = 0u; index < count; index++)
		{
			float2 offset = VogelSample(index, count, rotation) * ownRadius;
			float4 sampled = FetchHalf(texel, offset, size);
			float distance = length(offset);
			bool sampleNear = sampled.a < -c_DepthOfFieldInFocus;
			if (centerNear && sampled.a > centerCoC + 1.0)
			{
				behind += sampled.rgb;
				behindCount += 1.0;
			}
			if (sampleNear == centerNear)
			{
				float weight = Cover(abs(sampled.a), distance);
				sum += sampled.rgb * weight;
				total += weight;
			}
		}
		own = behindCount > 0.0 ? behind / behindCount : sum / total;
	}

	float4 near = float4(0.0);
	float nearRadius = u_Dilated.Load(int3(texel / int(u_DepthOfField.TileSize), 0)).x;
	if (nearRadius >= c_DepthOfFieldInFocus)
	{
		float3 sum = float3(0.0);
		float total = 0.0;
		float nearestAllowed = centerNear ? centerCoC + 0.5 : -c_DepthOfFieldInFocus;
		for (uint index = 0u; index < count; index++)
		{
			float2 offset = VogelSample(index, count, rotation) * nearRadius;
			float4 sampled = FetchHalf(texel, offset, size);
			if (sampled.a >= -c_DepthOfFieldInFocus || sampled.a > nearestAllowed)
				continue;
			float share = clamp(nearRadius * nearRadius / (sampled.a * sampled.a), 1.0, c_MaxNearShare);
			float weight = Cover(-sampled.a, length(offset)) * share;
			sum += sampled.rgb * weight;
			total += weight;
		}
		float coverage = clamp(total / float(count), 0.0, 1.0);
		if (total > 0.0)
			near = float4(sum / total * coverage, coverage);
	}
	o_Near[texel] = near;
	o_Own[texel] = float4(own, 1.0);
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
