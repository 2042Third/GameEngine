#include "Include/Common.slang"

#include "Include/DepthOfField.slang"

// Depth of field's tile maxima: one group per tile of TileSize half-resolution texels; x the largest near-field circle of
// confusion (half-resolution pixels, as a positive radius), y the largest far one.

Texture2D<float4> u_Half : register(t0);
[vk::image_format("rgba16f")] RWTexture2D<float4> o_TileMax : register(u0);

groupshared float2 s_Largest[64];

void st_Main()
{
	int2 size = TextureSize(u_Half, 0);
	int2 tile = int2(st_WorkGroupID.xy);
	int tileSize = int(u_DepthOfField.TileSize);
	int2 origin = tile * tileSize;
	float2 largest = float2(0.0);
	for (int y = int(st_LocalInvocationID.y); y < tileSize; y += 8)
	{
		for (int x = int(st_LocalInvocationID.x); x < tileSize; x += 8)
		{
			int2 texel = origin + int2(x, y);
			if (any((texel >= size)))
				continue;
			float coc = u_Half.Load(int3(texel, 0)).a;
			largest = max(largest, float2(-coc, coc));
		}
	}
	s_Largest[st_LocalInvocationIndex] = largest;
	GroupMemoryBarrierWithGroupSync();
	for (uint stride = 32u; stride > 0u; stride >>= 1u)
	{
		if (st_LocalInvocationIndex < stride)
			s_Largest[st_LocalInvocationIndex] = max(s_Largest[st_LocalInvocationIndex], s_Largest[st_LocalInvocationIndex + stride]);
		GroupMemoryBarrierWithGroupSync();
	}
	if (st_LocalInvocationIndex == 0u)
		o_TileMax[tile] = float4(s_Largest[0], 0.0, 0.0);
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
