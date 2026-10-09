#include "Network/FakeEditorProcess.h"

#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Network/EditorSession.h"
#include "Strata/Network/RpcServer.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

namespace Strata::Tests
{

	namespace
	{
		constexpr const char* c_FakeEditorVariable = "STRATA_TEST_FAKE_EDITOR";
		// A fake editor left behind by a failed test still goes away on its own.
		constexpr std::chrono::seconds c_FakeEditorLifetime = std::chrono::seconds(60);
		constexpr std::chrono::seconds c_QuitGracePeriod = std::chrono::seconds(2);
	}

	bool IsFakeEditorLaunch(int argc, char** argv)
	{
		if (Platform::GetEnvVar(c_FakeEditorVariable).value_or(std::string()) != "1")
			return false;
		// Only the arguments an editor launch passes, so the test run itself is never mistaken for a launch.
		for (int index = 1; index < argc; index++)
		{
			const std::string_view argument = argv[index];
			if (argument == "--project" && index + 1 < argc)
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
		for (int index = 1; index < argc; index++)
		{
			const std::string_view argument = argv[index];
			if (argument == "--project" && index + 1 < argc)
				project = argv[++index];
			else if (argument == "--headless")
				headless = true;
			else if (argument == "--no-gpu")
				noGpu = true;
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
			return RpcResult::Success(nlohmann::json { { "Project", project }, { "Headless", headless }, { "NoGpu", noGpu }, { "ProcessId", Platform::GetProcessID() } });
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
