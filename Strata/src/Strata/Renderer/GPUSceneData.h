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

}
