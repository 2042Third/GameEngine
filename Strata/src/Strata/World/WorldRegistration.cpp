#include "stpch.h"
#include "Strata/World/WorldRegistration.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/World/WorldAsset.h"

namespace Strata
{

	void RegisterWorldModule()
	{
		// Worlds are stored as their JSON source and validated fully when they load.
		AssetLoaderRegistry::Register(AssetType::World, [](const AssetMetadata&, AssetLoadData& data, std::string* outError) -> Ref<Asset>
		{
			return WorldAsset::Deserialize(data.GetBytes(), outError);
		});
	}

}
