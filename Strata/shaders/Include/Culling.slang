#ifndef STRATA_CULLING_SLANG
#define STRATA_CULLING_SLANG

// GPU culling's resources (Scene/Cull.comp, Scene/CullBuild.comp). Layouts mirror Strata/Renderer/GPUSceneData.h and
// SceneRenderData.h; keep them in sync.

#include "Include/Common.slang"
#include "Include/GeometryTables.slang"
#include "Include/OrderedFloat.slang"

// RenderProxyFlags
#define ST_PROXY_ACTIVE       1u
#define ST_PROXY_CAST_SHADOWS 2u
#define ST_PROXY_MIRRORED     4u
#define ST_PROXY_INDEXED      8u
#define ST_PROXY_MESH_BOUNDS  16u
#define ST_PROXY_SKINNED      64u

#define ST_CULL_MESH_SKINNED_ARENA     0x80000000u
#define ST_CULL_MATERIAL_AVAILABLE     1u
#define ST_CULL_MATERIAL_DOUBLE_SIDED  2u
#define ST_CULL_MATERIAL_ALPHA_MASK    4u
#define ST_CULL_MATERIAL_TRANSPARENT   8u
#define ST_CULL_MATERIAL_TEXTURED      16u

// GPUProxy::Flags: RenderProxyFlags below this shift, the override material slot plus one above it.
#define ST_PROXY_OVERRIDE_SHIFT        8u

// Visibility feedback (RenderData::c_Feedback*, FeedbackRecord).
#define ST_FEEDBACK_VIEW               1u
#define ST_FEEDBACK_HIGH               2u
#define ST_FEEDBACK_PREFETCH           4u
#define ST_FEEDBACK_SHADOW             8u
#define ST_FEEDBACK_LIST_HEADER        4u

#define ST_CULL_DRAW_ENTRY_INSTANCE_BITS 27u // RenderData::c_DrawEntryInstanceBits
#define ST_CULL_GEOMETRY_STATIC        0u
#define ST_CULL_GEOMETRY_SKINNED_ARENA 1u
#define ST_CULL_GEOMETRY_SKINNED       2u
#define ST_CULL_VIEW_BUCKETS           32u
#define ST_CULL_SECOND_PHASE_BUCKET    16u  // MakeViewBucket's second-phase bit
#define ST_CULL_SHADOW_BUCKETS         64u
#define ST_CULL_SHADOW_ALPHA_MASK_BUCKET 16u // MakeShadowBucket's alpha-masked bit
#define ST_CULL_SHADOW_SECOND_PHASE_BUCKET 32u // MakeShadowBucket's second-phase bit
#define ST_CULL_FIRST_LOCAL_BUCKET     288u // c_CullFirstLocalShadowBucket
#define ST_CULL_LOCAL_BUCKETS          32u  // c_CullLocalShadowBucketsPerView
#define ST_CULL_MAX_LOCAL_VIEWS        12u  // c_CullMaxLocalShadowViews
#define ST_CULL_BUCKETS                672u // c_CullBuckets: the bucket table's bucket -> dense index part
#define ST_CULL_MAX_CASCADES           4u
#define ST_VISIBILITY_CASCADE_SHIFT    2u   // RenderData::c_VisibilityCascadeShift
#define ST_CULL_NONE                   0xFFFFFFFFu

// CullCounters, by index
#define ST_COUNTER_VIEW_INSTANCES     0
#define ST_COUNTER_SHADOW_INSTANCES   1
#define ST_COUNTER_VIEW_DRAWS         2
#define ST_COUNTER_SHADOW_DRAWS       3
#define ST_COUNTER_VISIBLE_PROXIES    4
#define ST_COUNTER_SUB_TEXEL_CASTERS  5
#define ST_COUNTER_DRAW_LIST_USED     6
#define ST_COUNTER_OVERFLOW           7
#define ST_COUNTER_TRIANGLES_LOW      8
#define ST_COUNTER_TRIANGLES_HIGH     9
#define ST_COUNTER_SKINNED_INSTANCES  10
#define ST_COUNTER_SIZE_CULLED        11
#define ST_COUNTER_INSTANCES_PER_LOD  12
#define ST_LOD_STAT_COUNT             8
#define ST_COUNTER_OCCLUDED           20
#define ST_COUNTER_OCCLUDED_CASTERS   23
#define ST_COUNTER_SECOND_PHASE_SHADOW_DRAWS 24
#define ST_COUNTER_LOCAL_SHADOW_INSTANCES 25
#define ST_COUNTER_LOCAL_SHADOW_DRAWS 26

struct GPUProxy
{
	float3 LocalMin;
	uint FirstInstance;
	float3 LocalMax;
	uint InstanceCount;
	float3 WorldMin;
	uint MeshSlot;
	float3 WorldMax;
	uint Flags;
};

struct CullInstance
{
	float4 WorldRows[3];
	uint MaterialIndex;
	uint EntityID;
	uint Flags;
	uint SkinPalette;
};

