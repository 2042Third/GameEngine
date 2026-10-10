#include "stpch.h"
#include "Strata/Asset/RuntimeAssetManager.h"

#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/FileSystem.h"

#include <new>
#include <stdexcept>

namespace Strata
{

	Ref<RuntimeAssetManager> RuntimeAssetManager::Create(const std::filesystem::path& packPath, std::string* outError)
	{
		Scope<AssetPack> pack = AssetPack::Open(packPath, outError);
		if (!pack)
			return nullptr;
		// Registering copies what the pack's entries name, which AssetPack::Open bounds by the file's size; the memory may
		// still not be there.
		try
		{
			Ref<RuntimeAssetManager> manager(new RuntimeAssetManager());
			manager->AddPack(std::move(pack));
			return manager;
		}
		catch (const std::bad_alloc&)
		{
		}
		catch (const std::length_error&)
		{
		}
		if (outError)
			*outError = fmt::format("{}: not enough memory to register the pack's assets", FileSystem::ToUTF8(packPath));
		return nullptr;
	}

	RuntimeAssetManager::~RuntimeAssetManager()
	{
		WaitForInFlightLoads();
	}

	bool RuntimeAssetManager::Mount(const std::filesystem::path& packPath, std::string* outError)
	{
		Scope<AssetPack> pack = AssetPack::Open(packPath, outError);
		if (!pack)
			return false;
		try
		{
			// Loaded and loading assets of replaced entries read their new entry: a reload discards loads of the old one.
			for (AssetHandle replaced : AddPack(std::move(pack)))
				ReloadAsset(replaced);
			return true;
		}
		catch (const std::bad_alloc&)
		{
		}
		catch (const std::length_error&)
		{
		}
		if (outError)
			*outError = fmt::format("{}: not enough memory to register the pack's assets (some of them may be mounted)", FileSystem::ToUTF8(packPath));
		return false;
	}

	std::vector<AssetHandle> RuntimeAssetManager::AddPack(Scope<AssetPack> pack)
	{
		const AssetPack* added = pack.get();
		std::vector<AssetHandle> replaced;
		std::vector<std::pair<size_t, AssetMetadata>> registered;
		{
			std::unique_lock<std::shared_mutex> lock(m_PacksMutex);
			m_Packs.push_back(std::move(pack));
			const std::vector<AssetPackEntry>& entries = added->GetEntries();
			for (size_t index = 0; index < entries.size(); index++)
			{
				const AssetMetadata& metadata = entries[index].Metadata;
				// Built-in assets always come from the engine itself; a pack cannot replace them.
				if (BuiltinAssets::IsBuiltin(metadata.Handle))
				{
					ST_CORE_WARN("Asset pack entry '{}' uses a built-in asset handle; ignored", metadata.Path);
					continue;
				}
				auto [it, inserted] = m_PackEntries.insert_or_assign(metadata.Handle, EntryLocation { added, index });
				if (!inserted)
					replaced.push_back(metadata.Handle);
				registered.emplace_back(index, metadata);
			}
		}

		const std::vector<AssetPackEntry>& entries = added->GetEntries();
		for (const auto& [index, metadata] : registered)
		{
			RegisterAsset(metadata);
			const std::span<const AssetHandle> dependencies = added->GetDependencies(entries[index]);
			SetAssetReferences(metadata.Handle, std::vector<AssetHandle>(dependencies.begin(), dependencies.end()), entries[index].Bounds);
		}
		ST_CORE_INFO("Asset pack '{}' (version {}): {} assets{}", FileSystem::ToUTF8(added->GetPath().filename()), added->GetVersion(), registered.size(),
			replaced.empty() ? std::string() : fmt::format(", {} replacing earlier entries", replaced.size()));
		return replaced;
	}

	const AssetPackEntry* RuntimeAssetManager::FindEntry(AssetHandle handle, const AssetPack*& outPack) const
	{
		std::shared_lock<std::shared_mutex> lock(m_PacksMutex);
		auto it = m_PackEntries.find(handle);
		if (it == m_PackEntries.end())
			return nullptr;
		outPack = it->second.Pack;
		// Packs are never removed and never change once open: the entry stays valid without the lock.
		return &it->second.Pack->GetEntries()[it->second.Index];
	}

	bool RuntimeAssetManager::ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError)
	{
		const AssetPack* pack = nullptr;
		const AssetPackEntry* entry = FindEntry(metadata.Handle, pack);
		if (!entry)
		{
			if (outError)
				*outError = "The asset is not part of the asset pack";
			return false;
		}
		return pack->ReadData(*entry, outData, outError);
	}

	bool RuntimeAssetManager::ReadAssetRange(const AssetMetadata& metadata, uint64_t offset, uint64_t size, std::vector<uint8_t>& outData, std::string* outError)
	{
		const AssetPack* pack = nullptr;
		const AssetPackEntry* entry = FindEntry(metadata.Handle, pack);
		if (!entry)
		{
			if (outError)
				*outError = "The asset is not part of the asset pack";
			return false;
		}
		return pack->ReadRange(*entry, offset, size, outData, outError);
	}

}
