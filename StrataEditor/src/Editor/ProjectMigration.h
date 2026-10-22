#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Scene/PhysicsLayerSettings.h>

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	// The upgrade of projects saved by 0.5.0 and older (project file version 1 or 2), which EditorContext runs once when it
	// opens one, before its start scene: their scenes' collision layers become the project's (ProjectCollisionLayers). The
	// scan runs on a job; the merge and the project's change happen on the main thread. Scene files are not touched: the
	// layers they store are ignored and dropped at their next save. Docs/DeveloperGuide.md, "Collision layers".
	namespace ProjectMigration
	{

		// The collision layers one scene document stores (never the default table, which counts as none).
		struct StoredSceneLayers
		{
			std::string Path;                  // Relative to the asset directory, '/' separators
			AssetHandle Handle = UUID::Null(); // From its .meta; null without a valid one
			PhysicsLayerSettings Layers;
		};

		// What a scan of the asset directory found.
		struct SceneLayerScan
		{
			std::vector<StoredSceneLayers> Scenes; // Those that store layers, by path
			size_t ScenesRead = 0;                 // Every scene document read
			std::vector<std::string> Problems;     // Scene documents that could not be read, and why; the walk's error
		};

		// Reads the settings of every scene document (".stscene") of the asset directory with SceneDocumentReader, stopping at
		// the first entity when its settings came first (as SceneDocumentWriter writes them; older documents and world cells,
		// which store no settings, are read to the end). Stops early once `cancel` is set. Any thread.
		SceneLayerScan ScanSceneLayers(const std::filesystem::path& assetDirectory, const std::atomic<bool>* cancel = nullptr);

		// The project's table made of the scenes' layers.
		struct LayerMerge
		{
			std::optional<PhysicsLayerSettings> Layers; // Nothing when no scene stores layers
			std::vector<std::string> Scenes;            // The paths of the scenes that store layers, the start scene first
			std::vector<std::string> Conflicts;         // Each disagreement between scenes, one sentence each
		};

		// Names come from the start scene (`startScene`, null for none), then from the others by path: a scene that names a
		// layer another named differently, or names one with a name another layer has, is a conflict and the first name
		// stays. A pair of layers any scene keeps apart is kept apart, and scenes whose matrices differ (other than the default)
		// are a conflict, which names the pairs they disagree on.
		LayerMerge MergeSceneLayers(const std::vector<StoredSceneLayers>& scenes, AssetHandle startScene);

	}

}
