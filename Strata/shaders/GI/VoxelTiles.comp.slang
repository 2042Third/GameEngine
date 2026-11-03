#include "Include/Common.slang"

#include "Include/Voxelize.slang"

// One group per tile GI/VoxelTriangles.comp listed (dispatched indirectly, at most ST_GI_MAX_TILES): 64 threads, each a column
// of 8 voxels of the tile's 8^3, writing the large triangle into those it overlaps.

[numthreads(64, 1, 1)]
void main(uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	uint2 tile = u_Tiles[groupID.x];
	uint previous;
	if (localIndex == 0u)
		u_Counters.InterlockedAdd(ST_GI_COUNTER_TILES, 1u, previous);
	VoxelTriangle triangle;
	if (!LoadVoxelTriangle(tile.x >> 6, tile.x & 63u, triangle))
		return;
	VoxelRegion region = triangle.Region;
	int3 origin = int3(int(tile.y & 1023u), int((tile.y >> 10) & 1023u), int((tile.y >> 20) & 1023u));
	float3 lowCorner = min(min(triangle.A, triangle.B), triangle.C);
	float3 highCorner = max(max(triangle.A, triangle.B), triangle.C);
	int3 low = max(int3(ceil(lowCorner)) - 1, region.Minimum.xyz);
	int3 high = min(int3(floor(highCorner)), region.Maximum.xyz - 1);
	uint emission = any(triangle.Emission > float3(0.0, 0.0, 0.0)) ? PackRGB9E5(triangle.Emission) : 0u;
	int x = region.Minimum.x + origin.x + int(localIndex % 8u);
	int y = region.Minimum.y + origin.y + int(localIndex / 8u);
	if (x < low.x || x > high.x || y < low.y || y > high.y)
		return;
	for (int layer = 0; layer < ST_GI_TILE_VOXELS; layer++)
	{
		int z = region.Minimum.z + origin.z + layer;
		if (z < low.z || z > high.z)
			continue;
		if (TriangleBoxOverlap(float3(x, y, z) + 0.5, float3(0.5, 0.5, 0.5), triangle.A, triangle.B, triangle.C))
			WriteVoxel(triangle, int3(x, y, z) - region.Minimum.xyz, emission, uint(ST_GI_COVERAGE_FIXED_POINT));
	}
}
