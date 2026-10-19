#pragma once

#include "Strata/Asset/AssetTypes.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// The parts of EditorAssetManager's registration of source files that need no manager: reading .meta files, walking the
// asset directory, resolving handles that several files claim, and the time slices the main thread inserts prepared
// sources in. Jobs use them while a project's assets are scanned (EditorAssetManager::BeginScan).
namespace Strata
{

	// Where a scan of the asset directory is (EditorAssetManager::GetScanProgress).
	enum class AssetScanPhase : uint8_t
	{
		Walking = 0, // Listing the asset directory (Total unknown, 0; Done counts the files found)
		Registering, // Preparing each file on the background threads: its .meta, its importer, its cached import
		Applying     // Inserting the prepared files into the registry on the main thread, a slice per frame
	};

	const char* AssetScanPhaseToString(AssetScanPhase phase);

	struct AssetScanProgress
	{
		AssetScanPhase Phase = AssetScanPhase::Walking;
		uint64_t Done = 0;
		uint64_t Total = 0;
	};

	// How EditorAssetManager registers prepared files on the main thread.
	struct AssetRegistrationOptions
	{
		// A frame inserts until this long has passed, checked every EntriesPerCheck entries.
		uint32_t SliceMicroseconds = 1000;
		uint32_t EntriesPerCheck = 64;
		// Files a prepare job takes at once.
		uint32_t PrepareBatch = 256;
		// The clock slices are measured with, in microseconds; empty: the steady clock. Tests pass one of their own, so that
		// what a slice inserts is deterministic.
		std::function<uint64_t()> Clock;
	};

	// The time slice of one frame's registration work: HasTime is asked after each entry and reads the clock every
	// EntriesPerCheck entries.
	class RegistrationSlice
	{
	public:
		// Without a budget (blocking scans) the slice never ends.
		RegistrationSlice(const AssetRegistrationOptions& options, bool unlimited);

		bool HasTime();
		uint64_t GetEntries() const { return m_Entries; }
	private:
		uint64_t Now() const;
	private:
		const AssetRegistrationOptions& m_Options;
		bool m_Unlimited = false;
		uint64_t m_Start = 0;
		uint64_t m_Entries = 0;
		bool m_Ended = false;
	};

	enum class AssetMetaStatus : uint8_t
	{
		Missing = 0,
		Invalid,
		Valid
	};

	// A source file's ".meta" sidecar: its handle and import settings.
	struct AssetMeta
	{
		AssetMetaStatus Status = AssetMetaStatus::Missing;
		AssetHandle Handle = UUID::Null();
		nlohmann::json Settings = nlohmann::json::object();
		std::string Error; // Invalid: why
	};

	inline constexpr uint32_t c_AssetMetaVersion = 1;

	// Reads a .meta file (Missing when there is none). Any thread.
	AssetMeta ReadAssetMeta(const std::filesystem::path& metaPath);
	// Writes a .meta file (atomically). Any thread.
	bool WriteAssetMeta(const std::filesystem::path& metaPath, AssetHandle handle, AssetType type, const nlohmann::json& settings);

	// What a walk of the asset directory found: every file that is not hidden and not a .meta file, sorted, and every folder
	// that is not hidden (relative to the directory, '/' separators).
	struct AssetDirectoryListing
	{
		std::vector<std::filesystem::path> Files;
		std::vector<std::string> Folders;
		std::string Error; // The walk ended early (the listing holds what it found)
	};

	// Walks the asset directory (any thread): stops early once `cancel` is set, and counts the files found in `found`.
	AssetDirectoryListing WalkAssetDirectory(const std::filesystem::path& directory, const std::atomic<bool>* cancel = nullptr,
		std::atomic<uint64_t>* found = nullptr);

	// Files copied together with their .meta share a handle. The file the records store's path names keeps it (the handle's
	// recorded place); without one the shortest path does, which is the original for the usual "Name - Copy" / "Name copy" /
	// "Name (1)" naming of file managers. `handles` holds each file's .meta handle (null when it has none) and `paths` each
	// file's relative path, in the same order. Returns the files that need a new handle, in order, each with the one that
	// keeps the handle.
	struct DuplicateHandle
	{
		size_t File = 0;
		size_t Keeper = 0;
	};
	std::vector<DuplicateHandle> FindDuplicateHandles(std::span<const AssetHandle> handles, std::span<const std::string> paths,
		const std::unordered_map<AssetHandle, std::string>& recordedPaths);

}
