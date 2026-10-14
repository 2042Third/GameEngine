#ifndef STRATA_CULLING_GLSL
#define STRATA_CULLING_GLSL

// GPU culling's resources (Scene/Cull.comp, Scene/CullBuild.comp). Layouts mirror Strata/Renderer/GPUSceneData.h and
// SceneRenderData.h; keep them in sync.

#include "Include/Common.glsl"
#include "Include/LODSelection.glsl"

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

#define ST_CULL_GEOMETRY_STATIC        0u
#define ST_CULL_GEOMETRY_SKINNED_ARENA 1u
#define ST_CULL_GEOMETRY_SKINNED       2u
#define ST_CULL_VIEW_BUCKETS           16u
#define ST_CULL_SHADOW_BUCKETS         32u
#define ST_CULL_BUCKETS                144u // c_CullBuckets: the bucket table's bucket -> dense index part
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
	uint Reserved;
};

struct GeometryLOD
{
	uint IndexOffset;
	uint IndexCount;
	uint BaseVertex;
	uint Mesh;
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
	vec4 Planes[6];
	vec4 CameraPosition;     // xyz; w: 1 for orthographic cameras
	vec4 LODParameters;      // x: LOD threshold, y: tan(fov / 2) or the vertical extent, z: near plane, w: smallest projected pixels
	vec4 ViewParameters;     // x: viewport height in pixels
	mat4 LightView;
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
	uint Reserved0;
	uint Reserved1;
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

// The projected diameter of a bounding sphere as a fraction of the viewport height, as LODSelection::GetScreenFraction
// computes it.
float GetScreenFraction(vec3 center, float radius)
{
	if (u_Cull.CameraPosition.w > 0.5)
		return (2.0 * radius) / u_Cull.LODParameters.y;
	float distance = length(center - u_Cull.CameraPosition.xyz);
	return radius / (max(distance, u_Cull.LODParameters.z) * u_Cull.LODParameters.y);
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
