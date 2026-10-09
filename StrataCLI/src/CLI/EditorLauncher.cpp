#include "CLI/EditorLauncher.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Network/RpcClient.h"

#include <thread>

namespace Strata::CLI
{

	namespace
	{

		constexpr const char* c_EditorPathVariable = "STRATA_EDITOR_PATH";
		constexpr std::chrono::milliseconds c_SessionPollInterval = std::chrono::milliseconds(50);
		constexpr std::chrono::milliseconds c_SessionConnectTimeout = std::chrono::milliseconds(1000);

		std::filesystem::path GetDefaultEditorFileName()
		{
#if defined(ST_PLATFORM_WINDOWS)
			return "StrataEditor.exe";
#else
			return "StrataEditor";
#endif
		}

	}

	std::filesystem::path ResolveEditorPath(const std::optional<std::string>& explicitPath)
	{
		std::filesystem::path path;
		if (explicitPath && !explicitPath->empty())
			path = FileSystem::FromUTF8(*explicitPath);
		else if (const std::optional<std::string> environmentPath = Platform::GetEnvVar(c_EditorPathVariable); environmentPath && !environmentPath->empty())
			path = FileSystem::FromUTF8(*environmentPath);
		else
			return Platform::GetExecutableDirectory() / GetDefaultEditorFileName();

		// The path that is validated must be the one that is started: a relative path would otherwise be checked
		// against the current directory, but a bare name is searched on PATH when launching.
		std::error_code error;
		std::filesystem::path absolute = std::filesystem::absolute(path, error);
		return (error ? path : absolute).lexically_normal();
	}

	EditorLaunchResult LaunchEditor(const EditorLaunchSpecification& specification)
	{
		EditorLaunchResult result;
		if (!FileSystem::IsRegularFile(specification.EditorPath))
		{
			result.Error = fmt::format("Editor executable not found at '{}' (use --editor or set {})", FileSystem::ToUTF8(specification.EditorPath), c_EditorPathVariable);
			return result;
		}
		if (!FileSystem::IsDirectory(specification.ProjectDirectory))
		{
			result.Error = fmt::format("Project directory '{}' does not exist", FileSystem::ToUTF8(specification.ProjectDirectory));
			return result;
		}

		// The editor publishes its session in the same directory (it inherits STRATA_SESSION_DIR); without a usable
		// one the editor could not be found, so nothing is started.
		std::filesystem::path sessionDirectory = specification.SessionDirectory;
		if (sessionDirectory.empty())
		{
			std::optional<std::filesystem::path> defaultDirectory = EditorSession::GetSessionDirectory(&result.Error);
			if (!defaultDirectory)
				return result;
			sessionDirectory = std::move(*defaultDirectory);
		}

		std::error_code error;
		std::filesystem::path projectDirectory = std::filesystem::absolute(specification.ProjectDirectory, error);
		if (error)
			projectDirectory = specification.ProjectDirectory;
		projectDirectory = projectDirectory.lexically_normal();

		ProcessSpecification processSpecification;
		processSpecification.Executable = specification.EditorPath;
		processSpecification.Arguments = { "--project", FileSystem::ToUTF8(projectDirectory) };
		if (specification.Headless)
			processSpecification.Arguments.push_back("--headless");
		processSpecification.Output = ProcessOutputMode::Discard;
		processSpecification.Detached = true;

		result.EditorProcess = CreateScope<Process>();
		if (!result.EditorProcess->Start(processSpecification))
		{
			result.Error = result.EditorProcess->GetLastError();
			result.EditorProcess.reset();
			return result;
		}

		const uint32_t processId = result.EditorProcess->GetProcessID();
		ST_INFO("Started the editor (process {}), waiting for its session", processId);

		Process& process = *result.EditorProcess;
		std::optional<EditorSessionInfo> session = WaitForEditorSession(processId, sessionDirectory, specification.WaitTimeout,
			[&process]() { return process.IsRunning(); }, &result.Error);
		if (!session)
			return result;

		result.Success = true;
		result.Session = std::move(*session);
		return result;
	}

	std::optional<EditorSessionInfo> WaitForEditorSession(uint32_t processId, const std::filesystem::path& sessionDirectory, std::chrono::milliseconds timeout,
		const std::function<bool()>& isProcessAlive, std::string* error)
	{
		const std::filesystem::path sessionFile = EditorSession::GetSessionFilePath(sessionDirectory, processId);
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		std::string lastProblem = "no session file was written";
		while (true)
		{
			if (std::optional<EditorSessionInfo> session = EditorSession::ReadSessionFile(sessionFile))
			{
				RpcClient client;
				if (client.Connect("127.0.0.1", session->Port, session->Token, c_SessionConnectTimeout))
					return session;
				lastProblem = fmt::format("the session on port {} does not accept connections: {}", session->Port, client.GetLastError());
			}

			if (isProcessAlive && !isProcessAlive())
			{
				if (error)
					*error = fmt::format("The editor (process {}) exited before its session became available", processId);
				return std::nullopt;
			}

			if (std::chrono::steady_clock::now() >= deadline)
			{
				if (error)
					*error = fmt::format("Timed out after {} ms waiting for the editor (process {}): {}", timeout.count(), processId, lastProblem);
				return std::nullopt;
			}
			std::this_thread::sleep_for(c_SessionPollInterval);
		}
	}

}
