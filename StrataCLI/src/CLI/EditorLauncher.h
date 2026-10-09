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
		// The editor closes itself after this long without a connected client ("--idle-timeout <seconds>"), so an editor
		// started for a client that went away does not run forever. Unset: it runs until editor.quit.
		std::optional<std::chrono::seconds> IdleTimeout;
		// Where the editor publishes its session file; empty uses EditorSession::GetSessionDirectory(). The
		// editor inherits this process's environment, so STRATA_SESSION_DIR applies to both.
		std::filesystem::path SessionDirectory;
	};

	struct EditorLaunchResult
	{
		bool Success = false;
		EditorSessionInfo Session;
		std::string Error;
		// The started editor. Destroying it does not stop the editor; keeping it lets the caller poll IsRunning(), which
		// also reaps it on POSIX. After a failed wait the editor has been stopped (it is set so the caller can reap it).
		Scope<Process> EditorProcess;
	};

	// Starts the editor detached ("--project <dir>" when a project is given, "--headless", "--no-gpu", "--idle-timeout";
	// output discarded) and waits until its session accepts an authenticated connection. An editor that exits early or
	// does not become reachable in time is stopped, never left running unnoticed.
	EditorLaunchResult LaunchEditor(const EditorLaunchSpecification& specification);

	// Waits until the session file of processId exists in sessionDirectory and its endpoint accepts an
	// authenticated connection. isProcessAlive (optional) ends the wait early when the editor exits.
	// Returns nullopt on timeout or early exit, with the reason in error (if given). The timeout is clamped to
	// c_MaxSocketTimeout.
	std::optional<EditorSessionInfo> WaitForEditorSession(uint32_t processId, const std::filesystem::path& sessionDirectory, std::chrono::milliseconds timeout,
		const std::function<bool()>& isProcessAlive = {}, std::string* error = nullptr);

}
