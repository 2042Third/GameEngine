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
#define ST_GI_FIELD_PROBE_MOVE 0.5        // c_GIFieldProbeMove
#define ST_GI_RELOCATION_RINGS 4u         // c_GIRelocationRings
#define ST_GI_RELOCATION_STEP 0.1        // c_GIRelocationStep
#define ST_GI_RELOCATION_RING_COST 0.05  // c_GIRelocationRingCost
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
#define ST_GI_GATHER_VIEW_BIAS 0.3        // c_GIGatherViewBias
#define ST_GI_GATHER_MIN_VISIBLE 0.25     // c_GIGatherMinVisible
#define ST_GI_GATHER_GUARD_STEPS 16u      // c_GIGatherGuardSteps
#define ST_GI_GATHER_GUARD_CORE float2(0.6, 0.72) // c_GIGatherGuardCore
#define ST_GI_GATHER_PUSH_STEPS 2u        // c_GIGatherPushSteps
#define ST_GI_GATHER_PUSH_OCCUPANCY float2(0.1, 0.3) // c_GIGatherPushOccupancy
#define ST_GI_MAX_BOUNCE_ALBEDO 0.9       // c_GIMaxBounceAlbedo
#define ST_GI_FIELD_DEPTH_TEXELS 10       // c_GIFieldDepthTexels
#define ST_GI_FIELD_DIRECTIONS 64u        // c_GIFieldDirections
#define ST_GI_FIELD_DEPTH_SHARPNESS 8.0   // c_GIFieldDepthSharpness (FieldReduce.comp squares three times: change both)

// A probe's move off the surface it sits on (ProbeValidity.comp), stored in its validity texel's gba (RGBA8) as 128 + round(127 o)
// over 255, o the move over half a spacing (so no move is stored exactly), and read back in meters.
float3 EncodeProbeMove(float3 move, float spacing)
{
	return (128.0 + round(127.0 * clamp(move / (0.5 * spacing), -1.0, 1.0))) / 255.0;
}

float3 DecodeProbeMove(float4 validity, float spacing)
{
	return (round(validity.gba * 255.0) - 128.0) / 127.0 * (0.5 * spacing);
}

// The octahedral direction of a point of [-1, 1]^2: the engine's OctahedralDecode (Include/Scene.slang), written out so this header
// needs nothing of Scene.slang.
float3 DecodeOctahedral(float2 encoded)
{
	float3 n = float3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
	float t = max(-n.z, 0.0);
	n.x += n.x >= 0.0 ? -t : t;
	n.y += n.y >= 0.0 ? -t : t;
	return normalize(n);
}

// A direction's point of [-1, 1]^2: the engine's OctahedralEncode (Include/Scene.slang).
float2 EncodeOctahedral(float3 n)
{
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
}

// The octahedral direction of texel (u, v) of an a x a tile (GlobalIllumination::GetDirection): DecodeOctahedral of the texel's
// center mapped to [-1, 1]^2.
float3 GetTileDirection(uint2 texel, uint a)
{
	return DecodeOctahedral((float2(texel) + 0.5) / float(a) * 2.0 - 1.0);
}

// The interior texel a texel of a probe's depth tile holds (ST_GI_FIELD_DEPTH_TEXELS a side, a one-texel border): itself inside;
// on the border the interior texel across the octahedron's edge (an edge's two halves meet mirrored, the corners all at -z), so
// bilinear reads across the border are seamless (DDGI's border).
uint2 GetDepthTileSource(uint2 texel)
{
	uint last = ST_GI_FIELD_DEPTH_TEXELS - 1u;
	bool column = texel.x == 0u || texel.x == last;
	bool row = texel.y == 0u || texel.y == last;
	if (column && row)
		return uint2(texel.x == 0u ? last - 1u : 1u, texel.y == 0u ? last - 1u : 1u);
	if (column)
		return uint2(texel.x == 0u ? 1u : last - 1u, last - texel.y);
	if (row)
		return uint2(last - texel.x, texel.y == 0u ? 1u : last - 1u);
	return texel;
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
	float4 Anchor;      // xyz: the anchor voxel's world position (the CPU's double as a float): regions and voxel coordinates start there;
	                    // w: meters the window's center lies below the camera (VoxelLevelLayout::Drop)
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
	float4 Bounce;          // x: the bounce's gain (0: one bounce); yzw: this frame's camera minus last frame's (VoxelInject)
};

ConstantBuffer<GlobalIlluminationConstants> u_GI : register(b1);

// GetLevelWeight (below) for reads up to `reach` meters from the point on every axis (a probe's search for open space): the same
// blend moved inward by the reach, so wherever it is above 0 every read lies inside the level's valid box less its guard (windows
// snap down to steps of ST_GI_SCROLL_VOXELS, so on an axis's positive side the blend's own margin is all the room there is).
float GetLevelWeightWithin(uint level, float3 position, float reach)
{
	GIVoxelLevel data = u_GI.Levels[level];
	if (data.Texel.w == 0)
		return 0.0;
	float voxel = data.Origin.w;
	float inset = ST_GI_LEVEL_GUARD_VOXELS + reach / voxel;
	float3 local = (position - data.Origin.xyz) / voxel;
	if (any(local < float3(data.ValidMinimum.xyz) + inset) || any(local > float3(data.ValidMaximum.xyz) - inset))
		return 0.0;
	float3 margin = float3(u_GI.Dimensions.xyz) * (0.5 * voxel) - (ST_GI_SCROLL_VOXELS + ST_GI_LEVEL_GUARD_VOXELS) * voxel - reach - abs(position + float3(0.0, data.Anchor.w, 0.0));
	return saturate(min(margin.x, min(margin.y, margin.z)) / (ST_GI_LEVEL_BLEND_VOXELS * voxel));
}

// The weight with which a camera-relative point reads `level` rather than the coarser ones (GlobalIllumination::GetLevelWeight):
// 0 outside its valid box less c_GILevelGuardVoxels (trilinear filtering reads a voxel beyond), else by the distance from the
// camera: 1 within the window's half extent less (scroll + guard + blend) voxels on every axis around the window's center (the
// camera, the level's Anchor.w below it: c_GIFinestLevelDrop for level 0), falling to 0 over the blend.
float GetLevelWeight(uint level, float3 position)
{
	GIVoxelLevel data = u_GI.Levels[level];
	if (data.Texel.w == 0)
		return 0.0;
	float voxel = data.Origin.w;
	float3 local = (position - data.Origin.xyz) / voxel;
	if (any(local < float3(data.ValidMinimum.xyz) + ST_GI_LEVEL_GUARD_VOXELS) || any(local > float3(data.ValidMaximum.xyz) - ST_GI_LEVEL_GUARD_VOXELS))
		return 0.0;
	float3 margin = float3(u_GI.Dimensions.xyz) * (0.5 * voxel) - (ST_GI_SCROLL_VOXELS + ST_GI_LEVEL_GUARD_VOXELS) * voxel - abs(position + float3(0.0, data.Anchor.w, 0.0));
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
