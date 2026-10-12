#pragma once

#include "Editor/SceneEdit.h"
#include "Editor/UndoStack.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/Base.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	// A scene's unsaved changes that a recovery file holds (scene.recoveryStatus, the editor's recovery offer).
	struct RecoveryEntry
	{
		std::string Id;                   // The file's name without its extension: what scene.recover takes
		AssetHandle Scene = UUID::Null(); // Null for a scene that was never saved
		std::string SceneName;
		// Relative to the asset directory (empty for a scene never saved): where the scene is now, found by its handle (it may
		// have moved since the changes were written)
		std::string ScenePath;
		int64_t Time = 0;                 // When it was written: milliseconds since 1970-01-01 UTC
		// Written when the editor discarded the changes on purpose (force, Don't Save), rather than left behind by an editor
		// that ended without closing the scene (a crash).
		bool Discarded = false;
		uint64_t EntityCount = 0; // Entity states it restores
		bool Complete = false;    // A snapshot of every entity (scenes never saved) rather than the changed ones
		// Whether it applies to the scene as it is: the scene file must be the one the changes were made to, and the scene
		// asset must still exist. Problem says why not.
		bool Applicable = true;
		std::string Problem;
	};

	// The contents of a recovery file (SceneRecovery::Read).
	struct RecoveryData
	{
		RecoveryEntry Entry;
		std::optional<std::string> BaseHash; // SHA-256 (hexadecimal) of the scene file the changes apply to
		nlohmann::json Settings;             // The scene's settings as a scene file writes them
		// The entity states the edits left, step by step: applied one after the other (SceneEdit::ApplyEntities), as redo
		// would, they turn the scene file into the edited scene. A complete journal has one step holding every entity.
		std::vector<std::vector<EntityState>> Steps;
	};

	// What became of the unsaved changes of a scene that went away (SceneRecovery::EndScene).
	enum class DiscardOutcome : uint8_t
	{
		Unmodified = 0, // It had none
		Kept,           // Written as recovery data marked discarded: offered until recovered or discarded
		Lost            // Not kept: no project to keep them in, or changes that are not journaled (the reason says why)
	};

	// What the journal of the edited scene does (editor.status "recovery", scene.recoveryStatus).
	struct RecoveryJournalStatus
	{
		bool Active = false;   // A project is open: the edited scene's changes are journaled
		std::string File;      // The journal's file name in the recovery directory
		uint64_t Writes = 0;   // Writes asked for since the scene opened
		int64_t LastWrite = 0; // When the last write finished: milliseconds since 1970-01-01 UTC (0: none yet)
		bool Pending = false;  // Changes not written yet
		size_t PendingSteps = 0; // Undo steps applied since the last write, waiting for the next (merges count once)
		bool Busy = false;     // A file operation is running or waiting
		std::string Skipped;   // Why the edited scene's changes are not journaled (e.g. a large scene that was never saved)
	};

	// Undo step of applying a recovery file: the touched entities' states and the scene's settings before and after.
	class SceneRecoveryAction final : public EditorAction
	{
	public:
		SceneRecoveryAction(Scene& scene, std::vector<EntityState> before, std::vector<EntityState> after, const SceneSettings& settingsBefore,
			const SceneSettings& settingsAfter);

		const std::string& GetName() const override;
		bool Execute(std::string* outError) override;
		bool Undo(std::string* outError) override;
		SceneChangeKind GetSceneChange() const override { return SceneChangeKind::Entities; }
		Ref<const std::vector<EntityState>> GetEntityStates(bool undone) const override { return undone ? m_Before : m_After; }
	private:
		Scene& m_Scene;
		Ref<const std::vector<EntityState>> m_Before;
		Ref<const std::vector<EntityState>> m_After;
		SceneSettings m_SettingsBefore;
		SceneSettings m_SettingsAfter;
	};

	// Autosave and crash recovery of the edited scene.
	//
	// The journal of the edited scene keeps its unsaved changes in "<project>/.strata/Recovery/<scene>-<session>.strecovery":
	// the SHA-256 of the saved scene file they apply to, the scene's settings, and the entity states the undo steps since the
	// last save left (EntityState snapshots that SceneEditTransaction captured: shared with the undo history and followed
	// through UndoStackObserver, so following them costs a reference per step). Applied step by step, as redo would apply
	// them, they turn the saved file into the edited scene; consecutive steps that move no entity (component edits, drags)
	// fold into one, so the journal grows with the structural edits only. Its cost follows the edits, not the scene's
	// size: a write hands references to the steps since the last write to an I/O job, which folds and serializes them.
	// It is written c_QuietSeconds after changes stop, and at least every c_MaxIntervalSeconds while they go on; saving
	// removes it, and so does undoing back to the saved state.
	//
	// A scene that was never saved has no file to apply changes to: its journal is complete, starting from an empty scene
	// with every entity the scene started with (a template's handful, captured once when it opens), followed by the steps.
	// A step that is not made of entity states (kind Other) turns a journal into a complete snapshot of every entity at the
	// next write. Snapshots are captured on the main thread, so only of scenes with at most c_MaxSnapshotEntities (otherwise
	// the changes are not journaled until the scene is saved, and the status says so). When the edited scene is replaced
	// (another scene or project opens, the editor closes) with unsaved changes, they are written at once and marked
	// discarded, so even a forced discard loses nothing, and the project's entries are read again after that write, so they
	// are offered at once (File > Recover Unsaved Changes, scene.recoveryStatus). Of the changes discarded on purpose, a scan
	// offers the newest c_MaxOfferedDiscards (those that still apply first) and moves the others to Recovery/Discarded, so
	// that an agent discarding scene after scene does not bury the person in offers; what a crash left is never moved.
	//
	// File operations run one at a time on I/O jobs, in the order they were asked for, so a scan sees every write asked
	// for before it and a removal after a save is never overtaken by an earlier write. A scan lists the project's recovery
	// files; a file whose owner (an editor process) still runs and has not discarded it belongs to that editor and is left
	// out. Main thread only.
	class SceneRecovery final : public UndoStackObserver
	{
	public:
		static constexpr const char* c_DirectoryName = "Recovery";
		static constexpr const char* c_DiscardedDirectoryName = "Discarded";
		static constexpr const char* c_FileExtension = ".strecovery";
		// The timing and size limits; Docs/Architecture.md ("Autosave and recovery") has the measurements behind them.
		static constexpr double c_QuietSeconds = 2.0;
		static constexpr double c_MaxIntervalSeconds = 10.0;
		static constexpr size_t c_MaxSnapshotEntities = 256;
		// Discarded recovery files kept in Recovery/Discarded (the oldest go).
		static constexpr size_t c_MaxDiscardedFiles = 20;
		// Changes discarded on purpose that a scan offers (the newest; the others move to Recovery/Discarded).
		static constexpr size_t c_MaxOfferedDiscards = 8;

		// Problems the editor knows of a recovery entry beyond its scene file (e.g. the scene asset is gone): empty when it
		// applies. Runs on the main thread when a scan finishes.
		using EntryValidator = std::function<std::string(const RecoveryEntry& entry)>;
		// The project's scenes by handle, with their paths relative to the asset directory: where a scan looks for the scene
		// file of an entry, which names the scene's path as it was when the entry was written. Runs on the main thread.
		using ScenePaths = std::unordered_map<AssetHandle, std::string>;
		using SceneLocator = std::function<ScenePaths()>;

		SceneRecovery();
		// Finishes the file operations asked for: the editor is closing, and its last journal must reach the disk.
		~SceneRecovery() override;

		SceneRecovery(const SceneRecovery&) = delete;
		SceneRecovery& operator=(const SceneRecovery&) = delete;

		// The project whose recovery directory the journal writes to and Scan reads (empty paths: no project, nothing is
		// journaled). Does not scan. Forgets the entries of the previous project.
		void SetProject(const std::filesystem::path& recoveryDirectory, const std::filesystem::path& assetDirectory);
		bool IsActive() const { return !m_Directory.empty(); }
		const std::filesystem::path& GetDirectory() const { return m_Directory; }
		void SetEntryValidator(EntryValidator validator) { m_Validator = std::move(validator); }
		void SetSceneLocator(SceneLocator locator) { m_SceneLocator = std::move(locator); }

		// The edited scene goes away: with unsaved changes they are written now, marked discarded, and the entries are read
		// again after that write (Update starts the scan). Returns what became of the changes; outReason says why changes
		// were Lost.
		DiscardOutcome EndScene(const Scene& scene, bool modified, std::string* outReason = nullptr);
		// A journal starts for the scene that is now edited (sceneFile: its file, absolute, and scenePath relative to the
		// asset directory; both empty for a scene that was never saved, whose entities the journal captures now).
		void BeginScene(const Scene& scene, AssetHandle handle, const std::filesystem::path& sceneFile, const std::string& scenePath);
		// The edited scene was saved (as this asset, with this document): its changes are on disk, so the journal's file goes
		// and the next changes apply to the saved document.
		void OnSaved(AssetHandle handle, const std::filesystem::path& sceneFile, const std::string& scenePath, std::string document);
		// The edited scene's asset is at this place now (it may have moved: asset.move, a rename in the Content Browser): the
		// journal names it from its next write on, which comes as for an edit. Nothing happens when the place is the same.
		void OnSceneMoved(const std::filesystem::path& sceneFile, const std::string& scenePath);

		// UndoStackObserver: the edited scene's undo stack applied a step. A step that merged another (a frame of a drag)
		// replaces the states its notification before left waiting for the next write, so a long drag holds one step's.
		void OnApplied(const EditorAction& action, bool undone, bool merged) override;

		// Once per frame: writes the journal when it is due, starts the next file operation, takes the results of scans and
		// starts the scan a discard asked for.
		void Update(const Scene& scene, bool modified);
		// Writes the journal at once if it changed, whatever the timing. Returns whether a write (or the removal of the file
		// of an unmodified scene) was asked for.
		bool Flush(const Scene& scene, bool modified);
		// Blocks until every file operation asked for has finished (the editor closing, tests).
		void Wait();
		bool IsBusy() const;

		// Starts listing the project's recovery files (after the file operations asked for before). The entries are there
		// once IsScanning is false.
		void Scan();
		// Lists them again as soon as no scan runs (Update starts it), e.g. because a scene of an entry moved.
		void RequestScan() { m_RescanDue = true; }
		bool IsScanning() const { return m_Scan != nullptr; }
		// The recovery files of the last scan, newest first (applicable or not). The version changes with every scan and
		// discard.
		const std::vector<RecoveryEntry>& GetEntries() const { return m_Entries; }
		uint64_t GetEntriesVersion() const { return m_EntriesVersion; }
		const RecoveryEntry* FindEntry(const std::string& id) const;

		// Moves a recovery file to Recovery/Discarded (kept for a while, not offered again).
		bool Discard(const std::string& id, std::string* outError = nullptr);

		// Reads a recovery file on an I/O job (after the file operations asked for before) and checks it against its scene
		// file; poll IsDone.
		struct Load
		{
			std::string Id;
			std::filesystem::path File;
			std::filesystem::path SceneFile; // Checked against the file's base hash (empty: a scene never saved)
			std::optional<RecoveryData> Data;
			std::string Error;
			std::atomic<bool> Done = false;

			bool IsDone() const { return Done.load(std::memory_order_acquire); }
		};
		Ref<Load> Read(const std::string& id, const std::filesystem::path& sceneFile);
		// After a recovery file was applied to the edited scene, whose journal holds the same changes from then on: the
		// journal is written at once and the file removed after that write (never before, so the changes stay on disk).
		void Replace(const Scene& scene, bool modified, const std::string& id);

		RecoveryJournalStatus GetStatus() const;
		nlohmann::json DescribeStatus() const;
		static nlohmann::json DescribeEntry(const RecoveryEntry& entry);

		// The settings of a scene as a scene file stores them, and back (settings the JSON leaves out keep their value).
		static nlohmann::json SerializeSettings(const SceneSettings& settings);
		static bool DeserializeSettings(const nlohmann::json& json, SceneSettings& settings);
	private:
		// A step of the edits: the entity states it left, and the states before it (to tell whether it moved entities).
		struct AppliedStep
		{
			Ref<const std::vector<EntityState>> States;
			Ref<const std::vector<EntityState>> Prior;
			// The action it came from, only to recognize the step's later merges (OnApplied); never dereferenced.
			const EditorAction* Action = nullptr;
		};

		// One step of the journal on disk: entity states by entity, in the order they were first touched. A step of the edits
		// that creates, removes or moves entities starts a segment; one that does not folds into the latest segment, whatever
		// that holds, since it leaves every entity where that segment put it.
		struct Segment
		{
			std::vector<UUID> Order;
			std::unordered_map<UUID, std::pair<Ref<const std::vector<EntityState>>, size_t>> States;
		};

		// What the journal's writes and the file operations around it share. Changed only by the file operations, which run
		// one at a time (and by the main thread before it asks for the first one that uses it).
		struct Journal
		{
			std::string Stem;                    // File name without the extension
			AssetHandle Scene = UUID::Null();
			std::string ScenePath;
			std::filesystem::path SceneFile;
			std::optional<std::string> BaseHash; // Set by the operation that hashes the scene file or the saved document
			// A complete journal (Complete) starts from an empty scene: its first step is every entity the scene started with
			// (a scene never saved) or held at a snapshot.
			Ref<const std::vector<EntityState>> Base;
			std::vector<Segment> Segments;
			bool Complete = false;
			bool Written = false;                // A file of this journal is on disk
			bool BaseHashReported = false;       // A write said that the scene file's version is unknown
		};

		struct ScanResult
		{
			std::filesystem::path Directory;
			std::filesystem::path AssetDirectory;
			ScenePaths Scenes; // The scenes when the scan began (SceneLocator)
			std::vector<RecoveryEntry> Entries;
			std::atomic<bool> Done = false;
		};

		void Enqueue(std::function<void()> operation);
		void PumpOperations();
		// Queues a write of the edited scene's journal, or the removal of its file when the scene is unmodified.
		bool Write(const Scene& scene, bool modified, bool discarded);
		void FinishScan();
	private:
		std::filesystem::path m_Directory;      // <project>/.strata/Recovery (empty: no project)
		std::filesystem::path m_AssetDirectory;
		EntryValidator m_Validator;
		SceneLocator m_SceneLocator;
		uint32_t m_ProcessId = 0;
		uint64_t m_ProcessStartTime = 0;

		Ref<Journal> m_Journal;
		std::vector<AppliedStep> m_PendingSteps; // Applied since the last write, oldest first
		bool m_Changed = false;                  // Since the last write
		bool m_NeedsSnapshot = false;            // The next write captures every entity (a step of kind Other)
		bool m_Unjournaled = false;              // The scene's changes cannot be journaled until it is saved (m_Skipped says why)
		bool m_RescanDue = false;                // A discard was written, or a scene moved: the entries are read again (Update)
		std::string m_ScenePath;                 // Where the journal's scene is, as it was last told (OnSceneMoved)
		std::string m_Skipped;                   // Why the current changes are not journaled
		std::chrono::steady_clock::time_point m_LastChange;
		std::chrono::steady_clock::time_point m_LastWrite;
		uint64_t m_Writes = 0;
		Ref<std::atomic<int64_t>> m_LastWriteTime;

		std::deque<std::function<void()>> m_Operations;
		JobHandle m_Running;

		Ref<ScanResult> m_Scan; // The scan in progress
		std::vector<RecoveryEntry> m_Entries;
		uint64_t m_EntriesVersion = 0;
	};

}
