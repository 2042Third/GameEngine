#include "Include/Common.slang"

#include "Include/Common.slang"

// One level of the sun shadow cascades' depth ranges (u_ShadowRanges, Include/Shadows.glsl): each texel the nearest and
// farthest depth (reversed-Z: the nearest to the light is the largest) of 4x4 shadow map texels for level 0, or of the 2x2
// texels of the level below (ST_SHADOW_RANGES_DOWNSAMPLE, registered as Scene/ShadowRangesDownsample.comp), packed as
// (ceil(nearest * 65535) << 16) | floor(farthest * 65535): rounded outward, so a cell's range holds every depth in it. Texels
// past the source's edge are clamped to it. A layer per z of the dispatch; layers outside LayerMask (cascades a frame kept
// from an earlier one, whose ranges are kept with them) are left as they are.

#ifdef ST_SHADOW_RANGES_DOWNSAMPLE
Texture2DArray<uint4> u_Source : register(t0);
#else
Texture2DArray<float4> u_Source : register(t0);
#endif
[vk::image_format("r32ui")] RWTexture2DArray<uint4> o_Destination : register(u0);

struct ShadowRangeParameters
{
	uint2 SourceSize;
	uint2 DestinationSize;
	uint LayerMask;
};
[[vk::push_constant]] ConstantBuffer<ShadowRangeParameters> u_Ranges;

void st_Main()
{
	uint2 texel = st_GlobalInvocationID.xy;
	uint layer = st_GlobalInvocationID.z;
	if (any((texel >= u_Ranges.DestinationSize)) || (u_Ranges.LayerMask & (1u << layer)) == 0u)
		return;
	int2 last = int2(u_Ranges.SourceSize) - 1;
#ifdef ST_SHADOW_RANGES_DOWNSAMPLE
	// The packed ranges combine exactly: the largest nearest and the smallest farthest.
	uint nearest = 0u;
	uint farthest = 0xFFFFu;
	for (int y = 0; y < 2; y++)
	{
		for (int x = 0; x < 2; x++)
		{
			uint packed = TexelFetch(u_Source, int3(min(int2(texel) * 2 + int2(x, y), last), int(layer)), 0).r;
			nearest = max(nearest, packed >> 16u);
			farthest = min(farthest, packed & 0xFFFFu);
		}
	}
#else
	float nearestDepth = 0.0;
	float farthestDepth = 1.0;
	for (int y = 0; y < 4; y++)
	{
		for (int x = 0; x < 4; x++)
		{
			float depth = TexelFetch(u_Source, int3(min(int2(texel) * 4 + int2(x, y), last), int(layer)), 0).r;
			nearestDepth = max(nearestDepth, depth);
			farthestDepth = min(farthestDepth, depth);
		}
	}
	// Rounded outward as UnpackShadowRange (Include/Shadows.glsl) reads them back: a product that rounded across a step is
	// moved one step further out.
	nearestDepth = clamp(nearestDepth, 0.0, 1.0);
	farthestDepth = clamp(farthestDepth, 0.0, 1.0);
	uint nearest = uint(ceil(nearestDepth * 65535.0));
	if (nearest < 65535u && float(nearest) / 65535.0 < nearestDepth)
		nearest++;
	uint farthest = uint(floor(farthestDepth * 65535.0));
	if (farthest > 0u && float(farthest) / 65535.0 > farthestDepth)
		farthest--;
#endif
	o_Destination[int3(texel, layer)] = uint4((nearest << 16u) | farthest, 0u, 0u, 0u);
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
