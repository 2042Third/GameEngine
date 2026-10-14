#pragma once

#include <glm/glm.hpp>

#include <cstdint>

// GPU data layouts of the GPU-driven path (GPU culling and indirect draws), shared with its shaders. Every struct is std430
// compatible (16-byte aligned vectors, no implicit padding).
namespace Strata::RenderData
{

	// A render proxy as the GPU's culling reads it (RenderSceneGPU's proxy buffer, by RenderScene proxy index). A record of
	// zeros draws nothing: proxies destroyed, and those whose upload is still to come.
	struct GPUProxy
	{
		glm::vec3 LocalMin = glm::vec3(0.0f); // RenderScene::GetLocalBounds (posed bounds for posed skinned proxies)
		uint32_t FirstInstance = 0;
		glm::vec3 LocalMax = glm::vec3(0.0f);
		uint32_t InstanceCount = 0;           // One instance per submesh; 0 until the mesh layout is known
		glm::vec3 WorldMin = glm::vec3(0.0f); // RenderScene::GetWorldBounds
		uint32_t MeshSlot = 0;                // RenderScene's mesh slot (0xFFFFFFFF: none)
		glm::vec3 WorldMax = glm::vec3(0.0f);
		// RenderProxyFlags in the low c_GPUProxyOverrideShift bits; above them the override material slot plus one (0: none,
		// or a slot beyond c_GPUProxyMaxOverrideSlot), which feedback requests before the mesh's layout gives the proxy
		// instances.
		uint32_t Flags = 0;
	};
	static_assert(sizeof(GPUProxy) == 64);
	constexpr uint32_t c_GPUProxyOverrideShift = 8;
	constexpr uint32_t c_GPUProxyMaxOverrideSlot = (1u << (32 - c_GPUProxyOverrideShift)) - 2;

	// A mesh's place in the geometry pool (GeometryPool's entry table, by GeometryHandle).
	struct GeometryEntry
	{
		uint32_t FirstVertex = 0;
		uint32_t FirstIndex = 0;
		uint32_t FirstSubmesh = 0; // In the submesh table
		uint32_t SubmeshCount = 0;
	};
	static_assert(sizeof(GeometryEntry) == 16);

	struct GeometrySubmesh
	{
		uint32_t BaseVertex = 0; // Relative to the mesh's first vertex
		uint32_t FirstLOD = 0;   // In the LOD table: the draw record of each level (GPU culling counts per record)
		uint32_t LODCount = 0;
		uint32_t Reserved = 0;
		glm::vec4 UVDensity = glm::vec4(0.0f); // xyz: Submesh::UVDensity (texture streaming's UV pixels, from feedback)
	};
	static_assert(sizeof(GeometrySubmesh) == 32);

	// A level of detail of a submesh: the draw record GPU culling counts instances per.
	struct GeometryLOD
	{
		uint32_t IndexOffset = 0; // Relative to the mesh's first index
		uint32_t IndexCount = 0;
		uint32_t BaseVertex = 0;  // Its submesh's, relative to the mesh's first vertex
		uint32_t Mesh = 0;        // The GeometryHandle the level belongs to (CullBuild reads the mesh's offsets from it)
	};
	static_assert(sizeof(GeometryLOD) == 16);

	// GPU culling (Scene/Cull.comp, CullShadow, CullBuild.comp; Include/Culling.glsl mirrors these layouts).
	//
	// The mesh table (RenderSceneGPU) holds per mesh slot the GeometryHandle of the loaded mesh (0: none), with this bit for
	// meshes of the Skinned arena; the material flags per material slot these bits (0: not drawn).
	constexpr uint32_t c_CullMeshSkinnedArena = 1u << 31;
	constexpr uint32_t c_CullMaterialAvailable = 1u << 0;
	constexpr uint32_t c_CullMaterialDoubleSided = 1u << 1;
	constexpr uint32_t c_CullMaterialAlphaMask = 1u << 2;
	constexpr uint32_t c_CullMaterialTransparent = 1u << 3;
	constexpr uint32_t c_CullMaterialTextured = 1u << 4; // Names texture maps: feedback works out its UV pixels

