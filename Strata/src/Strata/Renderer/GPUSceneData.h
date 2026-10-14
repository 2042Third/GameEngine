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
		uint32_t Flags = 0;                   // RenderProxyFlags
	};
	static_assert(sizeof(GPUProxy) == 64);

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
	};
	static_assert(sizeof(GeometrySubmesh) == 16);

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
	// A renderer's mesh table holds per mesh slot the GeometryHandle of a mesh the frame draws (0: none), with this bit for
	// meshes of the Skinned arena; its material table per material slot these flags (0: not drawn).
	constexpr uint32_t c_CullMeshSkinnedArena = 1u << 31;
	constexpr uint32_t c_CullMaterialAvailable = 1u << 0;
	constexpr uint32_t c_CullMaterialDoubleSided = 1u << 1;
	constexpr uint32_t c_CullMaterialAlphaMask = 1u << 2;
	constexpr uint32_t c_CullMaterialTransparent = 1u << 3;

	// Draw buckets: one per pipeline state and vertex arena. A view bucket's index: mirrored (bit 0), double-sided (bit 1),
	// geometry (bits 2-3, c_CullGeometry*); shadow buckets follow the view's, c_CullShadowBucketsPerCascade per cascade,
	// alpha-masked in bit 4. Buckets the frame cannot have get no storage: the bucket table maps the present ones to dense
	// indices (c_CullNoBucket for the others).
	constexpr uint32_t c_CullGeometryStatic = 0;       // Static arena, unskinned pipelines
	constexpr uint32_t c_CullGeometrySkinnedArena = 1; // Skinned arena drawn in its bind pose: unskinned pipelines
	constexpr uint32_t c_CullGeometrySkinned = 2;      // Skinned arena, posed: the skinned pipelines
	constexpr uint32_t c_CullViewBuckets = 16;
	constexpr uint32_t c_CullShadowBucketsPerCascade = 32;
	constexpr uint32_t c_CullMaxCascades = 4;
	constexpr uint32_t c_CullBuckets = c_CullViewBuckets + c_CullMaxCascades * c_CullShadowBucketsPerCascade;
	constexpr uint32_t c_CullNoBucket = ~0u;

	constexpr uint32_t MakeViewBucket(bool mirrored, bool doubleSided, uint32_t geometry)
	{
		return (mirrored ? 1u : 0u) | (doubleSided ? 2u : 0u) | (geometry << 2);
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
		glm::vec4 ViewParameters = glm::vec4(0.0f);  // x: viewport height in pixels
		glm::mat4 LightView = glm::mat4(1.0f);
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
		uint32_t Reserved[2] = {};
	};
	static_assert(sizeof(CullConstants) == 6 * 16 + 3 * 16 + 64 + 8 * 16 + 48);

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
		uint32_t Reserved[12] = {};
	};
	static_assert(sizeof(CullCounters) == 128);

}
