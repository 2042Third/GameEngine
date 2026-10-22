#ifndef STRATA_CULLING_GLSL
#define STRATA_CULLING_GLSL

// GPU culling's resources (Scene/Cull.comp, Scene/CullBuild.comp). Layouts mirror Strata/Renderer/GPUSceneData.h and
// SceneRenderData.h; keep them in sync.

#include "Include/Common.glsl"
#include "Include/OrderedFloat.glsl"

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
#define ST_CULL_BUCKETS                288u // c_CullBuckets: the bucket table's bucket -> dense index part
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

struct GPUProxy
{
	vec3 LocalMin;
	uint FirstInstance;
	vec3 LocalMax;
	uint InstanceCount;
	vec3 WorldMin;
	uint MeshSlot;
	vec3 WorldMax;
	uint Flags;
};

struct GeometryEntry
{
	uint FirstVertex;
	uint FirstIndex;
	uint FirstSubmesh;
	uint SubmeshCount;
};

struct GeometrySubmesh
{
	uint BaseVertex;
	uint FirstLOD;
	uint LODCount;
	uint Flags;     // ST_GEOMETRY_SUBMESH_* bits
	vec4 UVDensity; // xyz: Submesh::UVDensity
};

struct GeometryLOD
{
	uint IndexOffset;
	uint IndexCount;
	uint BaseVertex;
	uint Mesh;
	float RelativeError; // MeshUtils::GetRelativeLODError of the level's error
	uint Reserved0;
	uint Reserved1;
	uint Reserved2;
};

struct CullInstance
{
	vec4 WorldRows[3];
	uint MaterialIndex;
	uint EntityID;
	uint Flags;
	uint SkinPalette;
};

struct CullConstants
{
	vec4 Planes[6];          // The view frustum in camera-relative space
	vec4 CameraPosition;     // xyz: world position; w: 1 for orthographic cameras
	vec4 LODParameters;      // x: LOD threshold, y: tan(fov / 2) or the vertical extent, z: near plane, w: smallest projected pixels
	vec4 ViewParameters;     // x: viewport height in pixels, y: texture streaming's near distance, z: prefetch distance, w: high priority fraction
	mat4 LightView;
	mat4 ViewProjection;     // The view's (camera-relative), for the occlusion test's screen rectangles
	vec4 OcclusionParameters; // xy: depth buffer size in pixels, z: depth pyramid levels, w: 1 when occlusion culled
	vec4 CascadeBoxes[4];    // Light view space: min.xy, max.xy
	vec4 CascadeParameters[4]; // x: far side z, y: smallest light-space extent drawn
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
	uint Reserved0;
	uint Reserved1;
	uint Reserved2;
};

struct CullCascade
{
	mat4 ViewProjection;
	vec4 Parameters;         // RenderData::CascadeData: w is the near plane's light view z (camera-relative)
};

layout(set = 0, binding = ST_CBV(0), std140) uniform CullBuffer
{
	CullConstants u_Cull;
};

layout(set = 0, binding = ST_SRV(0), std430) readonly buffer ProxyBuffer { GPUProxy u_Proxies[]; };
layout(set = 0, binding = ST_SRV(1), std430) readonly buffer CullInstanceBuffer { CullInstance u_CullInstances[]; };
layout(set = 0, binding = ST_SRV(2), std430) readonly buffer MeshTableBuffer { uint u_MeshTable[]; };
layout(set = 0, binding = ST_SRV(3), std430) readonly buffer MaterialFlagBuffer { uint u_MaterialFlags[]; };
layout(set = 0, binding = ST_SRV(4), std430) readonly buffer GeometryEntryBuffer { GeometryEntry u_GeometryEntries[]; };
layout(set = 0, binding = ST_SRV(5), std430) readonly buffer GeometrySubmeshBuffer { GeometrySubmesh u_GeometrySubmeshes[]; };
layout(set = 0, binding = ST_SRV(6), std430) readonly buffer GeometryLODBuffer { GeometryLOD u_GeometryLODs[]; };
// Per bucket its dense index, then per dense bucket its draw slots.
layout(set = 0, binding = ST_SRV(7), std430) readonly buffer BucketTableBuffer { uint u_BucketTable[]; };