struct CullConstants
{
	float4 Planes[6];          // The view frustum in camera-relative space
	float4 CameraPosition;     // xyz: world position; w: 1 for orthographic cameras
	float4 LODParameters;      // x: LOD threshold, y: tan(fov / 2) or the vertical extent, z: near plane, w: smallest projected pixels
	float4 ViewParameters;     // x: viewport height in pixels, y: texture streaming's near distance, z: prefetch distance, w: high priority fraction
	float4x4 LightView;
	float4x4 ViewProjection;     // The view's (camera-relative), for the occlusion test's screen rectangles
	float4 OcclusionParameters; // xy: depth buffer size in pixels, z: depth pyramid levels, w: 1 when occlusion culled
	float4 CascadeBoxes[4];    // Light view space: min.xy, max.xy
	float4 CascadeParameters[4]; // x: far side z, y: smallest light-space extent drawn
	uint ProxyCount;
	uint CascadeCount;
	uint LODCapacity;
	uint MeshSlotCount;
	uint MaterialSlotCount;
	uint DrawListCapacity;
	uint ArgumentsPerBucket;
	uint BucketCount;
	uint ViewBucketCount;    // Dense buckets of the view (they come first)
	uint DrawsBounded;       // 1: the draws read only a bucket's first slots (u_BucketTable[ST_CULL_BUCKETS + dense])
	uint Feedback;           // 1: the counting passes write visibility feedback
	uint FirstPhaseViewBuckets; // Of the view's dense buckets, the first phase's (they come first)
	uint VisibilityParity;   // The bit of u_Visibility holding the previous frame's visibility (0 or 1)
	float LODErrorThreshold; // LODSelection::GetErrorThreshold of the view
	uint LODFadeFrame;       // The frame's number for level-of-detail cross-fades
	uint LODFade;            // 1: levels of detail cross-fade
	uint ShadowOcclusion;    // 1: the cascades are occlusion culled
	uint SecondPhaseShadowBuckets; // The dense index of the cascades' first second-phase bucket
	uint ShadowMapSize;      // Each cascade's texels each way
	uint ShadowPyramidLevels;
	uint CachedCascades;     // The cascades that keep their layer: nothing is drawn into them
	uint FirstLocalBucket;   // The dense index of the first local shadow view bucket (they come last)
	uint Reserved1;
	uint Reserved2;
};

// A point or spot light's shadow view (RenderData::CullLocalShadowView).
struct CullLocalShadowView
{
	float4 Planes[6];      // Its frustum, camera-relative, inward
	float4 Light;          // xyz: the light's world position, w: its range
	float4 RelativeLight;  // xyz: the light's camera-relative position, w: the near plane
	float4 Parameters;     // x: tan(half its field of view), y: its tile's texels, z: the level-of-detail error threshold of its tile
};

struct CullCascade
{
	float4x4 ViewProjection;
	float4 Parameters;         // RenderData::CascadeData: w is the near plane's light view z (camera-relative)
};

ConstantBuffer<CullConstants> u_Cull : register(b0);

StructuredBuffer<GPUProxy> u_Proxies : register(t0);
StructuredBuffer<CullInstance> u_CullInstances : register(t1);
StructuredBuffer<uint> u_MeshTable : register(t2);
StructuredBuffer<uint> u_MaterialFlags : register(t3);
StructuredBuffer<GeometryEntry> u_GeometryEntries : register(t4);
StructuredBuffer<GeometrySubmesh> u_GeometrySubmeshes : register(t5);
StructuredBuffer<GeometryLOD> u_GeometryLODs : register(t6);
// Per bucket its dense index, then per dense bucket its draw slots.
StructuredBuffer<uint> u_BucketTable : register(t7);

// Instances per (dense bucket, LOD record), then their draw list offsets; the draws per dense bucket; the counters; the
// draws' arguments (DrawIndexedIndirectArguments, five words each) and the draw list itself.
RWStructuredBuffer<uint> u_Counts : register(u0);
RWStructuredBuffer<uint> u_Offsets : register(u1);
RWStructuredBuffer<uint> u_Arguments : register(u2);
RWStructuredBuffer<uint> u_BucketCounts : register(u3);
RWStructuredBuffer<uint> u_Counters : register(u4);
RWStructuredBuffer<uint> u_CullDrawList : register(u5);
// Visibility feedback: per mesh slot (pixels, use) and per material slot (pixels, UV pixels, use, unused), as float bits where
// they are sizes (non-negative floats order like their bits); the touched slots' lists (ST_FEEDBACK_LIST_HEADER words, then
// MeshSlotCount mesh slots, then the material slots); the packed records (Scene/CullFeedback.comp).
RWStructuredBuffer<uint> u_MeshFeedback : register(u6);
RWStructuredBuffer<uint> u_MaterialFeedback : register(u7);
RWStructuredBuffer<uint> u_FeedbackLists : register(u8);
RWStructuredBuffer<uint4> u_FeedbackRecords : register(u9);
// Per cascade the nearest drawn caster's light view z (EncodeOrderedFloat; 0: none), for Scene/CascadeFit.comp.
RWStructuredBuffer<uint> u_CasterDepths : register(u10);
// Per proxy whether the view saw it, in two bits that alternate frame by frame: the previous frame's
// (u_Cull.VisibilityParity) and this frame's, written by the second phase; and likewise per cascade whether the proxy
// cast into it, in bits ST_VISIBILITY_CASCADE_SHIFT + ST_CULL_MAX_CASCADES * parity + cascade.
RWStructuredBuffer<uint> u_Visibility : register(u11);
// Per instance its level-of-detail cross-fade state (UpdateLODFade, LODSelection::LODFadeState), written by the view's
// writing passes.
RWStructuredBuffer<uint2> u_LODFades : register(u12);
// The depth pyramid of the first phase's depth (Scene/HZBBuild.comp): each texel the farthest (smallest, reversed-Z)
// depth of the pixels it covers; level m's texel covers pixels p with p >> (m + 1) equal to it, its last row and column
// also those past them (GetPyramidFootprint).
Texture2D<float4> u_DepthPyramid : register(t8);
// Per cascade (a layer each) the depth pyramid of its first phase's shadow map: level m's texel covers texels t with
// t >> (m + 2) equal to it, its last row and column also those past them.
Texture2DArray<float4> u_ShadowPyramid : register(t9);
// The cascades' projections (Scene/CascadeFit.comp), fitted after the counting pass.
StructuredBuffer<CullCascade> u_CullCascades : register(t10);
// The frame's local shadow views (those it draws), in the order of their batches.
StructuredBuffer<CullLocalShadowView> u_LocalShadowViews : register(t11);

