#pragma once

#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Core/FileWatcher.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
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
		// Where MoveToTrash puts deleted assets (the editor's "<project>/.strata/Trash"; local data, never committed). Empty:
		// MoveToTrash fails, and only DeleteAsset deletes (tools).
		std::filesystem::path TrashDirectory;
		bool WatchFiles = true;               // Re-import and hot reload files changed on disk
	};

	// What EditorAssetManager::MoveToTrash moved, so that RestoreFromTrash puts it back: one asset (its file, its .meta and
	// its cached import) or a folder with everything in it (and the cached imports of its assets).
	struct TrashedAssets
	{
		std::string Path;                 // Where it was, relative to the asset directory ('/' separators)
		bool Folder = false;
		std::filesystem::path Entry;      // The trash folder that holds it ("<trash>/<UTC time>[-n]")
		std::vector<AssetHandle> Handles; // The source assets it held, by path
	};

	// A file or folder from outside the project that EditorAssetManager::ImportExternal copied in, and what became of it.
	struct ExternalImportItem
	{
		std::filesystem::path Source; // The file or folder outside the project
		std::string Path;             // Where it went, relative to the asset directory (the folder, for folder imports)
		bool Folder = false;
		// The source assets it brought: a file's own asset first, then the files it reads that are assets themselves (a model's
		// images); a folder's every asset.
		std::vector<AssetHandle> Assets;
		std::vector<std::string> Files; // Every file copied (with .meta files), relative to the asset directory
		std::string Error;              // Empty while it is fine
		bool RolledBack = false;        // Its failed files were removed again (no KeepOnFailure)
	};

	// A source asset as views of the asset directory list it (EditorAssetManager::ListAssets).
	struct AssetListing
	{
		AssetHandle Handle = UUID::Null();
		AssetType Type = AssetType::None;
		std::string Path;                  // Relative to the asset directory, '/' separators
		std::string Error;                 // Of its last import (empty when it succeeded or has not run)
		size_t WarningCount = 0;
		std::vector<AssetHandle> SubAssets;
		bool Importing = false;            // Its import is queued, running, or finished and not applied yet
	};

	// Where an import of external files is (EditorAssetManager::ImportExternal).
	enum class ExternalImportState : uint8_t
	{
		Copying = 0, // On an I/O job
		Importing,   // On the background threads, like re-imports
		Finished
	};

	struct ExternalImportRequest
	{
		std::vector<std::filesystem::path> Files;       // Absolute paths of files to copy and import
		std::vector<std::filesystem::path> Directories; // Absolute paths of folders to copy, keeping their tree, and import
		std::string TargetDirectory;                    // Relative to the asset directory ('/' separators, empty: the root)
		// Files of imports that failed stay in the project (asset.import keepOnFailure), with their errors; otherwise they are
		// removed with their .meta files.
		bool KeepOnFailure = false;
	};

	// An import of external files that EditorAssetManager::Update advances (main thread).
	class ExternalImport
	{
	public:
		ExternalImportState GetState() const { return m_State; }
		bool IsFinished() const { return m_State == ExternalImportState::Finished; }
		// Every item in the order requested (complete once copied).
		const std::vector<ExternalImportItem>& GetItems() const { return m_Items; }
		// Whether an item failed (its Error is set).
		bool HasFailures() const;
		// Why the import ended before its items were done (the project closed); empty otherwise.
		const std::string& GetError() const { return m_Error; }
	private:
		friend class EditorAssetManager;

		// What the copy job shares with the import (EditorAssetManager.cpp): the job never touches the manager.
		struct CopyTask;

		ExternalImportRequest m_Request;
		std::string m_Error;
		ExternalImportState m_State = ExternalImportState::Copying;
		std::vector<ExternalImportItem> m_Items;
		std::vector<std::string> m_CreatedFolders; // Folders the copy created, relative, parents first
		Ref<CopyTask> m_Task;
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
		// A fingerprint of an asset's stored form that lasts across sessions (unlike AssetSourceRevisions): its source file's
		// size and time, its import settings, its importer's version and whether its last import succeeded (sub-assets: their
		// file's). It changes whenever a re-import can change what is stored, so caches of derived data on disk (the editor's
		// thumbnails) know when theirs is stale. 0 for unknown and built-in assets. Thread-safe.
		uint64_t GetContentStamp(AssetHandle handle) const;
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

		// Copies an external file into the asset directory (relative target directory) and imports it on the calling thread:
		// tools and tests (asset.import uses ImportExternal). It is copied like ImportExternal copies files, with the files it
		// reads; a failed import keeps them (the asset reports its error). Returns the new handle, or null.
		AssetHandle ImportExternalFile(const std::filesystem::path& sourceFile, const std::string& targetDirectory, std::string* outError = nullptr);
		// Copies external files and folders into the asset directory without blocking a frame: the copy runs on an I/O job,
		// the imports on the background threads like re-imports, and Update advances them. A file goes into the target
		// directory (" (n)" appended when its name is taken). Files its importer reads besides it (AssetImporter::FindExternalFiles:
		// a glTF model's buffers and images) come along, keeping their layout below their nearest common folder, in a new
		// folder named after it; they may lie at most two folders above it. A folder goes there with its tree: its asset files,
		// their .meta files when they have them (handles that are taken get new ones) and the files models in it read inside
		// it. An item whose import fails is removed again (its files and .meta files; for a folder, the assets that failed)
		// unless KeepOnFailure; its error stays in the item. Fails at once (null) for a target outside the asset directory and
		// once closed. Closing ends it: a copy that has not finished removes what it copied.
		Ref<ExternalImport> ImportExternal(const ExternalImportRequest& request, std::string* outError = nullptr);
		// Imports of external files that have not finished (ImportExternal).
		uint32_t GetExternalImportCount() const { return static_cast<uint32_t>(m_ExternalImports.size()); }
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
		// the new place; other imports go on (the file watcher re-imports those that read the file as a dependency). Its own
		// path does nothing; a name that differs only in case renames it where the file system ignores case. Fails when
		// another file is at the destination.
		bool MoveAsset(AssetHandle handle, const std::string& newRelativePath, std::string* outError = nullptr);
		// Deletes the source file, its .meta and cached data, and unregisters the asset and its sub-assets. Never waits for
		// an import: the asset's own import is cancelled before the files go and stores nothing; other imports go on. Tools;
		// the editor moves assets to the trash instead (MoveToTrash), which can be undone.
		bool DeleteAsset(AssetHandle handle, std::string* outError = nullptr);

		// Moves an asset's file (with its .meta and its cached import), or a folder with everything in it, from the asset
		// directory into a new folder of the trash directory, and unregisters the assets it held, so that RestoreFromTrash can
		// bring them back with the same handles. Imports of those assets are cancelled first (never waited for). Fails,
		// moving nothing, without a trash directory, for paths outside the asset directory, the asset directory itself and
		// what is neither an asset file nor a folder.
		bool MoveToTrash(const std::string& relativePath, TrashedAssets& outTrashed, std::string* outError = nullptr);
		// Puts what MoveToTrash moved back where it was and registers its assets with their handles (imports cached with them
		// are current again, so nothing is imported anew), then removes the trash folder. Fails, restoring nothing, when
		// something else is at that place now or the trash folder is gone.
		bool RestoreFromTrash(const TrashedAssets& trashed, std::string* outError = nullptr);
		// Removes trash folders older than `maxAge` (on an I/O job; the editor calls it when a project opens).
		void PurgeTrash(std::chrono::hours maxAge);

		// Creates a folder (and its parents) in the asset directory. Fails for paths outside it, in hidden folders, and when
		// something exists there already.
		bool CreateFolder(const std::string& relativePath, std::string* outError = nullptr);
		// Removes an empty folder of the asset directory (the undo of CreateFolder). Fails when it holds anything.
		bool RemoveEmptyFolder(const std::string& relativePath, std::string* outError = nullptr);
		// Moves or renames a folder with everything in it (also files that are no assets, such as a model's buffers), keeping
		// every handle: the assets in it are registered at their new paths like MoveAsset does, imports of them that run are
		// cancelled and run again there, and models in it whose files moved with them are imported again. Its own path does
		// nothing, and a name that differs only in case renames it where the file system ignores case. Fails, moving nothing,
		// when something else is at the destination or it lies inside the folder or outside the asset directory.
		bool MoveFolder(const std::string& from, const std::string& to, std::string* outError = nullptr);

		//////////////////////////////////////////////////////////////////////////
		// Browsing (the Content Browser): folders, the assets in each, and what references an asset
		//////////////////////////////////////////////////////////////////////////

		// Changes whenever a source asset is added, removed, moved or imported (its error, warnings or sub-assets may differ),
		// an import starts or ends, and a folder is created, moved or removed, so that views of the asset directory rebuild
		// only then. Thread-safe.
		uint64_t GetChangeVersion() const { return m_ChangeVersion.load(std::memory_order_acquire); }
		// Every folder of the asset directory that is not hidden, sorted ('/' separators, the root left out). Folders created
		// outside the editor appear when a file is added in them, or after RescanFolders.
		std::vector<std::string> GetFolders() const;
		// Whether the folder exists in the index ("" is the asset directory).
		bool HasFolder(std::string_view relativePath) const;
		// The source assets directly in a folder ("" for the root), sorted by path.
		std::vector<AssetHandle> GetAssetsInFolder(std::string_view relativePath) const;
		// The source assets in a folder and every folder below it, sorted by path.
		std::vector<AssetHandle> GetAssetsBelowFolder(std::string_view relativePath) const;
		// The source assets directly in a folder (or, with `recursive`, also below it) with what a view shows of them, in one
		// look at the index, sorted by path: what the Content Browser lists when GetChangeVersion changed.
		std::vector<AssetListing> ListAssets(std::string_view relativePath, bool recursive) const;
		// Walks the asset directory's folders again (folders created or removed outside the editor). Blocking.
		void RescanFolders();

		// The assets whose recorded dependencies (GetDependencies) name this asset: the scenes, prefabs, models and materials
		// that use it, and the meshes of models that use a material. Answered from an index kept with the recorded references
		// on the main thread (no document is read). Sorted by handle.
		std::vector<AssetHandle> GetReferencers(AssetHandle handle) const;

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

		// SetAssetReferences, keeping the index of referencers (GetReferencers) current. Main thread.
		void RecordReferences(AssetHandle handle, std::span<const AssetHandle> references, std::optional<AABB> bounds);
		// Drops what an asset that is unregistered references from the index of referencers.
		void ForgetReferences(AssetHandle handle);
		// The folder index: a source asset's place (path) is added or removed. Requires m_SourceMutex.
		void AddToFolderIndex(AssetHandle handle, const std::string& path);
		void RemoveFromFolderIndex(AssetHandle handle, const std::string& path);
		// Adds a folder and its parents to the index. Requires m_SourceMutex.
		void AddFolderLocked(const std::string& relativePath);
		// Walks the folders below an absolute directory into the index. Requires m_SourceMutex.
		void IndexFoldersBelowLocked(const std::filesystem::path& directory);
		void MarkChanged() { m_ChangeVersion.fetch_add(1, std::memory_order_acq_rel); }
		// Registers a file that the editor put into the asset directory (a restore, a copy) and takes its import from the cache
		// when it is current there, else queues it. Returns its handle (null when it is no asset).
		AssetHandle RegisterAddedFile(const std::filesystem::path& absolutePath, bool queueImport);
		// Advances the imports of external files (ImportExternal): registers what a finished copy brought, and ends imports
		// whose assets are imported.
		void UpdateExternalImports();
		void FinishExternalImport(ExternalImport& import);
		// Removes what an item copied (it was never the user's project: permanently), unregistering its assets.
		void RollBackExternalFiles(const std::vector<std::string>& files, const std::vector<AssetHandle>& assets);
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

		// Folder index (GetFolders, GetAssetsInFolder); guarded by m_SourceMutex.
		std::set<std::string> m_Folders; // Every indexed folder, relative, without the root
		std::unordered_map<std::string, std::unordered_set<AssetHandle>> m_SourcesByFolder; // Folder ("" root) -> sources directly in it
		std::atomic<uint64_t> m_ChangeVersion = 1;

		// What every asset references as recorded, and the inverse (GetReferencers); guarded by m_ReferenceMutex.
		mutable std::mutex m_ReferenceMutex;
		std::unordered_map<AssetHandle, std::vector<AssetHandle>> m_RecordedReferences;
		std::unordered_map<AssetHandle, std::unordered_set<AssetHandle>> m_Referencers;

		std::vector<Ref<ExternalImport>> m_ExternalImports; // Not finished (main thread)
	};

}
