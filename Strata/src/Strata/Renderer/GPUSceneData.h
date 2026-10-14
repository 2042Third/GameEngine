#pragma once

#include <cstdint>

// GPU data layouts of the GPU-driven path (GPU culling and indirect draws), shared with its shaders. Every struct is std430
// compatible (16-byte aligned vectors, no implicit padding).
namespace Strata::RenderData
{

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