#define ST_LOD_RELATIVE_ERROR(record) u_GeometryLODs[record].RelativeError
#include "Include/LODSelection.slang"

#define ST_GEOMETRY_SUBMESH_LOD_ERRORS 1u // RenderData::c_GeometrySubmeshLODErrors

// The level of detail drawn of a submesh at a screen fraction (LODSelection::SelectLOD) for a view with this error threshold
// (LODSelection::GetErrorThreshold): by its levels' errors where they are known, else by halving the fraction.
uint ChooseLODFor(GeometrySubmesh submesh, float screenFraction, float errorThreshold)
{
	if ((submesh.Flags & ST_GEOMETRY_SUBMESH_LOD_ERRORS) != 0u)
		return SelectLODByError(screenFraction, submesh.FirstLOD, submesh.LODCount, errorThreshold);
	return SelectLOD(screenFraction, u_Cull.LODParameters.x, submesh.LODCount);
}

// The level for the view.
uint ChooseLOD(GeometrySubmesh submesh, float screenFraction)
{
	return ChooseLODFor(submesh, screenFraction, u_Cull.LODErrorThreshold);
}

// The projected diameter of a bounding sphere as a fraction of the viewport height, as LODSelection::GetScreenFraction
// computes it.
float GetScreenFraction(float3 center, float radius)
{
	if (u_Cull.CameraPosition.w > 0.5)
		return (2.0 * radius) / u_Cull.LODParameters.y;
	float distance = length(center - u_Cull.CameraPosition.xyz);
	return radius / (max(distance, u_Cull.LODParameters.z) * u_Cull.LODParameters.y);
}

// The texels of a depth pyramid's level that cover a rectangle of pixels [minPixel, maxPixel] of a target of `size` pixels,
// clamped to it: the finest level at which it spans at most two texels each way (corners `low` and `high`). Level 0 is the
// target's size divided by 1 << shift, rounded up, and every other level half the one above, rounded down
// (Scene/HZBBuild.comp): level m's texel covers the pixels p with p >> (m + shift) equal to it, and its last row and column
// also those past them, so a pixel's texel is clamped to them.
int GetPyramidFootprint(float2 minPixel, float2 maxPixel, float2 size, int levels, int shift, out int2 low, out int2 high)
{
	int2 lowPixel = int2(clamp(floor(minPixel), float2(0.0), size - 1.0));
	int2 highPixel = int2(clamp(floor(maxPixel), float2(0.0), size - 1.0));
	int2 levelZero = (int2(size) + (1 << shift) - 1) >> shift;
	int level = 0;
	low = lowPixel >> shift;
	high = highPixel >> shift;
	while (level + 1 < levels && any((high - low) > 1))
	{
		level++;
		int2 last = max(levelZero >> level, int2(1)) - 1;
		low = min(lowPixel >> (level + shift), last);
		high = min(highPixel >> (level + shift), last);
	}
	return level;
}

// Frustum::IsAABBVisible: the corner furthest along each plane's normal is inside.
bool IsBoxInPlanes(float3 boxMin, float3 boxMax, float4 planes[6])
{
	for (int index = 0; index < 6; index++)
	{
		float4 plane = planes[index];
		float3 positive = float3(plane.x >= 0.0 ? boxMax.x : boxMin.x, plane.y >= 0.0 ? boxMax.y : boxMin.y, plane.z >= 0.0 ? boxMax.z : boxMin.z);
		if (dot(plane.xyz, positive) + plane.w < 0.0)
			return false;
	}
	return true;
}

bool IsBoxInFrustum(float3 boxMin, float3 boxMax)
{
	return IsBoxInPlanes(boxMin, boxMax, u_Cull.Planes);
}

#endif
