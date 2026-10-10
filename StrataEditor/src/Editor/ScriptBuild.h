#pragma once

#include <Strata/Core/Base.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class Process;
	class Project;

	// The toolchain a game's scripts are built with. Script modules must match the engine's compiler, standard library
	// and configuration, so the defaults are the ones the engine itself was configured with (baked in at its configure
	// time, see StrataEditor/CMakeLists.txt).
	struct ScriptBuildSettings
	{
		std::filesystem::path CMake;       // CMake executable; when missing, "cmake" is looked up on PATH
		std::string Generator;             // e.g. "Visual Studio 18 2026", "Ninja"
		std::string Platform;              // -A (Visual Studio), empty for none
		std::string Toolset;               // -T, empty for none
		std::filesystem::path CXXCompiler; // Passed to generators that do not pick their own compiler (not Visual Studio)
		std::filesystem::path MakeProgram; // Ninja or make, for the generators that use one
		std::string Configuration;         // Debug, Release, Dist
		std::filesystem::path EngineDirectory; // Engine checkout: holds StrataScriptCore/CMake (the package)

		static ScriptBuildSettings GetEngineDefaults();

		bool IsMultiConfig() const;
		bool operator==(const ScriptBuildSettings& other) const = default;
	};

	// Arguments of the two CMake runs of a script build. The module lands in `binaryDirectory` (the library output
	// directory of the build, for every configuration).
	std::vector<std::string> MakeScriptConfigureArguments(const ScriptBuildSettings& settings, const std::filesystem::path& sourceDirectory,
		const std::filesystem::path& buildDirectory, const std::filesystem::path& binaryDirectory);
	std::vector<std::string> MakeScriptBuildArguments(const ScriptBuildSettings& settings, const std::filesystem::path& buildDirectory);

	// MSBuild's file tracker, which logs the files compilers and linkers touch, cannot create its logs at paths longer than
	// this (MAX_PATH less the terminating null), even where Windows allows long paths; builds then fail with errors that
	// do not name the cause (FTK1011 "could not create the new file tracking log file").
	constexpr size_t c_MSBuildMaxTrackedPathLength = 259;
	// The longest path below the build directory at which a Visual Studio generator's build of the module `targetName`
	// writes a tracking log, counting the separator after the build directory; 0 for generators without MSBuild.
	size_t GetScriptBuildTrackedPathDepth(const ScriptBuildSettings& settings, std::string_view targetName);
	// Why the module `targetName` cannot be built in `buildDirectory` with these settings, or nothing when it can: with a
	// Visual Studio generator, its tracking logs would exceed c_MSBuildMaxTrackedPathLength. Paths count as given, which is
	// how the tools receive them.
	std::optional<std::string> CheckScriptBuildPathLength(const ScriptBuildSettings& settings, const std::filesystem::path& buildDirectory,
		std::string_view targetName);

	// A compiler, linker or CMake message found in a build log.
	struct ScriptDiagnostic
	{
		std::string File;     // As printed (empty for linker messages without a file)
		uint32_t Line = 0;    // 0 when unknown
		uint32_t Column = 0;  // 0 when unknown
		std::string Severity; // "error" or "warning"
		std::string Code;     // e.g. "C2065", "LNK1104" (empty for GCC, Clang and CMake)
		std::string Message;

		bool operator==(const ScriptDiagnostic& other) const = default;
	};

	// Errors and warnings of MSVC, GCC, Clang, MSBuild, the GNU, LLVM, Apple and Microsoft linkers and CMake in a build
	// log, in order, without duplicates (MSBuild repeats every error in its summary). At most `maxDiagnostics` are returned.
	std::vector<ScriptDiagnostic> ParseScriptBuildDiagnostics(std::string_view log, size_t maxDiagnostics = 100);
	// "File(Line,Column): error C2065: message" style text of one diagnostic.
	std::string FormatScriptDiagnostic(const ScriptDiagnostic& diagnostic);
	// True for messages that only say that a step failed ("ld returned 1 exit status", "linker command failed",
	// LNK1120, "cl.exe exited with code 2"): the cause is elsewhere in the log.
	bool IsScriptBuildSummary(const ScriptDiagnostic& diagnostic);
	// The error of a failed build: the reason, then its first errors. When there are none, or only summaries, the end of
	// the build log follows, so the cause is never missing.
	std::string DescribeScriptBuildFailure(const std::string& reason, const std::vector<ScriptDiagnostic>& diagnostics, std::string_view log);
	// The last `lineCount` lines of a log.
	std::string GetLogTail(std::string_view log, size_t lineCount);

	// Splits streamed process output into lines (without "\n" or "\r\n"). Every byte is scanned once, and a line is cut
	// after `maxLineSize` bytes, so output that never ends its line cannot grow without bound.
	class OutputLineSplitter
	{
	public:
		explicit OutputLineSplitter(size_t maxLineSize);

		// Appends output; returns the lines it completed (and the parts of overlong lines).
		std::vector<std::string> Append(std::string_view output);
		// The unterminated rest at the end of the output, if any.
		std::optional<std::string> Flush();
		void Reset();
		size_t GetPendingSize() const { return m_Pending.size(); }
	private:
		size_t m_MaxLineSize;
		std::string m_Pending;
		bool m_PendingWasCut = false; // The pending line was just emitted as a cut part; its newline ends no new line
	};

	struct ScriptBuildResult
	{
		uint64_t ID = 0;      // Builds are numbered from 1 in each editor session
		bool Success = false;
		std::string Error;    // Why the build failed (with the first errors), empty on success
		std::vector<ScriptDiagnostic> Diagnostics;
		std::string LogTail;  // The end of the CMake output
		bool Configured = false; // The CMake configure step ran (first build, or the toolchain changed)
		double Seconds = 0.0;
		std::filesystem::path Module; // The built module file
		bool ModuleChanged = false;   // The build wrote a new module file (false when it was up to date)
	};

	enum class ScriptBuildPhase : uint8_t
	{
		Idle = 0,
		Configuring,
		Building
	};

	const char* ScriptBuildPhaseToString(ScriptBuildPhase phase);

	// Builds a project's scripts in the background: configures the project's script directory with CMake (when the build
	// tree does not exist yet or the toolchain changed) and builds it, in child processes that never block the caller.
	// Output is streamed to the log line by line. One build runs at a time. Main thread only; call Update once per frame.
	class ScriptBuilder
	{
	public:
		ScriptBuilder();
		// A running build is cancelled (its process is terminated).
		~ScriptBuilder();

		ScriptBuilder(const ScriptBuilder&) = delete;
		ScriptBuilder& operator=(const ScriptBuilder&) = delete;

		// Starts building the project's scripts. Fails (returning false with a reason) while a build runs, when the
		// project has no CMakeLists.txt in its script directory, or when its path is too long for the build's tools
		// (CheckScriptBuildPathLength).
		bool Start(const Project& project, const ScriptBuildSettings& settings, std::string* outError = nullptr);
		// Advances a running build. Returns true in the call in which the build finished (see GetLastResult).
		bool Update();
		// Terminates a running build; it finishes as failed.
		void Cancel();

		bool IsRunning() const { return m_Phase != ScriptBuildPhase::Idle; }
		ScriptBuildPhase GetPhase() const { return m_Phase; }
		// The running build's number (0 when none runs) and how long it has been running.
		uint64_t GetCurrentID() const { return IsRunning() ? m_NextID - 1 : 0; }
		double GetElapsedSeconds() const;
		// The result of the last finished build (ID 0 when no build finished yet).
		const ScriptBuildResult& GetLastResult() const { return m_LastResult; }
	private:
		bool StartProcess(const std::vector<std::string>& arguments, std::string* outError);
		// Moves new output into the log text and streams complete lines to the log.
		void CollectOutput(bool flushPartialLine);
		void Finish(bool success, std::string error);
	private:
		ScriptBuildSettings m_Settings;
		std::filesystem::path m_BuildDirectory;
		std::filesystem::path m_Module;
		int64_t m_ModuleTimeBefore = 0;
		bool m_ModuleExistedBefore = false;

		Scope<Process> m_Process;
		ScriptBuildPhase m_Phase = ScriptBuildPhase::Idle;
		bool m_Configured = false;
		std::chrono::steady_clock::time_point m_StartTime;
		// After the process exited its last output may still be in transit (or held by a process it started): it is
		// collected until the output ends, for a limited time after the exit.
		std::chrono::steady_clock::time_point m_ExitTime;
		bool m_Draining = false;
		std::string m_Log;
		OutputLineSplitter m_OutputLines;
		std::string m_PendingStamp; // Written once the configure step succeeded

		uint64_t m_NextID = 1;
		ScriptBuildResult m_LastResult;
	};

}
