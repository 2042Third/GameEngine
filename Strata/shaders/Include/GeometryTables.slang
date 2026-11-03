#ifndef STRATA_GEOMETRY_TABLES_SLANG
#define STRATA_GEOMETRY_TABLES_SLANG

// The geometry pool's tables (GeometryPool: per GeometryHandle its entry, the submeshes and their levels of detail), as GPU
// culling (Include/Culling.slang) and the voxelizer (Include/Voxelize.slang) read them. Layouts mirror
// Strata/Renderer/GPUSceneData.h; keep them in sync.

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
	float4 UVDensity; // xyz: Submesh::UVDensity
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

#endif
