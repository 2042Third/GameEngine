#include "Include/Common.slang"

#include "Include/GIProbes.slang"

// A coarser level's light from the finer level inside it (Docs/DeveloperGuide.md, "Global illumination"): a thread per voxel of
// level u_Pass.Level (1 and up, in order), whose eight children in the level below are all valid, takes their mean radiance (rgb
// times the surface weight, and the weight), blended with its own injection by the finer level's weight there (GetLevelWeight:
// continuous in the camera's motion). Each level injects its light with its own geometry, and a coarse level's voxels close small
// openings (a window's sill and lintel at 0.4 m): its sunlit patch was a third of the finer level's. Marches read coarse levels
// for far light; they now see the finer level's light wherever it holds the scene.
// A coarse voxel takes only the light of the finer surfaces that face its own way (CountsChild): one that holds both faces of a
// wall thinner than it, the sunlit outside and the dark inside, is two-sided and sends nothing (GI/VoxelFinalize.comp), and the
// mean of its children carried the lit side into the dark one wherever the finer level fades into it (a sealed room at Low, whose
// 0.2 m walls lie in single 0.4 m voxels of level 1 when they sit off its grid, read the sun through them).

Texture3D<float4> u_FinerRadiance : register(t49);
[vk::image_format("rgba16f")] RWTexture3D<float4> o_Radiance : register(u0);

// How much of a finer voxel's light a coarse voxel takes, by their normals (w: 1 one-sided, 0.5 double-sided, 0 no surface, -1
// two-sided; VoxelFinalize.comp): none for a two-sided coarse voxel, none from a one-sided child facing away from a one-sided
// coarse voxel (fully from 60 degrees in), all otherwise.
float CountsChild(float4 coarse, float4 child)
{
	if (coarse.w < -0.5)
		return 0.0;
	if (coarse.w < 0.75 || child.w < 0.75)
		return 1.0;
	return saturate(2.0 * dot(coarse.xyz, child.xyz));
}

[numthreads(4, 4, 4)]
void main(uint3 globalID : SV_DispatchThreadID)
{
	uint3 dimensions = u_GI.Dimensions.xyz;
	uint level = u_Pass.Level;
	if (any(globalID >= dimensions) || level == 0u)
		return;
	GIVoxelLevel data = u_GI.Levels[level];
	GIVoxelLevel finer = u_GI.Levels[level - 1u];
	if (data.Texel.w == 0 || finer.Texel.w == 0)
		return;
	int3 texel = int3(globalID);
	int3 local = ((texel - data.Texel.xyz) % int3(dimensions) + int3(dimensions)) % int3(dimensions);
	float3 center = data.Origin.xyz + (float3(local) + 0.5) * data.Origin.w;
	float weight = GetLevelWeight(level - 1u, center);
	if (weight <= 0.0)
		return;
	float4 coarseNormal = LoadGINormal(level, texel);
	float4 sum = float4(0.0, 0.0, 0.0, 0.0);
	[unroll]
	for (uint child = 0u; child < 8u; child++)
	{
		float3 offset = float3(float(child & 1u), float((child >> 1) & 1u), float((child >> 2) & 1u)) - 0.5;
		int3 childLocal = int3(floor((center + offset * (0.5 * data.Origin.w) - finer.Origin.xyz) / finer.Origin.w));
		if (any(childLocal < finer.ValidMinimum.xyz) || any(childLocal >= finer.ValidMaximum.xyz))
			return; // Not wholly inside the finer level: its own light
		int3 childTexel = GetVoxelTexel(level - 1u, childLocal);
		sum += u_FinerRadiance.Load(int4(childTexel, 0)) * CountsChild(coarseNormal, LoadGINormal(level - 1u, childTexel));
	}
	float4 own = o_Radiance[globalID];
	o_Radiance[globalID] = lerp(own, sum * 0.125, weight);
}