	// Visibility feedback (Scene/Cull.comp's counting passes, packed by CullFeedback.comp, read back frames later): how the
	// frame uses each mesh and material slot it touched. The use bits are SceneRenderer's request marks.
	constexpr uint32_t c_FeedbackView = 1u << 0;     // In the view
	constexpr uint32_t c_FeedbackHigh = 1u << 1;     // In the view above SceneRendererSpecification::HighPriorityScreenFraction
	constexpr uint32_t c_FeedbackPrefetch = 1u << 2; // Within SceneRendererSpecification::PrefetchDistance
	constexpr uint32_t c_FeedbackShadow = 1u << 3;   // Casts into a cascade
	// The touched-slot lists' header (mesh count, material count, two reserved words); the mesh slots follow, then (after
	// MeshSlotCount entries) the material slots.
	constexpr uint32_t c_FeedbackListHeader = 4;
	// A packed record: the header record (MeshRecords, MaterialRecords) first, then MeshSlotCount mesh records and
	// MaterialSlotCount material records, the touched ones first in each part.
	struct FeedbackRecord
	{
		uint32_t Slot = 0;
		uint32_t Use = 0;       // c_Feedback* bits
		float Pixels = 0.0f;    // The largest projected size in pixels of a drawable instance (0: none, e.g. not loaded)
		float UVPixels = 0.0f;  // Materials: the largest TextureStreamingPolicy::GetUVPixels of their textured instances
	};
	static_assert(sizeof(FeedbackRecord) == 16);

	// Draw buckets: one per pipeline state and vertex arena. A view bucket's index: mirrored (bit 0), double-sided (bit 1),
	// geometry (bits 2-3, c_CullGeometry*), the occlusion culling's second phase (bit 4: what the first phase did not draw,
	// found visible against the depth pyramid); shadow buckets follow the view's, c_CullShadowBucketsPerCascade per
	// cascade, alpha-masked in bit 4. Buckets the frame cannot have get no storage: the bucket table maps the present ones
	// to dense indices (c_CullNoBucket for the others).
	constexpr uint32_t c_CullGeometryStatic = 0;       // Static arena, unskinned pipelines
	constexpr uint32_t c_CullGeometrySkinnedArena = 1; // Skinned arena drawn in its bind pose: unskinned pipelines
	constexpr uint32_t c_CullGeometrySkinned = 2;      // Skinned arena, posed: the skinned pipelines
	constexpr uint32_t c_CullViewBuckets = 32;
	constexpr uint32_t c_CullShadowBucketsPerCascade = 32;
	constexpr uint32_t c_CullMaxCascades = 4;
	constexpr uint32_t c_CullBuckets = c_CullViewBuckets + c_CullMaxCascades * c_CullShadowBucketsPerCascade;
	constexpr uint32_t c_CullNoBucket = ~0u;

	constexpr uint32_t MakeViewBucket(bool mirrored, bool doubleSided, uint32_t geometry, bool secondPhase = false)
	{
		return (mirrored ? 1u : 0u) | (doubleSided ? 2u : 0u) | (geometry << 2) | (secondPhase ? 16u : 0u);
	}

	constexpr uint32_t MakeShadowBucket(uint32_t cascade, bool mirrored, bool doubleSided, uint32_t geometry, bool alphaMask)
	{
		return c_CullViewBuckets + cascade * c_CullShadowBucketsPerCascade + MakeViewBucket(mirrored, doubleSided, geometry) + (alphaMask ? 16u : 0u);
	}

