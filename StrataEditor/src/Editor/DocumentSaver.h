#pragma once

#include "Editor/UndoStack.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/Base.h>
#include <Strata/Core/Crypto.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Scene/SceneDocumentWriter.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	class Scene;

	// One document of a save request (DocumentSaver).
	struct DocumentSaveItem
	{
		// The engine-native asset written: one of the project, or a new one reserved for NewPath
		// (EditorAssetManager::ReserveNativeAsset, with a given handle or a new one).
		AssetHandle Asset = UUID::Null();
		// For a new asset: the path it was reserved at (relative to the asset directory), else empty.
		std::string NewPath;
		// What of the request's snapshot the document holds and how it is written (kind, entity subset, settings, name).
		SceneWriteOptions Write;
		// Bytes written as they are instead of the snapshot's entities: a small engine-native document made on the main thread
		// (a World document's refreshed index), validated by its type's loader on the writing job.
		std::optional<std::string> Document;
		// Loaded copies of the asset take the written version once it is published.
		bool Reload = false;
	};

	struct DocumentSaveRequest
	{
		// What the documents are written from: a copy of the edited scene taken on the main thread (Scene::Copy with
		// SceneCopyOptions::SerializedOnly), which nothing changes afterwards. Null when every item has its Document.
		Ref<const Scene> Snapshot;
		std::vector<DocumentSaveItem> Items;
		// The point of the edited scene's history the snapshot holds (UndoStack::GetSavePoint, taken after BreakMerge): what
		// the owner marks saved when the request is done (DocumentSaveStatus::SavePoint).
		UndoStack::SavePoint SavePoint;
		std::string Label; // What the person sees ("World1M")
	};

	enum class DocumentSaveState : uint8_t
	{
		Queued = 0, // Behind the request that writes
		Writing,    // On its background job
		Publishing, // Written; the main thread takes it over at its next update
		Done,
		Failed      // An item failed (the others are written)
	};

	const char* DocumentSaveStateToString(DocumentSaveState state);

	struct DocumentSaveItemResult
	{
		AssetHandle Asset = UUID::Null();
		std::string Path;      // Relative to the asset directory
		uint64_t Bytes = 0;
		uint64_t Entities = 0;
		std::vector<AssetHandle> References;
		Sha256Digest Sha256 = {}; // Of the written bytes
		std::string Error;        // Empty when the item was written and published
	};

	// Where a request is (DocumentSaver::GetStatus) and, once it ended, what it did.
	struct DocumentSaveStatus
	{
		uint64_t ID = 0;
		// The request whose snapshot was written for this one: its own ID, or that of a newer request for the same documents
		// that replaced it while it waited (both end when the newer one is written).
		uint64_t WrittenID = 0;
		std::string Label;
		size_t Documents = 0;    // Items of the request
		DocumentSaveState State = DocumentSaveState::Queued;
		float Progress = 0.0f;   // In [0, 1]
		uint64_t Bytes = 0;      // Written so far
		double Seconds = 0.0;    // From the start of the writing until now, or until it ended
		// The point of the history the written snapshot holds: what to mark saved (UndoStack::MarkSaved(SavePoint)).
		UndoStack::SavePoint SavePoint;
		std::vector<DocumentSaveItemResult> Items; // In request order, once it ended
		std::string Error;       // Failed: which items failed and why

		bool IsFinished() const { return State == DocumentSaveState::Done || State == DocumentSaveState::Failed; }
	};

	// The editor's one path for saving engine-native documents in the background (scene.save, a world's cells), so that
	// saving never holds a frame: the main thread only takes a snapshot; serializing (SceneDocumentWriter), scanning the
	// references, hashing and writing run on a background job; Update publishes the result on the main thread in one cheap
	// step (EditorAssetManager::PublishWrittenDocument: no document is read or validated again, the writer being the
	// engine's own).
	//
	// A request holds several documents with one completion (a world's dirty cells and its index): each item is written
	// next to its target through AtomicFileWriter and replaces it under the asset manager's exclusive file lock
	// (DocumentWriteClaim::Commit, which also writes a new asset's .meta and the import record). Items fail independently
	// (each file is atomic); the request fails when any did, naming them. One request writes at a time and the others
	// queue; a queued request for the same documents (the same assets) is replaced by a newer one, whose snapshot is written
	// for both (so repeated saves never hold two snapshots of a large scene), and both completions get its status.
	//
	// Main thread, but for the work on the background job. Requests use the asset manager set when they were submitted.
	class DocumentSaver
	{
	public:
		using Completion = std::function<void(const DocumentSaveStatus& status)>;

		// Statuses of ended requests GetStatus keeps.
		static constexpr size_t c_KeptStatuses = 64;

		DocumentSaver() = default;
		// Finishes every request (FinishAll).
		~DocumentSaver();

		DocumentSaver(const DocumentSaver&) = delete;
		DocumentSaver& operator=(const DocumentSaver&) = delete;

		// The project's asset manager, which requests submitted from now on write into (null: no project, Submit fails).
		void SetAssetManager(const Ref<EditorAssetManager>& assets) { m_Assets = assets; }

		// Queues a request and returns its ID, or 0 (with the reason) when it cannot be saved: no asset manager, no items, an
		// item whose asset is no engine-native asset of the project nor reserved, an item without a Document and no snapshot.
		// It starts writing at once when no other request writes, and otherwise waits for its turn. The completion runs on the
		// main thread, from Update (or FinishAll), once the request ended.
		uint64_t Submit(DocumentSaveRequest request, Completion onComplete, std::string* outError = nullptr);
		// Once per frame (EditorContext::Update): publishes a request whose writing ended and calls its completions, then
		// starts writing the next one.
		void Update();
		// Blocks until every request ended (tools, tests, and the editor's teardown after its last frame).
		void FinishAll();

		// A queued or running request, or one of the last c_KeptStatuses that ended.
		std::optional<DocumentSaveStatus> GetStatus(uint64_t id) const;
		// The requests that did not end, in order (the running one first).
		std::vector<DocumentSaveStatus> GetActiveStatuses() const;
		bool IsBusy() const { return m_Running != nullptr || !m_Queue.empty(); }
		// Whether a request that did not end writes this asset.
		bool IsSaving(AssetHandle asset) const;
	private:
		// What the writing job and the main thread share while a request runs.
		struct Job;

		struct Pending
		{
			std::vector<uint64_t> IDs; // Its own and those of the requests it replaced
			std::vector<Completion> Completions;
			DocumentSaveRequest Request;
			Ref<EditorAssetManager> Assets;
			Ref<Job> Running;          // Set once it writes
		};

		void Start(Scope<Pending> pending);
		// Publishes the running request once its job ended (main thread); returns whether it ended.
		bool Collect();
		DocumentSaveStatus MakeStatus(const Pending& pending, uint64_t id) const;
		void Keep(const DocumentSaveStatus& status);
	private:
		Ref<EditorAssetManager> m_Assets;
		std::deque<Scope<Pending>> m_Queue;
		Scope<Pending> m_Running;
		std::deque<DocumentSaveStatus> m_Ended; // The latest, oldest first
		uint64_t m_NextID = 1;
	};

}
