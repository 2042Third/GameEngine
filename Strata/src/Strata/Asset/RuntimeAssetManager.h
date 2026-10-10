#pragma once

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/AssetPack.h"

#include <filesystem>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace Strata
{

	// Asset manager of shipped games: every asset comes from asset packs built by the editor. Assets load asynchronously
	// like in the editor, but nothing is imported. Dependencies and mesh bounds come from the packs' tables.
	//
	// Packs are mounted in order, the game's own first: an entry of a later pack replaces the entry with the same handle of
	// the earlier ones (patches, downloadable content), and assets loaded from a replaced entry are reloaded from the new
	// one. Mounted packs stay open (one file handle each) until the manager is destroyed.
	class RuntimeAssetManager final : public AssetManagerBase
	{
	public:
		// Mounts the game's pack. Returns null (with an error) when the pack cannot be opened.
		static Ref<RuntimeAssetManager> Create(const std::filesystem::path& packPath, std::string* outError = nullptr);
		~RuntimeAssetManager() override;

		// Mounts another pack over the mounted ones (see above). Fails, changing nothing, when the pack cannot be opened.
		// Main thread.
		bool Mount(const std::filesystem::path& packPath, std::string* outError = nullptr);

		// The game's pack (the first mounted).
		const AssetPack& GetPack() const { return *m_Packs.front(); }
		// Every mounted pack, in mount order. Main thread.
		const std::vector<Scope<AssetPack>>& GetPacks() const { return m_Packs; }
	protected:
		bool ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError) override;
		bool ReadAssetRange(const AssetMetadata& metadata, uint64_t offset, uint64_t size, std::vector<uint8_t>& outData, std::string* outError) override;
	private:
		RuntimeAssetManager() = default;

		// Registers the entries of a pack (main thread): returns the handles that replaced entries of earlier packs.
		std::vector<AssetHandle> AddPack(Scope<AssetPack> pack);
		// The pack and entry that provide an asset; null when no mounted pack has it.
		const AssetPackEntry* FindEntry(AssetHandle handle, const AssetPack*& outPack) const;
	private:
		struct EntryLocation
		{
			const AssetPack* Pack = nullptr;
			size_t Index = 0;
		};

		std::vector<Scope<AssetPack>> m_Packs;
		// Read on I/O threads, changed by Mount: guarded by m_PacksMutex (packs themselves are never removed).
		mutable std::shared_mutex m_PacksMutex;
		std::unordered_map<AssetHandle, EntryLocation> m_PackEntries;
	};

}
