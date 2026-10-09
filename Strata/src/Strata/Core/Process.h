#pragma once

#include "Strata/Core/Base.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Strata
{

	enum class ProcessOutputMode : uint8_t
	{
		Capture, // stdout + stderr are merged into a buffer readable through Process::TakeOutput
		Inherit, // The child shares this process's stdout/stderr
		Discard  // The child's output is redirected to the null device
	};

	struct ProcessSpecification
	{
		// Absolute path, or a bare program name that is looked up on PATH.
		std::filesystem::path Executable;
		std::vector<std::string> Arguments; // UTF-8
		std::filesystem::path WorkingDirectory; // Empty inherits the current directory
		ProcessOutputMode Output = ProcessOutputMode::Capture;
		bool HideWindow = true; // Windows: no console window for console programs (when output is not inherited)
		bool Detached = false;  // Own process group; unaffected by console signals sent to this process
	};

	// A child process. Destroying a Process object does not terminate the child; call Terminate() for that.
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

		// Returns captured output produced since the previous call (thread-safe).
		std::string TakeOutput();
		std::optional<int> GetExitCode() const { return m_ExitCode; }
		uint32_t GetProcessID() const { return m_ProcessID; }
		const std::string& GetLastError() const { return m_LastError; }

		struct RunResult
		{
			bool Started = false;
			bool TimedOut = false;
			int ExitCode = -1;
			std::string Output;
			std::string Error;
		};

		// Runs a process to completion, capturing its output. On timeout the process is terminated.
		static RunResult Run(ProcessSpecification specification, std::optional<std::chrono::milliseconds> timeout = std::nullopt);
	private:
		void StartOutputReader();
		void StopOutputReader(std::chrono::milliseconds gracePeriod);
		void Close();
	private:
#if defined(ST_PLATFORM_WINDOWS)
		void* m_ProcessHandle = nullptr;
		void* m_OutputRead = nullptr;
#else
		int m_ProcessPid = -1;
		int m_OutputRead = -1;
#endif
		uint32_t m_ProcessID = 0;
		std::optional<int> m_ExitCode;
		std::string m_LastError;

		std::thread m_OutputThread;
		std::atomic<bool> m_StopReading = false;
		std::atomic<bool> m_ReaderFinished = true;
		std::mutex m_OutputMutex;
		std::string m_Output;
	};

}
