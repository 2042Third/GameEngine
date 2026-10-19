#include "stpch.h"
#include "Strata/Scene/SceneRegistration.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/SceneReadProgress.h"

namespace Strata
{

	void RegisterSceneModule()
	{
		RegisterSceneComponents();

		// Prefabs and scenes are stored as their JSON source, models in their cooked JSON form: all are read as streams, which
		// stop once the load is given up.
		AssetLoaderRegistry::Register(AssetType::Prefab, [](const AssetMetadata&, AssetLoadData& data, std::string* outError) -> Ref<Asset>
		{
			return Prefab::Deserialize(data.GetBytes(), outError, [&data]() { return data.IsCancelled(); });
		});
		AssetLoaderRegistry::Register(AssetType::Model, [](const AssetMetadata&, AssetLoadData& data, std::string* outError) -> Ref<Asset>
		{
			return Model::Deserialize(data.GetBytes(), outError, [&data]() { return data.IsCancelled(); });
		});
		// Reading a large scene takes seconds: the read reports how far it got (SceneReadProgress), which scene loads show, and
		// stops once the load is given up (a cancelled SceneLoadRequest, another scene opened, the manager closing).
		AssetLoaderRegistry::Register(AssetType::Scene, [](const AssetMetadata& metadata, AssetLoadData& data, std::string* outError) -> Ref<Asset>
		{
			SceneReadProgress::Tracker progress(metadata.Handle);
			return SceneAsset::Deserialize(data.GetBytes(), outError, [&progress](float share) { progress.Report(share); }, [&data]() { return data.IsCancelled(); });
		});
	}

}
