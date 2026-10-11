#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/UUID.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace Strata
{

	class EditorContext;
	class Scene;

	// An entity of a scene in memory whose component (an asset property, or a script field of asset type) names an asset.
	struct SceneEntityReference
	{
		UUID Entity = UUID::Null();
		std::string Name;
		std::string Component; // "MeshRenderer", "Script"
		std::string Property;  // "Material", or the script's "<Class>.<Field>"
		AssetHandle Asset = UUID::Null();
	};

	// What uses an asset: another asset whose recorded references name it, or the edited scene in memory.
	struct AssetReferencer
	{
		AssetHandle Asset = UUID::Null(); // The referencing asset (null: the edited scene, never saved)
		AssetType Type = AssetType::None;
		std::string Path;                 // Its file (sub-assets: their parent's), relative to the asset directory
		std::string Name;                 // Sub-assets: their name in their file ("Material/0")
		// The edited scene: its entities that use the asset now (also when they differ from what its file holds).
		bool OpenScene = false;
		std::vector<SceneEntityReference> Entities;
	};

	// Who references assets, for the commands that delete them (asset.delete, asset.deleteFolder), asset.findReferences and the
	// Content Browser's delete confirmation: the index of recorded references of the project's assets
	// (EditorAssetManager::GetReferencers) and a walk of the edited scene's components, so that unsaved uses count too.
	namespace AssetReferences
	{

		// The referencers of any of `targets` that are not in `ignored` (what goes with the targets, e.g. the other assets of
		// a folder that is deleted, or a model's own sub-assets). The edited scene is reported from memory: with the entities
		// that use the targets, and also when its file does not show them yet. Sub-assets that reference come with their file.
		std::vector<AssetReferencer> Find(EditorContext& context, const std::vector<AssetHandle>& targets, const std::unordered_set<AssetHandle>& ignored);
		// A target with its sub-assets (a model's meshes, materials and textures), which go with it.
		std::vector<AssetHandle> WithSubAssets(const EditorContext& context, const std::vector<AssetHandle>& targets);

		// The entities of a scene that use any of the assets: every asset property of every registered component and every
		// script field of asset type, at most `limit` of them.
		std::vector<SceneEntityReference> FindInScene(Scene& scene, const std::unordered_set<AssetHandle>& assets, size_t limit = 1000);

		// [{"asset", "path", "type", "name"?, "openScene"?, "entities"?: [{"id", "name", "component", "property", "asset"}]}]
		nlohmann::json ToJson(const std::vector<AssetReferencer>& referencers);
		// "Scenes/Main.stscene (open: Ground), Prefabs/Crate.stprefab and 3 more", for messages.
		std::string Describe(const std::vector<AssetReferencer>& referencers, size_t maxNames = 4);

	}

}
