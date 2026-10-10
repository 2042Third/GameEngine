#pragma once

#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Core/FileWatcher.h"

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
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

	// What an asset pack built from the project holds, and in which order (EditorAssetManager::PlanAssetPack).
	struct AssetPackBuildOptions
	{
		// Packs are ordered by first use, so that loading a scene reads the file front to back: this scene's dependency
		// closure breadth first (AssetManagerBase::GetDependencyClosure), then the closure of every other scene by path,
		// then the remaining assets by path. Null: scenes by path.
		AssetHandle StartScene = UUID::Null();
		// Only what the scenes use (their closures) and what AlwaysInclude names, with what that uses. Scripts can find
		// assets by path, so a game that does must name them in AlwaysInclude (the project's Export.AlwaysInclude).
		bool Prune = false;
		// Assets ("Prefabs/Enemy.stprefab": the file's asset and its sub-assets) and folders ("Prefabs" or "Prefabs/": every
		// asset below) relative to the asset directory, with '/' separators. With Prune, an entry that names nothing fails.
		std::vector<std::string> AlwaysInclude;
	};

	struct AssetPackBuildPlan
	{
		std::vector<AssetPackSource> Assets;  // In pack order, with their dependencies and bounds
		std::vector<AssetMetadata> Excluded;  // Left out by Prune, by path
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
		// `references`: what the document references, when the caller has it (AssetDependencies::ScanDocument of the JSON it
		// serialized), so that saving a large document does not parse it once more; null: the data is scanned.
		AssetHandle CreateNativeAsset(const std::string& relativePath, std::span<const uint8_t> data, std::string* outError = nullptr,
			const std::vector<AssetHandle>* references = nullptr);
		// Overwrites an engine-native asset's file. With reload, loaded copies are replaced by the saved version.
		// `references` as for CreateNativeAsset.
		bool SaveNativeAsset(AssetHandle handle, std::span<const uint8_t> data, bool reload, std::string* outError = nullptr,
			const std::vector<AssetHandle>* references = nullptr);
		// Moves or renames an asset (and its .meta) within the asset directory; the handle does not change.
		bool MoveAsset(AssetHandle handle, const std::string& newRelativePath, std::string* outError = nullptr);
		// Deletes the source file, its .meta and cached data, and unregisters the asset and its sub-assets.
		bool DeleteAsset(AssetHandle handle, std::string* outError = nullptr);

		// Whether background imports (hot reload), or searches of what changed engine-native assets reference, are queued,
		// running, or finished and not applied yet (Update applies them).
		bool HasPendingImports() const;

		// The assets a pack built now would hold (every project asset, not the built-in ones, or with Prune what the scenes
		// and AlwaysInclude need), in pack order, with their dependencies and bounds: a snapshot, which WriteAssetPack can
		// write on another thread. Main thread; finish imports first (WaitForImports, or until HasPendingImports is false).
		// Fails for AlwaysInclude entries that name no asset of the project (with Prune).
		bool PlanAssetPack(const AssetPackBuildOptions& options, AssetPackBuildPlan& outPlan, std::string* outError = nullptr) const;
		// Writes a planned pack, reading each asset's stored form like loads do. Fails if an asset failed to import, and when
		// a document (scene, prefab, model, material) changed since the plan to reference what the plan did not record for
		// it (naming the document), so that a pack never disagrees with its plan. Any thread; the manager must outlive the
		// call.
		bool WriteAssetPack(const std::filesystem::path& packPath, const AssetPackBuildPlan& plan, const AssetPackWriteOptions& options = {},
			AssetPackWriteStats* outStats = nullptr, std::string* outError = nullptr);
		// Waits for imports, plans and writes, blocking (tools and tests; the editor's export plans and writes on an I/O
		// thread).
		bool BuildAssetPack(const std::filesystem::path& packPath, std::string* outError = nullptr, const AssetPackBuildOptions& options = {},
			AssetPackBuildPlan* outPlan = nullptr, AssetPackWriteStats* outStats = nullptr);

		static constexpr std::string_view c_MetaExtension = ".meta";
		// Imports of sources larger than this run one at a time, bounding peak memory (decoded images are large).
		static constexpr uint64_t c_LargeImportSize = 64ull * 1024 * 1024;
	protected:
		bool ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError) override;
		// Reads the range from the cache file or the source file, without reading the rest.
		bool ReadAssetRange(const AssetMetadata& metadata, uint64_t offset, uint64_t size, std::vector<uint8_t>& outData, std::string* outError) override;
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
			// Of assets stored as-is: incremented whenever a search of what it references starts, so that only the latest
			// search's result is recorded (see QueueNativeReferences).
			uint64_t ReferencesGeneration = 0;
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
			std::vector<ImportedSubAsset> SubAssets; // Data already written to the cache (Data is empty); with references and bounds
			std::vector<std::string> Dependencies;   // Relative paths
			std::vector<AssetHandle> References;     // What the asset itself references (see AssetDependencies)
			std::optional<AABB> Bounds;
			uint64_t SourceSize = 0;
			int64_t SourceTime = 0;
			bool FromCache = false; // The cached import was current: loaded assets already hold this data
		};

		// What an engine-native asset (stored as its source, never imported) references, from its cached record or a scan.
		struct NativeReferences
		{
			AssetHandle Handle = UUID::Null();
			std::vector<AssetHandle> References;
			bool Scanned = false; // Found (from the record or a scan)
			uint64_t Generation = 0; // The source's ReferencesGeneration when the search started
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

		// References of engine-native assets (AssetDependencies::Scan of the source), cached per content in the asset's
		// import record. Any thread: reads the record, or scans the file (`data` when given, else read; `references` when the
		// caller knows them) and writes the record.
		NativeReferences FindNativeReferences(AssetHandle handle, AssetType type, const std::string& relativePath, std::span<const uint8_t> data = {},
			const std::vector<AssetHandle>* references = nullptr) const;
		// Records them with the base (main thread).
		void ApplyNativeReferences(const NativeReferences& references);
		// Records the references of a source asset stored as-is that the editor has just written: `references` when the
		// caller knows them, else found in `data` (its new contents). Does nothing for imported assets. Main thread.
		void RefreshNativeReferences(AssetHandle handle, std::span<const uint8_t> data, const std::vector<AssetHandle>* references);
		// Finds the references of a source asset stored as-is that was added or changed on disk on an I/O job (reading the
		// file, and parsing it unless its record is current, takes long for large scenes); Update records them unless a later
		// search started meanwhile. Does nothing for imported assets. Main thread.
		void QueueNativeReferences(AssetHandle handle);
		// The stored form's file of an asset (the cache file, or the source of assets stored as-is): calls `read` with it under
		// the shared file lock, again at its new place when it moved meanwhile. False when the asset is unknown, its import
		// failed or `read` fails (outError says which).
		bool ReadStoredFile(const AssetMetadata& metadata, const std::function<bool(const std::filesystem::path& path)>& read, std::string* outError);
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
		mutable std::mutex m_ImportMutex;
		std::condition_variable m_ImportCondition;
		uint32_t m_ImportsInFlight = 0;
		std::vector<ImportOutcome> m_CompletedImports;
		std::vector<NativeReferences> m_CompletedReferences; // Of QueueNativeReferences (counted in m_ImportsInFlight)
	};

}
