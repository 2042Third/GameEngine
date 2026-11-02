#include "Include/Common.slang"

#include "Include/ShadowView.slang"

// The shadows debug view's statistics (SceneRenderer::GetShadowViewStats): every pixel of the frame counted as sky, as a
// surface no cascade covers, or in its cascade's bin of screen pixels per shadow texel (Include/ShadowView.glsl, the same
// pixels Overlay/ShadowView.frag colors). Each group counts in shared memory and adds its non-zero counts to the buffer
// (RenderData::ShadowViewCounts, cleared before the dispatch) with one atomic add each.

Texture2D<float4> u_Depth : register(t22);

struct ShadowViewCountBuffer
{
	uint Bins[4 * 64]; // RenderData::c_MaxCascades rows of RenderData::c_ShadowViewBins
	uint SurfacePixels;
	uint SkyPixels;
	uint UncoveredPixels;
	uint Reserved;
};
RWStructuredBuffer<ShadowViewCountBuffer> st_o_Counts : register(u0);
#define o_Counts st_o_Counts[0]

groupshared uint s_Bins[4 * 64]; // One entry per thread of the group
groupshared uint s_Surface;
groupshared uint s_Sky;
groupshared uint s_Uncovered;

void st_Main()
{
	uint thread = st_LocalInvocationIndex;
	s_Bins[thread] = 0u;
	if (thread == 0u)
	{
		s_Surface = 0u;
		s_Sky = 0u;
		s_Uncovered = 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	int2 pixel = int2(st_GlobalInvocationID.xy);
	if (all((pixel < int2(u_Frame.ViewportSize.xy))))
	{
		float depth = u_Depth.Load(int3(pixel, 0)).r;
		ShadowViewSample viewSample;
		if (!(depth > 0.0))
			InterlockedAdd(s_Sky, 1u);
		else
		{
			InterlockedAdd(s_Surface, 1u);
			if (GetShadowViewSample(pixel, depth, viewSample))
				InterlockedAdd(s_Bins[viewSample.Cascade * 64 + GetShadowViewBin(viewSample.PixelsPerTexel)], 1u);
			else
				InterlockedAdd(s_Uncovered, 1u);
		}
	}
	GroupMemoryBarrierWithGroupSync();

	if (s_Bins[thread] != 0u)
		InterlockedAdd(o_Counts.Bins[thread], s_Bins[thread]);
	if (thread == 0u)
	{
		if (s_Surface != 0u)
			InterlockedAdd(o_Counts.SurfacePixels, s_Surface);
		if (s_Sky != 0u)
			InterlockedAdd(o_Counts.SkyPixels, s_Sky);
		if (s_Uncovered != 0u)
			InterlockedAdd(o_Counts.UncoveredPixels, s_Uncovered);
	}
}

// --- Entry point ---
[numthreads(16, 16, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
