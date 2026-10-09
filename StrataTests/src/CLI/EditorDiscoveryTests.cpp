#if defined(STRATA_TESTS_HAVE_CLI)

#include <doctest/doctest.h>

#include "CLI/EditorConnection.h"
#include "CLI/EditorLauncher.h"
#include "CLI/FakeEditor.h"
#include "Strata/Core/Platform.h"
#include "TestHelpers.h"

#include <atomic>
#include <string>
#include <thread>

using namespace Strata;
using namespace Strata::CLI;

namespace
{
	EditorConnectionOptions MakeOptions(const std::filesystem::path& sessionDirectory)
	{
		EditorConnectionOptions options;
		options.SessionDirectory = sessionDirectory;
		options.ConnectTimeout = std::chrono::milliseconds(3000);
		return options;
	}
}

TEST_SUITE("CLI.Discovery")
{
	TEST_CASE("Endpoints are discovered in precedence order")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("DiscoverySessions");
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("DiscoveryProject");
		const std::filesystem::path otherProject = Tests::CreateTemporaryDirectory("DiscoveryOtherProject");

		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(101, 40101, "2026-01-01T00:00:00Z", FileSystem::ToUTF8(project))));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(102, 40102, "2026-02-01T00:00:00Z", FileSystem::ToUTF8(otherProject))));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(103, 40103, "2026-03-01T00:00:00Z")));
		const EditorSessionInfo projectFileSession = Tests::MakeFakeSession(104, 40104, "2026-04-01T00:00:00Z", FileSystem::ToUTF8(project));
		REQUIRE(FileSystem::WriteText(EditorSession::GetProjectSessionFilePath(project), projectFileSession.ToJson().dump()));

		EditorConnectionOptions options = MakeOptions(sessionDirectory);

		SUBCASE("Without a project, every session newest first")
		{
			const std::vector<EditorEndpoint> endpoints = DiscoverEditorEndpoints(options);
			REQUIRE(endpoints.size() == 3);
			CHECK(endpoints[0].Port == 40103);
			CHECK(endpoints[1].Port == 40102);
			CHECK(endpoints[2].Port == 40101);
			CHECK(endpoints[0].Source == "session");
			CHECK(endpoints[0].Token == Tests::c_FakeEditorToken);
			CHECK(endpoints[0].Host == "127.0.0.1");
		}

		SUBCASE("With a project, its session file first, then its sessions only")
		{
			options.ProjectDirectory = project;
			const std::vector<EditorEndpoint> endpoints = DiscoverEditorEndpoints(options);
			REQUIRE(endpoints.size() == 2);
			CHECK(endpoints[0].Port == 40104);
			CHECK(endpoints[0].Source == "project");
			CHECK(endpoints[1].Port == 40101);
		}

		SUBCASE("An explicit endpoint disables discovery")
		{
			options.Port = 45000;
			options.Token = "explicit";
			options.ProjectDirectory = project;
			const std::vector<EditorEndpoint> endpoints = DiscoverEditorEndpoints(options);
			REQUIRE(endpoints.size() == 1);
			CHECK(endpoints[0].Port == 45000);
			CHECK(endpoints[0].Token == "explicit");
			CHECK(endpoints[0].Source == "explicit");
			CHECK_FALSE(endpoints[0].Session.has_value());
		}
	}

	TEST_CASE("Project paths are compared by location")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("SameProject");
		CHECK(IsSameProject(FileSystem::ToUTF8(project), project));
		CHECK(IsSameProject(FileSystem::ToUTF8(project) + "/", project));
		CHECK(IsSameProject(FileSystem::ToUTF8(project / "sub" / ".."), project));
		CHECK_FALSE(IsSameProject(FileSystem::ToUTF8(project / "other"), project));
		CHECK_FALSE(IsSameProject("", project));
	}

	TEST_CASE("The newest reachable session is used and stale sessions are skipped")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("DiscoveryStale");
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));

		// A newer session left behind by an editor that is gone, and an older one that is alive.
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(201, Tests::GetClosedPort(), "2026-05-01T00:00:00Z")));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(202, editor.GetPort(), "2026-01-01T00:00:00Z")));

		EditorConnection connection(MakeOptions(sessionDirectory));
		CHECK_FALSE(connection.IsConnected());
		REQUIRE_MESSAGE(connection.EnsureConnected(), connection.GetLastError());
		REQUIRE(connection.GetEndpoint().has_value());
		CHECK(connection.GetEndpoint()->Port == editor.GetPort());
		CHECK(connection.GetEndpoint()->Session->ProcessId == 202);

		const RpcResult result = connection.Call("entity.create", nlohmann::json { { "Name", "Found" } }, std::chrono::milliseconds(5000));
		REQUIRE(result.IsSuccess());

		nlohmann::json status = connection.DescribeStatus();
		CHECK(status["connected"] == true);
		CHECK(status["session"]["ProcessId"] == 202);
		CHECK_FALSE(status["session"].contains("Token"));
	}

	TEST_CASE("A dropped connection is re-established through discovery")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("DiscoveryReconnect");
		auto first = CreateScope<Tests::PumpedRpcServer>();
		REQUIRE(Tests::StartFakeEditor(*first));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(301, first->GetPort(), "2026-01-01T00:00:00Z")));

		EditorConnection connection(MakeOptions(sessionDirectory));
		REQUIRE(connection.Call("rpc.ping", nlohmann::json::object(), std::chrono::milliseconds(5000)).IsSuccess());

		// The editor restarts on another port and rewrites its session file.
		first->Stop();
		first.reset();
		Tests::PumpedRpcServer second;
		REQUIRE(Tests::StartFakeEditor(second));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(301, second.GetPort(), "2026-01-01T00:00:01Z")));

		const RpcResult result = connection.Call("rpc.ping", nlohmann::json::object(), std::chrono::milliseconds(5000));
		REQUIRE_MESSAGE(result.IsSuccess(), result.GetError().Message);
		CHECK(connection.GetEndpoint()->Port == second.GetPort());

		second.Stop();
		const RpcResult unreachable = connection.Call("rpc.ping", nlohmann::json::object(), std::chrono::milliseconds(5000));
		CHECK(unreachable.IsError());
		CHECK(unreachable.GetError().Code == JsonRpc::ErrorCode::ConnectionClosed);
		CHECK_FALSE(connection.GetLastError().empty());
	}

	TEST_CASE("The editor executable is resolved from the option, the environment or the default location")
	{
		Tests::ScopedEnvironmentVariable editorPath("STRATA_EDITOR_PATH", "");
		const std::filesystem::path defaultPath = ResolveEditorPath(std::nullopt);
		CHECK(defaultPath.parent_path() == Platform::GetExecutableDirectory());
		CHECK(defaultPath.stem() == "StrataEditor");

		{
			Tests::ScopedEnvironmentVariable fromEnvironment("STRATA_EDITOR_PATH", "C:/Tools/Editor \xC3\xA9.exe");
			CHECK(ResolveEditorPath(std::nullopt) == FileSystem::FromUTF8("C:/Tools/Editor \xC3\xA9.exe"));
			CHECK(ResolveEditorPath(std::string("D:/Explicit.exe")) == FileSystem::FromUTF8("D:/Explicit.exe"));
		}
		CHECK(ResolveEditorPath(std::string()) == defaultPath);
	}

	TEST_CASE("Launching validates the editor and project paths")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("LaunchValidation");

		EditorLaunchSpecification specification;
		specification.EditorPath = directory / "Missing.exe";
		specification.ProjectDirectory = directory;
		EditorLaunchResult result = LaunchEditor(specification);
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("not found") != std::string::npos);
		CHECK(result.EditorProcess == nullptr);

		specification.EditorPath = Tests::GetTestExecutablePath();
		specification.ProjectDirectory = directory / "NoSuchProject";
		result = LaunchEditor(specification);
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("does not exist") != std::string::npos);
	}

	TEST_CASE("Waiting for a launched editor's session")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("LaunchWait");
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));

		SUBCASE("The session is found once it is written and accepts connections")
		{
			// The editor writes its session file a little after starting, first with a port that is not up yet.
			std::thread writer([&]()
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(777, Tests::GetClosedPort(), "2026-01-01T00:00:00Z"));
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(777, editor.GetPort(), "2026-01-01T00:00:00Z"));
			});

			std::string error;
			const auto start = std::chrono::steady_clock::now();
			std::optional<EditorSessionInfo> session = WaitForEditorSession(777, sessionDirectory, std::chrono::milliseconds(5000), [] { return true; }, &error);
			writer.join();
			REQUIRE_MESSAGE(session.has_value(), error);
			CHECK(session->Port == editor.GetPort());
			CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(4000));
		}

		SUBCASE("A session of another process is not mistaken for it")
		{
			REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(778, editor.GetPort(), "2026-01-01T00:00:00Z")));
			std::string error;
			CHECK_FALSE(WaitForEditorSession(779, sessionDirectory, std::chrono::milliseconds(150), {}, &error).has_value());
			CHECK(error.find("Timed out") != std::string::npos);
		}

		SUBCASE("A session with a wrong token never counts as ready")
		{
			EditorSessionInfo session = Tests::MakeFakeSession(780, editor.GetPort(), "2026-01-01T00:00:00Z");
			session.Token = "not-the-token";
			REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, session));
			std::string error;
			CHECK_FALSE(WaitForEditorSession(780, sessionDirectory, std::chrono::milliseconds(200), {}, &error).has_value());
			CHECK(error.find("Authentication failed") != std::string::npos);
		}

		SUBCASE("The wait ends early when the editor exits")
		{
			std::atomic<int> checks = 0;
			std::string error;
			const auto start = std::chrono::steady_clock::now();
			CHECK_FALSE(WaitForEditorSession(781, sessionDirectory, std::chrono::milliseconds(10000), [&checks]() { return ++checks < 3; }, &error).has_value());
			CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(5000));
			CHECK(error.find("exited") != std::string::npos);
		}
	}
}

#endif
