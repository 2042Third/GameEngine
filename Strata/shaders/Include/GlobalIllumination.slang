#ifndef STRATA_GLOBAL_ILLUMINATION_SLANG
#define STRATA_GLOBAL_ILLUMINATION_SLANG

// Global illumination's voxel clipmap as shaders read it (Docs/DeveloperGuide.md, "Global illumination"): the levels' windows,
// texels, valid boxes and anchors in the GI's constant buffer, and the weight with which a point reads each level. Layouts
// mirror Strata/Renderer/SceneRenderData.h (RenderData::GlobalIlluminationConstants); the constants mirror its c_GI*.

#include "Include/Common.slang"

#define ST_GI_MAX_VOXEL_LEVELS 4          // c_GIMaxVoxelLevels
#define ST_GI_SCROLL_VOXELS 4.0           // c_GIScrollVoxels
#define ST_GI_LEVEL_GUARD_VOXELS 2.0      // c_GILevelGuardVoxels
#define ST_GI_LEVEL_BLEND_VOXELS 8.0      // c_GILevelBlendVoxels
#define ST_GI_GROUP_TRIANGLES 64u         // c_GIGroupTriangles
#define ST_GI_SMALL_TRIANGLE_VOXELS 64    // c_GISmallTriangleVoxels
#define ST_GI_TILE_VOXELS 8               // c_GITileVoxels
#define ST_GI_MAX_TILES 32768u            // c_GIMaxTiles
#define ST_GI_MASKED_OCCUPANCY 0.15       // c_GIMaskedOccupancy
#define ST_GI_TWO_SIDED_SUM 0.5           // c_GITwoSidedSum
#define ST_GI_NORMAL_FIXED_POINT 256.0    // c_GINormalFixedPoint
#define ST_GI_STEP_VOXELS 0.75            // c_GIStepVoxels

// VoxelWorkItem::Flags (c_VoxelItem*)
#define ST_VOXEL_ITEM_CLOSED 1u
#define ST_VOXEL_ITEM_ALPHA_MASKED 2u
#define ST_VOXEL_ITEM_DOUBLE_SIDED 4u
#define ST_VOXEL_ITEM_MIRRORED 8u
#define ST_VOXEL_ITEM_FILL_ONLY 16u

struct GIVoxelLevel
{
	float4 Origin;      // xyz: the window's minimum corner, camera-relative (m); w: the voxel size (m)
	int4 Texel;         // xyz: the texel of the window's minimum corner; w: 1 when the level holds valid voxels
	int4 ValidMinimum;  // xyz: the valid box's first voxel, relative to the window's minimum corner
	int4 ValidMaximum;  // xyz: one past its last voxel
	float4 Anchor;      // xyz: the anchor voxel's world position (the CPU's double as a float): regions and voxel coordinates start there
};

struct GlobalIlluminationConstants
{
	GIVoxelLevel Levels[ST_GI_MAX_VOXEL_LEVELS];
	uint4 Dimensions;   // xyz: voxels per axis of every level; w: the level count
	uint4 Batch;        // x: the voxels a batch's scratch holds (GlobalIlluminationLayout::BatchVoxels)
};

ConstantBuffer<GlobalIlluminationConstants> u_GI : register(b1);

// The weight with which a camera-relative point reads `level` rather than the coarser ones (GlobalIllumination::GetLevelWeight):
// 0 outside its valid box less c_GILevelGuardVoxels (trilinear filtering reads a voxel beyond), else by the distance from the
// camera: 1 within the window's half extent less (scroll + guard + blend) voxels on every axis, falling to 0 over the blend.
float GetLevelWeight(uint level, float3 position)
{
	GIVoxelLevel data = u_GI.Levels[level];
	if (data.Texel.w == 0)
		return 0.0;
	float voxel = data.Origin.w;
	float3 local = (position - data.Origin.xyz) / voxel;
	if (any(local < float3(data.ValidMinimum.xyz) + ST_GI_LEVEL_GUARD_VOXELS) || any(local > float3(data.ValidMaximum.xyz) - ST_GI_LEVEL_GUARD_VOXELS))
		return 0.0;
	float3 margin = float3(u_GI.Dimensions.xyz) * (0.5 * voxel) - (ST_GI_SCROLL_VOXELS + ST_GI_LEVEL_GUARD_VOXELS) * voxel - abs(position);
	return saturate(min(margin.x, min(margin.y, margin.z)) / (ST_GI_LEVEL_BLEND_VOXELS * voxel));
}

// Where a camera-relative point is in a level's textures, for SampleLevel with a repeating sampler (texel i's center at
// (i + 0.5) / dimensions; the window is stored toroidally).
float3 GetVoxelTextureCoordinate(uint level, float3 position)
{
	GIVoxelLevel data = u_GI.Levels[level];
	return ((position - data.Origin.xyz) / data.Origin.w + float3(data.Texel.xyz)) / float3(u_GI.Dimensions.xyz);
}

// The texel of a level's voxel at window-local voxel index `local` (toroidal).
int3 GetVoxelTexel(uint level, int3 local)
{
	int3 dimensions = int3(u_GI.Dimensions.xyz);
	return ((local + u_GI.Levels[level].Texel.xyz) % dimensions + dimensions) % dimensions;
}

#endif
