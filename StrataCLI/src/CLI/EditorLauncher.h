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
		// With a project: once the session answers, wait until the project is ready (its assets scanned and its start scene
		// open, editor.status), for as long as either progresses; WaitTimeout bounds only the wait for the session. False
		// returns at the session (--no-wait-scan).
		bool WaitForProject = true;
		// The wait for the project fails, and the editor is stopped, once neither the scan nor the start scene progressed for
		// this long (--stall-timeout).
		std::chrono::milliseconds StallTimeout = std::chrono::milliseconds(60000);
		// Hears how the project's opening progresses while LaunchEditor waits for it (StrataCLI launch prints it, the MCP
		// server sends progress notifications). Called on the calling thread.
		std::function<void(const struct EditorStartupProgress& progress)> OnProgress;
	};

	// How far a launched editor's project is (EditorLaunchSpecification::OnProgress).
	struct EditorStartupProgress
	{
		std::string Phase;   // "Walking", "Registering", "Applying" (the asset scan) or "OpeningScene" (the start scene)
		uint64_t Done = 0;   // Files, or percent of the start scene
		uint64_t Total = 0;  // 0 while unknown
		std::string Message; // For people: "Scanning the project's assets: 12,345 / 30,000"
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
	// output discarded) and waits until its session accepts an authenticated connection, then (with a project, unless
	// WaitForProject is off) until the project is ready. An editor that exits early, does not become reachable in time or
	// whose project stops progressing is stopped, never left running unnoticed.
	EditorLaunchResult LaunchEditor(const EditorLaunchSpecification& specification);

	// Waits until the editor of a session has its project ready: editor.status reports no asset scan
	// (assets.scanning null) and no scene being opened (sceneLoad null), polled every 250 ms. Returns false, with the reason
	// in error, when the editor exits (isProcessAlive) or stops answering, or neither progresses for stallTimeout.
	bool WaitForEditorProject(const EditorSessionInfo& session, std::chrono::milliseconds stallTimeout,
		const std::function<void(const EditorStartupProgress&)>& onProgress, const std::function<bool()>& isProcessAlive, std::string* error);

	// Waits until the session file of processId exists in sessionDirectory and its endpoint accepts an
	// authenticated connection. isProcessAlive (optional) ends the wait early when the editor exits.
	// Returns nullopt on timeout or early exit, with the reason in error (if given). The timeout is clamped to
	// c_MaxSocketTimeout.
	std::optional<EditorSessionInfo> WaitForEditorSession(uint32_t processId, const std::filesystem::path& sessionDirectory, std::chrono::milliseconds timeout,
		const std::function<bool()>& isProcessAlive = {}, std::string* error = nullptr);

}
