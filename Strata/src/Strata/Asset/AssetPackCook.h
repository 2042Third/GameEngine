#pragma once

#include "Strata/Asset/Asset.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Strata
{

	struct SceneSettings;

	// How an export writes the documents of entities (scenes, prefabs, models) into its pack.
	struct AssetPackCookOptions
	{
		// Write them cooked (CookedScene: a load reads them without parsing JSON); false keeps them JSON.
		bool Cook = true;
		// The export's scene transform: applied to every scene's settings between the read and the write, to cooked and JSON
		// output alike (null: none).
		std::function<void(AssetHandle scene, SceneSettings& settings)> AdjustSceneSettings;
	};

	// Reads a scene, prefab or model document (any layout, as a stream) and writes it as the pack holds it: cooked, or JSON
	// (in the writer's layout when AdjustSceneSettings changes a scene; unchanged otherwise). `outReferences` receives what
	// the document references, by AssetDependencies::Scan's rules, from the same read: the pack's reference check compares
	// them with its plan. Data of other types is left alone, with no references. False, with the reason, when the document
	// cannot be read or written.
	bool CookForPack(const AssetMetadata& metadata, std::vector<uint8_t>& data, const AssetPackCookOptions& options, std::vector<AssetHandle>& outReferences,
		std::string* outError = nullptr);

}