	// What a frame's culling tests and sizes (a constant buffer, std140).
	struct CullConstants
	{
		glm::vec4 Planes[6] = {};                    // The view frustum, inward (Frustum::GetPlanes)
		glm::vec4 CameraPosition = glm::vec4(0.0f);  // xyz; w: 1 for orthographic cameras
		// x: LODThreshold, y: tan(vertical FOV / 2) (perspective) or the vertical extent (orthographic), z: the near plane,
		// w: the smallest projected size drawn in pixels (0: no screen-size culling)
		glm::vec4 LODParameters = glm::vec4(0.0f);
		// x: viewport height in pixels, y: the near distance texture streaming measures from (TextureStreamingPolicy::
		// ViewScale::Near), z: the prefetch distance (0: none), w: SceneRendererSpecification::HighPriorityScreenFraction
		glm::vec4 ViewParameters = glm::vec4(0.0f);
		glm::mat4 LightView = glm::mat4(1.0f);
		glm::mat4 ViewProjection = glm::mat4(1.0f);  // The view's, for the occlusion test's screen rectangles
		// x, y: the depth buffer's size in pixels (the depth pyramid's level 0 is half of it), z: the pyramid's levels,
		// w: 1 when the view is occlusion culled
		glm::vec4 OcclusionParameters = glm::vec4(0.0f);
		glm::vec4 CascadeBoxes[c_CullMaxCascades] = {};      // Light view space: min.xy, max.xy
		glm::vec4 CascadeParameters[c_CullMaxCascades] = {}; // x: the far side's z, y: the smallest light-space extent drawn
		uint32_t ProxyCount = 0;
		uint32_t CascadeCount = 0;
		uint32_t LODCapacity = 0;        // LOD records (GeometryPool::GetLODCapacity): records per bucket in the count arrays
		uint32_t MeshSlotCount = 0;
		uint32_t MaterialSlotCount = 0;
		uint32_t DrawListCapacity = 0;
		uint32_t ArgumentsPerBucket = 0; // Draw arguments per dense bucket
		uint32_t BucketCount = 0;        // Dense buckets
		uint32_t ViewBucketCount = 0;    // Of those, the view's (they come first)
		// 1 where the draws read a bucket's first slots only (without a GPU-written count): records beyond them are counted
		// as overflow. Each dense bucket's slots follow the bucket table's c_CullBuckets entries.
		uint32_t DrawsBounded = 0;
		uint32_t Feedback = 0;               // 1: the counting passes write visibility feedback
		uint32_t FirstPhaseViewBuckets = 0;  // Of the view's dense buckets, the first phase's (they come first)
		uint32_t VisibilityParity = 0;       // Which bit of the visibility buffer holds the previous frame's (0 or 1)
		uint32_t Reserved0 = 0;
		uint32_t Reserved1 = 0;
		uint32_t Reserved2 = 0;
	};
	static_assert(sizeof(CullConstants) == 6 * 16 + 3 * 16 + 64 + 64 + 16 + 8 * 16 + 64);

	// What a frame's culling counted (read back frames later: SceneRendererGPUStats).
	struct CullCounters
	{
		uint32_t ViewInstances = 0;    // Opaque and alpha-masked instances drawn in the view (transparent ones are the CPU's)
		uint32_t ShadowInstances = 0;  // Instances drawn into cascades, summed over cascades
		uint32_t ViewDraws = 0;        // Records drawn (instanced draws) in the view and into the cascades
		uint32_t ShadowDraws = 0;
		uint32_t VisibleProxies = 0;   // Proxies passing the view's frustum and size tests
		uint32_t SubTexelCasters = 0;  // Casters left out of cascades in which they span less than a texel
		uint32_t DrawListUsed = 0;     // Entries of the draw list
		uint32_t Overflow = 0;         // Instances the draw list or the arguments had no room for (not drawn)
		uint32_t TrianglesLow = 0;     // View triangles, the low and the high 32 bits
		uint32_t TrianglesHigh = 0;
		uint32_t SkinnedInstances = 0; // Posed skinned instances drawn in the view
		uint32_t SizeCulled = 0;       // Proxies in the frustum smaller than SceneRendererSpecification::MinProjectedPixels
		uint32_t InstancesPerLOD[8] = {}; // View instances per level of detail, the last counting all higher levels
		uint32_t Occluded = 0;         // Proxies in the frustum hidden behind what the view draws (the second phase's test)
		uint32_t Reserved[11] = {};
	};
	static_assert(sizeof(CullCounters) == 128);

}
