#include "Include/Common.slang"

#include "Include/Common.slang"

// One level of a depth pyramid occlusion culling tests against (Cull.comp's second phases): each texel the farthest
// (smallest, reversed-Z) of the source's texels it covers. Level 0's source is the depth (Reduction x Reduction texels a
// texel: 2 for the view's depth buffer, 4 for a shadow cascade), and level 0 is its size divided by Reduction, rounded up;
// every other level's source is the level above (2x2 texels a texel), and its size is the texture's mip, half the level
// above, rounded down. A level's last row and column also cover the source's rows and columns past them (an odd source's
// last), so the texel of level m that covers a depth pixel p is p >> (m + log2 of level 0's Reduction), clamped to the
// level's last row and column (Include/Culling.slang's GetPyramidFootprint). Where the device filters with a minimum
// (samplerFilterMinmax) one sample per 2x2 does it; otherwise four loads. With ST_HZB_ARRAY (registered as
// Scene/HZBBuildArray.comp) the source and the destination are array textures (the shadow cascades'), a layer per z of the
// dispatch.

#ifdef ST_HZB_ARRAY
Texture2DArray<float4> u_Source : register(t0);
[vk::image_format("r32f")] RWTexture2DArray<float4> o_Destination : register(u0);
#define ST_HZB_LAYER int(st_GlobalInvocationID.z)
#define ST_HZB_FETCH(texel) TexelFetch(u_Source, int3(texel, ST_HZB_LAYER), 0).r
#define ST_HZB_SAMPLE(uv) u_Source.SampleLevel(u_MinimumSampler, float3(uv, float(ST_HZB_LAYER)), 0.0).r
#define ST_HZB_STORE(texel, value) o_Destination[int3(texel, ST_HZB_LAYER)] = float4(value)
#else
Texture2D<float4> u_Source : register(t0);
[vk::image_format("r32f")] RWTexture2D<float4> o_Destination : register(u0);
#define ST_HZB_FETCH(texel) TexelFetch(u_Source, texel, 0).r
#define ST_HZB_SAMPLE(uv) u_Source.SampleLevel(u_MinimumSampler, uv, 0.0).r
#define ST_HZB_STORE(texel, value) o_Destination[texel] = float4(value)
#endif
SamplerState u_MinimumSampler : register(s0);

struct PyramidParameters
{
	uint2 SourceSize;      // The source's own size: the depth's, or the level above's mip
	uint2 DestinationSize; // This level's mip
	uint UseMinimumSampler;
	uint Reduction; // Source texels per destination texel each way: 2, or 4
};
[[vk::push_constant]] ConstantBuffer<PyramidParameters> u_Pyramid;

// The farthest depth of the source's 2x2 texels from `base` (those past its last row and column repeat it).
float Farthest2x2(int2 base)
{
	if (u_Pyramid.UseMinimumSampler != 0u)
	{
		// The 2x2 footprint's center; clamping repeats an edge's texel, which leaves the minimum as it is. The coordinates must
		// be normalized by the source's own size: any other moves the footprint off the texels it covers, more the farther it
		// is from the origin.
		float2 uv = (float2(base) + 1.0) / float2(u_Pyramid.SourceSize);
		return ST_HZB_SAMPLE(uv);
	}
	int2 last = int2(u_Pyramid.SourceSize) - 1;
	return min(min(ST_HZB_FETCH(min(base, last)), ST_HZB_FETCH(min(base + int2(1, 0), last))),
		min(ST_HZB_FETCH(min(base + int2(0, 1), last)), ST_HZB_FETCH(min(base + int2(1, 1), last))));
}

void st_Main()
{
	uint2 texel = st_GlobalInvocationID.xy;
	if (any((texel >= u_Pyramid.DestinationSize)))
		return;
	int reduction = u_Pyramid.Reduction == 4u ? 4 : 2;
	// The source texels the texel covers: `reduction` each way, and from the last row and column to the source's end (three
	// where the source's size is odd and this level's half of it, rounded down).
	int2 sourceLast = int2(u_Pyramid.SourceSize) - 1;
	int2 first = int2(texel) * reduction;
	int2 last = min(first + reduction - 1, sourceLast);
	if (texel.x + 1u == u_Pyramid.DestinationSize.x)
		last.x = sourceLast.x;
	if (texel.y + 1u == u_Pyramid.DestinationSize.y)
		last.y = sourceLast.y;
	// In blocks of 2x2: one or two each way.
	bool wide = last.x - first.x >= 2;
	bool tall = last.y - first.y >= 2;
	float farthest = Farthest2x2(first);
	if (wide)
		farthest = min(farthest, Farthest2x2(first + int2(2, 0)));
	if (tall)
		farthest = min(farthest, Farthest2x2(first + int2(0, 2)));
	if (wide && tall)
		farthest = min(farthest, Farthest2x2(first + int2(2, 2)));
	ST_HZB_STORE(int2(texel), farthest);
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
