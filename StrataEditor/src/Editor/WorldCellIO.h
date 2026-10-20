#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/World/WorldAsset.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	class AssetManagerBase;
	class EditorAssetManager;
	class Entity;
	class Scene;

	// The editor's reads and measures of a world's documents: cells and the persistent scene are read with the streamed,
	// cancellable reader (SceneSerializer::DeserializeText); EditorWorld saves them through the DocumentSaver, and world.create
	// writes a new world's two small documents here.
	namespace WorldCellIO
	{

		// What the cook measures of a cell.
		struct CellSummary
		{
			WorldCellSummary Summary; // Key and Stamp are the caller's
			std::vector<UUID> EntityIDs; // Sorted
		};

		// Reads a scene document (its stored form) into an empty scene, with its world transforms, through the streamed reader:
		// `isCancelled` (may be null) is asked as it reads, and true ends the read, which fails. Any thread (an I/O job); blocks.
		// False with the reason for a document that cannot be read or is no scene.
		bool ReadScene(AssetManagerBase& assets, AssetHandle asset, Scene& outScene, const std::function<bool()>& isCancelled, std::string* outError = nullptr);
		// Reads a cell's scene document and measures it (ReadScene). Nullopt with the reason when it cannot be read.
		std::optional<CellSummary> ReadCellSummary(AssetManagerBase& assets, AssetHandle scene, const std::function<bool()>& isCancelled = {},
			std::string* outError = nullptr);
		// Measures a scene in memory (the persistent scene, a loaded cell): entities, physics bodies, bounds (entities'
		// positions, renderers' recorded mesh bounds, colliders) and entity IDs; ClosureBytes is the stored size of `asset`'s
		// dependency closure without other scenes (0 for a null asset).
		CellSummary SummarizeScene(const Scene& scene, const AssetManagerBase& assets, AssetHandle asset);
		// The same of some top-level entities of a scene with their descendants (a loaded cell of the edited world, as its save
		// begins): what its document will hold.
		CellSummary SummarizeEntities(const Scene& scene, std::span<const Entity> roots, const AssetManagerBase& assets, AssetHandle asset);
		// The stored bytes of an asset's dependency closure without other scenes (AssetClosureScope::Load): a cell's
		// WorldCellSummary::ClosureBytes.
		uint64_t GetClosureBytes(const AssetManagerBase& assets, AssetHandle asset);
		// Writes a new document of an engine-native type at a path relative to the asset directory (which must not exist) with
		// the given handle: its .meta first, so that the asset registers with it (a world's new cell, whose handle is derived
		// from the world's; a new world and its persistent scene, which name each other). The handle the asset got (null with
		// the reason when it could not be written).
		AssetHandle CreateDocument(EditorAssetManager& assets, const std::string& path, AssetType type, AssetHandle handle, const nlohmann::json& document,
			std::string* outError = nullptr);

	}

}
