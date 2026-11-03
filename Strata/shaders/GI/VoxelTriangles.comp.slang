#include "Include/Common.slang"

#include "Include/Voxelize.slang"

// One thread per triangle of the batch's triangle groups (a group of 64 threads per VoxelBatch record): its winding steps for
// closed meshes (radiance-cascades-lab's winding_steps, turned to columns along +y), then its voxels in its region. A small
// triangle (its bounds hold at most ST_GI_SMALL_TRIANGLE_VOXELS voxels) writes them itself; a large one lists
// the 8^3 tiles of its bounds it overlaps for GI/VoxelTiles.comp (one 220 m road triangle would keep one thread busy for
// milliseconds), or walks them itself when the list is full.

// Edge function of p against the edge a -> b in the (x, z) plane.
float EdgeFunction(float2 a, float2 b, float2 p)
{
	return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

// Top-left rule: of two triangles sharing an edge, exactly one claims a point on it.
bool Owns(float2 a, float2 b, float edge)
{
	float2 d = b - a;
	return edge > 0.0 || (edge == 0.0 && (d.y > 0.0 || (d.y == 0.0 && d.x < 0.0)));
}

// For a closed mesh: every column of the region (voxel centers along +y) whose line crosses the triangle gets +1 where the line
// enters the mesh and -1 where it leaves, at the first voxel whose center is above the crossing (the region's first layer
// for crossings below it, nothing for crossings above). GI/VoxelFill.comp sums them upward.
void AddWindingSteps(VoxelTriangle triangle)
{
	float3 a = triangle.A;
	float3 b = triangle.B;
	float3 c = triangle.C;
	float facing = cross(b - a, c - a).y;
	if ((triangle.Flags & ST_VOXEL_ITEM_MIRRORED) != 0u)
		facing = -facing;
	if (abs(facing) < 1e-9)
		return; // Parallel to y: the lines never cross it
	int stepValue = facing < 0.0 ? 1 : -1; // Facing down: going up, the line enters here
	float3 tb = b;
	float3 tc = c;
	if (EdgeFunction(a.xz, b.xz, c.xz) < 0.0)
	{
		tb = c;
		tc = b;
	}
	float2 pa = a.xz;
	float2 pb = tb.xz;
	float2 pc = tc.xz;
	float area = EdgeFunction(pa, pb, pc);
	// The lines are moved off the half-voxel grid a little, so no mesh edge lies on one.
	static const float2 c_Offset = float2(0.5 + 0.00137, 0.5 + 0.00291);
	VoxelRegion region = triangle.Region;
	int2 low = max(int2(floor(min(min(pa, pb), pc) - c_Offset)) + 1, region.Minimum.xz);
	int2 high = min(int2(floor(max(max(pa, pb), pc) - c_Offset)), region.Maximum.xz - 1);
	int height = region.Maximum.y - region.Minimum.y;
	uint previous;
	for (int z = low.y; z <= high.y; z++)
	{
		for (int x = low.x; x <= high.x; x++)
		{
			float2 p = float2(float(x), float(z)) + c_Offset;
			float w0 = EdgeFunction(pb, pc, p);
			float w1 = EdgeFunction(pc, pa, p);
			float w2 = EdgeFunction(pa, pb, p);
			if (!(Owns(pb, pc, w0) && Owns(pc, pa, w1) && Owns(pa, pb, w2)))
				continue;
			float y = (w0 * a.y + w1 * tb.y + w2 * tc.y) / area;
			int layer = max(int(floor(y - 0.5)) + 1 - region.Minimum.y, 0);
			if (layer < height)
				u_Scratch.InterlockedAdd(GetScratchAddress(ST_SCRATCH_WINDING, GetScratchVoxel(region, int3(x, layer, z) - int3(region.Minimum.x, 0, region.Minimum.z))),
					uint(stepValue), previous);
		}
	}
}

// The share of each voxel's face an emissive small triangle covers (radiance-cascades-lab: an 18 cm lens in 25 cm voxels glowed
// like one twice its size): its area in voxel faces over the voxels it touches in one layer. The voxels add their triangles'
// shares, so a glowing surface's voxels in a layer sum to its area whatever its triangles and its place on the grid.
float GetEmissionCoverage(VoxelTriangle triangle, int3 low, int3 high)
{
	float3 normal = cross(triangle.B - triangle.A, triangle.C - triangle.A);
	float area = 0.5 * sqrt(dot(normal, normal));
	float3 facing = abs(normal);
	int axis = facing.x > facing.y ? (facing.x > facing.z ? 0 : 2) : (facing.y > facing.z ? 1 : 2);
	float layers = float(high[axis] - low[axis] + 1);
	float touched = 0.0;
	for (int z = low.z; z <= high.z; z++)
	{
		for (int y = low.y; y <= high.y; y++)
		{
			for (int x = low.x; x <= high.x; x++)
				touched += TriangleBoxOverlap(float3(x, y, z) + 0.5, float3(0.5, 0.5, 0.5), triangle.A, triangle.B, triangle.C) ? 1.0 : 0.0;
		}
	}
	return area * layers / max(touched, 1.0);
}

[numthreads(64, 1, 1)]
void main(uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	if (groupID.x >= u_Pass.GroupCount)
		return;
	uint group = u_Pass.FirstGroup + groupID.x;
	VoxelTriangle triangle;
	if (!LoadVoxelTriangle(group, localIndex, triangle))
		return;
	if ((triangle.Flags & ST_VOXEL_ITEM_CLOSED) != 0u)
		AddWindingSteps(triangle);
	if ((triangle.Flags & ST_VOXEL_ITEM_FILL_ONLY) != 0u)
		return;

	// The voxels of its bounds within the region (anchor-relative).
	VoxelRegion region = triangle.Region;
	float3 lowCorner = min(min(triangle.A, triangle.B), triangle.C);
	float3 highCorner = max(max(triangle.A, triangle.B), triangle.C);
	if (any(highCorner < float3(region.Minimum.xyz)) || any(lowCorner > float3(region.Maximum.xyz)))
		return;
	// Touching counts: a face on a voxel boundary marks the voxels on both sides. Whether a triangle is small, and its coverage,
	// follow its whole bounds, never the region's part of them: a voxel is the same however the window is cut into regions.
	int3 fullLow = int3(ceil(lowCorner)) - 1;
	int3 fullHigh = int3(floor(highCorner));
	int3 low = max(fullLow, region.Minimum.xyz);
	int3 high = min(fullHigh, region.Maximum.xyz - 1);
	if (any(high < low))
		return;
	int3 fullCount = fullHigh - fullLow + 1;
	uint emission = any(triangle.Emission > float3(0.0, 0.0, 0.0)) ? PackRGB9E5(triangle.Emission) : 0u;
	// Each axis first: the product of a large triangle's counts (1,300 voxels an axis, 130 m at level 0) passes 2^31.
	if (all(fullCount <= int3(ST_GI_SMALL_TRIANGLE_VOXELS, ST_GI_SMALL_TRIANGLE_VOXELS, ST_GI_SMALL_TRIANGLE_VOXELS))
		&& fullCount.x * fullCount.y * fullCount.z <= ST_GI_SMALL_TRIANGLE_VOXELS)
	{
		uint coverage = emission != 0u ? uint(round(GetEmissionCoverage(triangle, fullLow, fullHigh) * ST_GI_COVERAGE_FIXED_POINT)) : 0u;
		for (int z = low.z; z <= high.z; z++)
		{
			for (int y = low.y; y <= high.y; y++)
			{
				for (int x = low.x; x <= high.x; x++)
				{
					if (TriangleBoxOverlap(float3(x, y, z) + 0.5, float3(0.5, 0.5, 0.5), triangle.A, triangle.B, triangle.C))
						WriteVoxel(triangle, int3(x, y, z) - region.Minimum.xyz, emission, coverage);
				}
			}
		}
		return;
	}

	// Large: the tiles (8^3, from the region's minimum) of its bounds that it overlaps. A large glowing triangle covers the voxels it
	// touches (its edges' are a small share of them).
	const uint coverage = uint(ST_GI_COVERAGE_FIXED_POINT);
	int3 firstTile = (low - region.Minimum.xyz) / ST_GI_TILE_VOXELS;
	int3 lastTile = (high - region.Minimum.xyz) / ST_GI_TILE_VOXELS;
	uint previous;
	for (int tz = firstTile.z; tz <= lastTile.z; tz++)
	{
		for (int ty = firstTile.y; ty <= lastTile.y; ty++)
		{
			for (int tx = firstTile.x; tx <= lastTile.x; tx++)
			{
				int3 origin = int3(tx, ty, tz) * ST_GI_TILE_VOXELS;
				float3 center = float3(region.Minimum.xyz + origin) + 0.5 * ST_GI_TILE_VOXELS;
				if (!TriangleBoxOverlap(center, float3(0.5, 0.5, 0.5) * ST_GI_TILE_VOXELS, triangle.A, triangle.B, triangle.C))
					continue;
				uint slot;
				u_TileArguments.InterlockedAdd(0u, 1u, slot);
				if (slot < ST_GI_MAX_TILES)
				{
					u_Tiles[slot] = uint2((group << 6) | localIndex, uint(origin.x) | (uint(origin.y) << 10) | (uint(origin.z) << 20));
					continue;
				}
				// The list is full: take the slot back (the count ends at ST_GI_MAX_TILES) and walk the tile here, slowly but
				// completely, so no region is left with triangles missing.
				u_TileArguments.InterlockedAdd(0u, 0xFFFFFFFFu, previous);
				u_Counters.InterlockedAdd(ST_GI_COUNTER_TILE_OVERFLOW, 1u, previous);
				int3 tileLow = max(region.Minimum.xyz + origin, low);
				int3 tileHigh = min(region.Minimum.xyz + origin + ST_GI_TILE_VOXELS - 1, high);
				for (int z = tileLow.z; z <= tileHigh.z; z++)
				{
					for (int y = tileLow.y; y <= tileHigh.y; y++)
					{
						for (int x = tileLow.x; x <= tileHigh.x; x++)
						{
							if (TriangleBoxOverlap(float3(x, y, z) + 0.5, float3(0.5, 0.5, 0.5), triangle.A, triangle.B, triangle.C))
								WriteVoxel(triangle, int3(x, y, z) - region.Minimum.xyz, emission, coverage);
						}
					}
				}
			}
		}
	}
}
