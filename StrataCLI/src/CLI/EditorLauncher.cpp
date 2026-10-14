#include "CLI/EditorLauncher.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/StringUtils.h"
#include "Strata/Network/JsonRpc.h"
#include "Strata/Network/RpcClient.h"
#include "Strata/Network/Socket.h"

#include <algorithm>
#include <thread>

namespace Strata::CLI
{

	namespace
	{

		constexpr const char* c_EditorPathVariable = "STRATA_EDITOR_PATH";
		constexpr std::chrono::milliseconds c_SessionPollInterval = std::chrono::milliseconds(50);
		constexpr std::chrono::milliseconds c_SessionConnectTimeout = std::chrono::milliseconds(1000);
		// How often a launch asks the editor how far its project is, and how long one answer may take.
		constexpr std::chrono::milliseconds c_ProjectPollInterval = std::chrono::milliseconds(250);
		constexpr std::chrono::milliseconds c_StatusTimeout = std::chrono::milliseconds(10000);
		constexpr std::string_view c_LaunchClientName = "StrataCLI launch";

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
		if (!specification.ProjectDirectory.empty() && !FileSystem::IsDirectory(specification.ProjectDirectory))
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

		ProcessSpecification processSpecification;
		processSpecification.Executable = specification.EditorPath;
		if (!specification.ProjectDirectory.empty())
		{
			std::error_code error;
			std::filesystem::path projectDirectory = std::filesystem::absolute(specification.ProjectDirectory, error);
			if (error)
				projectDirectory = specification.ProjectDirectory;
			processSpecification.Arguments = { "--project", FileSystem::ToUTF8(projectDirectory.lexically_normal()) };
		}
		if (specification.Headless)
			processSpecification.Arguments.push_back("--headless");
		if (specification.NoGpu)
			processSpecification.Arguments.push_back("--no-gpu");
		if (specification.IdleTimeout)
		{
			processSpecification.Arguments.push_back("--idle-timeout");
			processSpecification.Arguments.push_back(std::to_string(specification.IdleTimeout->count()));
		}
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
		{
			// Nobody could reach it (and it may never quit by itself): stop it rather than leave it running unseen.
			if (process.IsRunning() && process.Terminate())
				result.Error += "; the editor was stopped";
			return result;
		}

		if (specification.WaitForProject && !specification.ProjectDirectory.empty()
			&& !WaitForEditorProject(*session, specification.StallTimeout, specification.OnProgress, [&process]() { return process.IsRunning(); }, &result.Error))
		{
			if (process.IsRunning() && process.Terminate())
				result.Error += "; the editor was stopped";
			return result;
		}

		result.Success = true;
		result.Session = std::move(*session);
		return result;
	}

