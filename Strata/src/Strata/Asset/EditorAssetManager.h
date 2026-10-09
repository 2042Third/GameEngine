#pragma once

#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/FileWatcher.h"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Strata
{

	struct EditorAssetManagerSpecification
	{
		std::filesystem::path AssetDirectory; // Project asset root (absolute)
		std::filesystem::path CacheDirectory; // Imported data; derived, safe to delete (absolute)
		bool WatchFiles = true;               // Re-import and hot reload files changed on disk
	};

	// Import state of a source asset (a file in the asset directory).
	struct AssetImportInfo
	{
		bool Imported = false; // False until the first import finished (or failed)
		std::string Error;     // Empty when the last import succeeded
		std::vector<std::string> Warnings;
		std::vector<AssetHandle> SubAssets;
	};

	// Asset manager of the editor. Every file with a known importer under the asset directory is an asset; its handle
	// and import settings live in a "<file>.meta" sidecar (commit it with the file). Imported data is cached per
	// handle in the cache directory and redone automatically when the source, its settings, the importer or another
	// file the import read (a model's external buffers and images) change.
	// Engine-native assets (materials, prefabs, scenes, fonts) are loaded straight from their source file.
	//
	// File operations keep handles stable: moving or renaming through MoveAsset keeps references intact. Files changed
	// outside the editor are picked up by the file watcher and hot reloaded. Main thread unless noted.
	class EditorAssetManager final : public AssetManagerBase
	{
	public:
		explicit EditorAssetManager(const EditorAssetManagerSpecification& specification);
		~EditorAssetManager() override;

		// Registers every asset, imports those whose cached data is missing or stale (in parallel; blocks until done)
		// and starts watching the directory. Call once after construction.
		void Scan();

		// Applies file changes and finished background imports (hot reload), then finalizes loads.
		void Update() override;
		// Blocks until no background import is pending and applies the results.
		void WaitForImports();

		const std::filesystem::path& GetAssetDirectory() const { return m_Specification.AssetDirectory; }
		const std::filesystem::path& GetCacheDirectory() const { return m_Specification.CacheDirectory; }

		// Source file of an asset (sub-assets: their parent's file). Empty for unknown and built-in assets. Thread-safe.
		std::filesystem::path GetAbsolutePath(AssetHandle handle) const;
		// Thread-safe. Accepts absolute paths inside the asset directory.
		AssetHandle FindAssetByAbsolutePath(const std::filesystem::path& path) const;
		// Import state of a source asset (sub-assets report their parent's).
		AssetImportInfo GetImportInfo(AssetHandle handle) const;
		nlohmann::json GetImportSettings(AssetHandle handle) const;
		// Merges the given settings into the asset's settings, saves its .meta and re-imports it (blocking).
		bool SetImportSettings(AssetHandle handle, const nlohmann::json& settings, std::string* outError = nullptr);
		// Re-imports a source asset (blocking) and hot reloads it.
		bool ReimportAsset(AssetHandle handle, std::string* outError = nullptr);

		// Copies an external file into the asset directory (relative target directory) and imports it. If the name is
		// taken, " (n)" is appended. Returns the new handle, or null.
		AssetHandle ImportExternalFile(const std::filesystem::path& sourceFile, const std::string& targetDirectory, std::string* outError = nullptr);
		// Writes a new engine-native asset (".stmat", ".stprefab", ".stscene") at a relative path that must not exist.
		AssetHandle CreateNativeAsset(const std::string& relativePath, std::span<const uint8_t> data, std::string* outError = nullptr);
		// Overwrites an engine-native asset's file. With reload, loaded copies are replaced by the saved version.
		bool SaveNativeAsset(AssetHandle handle, std::span<const uint8_t> data, bool reload, std::string* outError = nullptr);
		// Moves or renames an asset (and its .meta) within the asset directory; the handle does not change.
		bool MoveAsset(AssetHandle handle, const std::string& newRelativePath, std::string* outError = nullptr);
		// Deletes the source file, its .meta and cached data, and unregisters the asset and its sub-assets.
		bool DeleteAsset(AssetHandle handle, std::string* outError = nullptr);

		// Writes every project asset (not the built-in ones) in its stored form into an asset pack for the runtime.
		// Fails if any asset failed to import.
		bool BuildAssetPack(const std::filesystem::path& packPath, std::string* outError = nullptr);

		static constexpr std::string_view c_MetaExtension = ".meta";
		// Imports of sources larger than this run one at a time, bounding peak memory (decoded images are large).
		static constexpr uint64_t c_LargeImportSize = 64ull * 1024 * 1024;
	protected:
		bool ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError) override;
	private:
		struct SourceAsset
		{
			AssetHandle Handle = UUID::Null();
			AssetType Type = AssetType::None;
			std::string Path; // Relative to the asset directory, '/' separators
			const AssetImporter* Importer = nullptr;
			nlohmann::json Settings = nlohmann::json::object();
			AssetImportInfo Import;
			uint64_t SourceSize = 0; // File state the cached import (or registration) corresponds to
			int64_t SourceTime = 0;
			std::vector<std::string> Dependencies; // Other files the last import read (relative paths)
			uint64_t PathGeneration = 0;           // Incremented whenever the file moves
			bool ImportInFlight = false;
			bool ImportQueuedAgain = false; // Changed again while importing
		};

		// Everything an import needs, copied so imports can run on any thread.
		struct ImportRequest
		{
			AssetHandle Handle = UUID::Null();
			std::string Path;
			const AssetImporter* Importer = nullptr;
			nlohmann::json Settings;
		};

		struct ImportOutcome
		{
			AssetHandle Handle = UUID::Null();
			bool Success = false;
			std::string Error;
			std::vector<std::string> Warnings;
			std::vector<ImportedSubAsset> SubAssets; // Data already written to the cache (Data is empty)
			std::vector<std::string> Dependencies;   // Relative paths
			uint64_t SourceSize = 0;
			int64_t SourceTime = 0;
			bool FromCache = false; // The cached import was current: loaded assets already hold this data
		};

		std::filesystem::path ToAbsolute(std::string_view relativePath) const;
		std::string ToRelative(const std::filesystem::path& absolutePath) const; // Empty when outside
		std::filesystem::path GetCachePath(AssetHandle handle) const;
		std::filesystem::path GetImportRecordPath(AssetHandle handle) const;

		// Reads or creates the .meta sidecar and registers the file. Returns null if the file has no importer, lies in a
		// hidden directory, or has an invalid .meta (which is never overwritten: it may hold a merge conflict). With
		// assignNewHandle the .meta's handle is replaced (duplicated .meta files).
		AssetHandle RegisterSourceFile(const std::filesystem::path& absolutePath, bool assignNewHandle = false);
		void UnregisterSource(AssetHandle handle, bool deleteCache);
		void SetDependencies(SourceAsset& source, std::vector<std::string> dependencies); // Requires m_SourceMutex
		std::vector<AssetHandle> GetDependents(const std::filesystem::path& absolutePath) const;
		bool WriteMetaFile(const SourceAsset& source) const;
		ImportRequest MakeImportRequest(const SourceAsset& source) const;
		// True when the cached import matches the source; fills the outcome (sub-assets, warnings) from the record.
		bool LoadCurrentImport(const ImportRequest& request, ImportOutcome& outOutcome) const;
		ImportOutcome RunImport(const ImportRequest& request) const; // Any thread
		void ApplyImportOutcome(ImportOutcome outcome, bool reload);
		void QueueImport(AssetHandle handle);
		void ProcessFileChanges();
		void ProcessCompletedImports();
		AssetHandle ResolveProjectFile(const std::filesystem::path& absolutePath) const;
		// Handle -> path of every source asset, persisted in the cache so duplicated .meta files resolve the same way
		// on every scan (the original keeps its handle).
		std::unordered_map<uint64_t, std::string> LoadAssetIndex() const;
		void SaveAssetIndex();
		// Deletes cached data of assets that no longer exist and leftovers of interrupted writes (during Scan).
		void RemoveUnusedCacheFiles();
	private:
		EditorAssetManagerSpecification m_Specification;
		FileWatcher m_Watcher;

		mutable std::mutex m_SourceMutex;
		std::unordered_map<AssetHandle, SourceAsset> m_Sources;      // Top-level assets
		std::unordered_map<std::string, AssetHandle> m_SourcesByPath; // Relative path -> handle
		std::unordered_map<std::string, std::vector<AssetHandle>> m_DependentsByPath; // Dependency key -> importing sources
		bool m_AssetIndexDirty = false;                               // Guarded by m_SourceMutex

		// Files read on I/O threads (cache files, engine-native sources) are replaced, moved or deleted by the main
		// thread; on Windows that fails while a reader holds them open. Reads take the lock shared, changes exclusive.
		// Lock order: m_SourceMutex before m_FileMutex.
		mutable std::shared_mutex m_FileMutex;

		// Background imports (hot reload)
		std::mutex m_ImportMutex;
		std::condition_variable m_ImportCondition;
		uint32_t m_ImportsInFlight = 0;
		std::vector<ImportOutcome> m_CompletedImports;
	};

}
