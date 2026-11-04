#ifndef STRATA_GLOBAL_ILLUMINATION_SLANG
#define STRATA_GLOBAL_ILLUMINATION_SLANG

// Global illumination's voxel clipmap as shaders read it (Docs/DeveloperGuide.md, "Global illumination"): the levels' windows,
// texels, valid boxes and anchors in the GI's constant buffer, and the weight with which a point reads each level. Layouts
// mirror Strata/Renderer/SceneRenderData.h (RenderData::GlobalIlluminationConstants); the constants mirror its c_GI*.

#include "Include/Common.slang"

#define ST_GI_MAX_VOXEL_LEVELS 4          // c_GIMaxVoxelLevels
#define ST_GI_SCROLL_VOXELS 4.0           // c_GIScrollVoxels
#define ST_GI_LEVEL_GUARD_VOXELS 2.0      // c_GILevelGuardVoxels
#define ST_GI_LEVEL_BLEND_VOXELS 2.0      // c_GILevelBlendVoxels
#define ST_GI_GROUP_TRIANGLES 64u         // c_GIGroupTriangles
#define ST_GI_SMALL_TRIANGLE_VOXELS 64    // c_GISmallTriangleVoxels
#define ST_GI_TILE_VOXELS 8               // c_GITileVoxels
#define ST_GI_MAX_TILES 32768u            // c_GIMaxTiles
#define ST_GI_MASKED_OCCUPANCY 0.15       // c_GIMaskedOccupancy
#define ST_GI_TOUCH_OCCUPANCY 0.05        // c_GITouchOccupancy
#define ST_GI_TWO_SIDED_SUM 0.5           // c_GITwoSidedSum
#define ST_GI_OPPOSED_FACING 0.75        // c_GIOpposedFacing
#define ST_GI_OPPOSED_SHARE 0.25         // c_GIOpposedShare
#define ST_GI_SNAP_VOXELS (1.0 / 256.0)   // c_GISnapVoxels
#define ST_GI_SOLID_INSET_VOXELS (1.0 / 64.0) // c_GISolidInsetVoxels
#define ST_GI_NORMAL_FIXED_POINT 256.0    // c_GINormalFixedPoint
#define ST_GI_STEP_VOXELS 0.75            // c_GIStepVoxels
#define ST_GI_OCCUPANCY_GAIN 2.5          // c_GIOccupancyGain
#define ST_GI_LEAD_IN_OCCUPANCY 0.3       // c_GILeadInOccupancy
#define ST_GI_PROBE_MOVE_VOXELS 1.5       // c_GIProbeMoveVoxels
#define ST_GI_REFRESH_OCCUPANCY 0.1       // c_GIRefreshOccupancy
#define ST_GI_SHADOW_OFFSET_VOXELS 1.0    // c_GIShadowOffsetVoxels
#define ST_GI_SHADOW_TILT_VOXELS 1.5      // c_GIShadowTiltVoxels
#define ST_GI_SUN_OWN_LEVEL_VOXELS 2.0    // c_GISunOwnLevelVoxels
#define ST_GI_TRANSMITTANCE_CUTOFF 1e-4   // c_GITransmittanceCutoff
#define ST_GI_RADIANCE_SCALE (1.0 / 16.0) // c_GIRadianceScale
#define ST_GI_MAX_RADIANCE 60000.0        // c_GIMaxRadiance
#define ST_GI_SKY_LEVEL 5.3               // c_GISkyLevel
#define ST_GI_MAX_MARCH_STEPS 1024u       // c_GIMaxMarchSteps
#define ST_GI_GATHER_NORMAL_BIAS 0.25     // c_GIGatherNormalBias
#define ST_GI_GATHER_START_VOXELS 1.5     // c_GIGatherStartVoxels
#define ST_GI_VISIBILITY_CUTOFF 0.05      // c_GIVisibilityCutoff

// The octahedral direction of texel (u, v) of an a x a tile (GlobalIllumination::GetDirection): OctahedralDecode (Include/Scene.slang)
// of the texel's center mapped to [-1, 1]^2. The same arithmetic, written out so this header needs nothing of Scene.slang.
float3 GetTileDirection(uint2 texel, uint a)
{
	float2 encoded = (float2(texel) + 0.5) / float(a) * 2.0 - 1.0;
	float3 n = float3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
	float t = max(-n.z, 0.0);
	n.x += n.x >= 0.0 ? -t : t;
	n.y += n.y >= 0.0 ? -t : t;
	return normalize(n);
}

// A color packed as shared-exponent RGB9E5 (GlobalIllumination::PackRGB9E5).
float3 UnpackRGB9E5(uint value)
{
	float scale = exp2(float(int(value >> 27)) - 24.0);
	return float3(float(value & 511u), float((value >> 9) & 511u), float((value >> 18) & 511u)) * scale;
}

// A face's axis (+X, -X, +Y, -Y, +Z, -Z: GlobalIllumination::GetFaceAxis).
float3 GetFaceAxis(uint face)
{
	float3 axis = float3(0.0, 0.0, 0.0);
	axis[face / 2u] = (face & 1u) == 0u ? 1.0 : -1.0;
	return axis;
}

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
	float4 FieldOrigin; // xyz: the probe field's minimum corner, camera-relative (m); w: cascade 0's spacing (m)
	uint4 FieldGrid;    // xyz: cascade 0's probes per axis; w: the cascade count
	float4 SolidAngleNorms; // The solid angle of a tile texel is this over |p|^3 (p its octahedron point): x cascade 0's tiles
	float4 FaceWeights[2];  // 1 / C_f per face (+X, -X, +Y, -Y, +Z, -Z) over cascade 0's directions
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
