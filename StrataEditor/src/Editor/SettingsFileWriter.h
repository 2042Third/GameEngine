#pragma once

#include <Strata/Core/Base.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace Strata
{

	// Where a settings file stands (SettingsFileWriter::GetState).
	enum class SettingsFileState : uint8_t
	{
		Written = 0, // The file holds the latest state submitted (or nothing was submitted)
		Writing,     // A write runs, or waits for its turn
		Unwritten    // The last write failed: the latest state is only in memory until a later write succeeds
	};

	const char* SettingsFileStateToString(SettingsFileState state);

	// Writes one of the editor's settings files (the project file; Docs/DeveloperGuide.md, "Project settings") off the main
	// thread, as saves must be: the main thread submits a write that owns a snapshot of the values (small copies), a
	// background I/O job runs it (serializing and writing the file atomically), and Update publishes how it ended. Writes
	// coalesce: one runs at a time, and of those submitted meanwhile only the newest runs next, as it holds the newest state.
	// A failed write leaves the file Unwritten until a later one succeeds; the next submit retries.
	//
	// Main thread, but for the writes themselves, which run on their job.
	class SettingsFileWriter
	{
	public:
		// Serializes its snapshot and writes the file; false with the reason. Runs on a background thread, so it touches only
		// what it owns.
		using WriteFunction = std::function<bool(std::string* outError)>;
		// How a write ended, told on the main thread (Update, Finish): an empty error when it succeeded.
		using Completion = std::function<void(const std::string& error)>;

		explicit SettingsFileWriter(Completion onWritten = {});
		// Finishes the write in flight (Finish).
		~SettingsFileWriter();

		SettingsFileWriter(const SettingsFileWriter&) = delete;
		SettingsFileWriter& operator=(const SettingsFileWriter&) = delete;

		// Queues a write of the newest state: it starts at once when no write runs, and otherwise replaces the one waiting.
		void Submit(WriteFunction write);
		// Once per frame: publishes a write that ended (its completion runs) and starts the one waiting.
		void Update();
		// Blocks until no write is in flight, running one that has not started on this thread (the job system's rule): only
		// for the file's owner going away (the editor releasing its project), never in a frame.
		void Finish();

		SettingsFileState GetState() const;
		bool IsBusy() const { return m_Running != nullptr || m_Waiting != nullptr; }
		// Why the last write failed while the file is Unwritten; empty otherwise.
		const std::string& GetLastError() const { return m_LastError; }
		// Writes that ended so far, successful or not.
		uint64_t GetWriteCount() const { return m_WriteCount; }

		// Tests: runs on the writing thread before each write (a test records the thread, or holds a write in flight). Writes
		// submitted from now on use it.
		void SetTestHook(std::function<void()> hook) { m_TestHook = std::move(hook); }
	private:
		// What the writing job and the main thread share while a write runs.
		struct Job;

		void Start(WriteFunction write);
		// Publishes the running write once its job ended (main thread); returns whether none runs any more.
		bool Collect();
	private:
		Ref<Job> m_Running;
		WriteFunction m_Waiting;
		Completion m_OnWritten;
		std::function<void()> m_TestHook;
		std::string m_LastError;
		bool m_Failed = false;
		uint64_t m_WriteCount = 0;
	};

}
