#include "Include/Common.slang"

#include "Include/Voxelize.slang"

// One thread per voxel of the batch's regions: writes the level's textures at the region's texels from the scratch.
// - Material: the key's albedo; occupancy 1 for opaque and interior voxels, ST_GI_MASKED_OCCUPANCY for alpha-masked ones, 0 empty.
// - Emission: the brightest of the voxel's glowing triangles times their coverage together, at most 1.
// - Normal: with triangles, their summed unit normals normalized, w 1; where the sum's length is below ST_GI_TWO_SIDED_SUM times
//   the triangles that added (they face against each other), w -1 (two-sided: both faces of a wall thinner than the voxel; it
//   gets no light rather than let one side's into the other). Also two-sided: triangles whose normals reach at least
//   ST_GI_OPPOSED_COMPONENT both ways along an axis, whatever else adds (the inside and outside of a wall thinner than a coarse
//   level's voxel, with the sunlit ground beside its foot, summed to a normal facing the room and lit it through the wall). A
//   face's triangles count by how many touch the voxel (one near its
//   middle, both on its diagonal), so a plain sum would not cancel. Faces add only to voxels not wholly behind them (WriteVoxel):
//   a voxel faces touch only from behind (inside a box on the grid, under a floor's top) has none, w 0. A double-sided
//   material's w 0.5 (lit from either side). Interior and empty voxels: 0.

[vk::image_format("rgba8")] RWTexture3D<float4> o_Material : register(u0);
[vk::image_format("r32ui")] RWTexture3D<uint> o_Emission : register(u1);
[vk::image_format("rgba8snorm")] RWTexture3D<float4> o_Normal : register(u2);

[numthreads(64, 1, 1)]
void main(uint3 globalID : SV_DispatchThreadID)
{
	uint voxel = globalID.x;
	if (voxel >= u_Pass.Voxels)
		return;
	// The region holding the voxel: the last whose first scratch voxel is at most it.
	uint first = u_Pass.FirstRegion;
	uint count = u_Pass.RegionCount;
	while (count > 1u)
	{
		uint halfCount = count / 2u;
		if (uint(u_Regions[first + halfCount].Maximum.w) <= voxel)
		{
			first += halfCount;
			count -= halfCount;
		}
		else
		{
			count = halfCount;
		}
	}
	VoxelRegion region = u_Regions[first];
	int3 size = region.Maximum.xyz - region.Minimum.xyz;
	int local = int(voxel) - region.Maximum.w;
	int3 position = int3(local % size.x, (local / size.x) % size.y, local / (size.x * size.y));
	int3 texel = region.Texel.xyz + position;

	uint key = u_Scratch.Load(GetScratchAddress(ST_SCRATCH_KEYS, voxel));
	uint voxelClass = key >> 30;
	float occupancy = voxelClass == ST_VOXEL_CLASS_MASKED ? ST_GI_MASKED_OCCUPANCY
		: voxelClass == ST_VOXEL_CLASS_OPAQUE && (key & ST_VOXEL_KEY_SOLID) == 0u ? ST_GI_TOUCH_OCCUPANCY : (voxelClass != 0u ? 1.0 : 0.0);
	float3 albedo = voxelClass == ST_VOXEL_CLASS_INTERIOR ? float3(0.0, 0.0, 0.0) : UnpackUnorm4x8(key & 0xFFFFFFu).rgb;
	float4 normal = float4(0.0, 0.0, 0.0, 0.0);
	if (voxelClass == ST_VOXEL_CLASS_OPAQUE || voxelClass == ST_VOXEL_CLASS_MASKED)
	{
		int3 sum = int3(int(u_Scratch.Load(GetScratchAddress(ST_SCRATCH_NORMALS + 0u, voxel))), int(u_Scratch.Load(GetScratchAddress(ST_SCRATCH_NORMALS + 1u, voxel))),
			int(u_Scratch.Load(GetScratchAddress(ST_SCRATCH_NORMALS + 2u, voxel))));
		float3 summed = float3(sum) / ST_GI_NORMAL_FIXED_POINT;
		float summedLength = sqrt(dot(summed, summed));
		uint added = u_Scratch.Load(GetScratchAddress(ST_SCRATCH_NORMAL_COUNT, voxel));
		float3 spread = float3(u_Scratch.Load(GetScratchAddress(ST_SCRATCH_NORMAL_SPREAD + 0u, voxel)), u_Scratch.Load(GetScratchAddress(ST_SCRATCH_NORMAL_SPREAD + 1u, voxel)),
			u_Scratch.Load(GetScratchAddress(ST_SCRATCH_NORMAL_SPREAD + 2u, voxel))) / ST_GI_NORMAL_FIXED_POINT;
		// Per axis, how far the normals reach the other way: (|n| sum - signed sum) / 2 one way, (|n| sum + signed sum) / 2 the other.
		float3 opposed = min(spread - summed, spread + summed) * 0.5;
		bool opposing = any(opposed >= ST_GI_OPPOSED_COMPONENT);
		if ((key & ST_VOXEL_KEY_DOUBLE_SIDED) != 0u)
			normal = float4(summedLength > 0.0 ? summed / summedLength : float3(0.0, 0.0, 0.0), 0.5);
		else if (added == 0u)
			normal = float4(0.0, 0.0, 0.0, 0.0); // Behind every face that touched it: no surface of its own
		else if (!opposing && summedLength >= ST_GI_TWO_SIDED_SUM * float(added))
			normal = float4(summed / summedLength, 1.0);
		else
			normal = float4(0.0, 0.0, 0.0, -1.0);
	}
	o_Material[texel] = float4(albedo, occupancy);
	uint emission = u_Scratch.Load(GetScratchAddress(ST_SCRATCH_EMISSION, voxel));
	if (emission != 0u)
	{
		float coverage = min(float(u_Scratch.Load(GetScratchAddress(ST_SCRATCH_COVERAGE, voxel))) / ST_GI_COVERAGE_FIXED_POINT, 1.0);
		emission = PackRGB9E5(UnpackRGB9E5(emission) * coverage);
	}
	o_Emission[texel] = emission;
	o_Normal[texel] = normal;
}