	bool WaitForEditorProject(const EditorSessionInfo& session, std::chrono::milliseconds stallTimeout,
		const std::function<void(const EditorStartupProgress&)>& onProgress, const std::function<bool()>& isProcessAlive, std::string* error)
	{
		auto fail = [error](std::string message)
		{
			if (error)
				*error = std::move(message);
			return false;
		};
		RpcClient client;
		if (!client.Connect(session.Address, session.Port, session.Token, c_SessionConnectTimeout, c_LaunchClientName))
			return fail(fmt::format("The editor's session on port {} does not accept connections: {}", session.Port, client.GetLastError()));

		std::optional<EditorStartupProgress> last;
		auto lastChange = std::chrono::steady_clock::now();
		while (true)
		{
			const RpcResult status = client.Call("editor.status", nlohmann::json::object(), c_StatusTimeout);
			// An editor without the status (an older one) has nothing to wait for.
			if (status.IsError() && status.GetError().Code == static_cast<int>(JsonRpc::ErrorCode::MethodNotFound))
				return true;
			if (status.IsError())
			{
				if (isProcessAlive && !isProcessAlive())
					return fail("The editor exited while its project was opening");
				if (!client.IsConnected() && !client.Connect(session.Address, session.Port, session.Token, c_SessionConnectTimeout, c_LaunchClientName))
					return fail(fmt::format("The editor stopped answering while its project was opening: {}", client.GetLastError()));
			}
			else
			{
				const nlohmann::json& value = status.GetValue();
				const nlohmann::json* assets = value.is_object() ? JsonUtils::Find(value, "assets") : nullptr;
				const nlohmann::json* scanning = assets && assets->is_object() ? JsonUtils::Find(*assets, "scanning") : nullptr;
				const nlohmann::json* sceneLoad = value.is_object() ? JsonUtils::Find(value, "sceneLoad") : nullptr;
				EditorStartupProgress progress;
				if (scanning && scanning->is_object())
				{
					progress.Phase = JsonUtils::GetString(*scanning, "phase");
					progress.Done = JsonUtils::GetUInt(*scanning, "done", 0);
					progress.Total = JsonUtils::GetUInt(*scanning, "total", 0);
					progress.Message = progress.Total > 0
						? fmt::format("Scanning the project's assets: {} / {}", StringUtils::FormatThousands(progress.Done), StringUtils::FormatThousands(progress.Total))
						: fmt::format("Scanning the project's assets: {} files found", StringUtils::FormatThousands(progress.Done));
				}
				else if (sceneLoad && sceneLoad->is_object())
				{
					progress.Phase = "OpeningScene";
					progress.Done = static_cast<uint64_t>(std::clamp(static_cast<double>(JsonUtils::GetFloat(*sceneLoad, "progress", 0.0f)), 0.0, 1.0) * 100.0);
					progress.Total = 100;
					progress.Message = fmt::format("Opening the start scene: {}%", progress.Done);
				}
				else
				{
					return true;
				}
				const bool changed = !last || last->Phase != progress.Phase || last->Done != progress.Done || last->Total != progress.Total;
				if (changed)
				{
					lastChange = std::chrono::steady_clock::now();
					if (onProgress)
						onProgress(progress);
					last = std::move(progress);
				}
			}
			if (std::chrono::steady_clock::now() - lastChange >= stallTimeout)
			{
				const std::string where = last ? last->Message : std::string("the editor's status");
				return fail(fmt::format("The editor's project stopped progressing: no change for {} s ({})",
					std::chrono::duration_cast<std::chrono::seconds>(stallTimeout).count(), where));
			}
			if (isProcessAlive && !isProcessAlive())
				return fail("The editor exited while its project was opening");
			std::this_thread::sleep_for(c_ProjectPollInterval);
		}
	}

	std::optional<EditorSessionInfo> WaitForEditorSession(uint32_t processId, const std::filesystem::path& sessionDirectory, std::chrono::milliseconds timeout,
		const std::function<bool()>& isProcessAlive, std::string* error)
	{
		const std::filesystem::path sessionFile = EditorSession::GetSessionFilePath(sessionDirectory, processId);
		// Clamped so that the deadline cannot overflow (e.g. for milliseconds::max() meaning "wait as long as it takes").
		const std::chrono::milliseconds limit = ClampSocketTimeout(timeout);
		const auto deadline = std::chrono::steady_clock::now() + limit;
		std::string lastProblem = "no session file was written";
		while (true)
		{
			if (std::optional<EditorSessionInfo> session = EditorSession::ReadSessionFile(sessionFile))
			{
				RpcClient client;
				if (!IsLoopbackAddress(session->Address))
				{
					if (error)
						*error = fmt::format("The editor's session names '{}', which is not a loopback address", session->Address);
					return std::nullopt;
				}
				if (client.Connect(session->Address, session->Port, session->Token, c_SessionConnectTimeout))
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
					*error = fmt::format("Timed out after {} ms waiting for the editor (process {}): {}", limit.count(), processId, lastProblem);
				return std::nullopt;
			}
			std::this_thread::sleep_for(c_SessionPollInterval);
		}
	}

}