// Instances per (dense bucket, LOD record), then their draw list offsets; the draws per dense bucket; the counters; the
// draws' arguments (DrawIndexedIndirectArguments, five words each) and the draw list itself.
layout(set = 0, binding = ST_UAV(0), std430) buffer CountBuffer { uint u_Counts[]; };
layout(set = 0, binding = ST_UAV(1), std430) buffer OffsetBuffer { uint u_Offsets[]; };
layout(set = 0, binding = ST_UAV(2), std430) buffer ArgumentBuffer { uint u_Arguments[]; };
layout(set = 0, binding = ST_UAV(3), std430) buffer BucketCountBuffer { uint u_BucketCounts[]; };
layout(set = 0, binding = ST_UAV(4), std430) buffer CounterBuffer { uint u_Counters[]; };
layout(set = 0, binding = ST_UAV(5), std430) buffer CullDrawListBuffer { uint u_CullDrawList[]; };
// Visibility feedback: per mesh slot (pixels, use) and per material slot (pixels, UV pixels, use, unused), as float bits where
// they are sizes (non-negative floats order like their bits); the touched slots' lists (ST_FEEDBACK_LIST_HEADER words, then
// MeshSlotCount mesh slots, then the material slots); the packed records (Scene/CullFeedback.comp).
layout(set = 0, binding = ST_UAV(6), std430) buffer MeshFeedbackBuffer { uint u_MeshFeedback[]; };
layout(set = 0, binding = ST_UAV(7), std430) buffer MaterialFeedbackBuffer { uint u_MaterialFeedback[]; };
layout(set = 0, binding = ST_UAV(8), std430) buffer FeedbackListBuffer { uint u_FeedbackLists[]; };
layout(set = 0, binding = ST_UAV(9), std430) buffer FeedbackRecordBuffer { uvec4 u_FeedbackRecords[]; };
// Per cascade the nearest drawn caster's light view z (EncodeOrderedFloat; 0: none), for Scene/CascadeFit.comp.
layout(set = 0, binding = ST_UAV(10), std430) buffer CasterDepthBuffer { uint u_CasterDepths[]; };
// Per proxy whether the view saw it, in two bits that alternate frame by frame: the previous frame's
// (u_Cull.VisibilityParity) and this frame's, written by the second phase; and likewise per cascade whether the proxy
// cast into it, in bits ST_VISIBILITY_CASCADE_SHIFT + ST_CULL_MAX_CASCADES * parity + cascade.
layout(set = 0, binding = ST_UAV(11), std430) buffer VisibilityBuffer { uint u_Visibility[]; };
// Per instance its level-of-detail cross-fade state (UpdateLODFade, LODSelection::LODFadeState), written by the view's
// writing passes.
layout(set = 0, binding = ST_UAV(12), std430) buffer LODFadeBuffer { uvec2 u_LODFades[]; };
// The depth pyramid of the first phase's depth (Scene/HZBBuild.comp): each texel the farthest (smallest, reversed-Z)
// depth of the pixels it covers; level m's texel covers pixels p with p >> (m + 1) equal to it.
layout(set = 0, binding = ST_SRV(8)) uniform texture2D u_DepthPyramid;
// Per cascade (a layer each) the depth pyramid of its first phase's shadow map: level m's texel covers texels t with
// t >> (m + 2) equal to it.
layout(set = 0, binding = ST_SRV(9)) uniform texture2DArray u_ShadowPyramid;
// The cascades' projections (Scene/CascadeFit.comp), fitted after the counting pass.
layout(set = 0, binding = ST_SRV(10), std430) readonly buffer CullCascadeBuffer { CullCascade u_CullCascades[]; };

#define ST_LOD_RELATIVE_ERROR(record) u_GeometryLODs[record].RelativeError
#include "Include/LODSelection.glsl"

#define ST_GEOMETRY_SUBMESH_LOD_ERRORS 1u // RenderData::c_GeometrySubmeshLODErrors

// The level of detail drawn of a submesh at a screen fraction (LODSelection::SelectLOD): by its levels' errors where they are
// known, else by halving the fraction.
uint ChooseLOD(GeometrySubmesh submesh, float screenFraction)
{
	if ((submesh.Flags & ST_GEOMETRY_SUBMESH_LOD_ERRORS) != 0u)
		return SelectLODByError(screenFraction, submesh.FirstLOD, submesh.LODCount, u_Cull.LODErrorThreshold);
	return SelectLOD(screenFraction, u_Cull.LODParameters.x, submesh.LODCount);
}

// The projected diameter of a bounding sphere as a fraction of the viewport height, as LODSelection::GetScreenFraction
// computes it.
float GetScreenFraction(vec3 center, float radius)
{
	if (u_Cull.CameraPosition.w > 0.5)
		return (2.0 * radius) / u_Cull.LODParameters.y;
	float distance = length(center - u_Cull.CameraPosition.xyz);
	return radius / (max(distance, u_Cull.LODParameters.z) * u_Cull.LODParameters.y);
}

// The texels of a depth pyramid's level that cover a rectangle of pixels [minPixel, maxPixel] of a target of `size` pixels,
// clamped to it: the finest level at which it spans at most two texels each way (corners `low` and `high`). Level m's texel
// covers the pixels p with p >> (m + shift) equal to it.
int GetPyramidFootprint(vec2 minPixel, vec2 maxPixel, vec2 size, int levels, int shift, out ivec2 low, out ivec2 high)
{
	low = ivec2(clamp(floor(minPixel), vec2(0.0), size - 1.0));
	high = ivec2(clamp(floor(maxPixel), vec2(0.0), size - 1.0));
	int level = 0;
	while (level + 1 < levels && (((high.x >> (level + shift)) - (low.x >> (level + shift))) > 1 || ((high.y >> (level + shift)) - (low.y >> (level + shift))) > 1))
		level++;
	low = low >> (level + shift);
	high = high >> (level + shift);
	return level;
}

// Frustum::IsAABBVisible: the corner furthest along each plane's normal is inside.
bool IsBoxInFrustum(vec3 boxMin, vec3 boxMax)
{
	for (int index = 0; index < 6; index++)
	{
		vec4 plane = u_Cull.Planes[index];
		vec3 positive = vec3(plane.x >= 0.0 ? boxMax.x : boxMin.x, plane.y >= 0.0 ? boxMax.y : boxMin.y, plane.z >= 0.0 ? boxMax.z : boxMin.z);
		if (dot(plane.xyz, positive) + plane.w < 0.0)
			return false;
	}
	return true;
}

#endif
