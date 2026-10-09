#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/Process.h"
#include "Strata/Network/EditorSession.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace Strata::CLI
{

	// The editor executable as an absolute path: explicitPath (--editor), else the STRATA_EDITOR_PATH environment
	// variable, else StrataEditor[.exe] next to the running executable.
	std::filesystem::path ResolveEditorPath(const std::optional<std::string>& explicitPath);

	struct EditorLaunchSpecification
	{
		std::filesystem::path EditorPath;
		std::filesystem::path ProjectDirectory; // Empty starts the editor without a project (e.g. to create one)
		bool Headless = false;                  // No window ("--headless")
		bool NoGpu = false;                     // No graphics device either ("--no-gpu", implies headless)
		std::chrono::milliseconds WaitTimeout = std::chrono::milliseconds(60000);
		// Where the editor publishes its session file; empty uses EditorSession::GetSessionDirectory(). The
		// editor inherits this process's environment, so STRATA_SESSION_DIR applies to both.
		std::filesystem::path SessionDirectory;
	};

	struct EditorLaunchResult
	{
		bool Success = false;
		EditorSessionInfo Session;
		std::string Error;
		// The started editor (also set when waiting failed but the process is still running). Destroying it does
		// not stop the editor; keeping it lets the caller poll IsRunning(), which also reaps it on POSIX.
		Scope<Process> EditorProcess;
	};

	// Starts the editor detached ("--project <dir>" when a project is given, "--headless", "--no-gpu"; output discarded)
	// and waits until its session accepts an authenticated connection.
	EditorLaunchResult LaunchEditor(const EditorLaunchSpecification& specification);

	// Waits until the session file of processId exists in sessionDirectory and its endpoint accepts an
	// authenticated connection. isProcessAlive (optional) ends the wait early when the editor exits.
	// Returns nullopt on timeout or early exit, with the reason in error (if given). The timeout is clamped to
	// c_MaxSocketTimeout.
	std::optional<EditorSessionInfo> WaitForEditorSession(uint32_t processId, const std::filesystem::path& sessionDirectory, std::chrono::milliseconds timeout,
		const std::function<bool()>& isProcessAlive = {}, std::string* error = nullptr);

}
