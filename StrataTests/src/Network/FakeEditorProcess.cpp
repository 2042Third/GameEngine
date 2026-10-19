#include "Network/FakeEditorProcess.h"

#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Network/EditorSession.h"
#include "Strata/Network/RpcServer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

namespace Strata::Tests
{

	namespace
	{
		constexpr const char* c_FakeEditorVariable = "STRATA_TEST_FAKE_EDITOR";
		constexpr const char* c_FakeScanVariable = "STRATA_TEST_FAKE_SCAN";
		constexpr const char* c_FakeSceneLoadVariable = "STRATA_TEST_FAKE_SCENE_LOAD";
		// A fake editor left behind by a failed test still goes away on its own.
		constexpr std::chrono::seconds c_FakeEditorLifetime = std::chrono::seconds(60);
		constexpr std::chrono::seconds c_QuitGracePeriod = std::chrono::seconds(2);
	}

	bool IsFakeEditorLaunch(int argc, char** argv)
	{
		const std::string mode = Platform::GetEnvVar(c_FakeEditorVariable).value_or(std::string());
		if (mode != "1" && mode != "silent")
			return false;
		// At least one argument, and only the ones an editor launch passes, so the test run itself is never mistaken for
		// a launch.
		if (argc < 2)
			return false;
		for (int index = 1; index < argc; index++)
		{
			const std::string_view argument = argv[index];
			if ((argument == "--project" || argument == "--idle-timeout") && index + 1 < argc)
				index++;
			else if (argument != "--headless" && argument != "--no-gpu")
				return false;
		}
		return true;
	}

	int RunFakeEditor(int argc, char** argv)
	{
		std::string project;
		bool headless = false;
		bool noGpu = false;
		std::string idleTimeout;
		for (int index = 1; index < argc; index++)
		{
			const std::string_view argument = argv[index];
			if (argument == "--project" && index + 1 < argc)
				project = argv[++index];
			else if (argument == "--idle-timeout" && index + 1 < argc)
				idleTimeout = argv[++index];
			else if (argument == "--headless")
				headless = true;
			else if (argument == "--no-gpu")
				noGpu = true;
		}

		// "silent": an editor that never becomes reachable (it publishes no session), for the launch timeout tests.
		if (Platform::GetEnvVar(c_FakeEditorVariable).value_or(std::string()) == "silent")
		{
			std::this_thread::sleep_for(c_FakeEditorLifetime);
			return 0;
		}

		LogSpecification logSpecification;
		logSpecification.ConsoleOutput = false;
		Log::Init(logSpecification);

		std::atomic<bool> quitRequested = false;
		RpcServer server;
		RpcMethodInfo info;
		info.Name = "editor.info";
		info.Description = "Describes the fake editor";
		server.RegisterMethod(info, [&](const nlohmann::json&)
		{
			return RpcResult::Success(nlohmann::json { { "Project", project }, { "Headless", headless }, { "NoGpu", noGpu }, { "IdleTimeout", idleTimeout },
				{ "ProcessId", Platform::GetProcessID() } });
		});
		// What a launch waits for: the project's asset scan (STRATA_TEST_FAKE_SCAN: "<milliseconds>" from the session on,
		// advancing from 0 to 1,000 files; "stall": stuck at 5 of 1,000) and its start scene's load (STRATA_TEST_FAKE_SCENE_LOAD:
		// "<milliseconds>" from the session on), then nothing. Like the real editor's compact answer, the status has the scene's
		// load only when sections asks for it.
		const std::string scanMode = Platform::GetEnvVar(c_FakeScanVariable).value_or(std::string());
		const std::string sceneLoadMode = Platform::GetEnvVar(c_FakeSceneLoadVariable).value_or(std::string());
		const auto scanStart = std::chrono::steady_clock::now();
		RpcMethodInfo status;
		status.Name = "editor.status";
		status.Description = "The fake editor's status (its asset scan and its start scene's load)";
		server.RegisterMethod(status, [&scanMode, &sceneLoadMode, scanStart](const nlohmann::json& parameters)
		{
			const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scanStart).count();
			constexpr uint64_t c_Files = 1000;
			nlohmann::json scanning = nullptr;
			if (scanMode == "stall")
			{
				scanning = { { "phase", "Registering" }, { "done", 5 }, { "total", c_Files } };
			}
			else if (!scanMode.empty())
			{
				const double duration = std::max(1.0, std::strtod(scanMode.c_str(), nullptr));
				if (elapsed < duration)
					scanning = { { "phase", "Registering" }, { "done", static_cast<uint64_t>(elapsed / duration * c_Files) }, { "total", c_Files } };
			}
			nlohmann::json result = { { "assets", { { "scanning", scanning } } } };
			const auto sections = parameters.is_object() ? parameters.find("sections") : parameters.end();
			if (sections != parameters.end() && sections->is_array() && std::find(sections->begin(), sections->end(), "sceneLoad") != sections->end())
			{
				result["sceneLoad"] = nullptr;
				const double duration = sceneLoadMode.empty() ? 0.0 : std::max(1.0, std::strtod(sceneLoadMode.c_str(), nullptr));
				if (elapsed < duration)
					result["sceneLoad"] = { { "scene", "000000000000A001" }, { "startScene", true }, { "state", "LoadingScene" }, { "progress", elapsed / duration } };
			}
			return RpcResult::Success(std::move(result));
		});

		RpcMethodInfo quit;
		quit.Name = "editor.quit";
		quit.Description = "Exits the fake editor";
		server.RegisterMethod(quit, [&](const nlohmann::json&)
		{
			quitRequested = true;
			return RpcResult::Success(true);
		});

		RpcServerSpecification specification;
		specification.AuthToken = EditorSession::GenerateSessionToken();
		if (!server.Start(specification))
			return 2;

		EditorSessionInfo session = EditorSession::DescribeCurrentProcess();
		session.Address = specification.BindAddress;
		session.Port = server.GetPort();
		session.Token = specification.AuthToken;
		session.ProjectPath = project;
		session.Headless = headless || noGpu;
		if (!EditorSession::WriteSessionFiles(session))
			return 3;

		const auto deadline = std::chrono::steady_clock::now() + c_FakeEditorLifetime;
		while (!quitRequested.load() && std::chrono::steady_clock::now() < deadline)
		{
			server.ProcessRequests();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}

		// Let the reply to editor.quit reach the client before the server goes away.
		const auto quitDeadline = std::chrono::steady_clock::now() + c_QuitGracePeriod;
		while (server.GetClientCount() > 0 && std::chrono::steady_clock::now() < quitDeadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));

		EditorSession::RemoveSessionFiles(session);
		server.Stop();
		Log::Shutdown();
		return 0;
	}

}
