#pragma once

#include "Strata/Asset/AssetTypes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Strata
{

	class Asset;

	// The triangles of a mesh asset as positions and a triangle list (three indices into Positions per triangle,
	// counter-clockwise front faces): what modules that do not draw meshes read of them (physics' mesh colliders).
	struct MeshGeometry
	{
		std::vector<glm::vec3> Positions;
		std::vector<uint32_t> Indices;
	};

	// Fills the geometry from an asset of the type it was registered for; false when the asset has none to give.
	using MeshGeometryReader = bool (*)(const Asset& asset, MeshGeometry& outGeometry);

	// The readers of mesh geometry, registered by the module that owns each mesh type (the renderer's Mesh), so that other
	// modules read meshes without depending on that module. Engine::RegisterBuiltinModules opens it; using it before fails
	// ST_CORE_VERIFY. Registering a type again replaces its reader (at startup, before anything reads). Thread-safe.
	class MeshGeometryRegistry
	{
	public:
		static void BeginRegistration();
		static void Register(AssetType type, MeshGeometryReader reader);
		// Reads the geometry of the asset with the reader of its type: false (and outGeometry emptied) when no reader is
		// registered for it or the reader has nothing to give.
		static bool Read(const Asset& asset, MeshGeometry& outGeometry);
	};

}
