#pragma once

#include <doctest/doctest.h>

#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"

#include <cstdint>
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

}
