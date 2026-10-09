#pragma once

#include "Strata/Asset/Asset.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	// Asset pack (".stpak"): every asset of a game in its stored form, in one file the runtime streams from.
	//
	// Layout (little endian): header { "STPK", version, entry count, reserved, table offset, table size }, the asset
	// data blobs back to back, then the entry table (handle, type, parent, data offset, data size, path, sub-asset
	// key, name per entry). The table comes last so packs are written in one pass.
	struct AssetPackEntry
	{
		AssetMetadata Metadata;
		uint64_t Offset = 0;
		uint64_t Size = 0;
	};

	class AssetPack
	{
	public:
		static constexpr uint32_t c_Magic = 0x4B505453; // "STPK"
		static constexpr uint32_t c_Version = 1;

		// Supplies an asset's stored bytes while writing. Return false to abort with an error.
		using DataProvider = std::function<bool(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError)>;

		// Writes a pack atomically (a temporary file replaces the destination once complete).
		static bool Write(const std::filesystem::path& path, const std::vector<AssetMetadata>& assets, const DataProvider& provider, std::string* outError = nullptr);

		// Reads and validates the entry table.
		static Scope<AssetPack> Open(const std::filesystem::path& path, std::string* outError = nullptr);

		const std::vector<AssetPackEntry>& GetEntries() const { return m_Entries; }
		const std::filesystem::path& GetPath() const { return m_Path; }

		// Thread-safe.
		bool ReadData(const AssetPackEntry& entry, std::vector<uint8_t>& outData, std::string* outError = nullptr) const;
	private:
		AssetPack() = default;
	private:
		std::filesystem::path m_Path;
		uint64_t m_FileSize = 0;
		std::vector<AssetPackEntry> m_Entries;
	};

}
