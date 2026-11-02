#include "Include/Common.slang"

#include "Include/DepthOfField.slang"

// Depth of field's dilated tile maxima: per tile, the largest of its 3x3 neighborhood's (x near, y far), so the near field's
// gather reaches every near pixel whose blur spreads over the tile (a tile is at least as wide as the largest blur's radius).

Texture2D<float4> u_TileMax : register(t2);
[vk::image_format("rgba16f")] RWTexture2D<float4> o_Dilated : register(u0);

void st_Main()
{
	int2 tile = int2(st_GlobalInvocationID.xy);
	int2 halfSize = (int2(u_Frame.ViewportSize.xy) + 1) / 2;
	int tileSize = int(u_DepthOfField.TileSize);
	int2 tiles = (halfSize + tileSize - 1) / tileSize;
	if (any((tile >= tiles)))
		return;
	float2 largest = float2(0.0);
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
			largest = max(largest, u_TileMax.Load(int3(clamp(tile + int2(x, y), int2(0), tiles - 1), 0)).xy);
	}
	o_Dilated[tile] = float4(largest, 0.0, 0.0);
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
