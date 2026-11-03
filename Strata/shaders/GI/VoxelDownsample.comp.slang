#include "Include/Common.slang"

#include "Include/GIProbes.slang"

// A coarser level's light from the finer level inside it (Docs/DeveloperGuide.md, "Global illumination"): a thread per voxel of
// level u_Pass.Level (1 and up, in order), whose eight children in the level below are all valid, takes their mean radiance (rgb
// times the surface weight, and the weight), blended with its own injection by the finer level's weight there (GetLevelWeight:
// continuous in the camera's motion). Each level injects its light with its own geometry, and a coarse level's voxels close small
// openings (a window's sill and lintel at 0.4 m): its sunlit patch was a third of the finer level's. Marches read coarse levels
// for far light; they now see the finer level's light wherever it holds the scene.

Texture3D<float4> u_FinerRadiance : register(t49);
[vk::image_format("rgba16f")] RWTexture3D<float4> o_Radiance : register(u0);

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
	float4 sum = float4(0.0, 0.0, 0.0, 0.0);
	[unroll]
	for (uint child = 0u; child < 8u; child++)
	{
		float3 offset = float3(float(child & 1u), float((child >> 1) & 1u), float((child >> 2) & 1u)) - 0.5;
		int3 childLocal = int3(floor((center + offset * (0.5 * data.Origin.w) - finer.Origin.xyz) / finer.Origin.w));
		if (any(childLocal < finer.ValidMinimum.xyz) || any(childLocal >= finer.ValidMaximum.xyz))
			return; // Not wholly inside the finer level: its own light
		sum += u_FinerRadiance.Load(int4(GetVoxelTexel(level - 1u, childLocal), 0));
	}
	float4 own = o_Radiance[globalID];
	o_Radiance[globalID] = lerp(own, sum * 0.125, weight);
}
