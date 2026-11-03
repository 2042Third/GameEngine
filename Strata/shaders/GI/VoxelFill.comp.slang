#include "Include/Common.slang"

#include "Include/Voxelize.slang"

// One thread per column (x, z) of the batch's regions (radiance-cascades-lab's fill_solid, along +y): sums the closed meshes'
// winding steps upward and gives every voxel inside one (the sum not 0) that no triangle marked the interior class (occupancy 1,
// no albedo, no normal), and those a face marked only from the front the solid bit. An interior voxel has no empty neighbor, so its
// color never shows: it blocks rays and kills probes.

[numthreads(64, 1, 1)]
void main(uint3 globalID : SV_DispatchThreadID)
{
	uint column = globalID.x;
	if (column >= u_Pass.Columns)
		return;
	// The region holding the column: the last whose first column is at most it.
	uint first = u_Pass.FirstRegion;
	uint count = u_Pass.RegionCount;
	while (count > 1u)
	{
		uint halfCount = count / 2u;
		if (uint(u_Regions[first + halfCount].Texel.w) <= column)
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
	int local = int(column) - region.Texel.w;
	int x = local % size.x;
	int z = local / size.x;
	int sum = 0;
	for (int y = 0; y < size.y; y++)
	{
		uint voxel = GetScratchVoxel(region, int3(x, y, z));
		sum += int(u_Scratch.Load(GetScratchAddress(ST_SCRATCH_WINDING, voxel)));
		if (sum == 0)
			continue;
		// Inside a closed mesh: interior where nothing marked it, and solid where a face only touched it from the front (a box's
		// bottom layer on a floor's top).
		uint key = u_Scratch.Load(GetScratchAddress(ST_SCRATCH_KEYS, voxel));
		if ((key >> 30) == 0u)
			u_Scratch.Store(GetScratchAddress(ST_SCRATCH_KEYS, voxel), ST_VOXEL_CLASS_INTERIOR << 30);
		else if ((key & ST_VOXEL_KEY_SOLID) == 0u)
			u_Scratch.Store(GetScratchAddress(ST_SCRATCH_KEYS, voxel), key | ST_VOXEL_KEY_SOLID);
	}
}
