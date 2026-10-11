#pragma once

#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Core/FileWatcher.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Strata
{

	// How EditorAssetManager::Scan imports the assets whose cached import is missing or stale.
	enum class ScanImports : uint8_t
	{
		Wait = 0,    // In parallel before Scan returns (tools, tests, scripted editor runs)
		InBackground // As background jobs that Update applies: Scan returns once every asset is registered (the editor)
	};

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
	//
	// Background work never holds the main thread. An import whose result would be out of date (its file changed, moved or
	// was deleted, its settings changed, it is imported again) or that is no longer wanted (Close) is cancelled, and the
	// manager goes on without waiting for it: a queued one never starts, a running one stops at its importer's next
	// cancellation check (ImportContext::Cancel) and stores nothing, because the cache files and the record of an import are
	// published together under the exclusive file lock, by an import that is still wanted when it holds the lock. Jobs only
	// use what they share with the manager (BackgroundWork), never the manager itself, so it may be gone while a cancelled
	// job still runs. Background work reads project files under the shared file lock, so moving or deleting a file never
	// fails because an import has it open.
	class EditorAssetManager final : public AssetManagerBase
	{
	public:
		explicit EditorAssetManager(const EditorAssetManagerSpecification& specification);
		// Close, then waits for the loads in flight (reads of single files).
		~EditorAssetManager() override;

		// Registers every asset, imports those whose cached data is missing or stale and starts watching the directory. Call
		// once after construction. With ScanImports::InBackground the imports run as background jobs (like re-imports after
		// file changes) that Update applies; meanwhile a load of such an asset waits for its import instead of failing
		// (AssetManagerBase::IsAwaitingImport), its sub-assets appear once it is applied, and HasPendingImports is true.
		void Scan(ScanImports imports = ScanImports::Wait);
		// Ends the manager's background work for good and returns at once, without waiting for any of it: stops the file
		// watcher and cancels every import and search of references (queued ones never start; running ones stop at their next
		// cancellation check on their own and store nothing). Nothing is imported in the background afterwards and results
		// that were not applied are dropped; loads keep working. The editor calls it when the project closes, the destructor
		// too. Imports cancelled before they stored their result run again when the project is next opened.
		void Close();

		// Applies file changes and finished background imports (hot reload), then finalizes loads.
		void Update() override;
		// Blocks until no background import or search is pending and applies the results: tools and tests (it waits for
		// every cook). Editors poll HasPendingImports or IsImportPending between frames instead.
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
		// The importer of a source asset (sub-assets: their parent's); null for unknown and built-in assets. Thread-safe.
		const AssetImporter* GetImporter(AssetHandle handle) const;
		// Merges the given settings into the asset's settings, saves its .meta and re-imports it (blocking).
		bool SetImportSettings(AssetHandle handle, const nlohmann::json& settings, std::string* outError = nullptr);
		// Re-imports a source asset on the calling thread and hot reloads it (tools, tests, asset.import). A background import
		// of the asset is cancelled first, without waiting for it (its result would be out of date); other imports go on.
		bool ReimportAsset(AssetHandle handle, std::string* outError = nullptr);
		// Like SetImportSettings, but the import runs as a background job like a hot reload: returns once the .meta is saved
		// and the import is queued. Update applies the result and reloads what was loaded; IsImportPending tells when.
		// Settings that equal those of the cached import find it current, so nothing is imported again. An import of the
		// asset that is queued or running already is cancelled: this one replaces it.
		bool SetImportSettingsInBackground(AssetHandle handle, const nlohmann::json& settings, std::string* outError = nullptr);
		// Like ReimportAsset, in the background (see SetImportSettingsInBackground). The import runs even when the cached one
		// is current. Engine documents (stored as their source) are reloaded at once instead.
		bool ReimportAssetInBackground(AssetHandle handle, std::string* outError = nullptr);
		// Whether an import of the source asset (for a sub-asset: of its parent) is queued or running, or finished without
		// Update having applied it yet. False for unknown assets.
		bool IsImportPending(AssetHandle handle) const;
		// Source assets whose import is queued or running, or finished and not applied yet (what Scan left to the
		// background, re-imports). Thread-safe.
		uint32_t GetPendingImportCount() const;

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
		// Moves or renames an asset (and its .meta) within the asset directory; the handle does not change. Never waits for
		// an import: an import of the asset that is queued or running is cancelled before the file moves and runs again at
		// the new place; other imports go on (the file watcher re-imports those that read the file as a dependency).
		bool MoveAsset(AssetHandle handle, const std::string& newRelativePath, std::string* outError = nullptr);
		// Deletes the source file, its .meta and cached data, and unregisters the asset and its sub-assets. Never waits for
		// an import: the asset's own import is cancelled before the files go and stores nothing; other imports go on.
		bool DeleteAsset(AssetHandle handle, std::string* outError = nullptr);

		// Whether background imports (hot reload), or searches of what changed engine-native assets reference, are queued,
		// running, or finished and not applied yet (Update applies them).
		bool HasPendingImports() const;

		// The assets a pack built now would hold (every project asset, not the built-in ones, or with Prune what the scenes
		// and AlwaysInclude need), in pack order, with their dependencies and bounds: a snapshot, which WriteAssetPack can
		// write on another thread. Main thread; finish imports first (WaitForImports, or until HasPendingImports is false).
		// Fails for AlwaysInclude entries that name no asset of the project (with Prune).
		bool PlanAssetPack(const AssetPackBuildOptions& options, AssetPackBuildPlan& outPlan, std::string* outError = nullptr) const;
		// Whether every entry of an AlwaysInclude list (AssetPackBuildOptions::AlwaysInclude) names an asset or a folder of
		// assets of the project, as a pruned PlanAssetPack checks them, without planning (which follows every scene's
		// dependencies). The error names the first entry that names nothing.
		bool CheckAlwaysInclude(const std::vector<std::string>& alwaysInclude, std::string* outError = nullptr) const;
		// How many plans PlanAssetPack made: planning walks the dependencies of every scene and asset, so code that plans on
		// the main thread does it once (tests count it).
		uint64_t GetPackPlanCount() const { return m_PackPlans.load(std::memory_order_relaxed); }
		// Writes a planned pack, reading each asset's stored form like loads do. Fails if an asset failed to import, and when
		// a document (scene, prefab, model, material) changed since the plan to reference what the plan did not record for
		// it (naming the document), so that a pack never disagrees with its plan. Any thread; the manager must outlive the
		// call.
		bool WriteAssetPack(const std::filesystem::path& packPath, const AssetPackBuildPlan& plan, const AssetPackWriteOptions& options = {},
			AssetPackWriteStats* outStats = nullptr, std::string* outError = nullptr);
		// Waits for every import (WaitForImports), plans and writes, blocking: tools and tests. The editor's export
		// (GameExport) instead polls HasPendingImports between frames, plans once none is pending and writes on an I/O
		// thread, so it never blocks a frame.
		bool BuildAssetPack(const std::filesystem::path& packPath, std::string* outError = nullptr, const AssetPackBuildOptions& options = {},
			AssetPackBuildPlan* outPlan = nullptr, AssetPackWriteStats* outStats = nullptr);

		static constexpr std::string_view c_MetaExtension = ".meta";
		// Imports of sources larger than this run one at a time, bounding peak memory (decoded images are large).
		static constexpr uint64_t c_LargeImportSize = 64ull * 1024 * 1024;
	protected:
		bool ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError) override;
		// Assets of a source whose first import has not been applied (Scan in the background, a file added since).
		bool IsAwaitingImport(const AssetMetadata& metadata) const override;
		// Reads the range from the cache file or the source file, without reading the rest.
		bool ReadAssetRange(const AssetMetadata& metadata, uint64_t offset, uint64_t size, std::vector<uint8_t>& outData, std::string* outError) override;
	private:
		// A background job of the manager (an import, or a search of what a document references), shared with its closure.
		// Whoever no longer wants it cancels it (BackgroundWork::Cancel): a job checks the flag before it starts, its importer
		// at every cancellation check (the flag is its ImportContext::Cancel), and a cancelled job hands nothing over and
		// stores nothing.
		struct BackgroundJob
		{
			std::atomic<bool> Cancel = false;
			std::atomic<bool> Started = false; // It began to run (Close logs how many it stopped while they ran)
			bool Force = false;                // An import that runs even when the cached one is current (main thread)
		};

		// What background jobs share with the manager: the directories, the file lock, the wanted jobs and their results
		// (EditorAssetManager.cpp). Jobs keep it alive and never touch the manager, which therefore never waits for them.
		struct BackgroundWork;

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
			// The wanted background import (queued, running, or finished and not applied yet); null without one. Only its
			// outcome is applied: one that replaced it cancelled it (StopImport).
			Ref<BackgroundJob> ImportJob;
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
			bool Force = false; // Import even when the cached import is current (a requested re-import)
			// It replaced an import that may have stored its result before it was cancelled, unapplied: a cached import found
			// current may be that one, which loaded assets do not hold yet, so they are reloaded.
			bool Replaced = false;
		};

		struct ImportOutcome
		{
			AssetHandle Handle = UUID::Null();
			Ref<BackgroundJob> Job; // Of a background import: applied only while it is still the source's ImportJob
			bool Cancelled = false; // Stopped by its cancellation flag: it stored nothing and is never applied
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
			// Of a background search: the record to write once Update applies the result (only while it is the latest search,
			// so a search that a save overtook never overwrites the save's record).
			std::optional<nlohmann::json> Record;
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
		void ApplyImportOutcome(ImportOutcome outcome, bool reload);
		// Queues an import as a background job (JobPriority::Background; hot reload); with force, also when the cached import is
		// current. An import of the source that is queued or running read an earlier state: it is cancelled (StopImport) and
		// this one replaces it, forced if either is. `replacing`: the caller stopped the source's import itself (see
		// ImportRequest::Replaced). Does nothing once the manager is closed.
		void QueueImport(AssetHandle handle, bool force = false, bool replacing = false);
		// Submits the job QueueImport made the source's ImportJob.
		void SubmitImport(const Ref<BackgroundJob>& job, const ImportRequest& request);
		// Cancels the source's background import, without waiting for it, and forgets it: a queued one never starts, a running
		// one stops at its next cancellation check and stores nothing. Returns it (null without one). Requires m_SourceMutex.
		Ref<BackgroundJob> StopImport(SourceAsset& source);
		// Merges settings into the asset's and saves its .meta (an invalid .meta is never overwritten).
		bool WriteImportSettings(AssetHandle handle, const nlohmann::json& settings, std::string* outError);
		// Engine documents are read from their source: a re-import only takes the file's current state and reloads it.
		void ReloadDirectSource(AssetHandle handle);
		// Queues the import of a source asset (with force, also when the cached import is current), or reloads an engine
		// document. Fails for assets that are no source of the project.
		bool QueueBackgroundImport(AssetHandle handle, bool force, std::string* outError);
		void ProcessFileChanges();
		void ProcessCompletedImports();
		// Handle -> path of every source asset, persisted in the cache so duplicated .meta files resolve the same way
		// on every scan (the original keeps its handle).
		std::unordered_map<uint64_t, std::string> LoadAssetIndex() const;
		void SaveAssetIndex();
		// Deletes cached data of assets that no longer exist and leftovers of interrupted writes (during Scan).
		void RemoveUnusedCacheFiles();

		// Records what an engine-native asset references (BackgroundWork::FindNativeReferences) with the base (main thread).
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
		uint32_t m_SourcesImporting = 0;                              // Sources with an ImportJob; guarded by m_SourceMutex

		// The file lock, the background jobs and their results, shared with the jobs. Lock order: m_SourceMutex before the
		// work's file lock and before its job lock.
		Ref<BackgroundWork> m_Work;
		bool m_Closed = false; // Close was called: no background work starts (main thread)

		mutable std::atomic<uint64_t> m_PackPlans = 0; // PlanAssetPack calls (GetPackPlanCount)
	};

}
