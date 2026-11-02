#include "Include/Common.slang"

#include "Include/MotionBlur.slang"

// The motion blur's tile maxima: one group per tile of TileSize pixels; its threads scan the tile's pixels in turn. The blur
// velocity (GetBlurVelocity) of the largest length goes to the tile's texel (xy) with the diagonal of the box that holds all
// of the tile's blur velocities (z: how far they spread). Every pixel's blur radius and view distance go to the pixel target,
// which the gather reads for its samples (one fetch where velocity and depth would take two, and none of their math).

Texture2D<float4> u_Velocity : register(t0);
Texture2D<float4> u_Depth : register(t1);
[vk::image_format("rgba16f")] RWTexture2D<float4> o_TileMax : register(u0);
[vk::image_format("r32ui")] RWTexture2D<uint4> o_Pixels : register(u1);

groupshared float2 s_Largest[64];
groupshared float4 s_Bounds[64]; // xy the smallest, zw the largest blur velocity's components

void st_Main()
{
	int2 size = TextureSize(u_Depth, 0);
	int2 tile = int2(st_WorkGroupID.xy);
	int tileSize = int(u_MotionBlur.TileSize);
	int2 origin = tile * tileSize;
	float2 largest = float2(0.0);
	float4 bounds = float4(1e30, 1e30, -1e30, -1e30);
	for (int y = int(st_LocalInvocationID.y); y < tileSize; y += 8)
	{
		for (int x = int(st_LocalInvocationID.x); x < tileSize; x += 8)
		{
			int2 pixel = origin + int2(x, y);
			if (any((pixel >= size)))
				continue;
			float2 uv = (float2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
			float depth = u_Depth.Load(int3(pixel, 0)).r;
			float2 velocity = GetBlurVelocity(u_Velocity.Load(int3(pixel, 0)).rg, depth, uv);
			o_Pixels[pixel] = uint4(PackBlurPixel(GetBlurRadius(velocity), GetViewDistance(uv, depth)), 0u, 0u, 0u);
			if (dot(velocity, velocity) > dot(largest, largest))
				largest = velocity;
			bounds = float4(min(bounds.xy, velocity), max(bounds.zw, velocity));
		}
	}
	s_Largest[st_LocalInvocationIndex] = largest;
	s_Bounds[st_LocalInvocationIndex] = bounds;
	GroupMemoryBarrierWithGroupSync();
	for (uint stride = 32u; stride > 0u; stride >>= 1u)
	{
		if (st_LocalInvocationIndex < stride)
		{
			float2 other = s_Largest[st_LocalInvocationIndex + stride];
			if (dot(other, other) > dot(s_Largest[st_LocalInvocationIndex], s_Largest[st_LocalInvocationIndex]))
				s_Largest[st_LocalInvocationIndex] = other;
			float4 otherBounds = s_Bounds[st_LocalInvocationIndex + stride];
			float4 ownBounds = s_Bounds[st_LocalInvocationIndex];
			s_Bounds[st_LocalInvocationIndex] = float4(min(ownBounds.xy, otherBounds.xy), max(ownBounds.zw, otherBounds.zw));
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (st_LocalInvocationIndex == 0u)
	{
		// A tile always holds a pixel (the grid covers the image and no more), so its bounds are set.
		float4 tileBounds = s_Bounds[0];
		o_TileMax[tile] = float4(s_Largest[0], length(tileBounds.zw - tileBounds.xy), 0.0);
	}
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
