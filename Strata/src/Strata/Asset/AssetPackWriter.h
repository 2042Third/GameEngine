#pragma once

#include "Strata/Asset/AssetPack.h"
#include "Strata/Math/AABB.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	struct SceneSettings;

	// An asset to write into a pack, in pack order.
	struct AssetPackSource
	{
		AssetMetadata Metadata;                // StoredSize (when known) weighs the asset in the progress
		std::vector<AssetHandle> Dependencies; // Its direct dependencies (AssetManagerBase::GetDependencies)
		std::optional<AABB> Bounds;            // Local bounds of meshes
	};

	struct AssetPackWriteOptions
	{
		// The format version to write: 2, or 1 for runtimes that read only version 1 (uncompressed, read whole, without
		// dependencies, bounds or checksums).
		uint32_t Version = AssetPack::c_Version;
		// zstd's compression level: higher levels write smaller packs, more slowly; reading costs the same.
		int CompressionLevel = 9;
		// Checked between blocks: once set, the write stops, removes what it wrote and fails ("cancelled").
		const std::atomic<bool>* Cancel = nullptr;
		// Called on the writing thread as the write advances, with the stored bytes written so far and in all (each asset
		// counts its metadata's StoredSize, 1 when that is unknown, until it is read).
		std::function<void(uint64_t done, uint64_t total)> Progress;
		// EditorAssetManager::WriteAssetPack: write scenes, prefabs and models cooked (CookForPack; exports do), and adjust
		// every scene's settings with AdjustSceneSettings, cooked or not (AssetPackCookOptions).
		bool CookDocuments = false;
		std::function<void(AssetHandle scene, SceneSettings& settings)> AdjustSceneSettings;
	};

	struct AssetPackWriteStats
	{
		uint64_t FileSize = 0;
		uint64_t StoredBytes = 0;      // The assets' stored forms before compression
		uint32_t Blocks = 0;
		uint32_t CompressedBlocks = 0; // The other blocks are stored as they are
	};

	// Writes asset packs (see AssetPack for the format). Tooling: the editor's export builds packs with it, games only
	// read them, so shipped games link none of the compressor.
	class AssetPackWriter
	{
	public:
		// Supplies an asset's stored bytes while writing (called on the writing thread). Return false to abort with an error.
		using DataProvider = std::function<bool(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError)>;

		// Writes the assets in the given order, atomically: a temporary file next to `path` replaces it once complete, and is
		// removed when anything fails. Blocks are compressed in parallel on the job system. Refuses null and duplicate
		// handles and unknown types. May run on any thread.
		static bool Write(const std::filesystem::path& path, std::span<const AssetPackSource> assets, const DataProvider& provider,
			const AssetPackWriteOptions& options = {}, AssetPackWriteStats* outStats = nullptr, std::string* outError = nullptr);
		// Assets without dependencies or bounds, with the default options.
		static bool Write(const std::filesystem::path& path, const std::vector<AssetMetadata>& assets, const DataProvider& provider, std::string* outError = nullptr);
	};

}
