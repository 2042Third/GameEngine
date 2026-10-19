#pragma once

#include <doctest/doctest.h>

#include "Strata/Core/BinaryStream.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Strata::Tests
{

	// A cube of `size` units drawn as `parts` submeshes (each over all of its geometry), whose materials are
	// firstMaterial, firstMaterial + 1, ...: meshes with other layouts than the built-in ones, for reloads that change them.
	inline Ref<Mesh> CreatePartedCube(float size, uint32_t parts, uint64_t firstMaterial = 0xA1)
	{
		const Ref<Mesh> cube = MeshFactory::CreateCube(size);
		REQUIRE(cube);
		std::vector<Submesh> submeshes;
		for (uint32_t part = 0; part < parts; part++)
		{
			Submesh submesh = cube->GetSubmeshes()[0];
			submesh.Material = UUID(firstMaterial + part);
			submeshes.push_back(submesh);
		}
		const Ref<Mesh> mesh = Mesh::Create(cube->GetPositions(), cube->GetAttributes(), cube->GetIndices(), std::move(submeshes));
		REQUIRE(mesh);
		return mesh;
	}

	// An unskinned mesh as cooked mesh versions 1 and 2 wrote it (the levels without their errors, version 1 without the
	// header's flags): what earlier builds left in projects, for the loader's compatibility. Written here as those versions
	// laid it out, not through Mesh::Serialize, so changes of the current format do not change it.
	inline std::vector<uint8_t> SerializeMeshVersion2(const Mesh& mesh, uint32_t version = 2)
	{
		struct CookedSubmesh
		{
			uint32_t BaseVertex;
			uint32_t VertexCount;
			uint32_t LODCount;
			uint32_t Reserved;
			uint64_t Material;
			glm::vec3 BoundsMin;
			glm::vec3 BoundsMax;
		};
		struct CookedLOD
		{
			uint32_t IndexOffset;
			uint32_t IndexCount;
		};
		REQUIRE_FALSE(mesh.IsSkinned());
		BinaryWriter writer;
		writer.Write(std::array<uint32_t, 4> { 0x484D5453u, version, static_cast<uint32_t>(mesh.GetSubmeshes().size()), 0u });
		writer.WriteArray(std::span<const glm::vec3>(mesh.GetPositions()));
		writer.WriteArray(std::span<const MeshVertexAttributes>(mesh.GetAttributes()));
		writer.WriteArray(std::span<const uint32_t>(mesh.GetIndices()));
		for (const Submesh& submesh : mesh.GetSubmeshes())
		{
			writer.Write(CookedSubmesh { submesh.BaseVertex, submesh.VertexCount, static_cast<uint32_t>(submesh.LODs.size()), 0,
				static_cast<uint64_t>(submesh.Material), submesh.Bounds.Min, submesh.Bounds.Max });
			writer.WriteString(submesh.Name);
			std::vector<CookedLOD> lods;
			for (const MeshLOD& lod : submesh.LODs)
				lods.push_back(CookedLOD { lod.IndexOffset, lod.IndexCount });
			writer.WriteArray(std::span<const CookedLOD>(lods));
		}
		return writer.TakeData();
	}

}
