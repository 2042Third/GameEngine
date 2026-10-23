#pragma once

#include "Editor/WorldCook.h"

#include <Strata/Asset/AssetPackWriter.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/Base.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Project/GameManifest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class DocumentSaver;
	class EditorContext;

	struct GameExportOptions
	{
		std::filesystem::path Directory;         // Output directory (created if needed)
		std::filesystem::path RuntimeExecutable; // Empty: StrataRuntime next to the editor executable
		bool IncludeRuntime = true;              // Copy the runtime executable (as "<Game>[.exe]")
		// Copy the script module's debug symbols (the PDB on Windows) along with it. Shipping (Dist) builds leave them out
		// unless asked.
#if defined(ST_DIST)
		bool IncludeScriptSymbols = false;
#else
		bool IncludeScriptSymbols = true;
#endif
		uint32_t WindowWidth = 1280;
		uint32_t WindowHeight = 720;
		bool Fullscreen = false;
		// Pack only what the scenes use and what the project's Export.AlwaysInclude names, with what that uses
		// (AssetPackBuildOptions::Prune). Off by default: scripts can find assets by path, which no scene shows.
		bool Prune = false;
		// Put scenes, prefabs and models into the pack cooked (CookedScene: loads read them without parsing JSON); false keeps
		// their JSON.
		bool CookDocuments = true;
	};

	struct GameExportResult
	{
		std::filesystem::path Executable; // Empty without the runtime
		std::filesystem::path Manifest;
		std::filesystem::path AssetPack;
		std::filesystem::path ScriptModule; // Empty for games without scripts
		std::filesystem::path Icon;         // The game's icon, <File>.png (GameIcon); empty without one
		std::filesystem::path DesktopEntryFile; // <File>.desktop (Editor/DesktopEntry): on Linux hosts, with the runtime
		std::vector<std::string> Warnings;  // What a person should know about the export (an enlarged icon)
		size_t AssetCount = 0;              // Assets in the pack
		std::vector<AssetMetadata> ExcludedAssets; // Left out by pruning
		AssetPackWriteStats Pack;
		double Seconds = 0.0; // From the start to the end of the export
	};

	enum class GameExportState : uint8_t
	{
		WaitingForImports = 0, // Background imports and saves finish first, so that the pack holds their results (and documents
		                       // whose reference search has not run are scanned)
		CookingWorlds,         // The project's worlds' indexes are brought up to date (WorldCook), so that the pack holds them
		Packing,               // Writing the asset pack
		Copying,               // Writing the script module, the manifest and the runtime
		Succeeded,
		Failed,
		Cancelled
	};

	const char* GameExportStateToString(GameExportState state);

	// Exports the open project as a playable game without blocking a frame: the project's worlds' indexes brought up to date
	// (WorldCook, for cells whose files changed since), an asset pack (EditorAssetManager::PlanAssetPack,
	// ordered by first use from the project's start scene, or the open scene when none is set), the script module the editor
	// runs (the loaded module, exactly as it was loaded: see EditorContext::ReadRunningScriptModule), a game manifest naming
	// the start scene, the module and the game's identity (ProjectGameInfo), the game's icon as a 256-pixel PNG (GameIcon),
	// and the runtime executable with the third-party notices found next to it.
	//
	// Start checks at once what it can without reading the project's files (a saved scene that is not running, no script
	// build, the loaded script module unchanged, a usable output directory outside the project, what AlwaysInclude names)
	// and fails without side effects. The export then waits for background imports, takes a snapshot of what to pack
	// (main thread), and writes the files on an I/O thread, reporting progress; it fails there when a packed scene or
	// prefab uses scripts but no script module is loaded. Update advances it once per frame (EditorContext does). Cancel
	// stops it between blocks. The editor's asset manager stays alive until the export finished.
	class GameExport
	{
	public:
		static Ref<GameExport> Start(EditorContext& context, const GameExportOptions& options, std::string* outError = nullptr);
		// Cancels the export and waits for it.
		~GameExport();

		GameExport(const GameExport&) = delete;
		GameExport& operator=(const GameExport&) = delete;

		// Main thread, once per frame: plans the pack and starts writing once no background import is pending, and takes
		// the outcome of a finished write.
		void Update();
		// Asks the export to stop: it ends Cancelled (unless it was done already) once its I/O job noticed.
		void Cancel();
		// Main thread: blocks until the I/O job (if one runs) has ended and takes its outcome; an export cancelled while it
		// waited for imports or cooked worlds ends at once.
		void Wait();

		GameExportState GetState() const;
		bool IsFinished() const;
		// The share of the work done, in [0, 1]: the pack's stored bytes, then the files copied.
		float GetProgress() const;
		// What it is doing ("Packing 12 of 340 assets"), or how it ended.
		std::string GetStage() const;
		const GameExportOptions& GetOptions() const { return m_Options; }
		// Once finished: what was written (Succeeded), or why not (Failed, Cancelled).
		const GameExportResult& GetResult() const { return m_Result; }
		const std::string& GetError() const { return m_Error; }
	private:
		GameExport() = default;

		// Starts cooking the project's worlds whose indexes miss changed cells (main thread); false when there is none.
		bool BeginCooking();
		// Writes the fresh indexes once every cook finished; false, with the export failed, when one failed (main thread).
		bool FinishCooking();
		// Plans the pack and starts the I/O job (main thread).
		void Begin();
		// Takes the outcome of the I/O job once it ended (main thread).
		void Collect();
	private:
		// What the I/O job and the main thread share.
		struct Shared
		{
			std::atomic<bool> Cancel = false;
			std::atomic<GameExportState> State = GameExportState::WaitingForImports;
			std::atomic<uint64_t> DoneBytes = 0;
			std::atomic<uint64_t> TotalBytes = 1;
			std::atomic<uint32_t> PackedAssets = 0;

			std::mutex Mutex; // Guards the outcome
			bool Ended = false;
			GameExportState Outcome = GameExportState::Failed;
			std::string Error;
			GameExportResult Result;
		};

		GameExportOptions m_Options;
		Ref<EditorAssetManager> m_Assets; // Kept alive while the I/O job reads from it
		// The editor's saves: the pack is planned once none is in flight, so that it holds what they write (the context
		// outlives its export).
		const DocumentSaver* m_Saver = nullptr;
		// The save of the modified scene the export started during (EditorContext::FindSaveOfCurrentState), or 0: the pack is
		// planned only once it succeeded, or it would hold the file from before.
		uint64_t m_AwaitedSave = 0;
		Ref<WorldCookCache> m_WorldCookCache; // The editor's (EditorContext::GetWorldCookCache), shared with the cooks
		std::vector<Scope<WorldCook>> m_Cooks;
		// The physics capacity each cooked world's persistent scene needs (WorldCookReport::RecommendedMaxBodies): the packed
		// scene gets it when it saved less (AssetPackWriteOptions::AdjustSceneSettings).
		std::unordered_map<AssetHandle, uint32_t> m_WorldCapacities;
		AssetPackBuildOptions m_PackOptions;
		GameManifest m_Manifest;
		std::string m_FileName; // Of the pack, manifest and executable, without extension
		std::filesystem::path m_Runtime;
		std::filesystem::path m_ScriptModulePath;   // Empty without scripts
		std::filesystem::path m_IconSource;         // The game's icon's image (GameIcon::FindSource); empty without one
		std::vector<uint8_t> m_ScriptModuleBytes;
		std::chrono::steady_clock::time_point m_StartTime;

		Ref<Shared> m_Shared;
		JobHandle m_Job;
		// Documents whose search of what they reference had not run when the imports were done (changed on disk meanwhile):
		// scanned by a job of the export before it plans, rather than waiting for searches that wait for their documents to
		// settle.
		struct DocumentScan
		{
			std::vector<AssetHandle> Documents;
			std::vector<std::optional<std::vector<AssetHandle>>> References; // Per document, once Done
			std::atomic<bool> Done = false;
		};
		Ref<DocumentScan> m_DocumentScan;
		JobHandle m_DocumentScanJob;
		bool m_DocumentsScanned = false;
		uint32_t m_AssetCount = 0; // Planned
		bool m_Collected = false;
		GameExportResult m_Result;
		std::string m_Error;
	};

	// Whether `directory` can receive an export of the project in `projectDirectory`: an absolute, local path outside the
	// project that is not a file, without reserved device names. The reason when it cannot.
	std::optional<std::string> CheckExportDirectory(const std::filesystem::path& directory, const std::filesystem::path& projectDirectory);

	// Platform file name of an executable ("Name.exe" on Windows, "Name" elsewhere).
	std::string GetExecutableFileName(const std::string& name);

}
