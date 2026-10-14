#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/World/WorldAsset.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	class AssetManagerBase;
	class EditorAssetManager;
	class Entity;
	class Scene;

	// The editor's reads and writes of a world's cell documents (the cook's summaries now; EditorWorld's saves later). The
	// seam behind which world-io's cancellable reader and request-based saver replace these plain file reads once they land.
	namespace WorldCellIO
	{

		// What the cook measures of a cell.
		struct CellSummary
		{
			WorldCellSummary Summary; // Key and Stamp are the caller's
			std::vector<UUID> EntityIDs; // Sorted
		};

		// Reads a cell's scene document (its stored form) and measures it. Any thread (an I/O job); blocks. Nullopt with the
		// reason for a document that cannot be read or is no scene.
		std::optional<CellSummary> ReadCellSummary(AssetManagerBase& assets, AssetHandle scene, std::string* outError = nullptr);
		// Measures a scene in memory (the persistent scene, a loaded cell): entities, physics bodies, bounds (entities'
		// positions, renderers' recorded mesh bounds, colliders) and entity IDs; ClosureBytes is the stored size of `asset`'s
		// dependency closure without other scenes (0 for a null asset).
		CellSummary SummarizeScene(const Scene& scene, const AssetManagerBase& assets, AssetHandle asset);

		// The scene document of some top-level entities of a scene with their descendants, in hierarchy order: a cell's
		// (withSettings false: cells use their host scene's settings) or the persistent scene's.
		nlohmann::json SerializeDocument(const Scene& scene, const std::string& name, const std::vector<Entity>& roots, bool withSettings);
		// Writes a document over an engine-native asset's file (EditorAssetManager::SaveNativeAsset, with its references;
		// reload: loaded copies take the saved version). The bytes written, or nullopt with the reason.
		std::optional<size_t> WriteDocument(EditorAssetManager& assets, AssetHandle asset, AssetType type, const nlohmann::json& document, bool reload,
			std::string* outError = nullptr);
		// Writes a new scene document at a path relative to the asset directory (which must not exist) with the given handle:
		// its .meta first, so that the asset registers with it (a world's new cell, whose handle is derived from the world's).
		// The handle the asset got (null with the reason when it could not be written).
		AssetHandle CreateDocument(EditorAssetManager& assets, const std::string& path, AssetHandle handle, const nlohmann::json& document,
			std::string* outError = nullptr);

	}

}
