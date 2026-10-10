#include "stpch.h"
#include "Strata/Asset/RuntimeAssetManager.h"

#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/FileSystem.h"

namespace Strata
{

	Ref<RuntimeAssetManager> RuntimeAssetManager::Create(const std::filesystem::path& packPath, std::string* outError)
	{
		Scope<AssetPack> pack = AssetPack::Open(packPath, outError);
		if (!pack)
			return nullptr;
		return Ref<RuntimeAssetManager>(new RuntimeAssetManager(std::move(pack)));
	}

	RuntimeAssetManager::RuntimeAssetManager(Scope<AssetPack> pack)
		: m_Pack(std::move(pack))
	{
		const std::vector<AssetPackEntry>& entries = m_Pack->GetEntries();
		for (size_t index = 0; index < entries.size(); index++)
		{
			AssetMetadata metadata = entries[index].Metadata;
			// Built-in assets always come from the engine itself; a pack cannot replace them.
			if (BuiltinAssets::IsBuiltin(metadata.Handle))
			{
				ST_CORE_WARN("Asset pack entry '{}' uses a built-in asset handle; ignored", metadata.Path);
				continue;
			}
			metadata.StoredSize = entries[index].Size;
			m_EntryIndices.emplace(metadata.Handle, index);
			RegisterAsset(metadata);
		}
		ST_CORE_INFO("Asset pack '{}': {} assets", FileSystem::ToUTF8(m_Pack->GetPath().filename()), m_EntryIndices.size());
	}

	RuntimeAssetManager::~RuntimeAssetManager()
	{
		WaitForInFlightLoads();
	}

	bool RuntimeAssetManager::ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError)
	{
		auto it = m_EntryIndices.find(metadata.Handle);
		if (it == m_EntryIndices.end())
		{
			if (outError)
				*outError = "The asset is not part of the asset pack";
			return false;
		}
		return m_Pack->ReadData(m_Pack->GetEntries()[it->second], outData, outError);
	}

}
