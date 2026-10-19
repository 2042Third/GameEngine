#pragma once

#include "Scene/SceneLoaderTestUtils.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Scene/Scene.h"
#include "Strata/World/WorldAsset.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Generated worlds for the World.* suites: a World document, its cell scenes and a persistent scene written into a pack
// and mounted as the active asset manager, the way a game reads them.

namespace Strata::Tests
{

	struct GeneratedWorldSpecification
	{
		std::string Name = "Generated";
		glm::dvec2 GridOrigin { 0.0 };
		// The "Ground" layer: GridX by GridZ cells from cell (0, 0).
		uint32_t GridX = 4;
		uint32_t GridZ = 4;
		float CellSize = 32.0f;
		float StreamingDistance = 48.0f;
		bool GroundLoadBeforeStart = true;
		// Entities per Ground cell, the first BodiesPerCell of them static boxes; spread over the cell, inside its square.
		uint32_t EntitiesPerCell = 20;
		uint32_t BodiesPerCell = 5;
		// A floor box under each Ground cell (one more body), for bodies resting on the ground.
		bool Floors = false;
		// A "Props" layer of cells half the Ground's size over the same area, with PropsPerCell renderers each (0: none).
		uint32_t PropsPerCell = 0;
		float PropsStreamingDistance = 24.0f;
		// Whether the World document carries a cooked index of the generated counts.
		bool Index = true;
		// Ground cells whose stored form is garbage (their loads fail), by index in the Ground's cells.
		std::vector<uint32_t> BrokenCells;
		// Ground cells made only as the pack is written, one at a time from their coordinates: worlds too large to hold in
		// memory (the 25M-entity drive). Their index entries are counted from the specification, without closure bytes.
		bool GenerateCellsWhenPacked = false;
		// Scenes packed in world-io's cooked binary form (AssetPackWriteOptions::CookDocuments), as exports pack them.
		bool CookDocuments = false;
		// Fills the persistent scene (a primary camera at the origin is there already).
		std::function<void(Scene& persistent, AssetHandle world)> BuildPersistent;
	};

	struct GeneratedWorld
	{
		std::filesystem::path PackPath;
		AssetHandle World = UUID::Null();
		AssetHandle Persistent = UUID::Null();
		WorldData Data;
		uint32_t GroundCells = 0;
		uint64_t BodiesPerGroundCell = 0;

		// The scene of a cell of the Ground (layer 1) or Props (layer 2) layer.
		AssetHandle GetCellScene(uint8_t layer, int32_t x, int32_t z) const;
	};

	// Writes the world into `directory`/World.stpak. The Try forms return nullopt with the reason instead of failing the test
	// (for helper processes, which run no test).
	GeneratedWorld WriteGeneratedWorld(const std::filesystem::path& directory, const GeneratedWorldSpecification& specification);
	std::optional<GeneratedWorld> TryWriteGeneratedWorld(const std::filesystem::path& directory, const GeneratedWorldSpecification& specification,
		std::string* outError);
	// Writes the world and a game manifest (`directory`/World.stgame) whose start scene is its persistent scene; the
	// manifest's path.
	std::filesystem::path WriteGeneratedWorldGame(const std::filesystem::path& directory, const GeneratedWorldSpecification& specification,
		GeneratedWorld* outWorld = nullptr);
	std::optional<std::filesystem::path> TryWriteGeneratedWorldGame(const std::filesystem::path& directory, const GeneratedWorldSpecification& specification,
		GeneratedWorld* outWorld, std::string* outError);

	// A generated world's pack mounted in a runtime asset manager, active while it lives.
	struct MountedWorld
	{
		GeneratedWorld World;
		Ref<RuntimeAssetManager> Manager;
		Scope<ScopedActiveManager> Active;

		MountedWorld(const std::string& name, const GeneratedWorldSpecification& specification);
	};

}
