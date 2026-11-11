#include "Include/Common.slang"

// One level of the local shadow atlas's depth ranges (u_LocalShadowRanges, Include/AreaShadows.slang) over the frame's drawn
// rect light tiles (SceneRenderer::BuildLocalShadowRanges): each texel the nearest and farthest depth (reversed-Z: the nearest
// to the light is the largest) of 4x4 atlas texels for level 0, or of the 2x2 texels of the level below
// (ST_LOCAL_SHADOW_RANGES_DOWNSAMPLE, registered as Scene/LocalShadowRangesDownsample.comp), packed as
// (ceil(nearest * 65535) << 16) | floor(farthest * 65535): rounded outward, so a cell's range holds every depth in it. A tile
// per z of the dispatch; a level whose cells would exceed an eighth of a tile is not built for it (its lookups never take
// one), so a cell never holds two tiles. The cells of tiles not drawn this frame keep what they hold.

#ifdef ST_LOCAL_SHADOW_RANGES_DOWNSAMPLE
Texture2D<uint4> u_Source : register(t0);
#else
Texture2D<float4> u_Source : register(t0);
#endif
// The tiles: x, y and size in atlas texels.
StructuredBuffer<uint4> u_Tiles : register(t1);
[vk::image_format("r32ui")] RWTexture2D<uint4> o_Destination : register(u0);

struct LocalShadowRangeParameters
{
	uint2 SourceSize;
	uint Level;
	uint TileCount;
	uint CellTexels;
	uint Reserved0;
	uint Reserved1;
	uint Reserved2;
};
[[vk::push_constant]] ConstantBuffer<LocalShadowRangeParameters> u_Ranges;

static const uint c_LocalShadowRangeMinTileShare = 8u; // RenderData::c_LocalShadowRangeMinTileShare

void st_Main()
{
	uint tileIndex = st_GlobalInvocationID.z;
	if (tileIndex >= u_Ranges.TileCount)
		return;
	uint4 tile = u_Tiles[tileIndex];
	if (u_Ranges.CellTexels * c_LocalShadowRangeMinTileShare > tile.z)
		return;
	uint cells = tile.z / u_Ranges.CellTexels;
	if (any((st_GlobalInvocationID.xy >= uint2(cells))))
		return;
	uint2 cell = tile.xy / u_Ranges.CellTexels + st_GlobalInvocationID.xy;
	int2 last = int2(u_Ranges.SourceSize) - 1;
#ifdef ST_LOCAL_SHADOW_RANGES_DOWNSAMPLE
	// The packed ranges combine exactly: the largest nearest and the smallest farthest.
	uint nearest = 0u;
	uint farthest = 0xFFFFu;
	for (int y = 0; y < 2; y++)
	{
		for (int x = 0; x < 2; x++)
		{
			uint packed = TexelFetch(u_Source, min(int2(cell) * 2 + int2(x, y), last), 0).r;
			nearest = max(nearest, packed >> 16u);
			farthest = min(farthest, packed & 0xFFFFu);
		}
	}
#else
	float nearestDepth = 0.0;
	float farthestDepth = 1.0;
	for (int y = 0; y < 4; y++)
	{
		for (int x = 0; x < 4; x++)
		{
			float depth = TexelFetch(u_Source, min(int2(cell) * 4 + int2(x, y), last), 0).r;
			nearestDepth = max(nearestDepth, depth);
			farthestDepth = min(farthestDepth, depth);
		}
	}
	// Rounded outward as Include/AreaShadows.slang reads them back: a product that rounded across a step is moved one step
	// further out.
	nearestDepth = clamp(nearestDepth, 0.0, 1.0);
	farthestDepth = clamp(farthestDepth, 0.0, 1.0);
	uint nearest = uint(ceil(nearestDepth * 65535.0));
	if (nearest < 65535u && float(nearest) / 65535.0 < nearestDepth)
		nearest++;
	uint farthest = uint(floor(farthestDepth * 65535.0));
	if (farthest > 0u && float(farthest) / 65535.0 > farthestDepth)
		farthest--;
#endif
	o_Destination[int2(cell)] = uint4((nearest << 16u) | farthest, 0u, 0u, 0u);
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
