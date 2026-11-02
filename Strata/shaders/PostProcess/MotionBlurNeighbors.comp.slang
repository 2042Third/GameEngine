#include "Include/Common.slang"

#include "Include/MotionBlur.slang"

// The motion blur's neighbor maxima: per tile, the largest tile maximum of its 3x3 neighborhood (xy), so a pixel's gather reaches
// as far as anything that can blur over it. A diagonal neighbor counts only when its blur runs toward the center tile (within 45
// degrees of the diagonal, either way: the blur spreads both ways along it); the edge neighbors always reach. z bounds how far
// any blur of the neighborhood, diagonals included, lies from that largest one: the gather's samples stay within it (they
// reach at most half the longest blur, and a tile is at least that wide), and a small z lets them read only colors.

Texture2D<float4> u_TileMax : register(t2);
[vk::image_format("rgba16f")] RWTexture2D<float4> o_NeighborMax : register(u0);

void st_Main()
{
	int2 tile = int2(st_GlobalInvocationID.xy);
	int2 depthSize = int2(u_Frame.ViewportSize.xy);
	int tileSize = int(u_MotionBlur.TileSize);
	int2 tiles = (depthSize + tileSize - 1) / tileSize;
	if (any((tile >= tiles)))
		return;
	float3 neighborhood[9];
	float2 largest = float2(0.0);
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			int2 neighbor = clamp(tile + int2(x, y), int2(0), tiles - 1);
			float3 tileMax = u_TileMax.Load(int3(neighbor, 0)).xyz;
			neighborhood[(y + 1) * 3 + (x + 1)] = tileMax;
			if (any(neighbor != tile + int2(x, y)))
				continue;
			float2 velocity = tileMax.xy;
			float speed = length(velocity);
			if (x != 0 && y != 0 && speed > 0.0 && abs(dot(velocity / speed, float2(x, y) * 0.70710678)) < 0.70710678)
				continue;
			if (dot(velocity, velocity) > dot(largest, largest))
				largest = velocity;
		}
	}
	// Every blur v of a tile lies in the box of the tile's blurs, as its largest does: |v - largest| is at most the box's
	// diagonal plus the distance of the tile's largest from the neighborhood's.
	float spread = 0.0;
	for (int index = 0; index < 9; index++)
		spread = max(spread, neighborhood[index].z + length(neighborhood[index].xy - largest));
	o_NeighborMax[tile] = float4(largest, spread, 0.0);
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
