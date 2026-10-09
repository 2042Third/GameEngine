#pragma once

#include "Strata/Core/Base.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Strata
{

	enum class ProcessOutputMode : uint8_t
	{
		Capture,        // stdout + stderr are merged into a buffer readable through Process::TakeOutput
		Inherit,        // The child shares this process's stdout/stderr
		Discard,        // The child's output is redirected to the null device
		CaptureSeparate // stdout is read through Process::TakeOutput and stderr through Process::TakeErrorOutput
	};

	struct ProcessSpecification
	{
		// Absolute path, or a bare program name that is looked up on PATH.
		std::filesystem::path Executable;
		std::vector<std::string> Arguments; // UTF-8
		std::filesystem::path WorkingDirectory; // Empty inherits the current directory
		ProcessOutputMode Output = ProcessOutputMode::Capture;
		// The child's stdin is a pipe written through Process::WriteInput and ended by CloseInput (e.g. to talk to a
		// program that reads requests from stdin). Requires captured or discarded output. Without it, stdin is the null
		// device, or this process's stdin when the output is inherited.
		bool PipeInput = false;
		bool HideWindow = true; // Windows: no console window for console programs (when output is not inherited)
		bool Detached = false;  // Own process group; unaffected by console signals sent to this process
		// The child and every process it starts form one unit (for build tools, whose compilers and linkers must not
		// outlive a cancelled build): Terminate() ends all of them, and so does destroying the Process object (or starting
		// it again) while the child runs. On Windows the unit is a job object, which also ends processes that outlive the
		// child when the Process object is destroyed or this process exits; processes that explicitly break away from it
		// (shared servers) may. On POSIX it is the child's own process group; processes that leave it are not ended.
		bool TerminateTree = false;
	};

	// A child process. Destroying a Process object does not terminate the child (unless it was started with TerminateTree);
	// call Terminate() for that.
	class Process
	{
	public:
		Process() = default;
		~Process();

		Process(const Process&) = delete;
		Process& operator=(const Process&) = delete;

		bool Start(const ProcessSpecification& specification);

		bool IsRunning();
		// Waits for the process to exit; returns its exit code, or nullopt if the timeout elapsed first.
		std::optional<int> Wait(std::optional<std::chrono::milliseconds> timeout = std::nullopt);
		bool Terminate();

		// Writes to the child's stdin (ProcessSpecification::PipeInput), blocking while the pipe is full. Returns false
		// when input is not piped or was closed, or once the child has closed its end (e.g. it exited); a closed pipe
		// never raises SIGPIPE. WriteInput and CloseInput belong to one thread.
		bool WriteInput(std::string_view data);
		// Ends the child's input: it reads end of file. Closing the Process does the same.
		void CloseInput();

		// Returns captured output produced since the previous call (thread-safe). With CaptureSeparate, stdout only.
		std::string TakeOutput();
		// Returns captured stderr produced since the previous call (CaptureSeparate; thread-safe).
		std::string TakeErrorOutput();
		// True once the captured output has ended (both streams with CaptureSeparate): every process holding it (the
		// child, and descendants that inherited it) closed it, and everything was read (TakeOutput and TakeErrorOutput
		// return the rest). Output can still arrive after the child exited. Always true when output is not captured.
		bool IsOutputFinished() const { return m_ReaderFinished.load() && m_ErrorReaderFinished.load(); }
		std::optional<int> GetExitCode() const { return m_ExitCode; }
		uint32_t GetProcessID() const { return m_ProcessID; }
		const std::string& GetLastError() const { return m_LastError; }

		struct RunResult
		{
			bool Started = false;
			bool TimedOut = false;
			int ExitCode = -1;
			std::string Output;      // stdout and stderr merged, or stdout only with CaptureSeparate
			std::string ErrorOutput; // stderr with CaptureSeparate
			std::string Error;
		};

		// Runs a process to completion, capturing its output (merged unless the specification asks for CaptureSeparate;
		// input is not piped). On timeout the process is terminated.
		static RunResult Run(ProcessSpecification specification, std::optional<std::chrono::milliseconds> timeout = std::nullopt);
	private:
		void StartOutputReaders();
		void StopOutputReaders(std::chrono::milliseconds gracePeriod);
		void Close();
	private:
#if defined(ST_PLATFORM_WINDOWS)
		void* m_ProcessHandle = nullptr;
		void* m_OutputRead = nullptr;
		void* m_ErrorRead = nullptr;
		void* m_InputWrite = nullptr;
		void* m_JobHandle = nullptr; // TerminateTree: the job holding the child and its descendants
#else
		int m_ProcessPid = -1;
		int m_OutputRead = -1;
		int m_ErrorRead = -1;
		int m_InputWrite = -1;
		bool m_TerminateTree = false; // The child leads its own process group, which Terminate() ends
#endif
		uint32_t m_ProcessID = 0;
		std::optional<int> m_ExitCode;
		std::string m_LastError;

		std::thread m_OutputThread;
		std::thread m_ErrorThread;
		std::atomic<bool> m_StopReading = false;
		std::atomic<bool> m_ReaderFinished = true;
		std::atomic<bool> m_ErrorReaderFinished = true;
		std::mutex m_OutputMutex; // Guards m_Output and m_ErrorOutput
		std::string m_Output;
		std::string m_ErrorOutput;
	};

}
