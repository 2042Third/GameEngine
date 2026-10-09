#if defined(STRATA_TESTS_HAVE_CLI)

#include <doctest/doctest.h>

#include "CLI/EditorConnection.h"
#include "CLI/EditorLauncher.h"
#include "CLI/FakeEditor.h"
#include "Strata/Core/Platform.h"
#include "Strata/Network/RpcClient.h"
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

	// A fake editor: an automation server in this process, published under a live helper process's id.
	struct FakeEditorInstance
	{
		Tests::LiveProcess Owner;
		Tests::PumpedRpcServer Server;
		EditorSessionInfo Session;

		bool Start(const std::filesystem::path& sessionDirectory, std::string startedAt, std::string projectPath = {})
		{
			if (!Tests::StartFakeEditor(Server))
				return false;
			Session = Tests::MakeFakeSession(Owner.GetProcessId(), Server.GetPort(), std::move(startedAt), std::move(projectPath));
			return Tests::WriteFakeSessionFile(sessionDirectory, Session);
		}
	};
}

TEST_SUITE("CLI.Discovery")
{
	TEST_CASE("Endpoints are discovered in precedence order")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("DiscoverySessions");
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("DiscoveryProject");
		const std::filesystem::path otherProject = Tests::CreateTemporaryDirectory("DiscoveryOtherProject");

		Tests::LiveProcess first;
		Tests::LiveProcess second;
		Tests::LiveProcess third;
		Tests::LiveProcess pointed;
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(first.GetProcessId(), 40101, "2026-01-01T00:00:00Z", FileSystem::ToUTF8(project))));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(second.GetProcessId(), 40102, "2026-02-01T00:00:00Z", FileSystem::ToUTF8(otherProject))));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(third.GetProcessId(), 40103, "2026-03-01T00:00:00Z")));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(pointed.GetProcessId(), 40104, "2025-12-01T00:00:00Z", FileSystem::ToUTF8(project))));
		REQUIRE(FileSystem::WriteText(EditorSession::GetProjectSessionFilePath(project), nlohmann::json { { "ProcessId", pointed.GetProcessId() } }.dump()));

		EditorConnectionOptions options = MakeOptions(sessionDirectory);
		options.Host = "10.1.2.3"; // Only used for explicit endpoints

		SUBCASE("Without a project, every running editor's session newest first, always on loopback")
		{
			const std::vector<EditorEndpoint> endpoints = DiscoverEditorEndpoints(options);
			REQUIRE(endpoints.size() == 4);
			CHECK(endpoints[0].Port == 40103);
			CHECK(endpoints[1].Port == 40102);
			CHECK(endpoints[2].Port == 40101);
			CHECK(endpoints[3].Port == 40104);
			for (const EditorEndpoint& endpoint : endpoints)
			{
				CHECK(endpoint.Source == "session");
				CHECK(endpoint.Host == "127.0.0.1");
				CHECK(endpoint.Token == Tests::c_FakeEditorToken);
			}
		}

		SUBCASE("With a project, its pointed-to session first, then its other sessions only")
		{
			options.ProjectDirectory = project;
			const std::vector<EditorEndpoint> endpoints = DiscoverEditorEndpoints(options);
			REQUIRE(endpoints.size() == 2);
			CHECK(endpoints[0].Port == 40104);
			CHECK(endpoints[0].Source == "project");
			CHECK(endpoints[1].Port == 40101);
			CHECK(endpoints[1].Source == "session");
		}

		SUBCASE("An explicit endpoint disables discovery")
		{
			options.Port = 45000;
			options.Token = "explicit";
			options.ProjectDirectory = project;
			const std::vector<EditorEndpoint> endpoints = DiscoverEditorEndpoints(options);
			REQUIRE(endpoints.size() == 1);
			CHECK(endpoints[0].Port == 45000);
			CHECK(endpoints[0].Host == "10.1.2.3");
			CHECK(endpoints[0].Token == "explicit");
			CHECK(endpoints[0].Source == "explicit");
			CHECK_FALSE(endpoints[0].Session.has_value());
		}
	}

	TEST_CASE("Sessions are reached at the loopback address they record")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("DiscoveryAddresses");
		Tests::LiveProcess ipv6Owner;
		Tests::LiveProcess remoteOwner;
		EditorSessionInfo ipv6 = Tests::MakeFakeSession(ipv6Owner.GetProcessId(), 40301, "2026-02-01T00:00:00Z");
		ipv6.Address = "::1";
		EditorSessionInfo remote = Tests::MakeFakeSession(remoteOwner.GetProcessId(), 40302, "2026-03-01T00:00:00Z");
		remote.Address = "192.168.1.5";
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, ipv6));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, remote));

		// A session naming anything but a loopback address is never followed.
		const std::vector<EditorEndpoint> endpoints = DiscoverEditorEndpoints(MakeOptions(sessionDirectory));
		REQUIRE(endpoints.size() == 1);
		CHECK(endpoints[0].Host == "::1");
		CHECK(endpoints[0].Port == 40301);

		EditorConnection connection(MakeOptions(sessionDirectory));
		CHECK_FALSE(connection.ConnectToSession(remote));
		CHECK(connection.GetLastError().find("loopback") != std::string::npos);

		// Sessions written without an address are on IPv4 loopback.
		nlohmann::json legacy = ipv6.ToJson();
		legacy.erase("Address");
		CHECK(EditorSessionInfo::FromJson(legacy)->Address == "127.0.0.1");
	}

	TEST_CASE("An editor listening on IPv6 loopback is found through its session")
	{
		Tests::PumpedRpcServer editor;
		Tests::RegisterFakeEditorMethods(editor.GetServer());
		RpcServerSpecification specification;
		specification.AuthToken = Tests::c_FakeEditorToken;
		specification.BindAddress = "::1";
		if (!editor.Start(specification))
		{
			MESSAGE("IPv6 loopback is not available on this machine; the address handling is covered by the test above");
			return;
		}

		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("DiscoveryIPv6");
		Tests::LiveProcess owner;
		EditorSessionInfo session = Tests::MakeFakeSession(owner.GetProcessId(), editor.GetPort(), "2026-01-01T00:00:00Z");
		session.Address = "::1";
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, session));

		EditorConnection connection(MakeOptions(sessionDirectory));
		const RpcResult result = connection.Call("rpc.ping", nlohmann::json::object(), std::chrono::milliseconds(5000));
		REQUIRE_MESSAGE(result.IsSuccess(), result.GetError().Message);
		CHECK(connection.GetEndpoint()->Host == "::1");
	}

	TEST_CASE("Project paths are compared by location")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("SameProject");
		CHECK(EditorSession::IsSameProject(FileSystem::ToUTF8(project), project));
		CHECK(EditorSession::IsSameProject(FileSystem::ToUTF8(project) + "/", project));
		CHECK(EditorSession::IsSameProject(FileSystem::ToUTF8(project / "sub" / ".."), project));
		CHECK_FALSE(EditorSession::IsSameProject(FileSystem::ToUTF8(project / "other"), project));
		CHECK_FALSE(EditorSession::IsSameProject("", project));
	}

	TEST_CASE("The newest reachable session is used and stale sessions are skipped")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("DiscoveryStale");
		FakeEditorInstance editor;
		REQUIRE(editor.Start(sessionDirectory, "2026-01-01T00:00:00Z"));

		// Newer sessions of an editor whose process is gone, and of a live process that no longer serves its port.
		Tests::ExitedProcess exited;
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(exited.GetProcessId(), 40200, "2026-06-01T00:00:00Z")));
		Tests::LiveProcess hung;
		Tests::RefusingPort deadPort;
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(hung.GetProcessId(), deadPort.GetPort(), "2026-05-01T00:00:00Z")));

		EditorConnection connection(MakeOptions(sessionDirectory));
		CHECK_FALSE(connection.IsConnected());
		REQUIRE_MESSAGE(connection.EnsureConnected(), connection.GetLastError());
		REQUIRE(connection.GetEndpoint().has_value());
		CHECK(connection.GetEndpoint()->Port == editor.Server.GetPort());
		CHECK(connection.GetEndpoint()->Session->ProcessId == editor.Session.ProcessId);

		const RpcResult result = connection.Call("entity.create", nlohmann::json { { "Name", "Found" } }, std::chrono::milliseconds(5000));
		REQUIRE(result.IsSuccess());

		nlohmann::json status = connection.DescribeStatus();
		CHECK(status["connected"] == true);
		CHECK(status["session"]["ProcessId"] == editor.Session.ProcessId);
		CHECK_FALSE(status["session"].contains("Token"));
		CHECK(status["pinnedEditor"]["ProcessId"] == editor.Session.ProcessId);
	}

	TEST_CASE("A restarted editor is found again")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("DiscoveryReconnect");
		Tests::LiveProcess owner;
		auto first = CreateScope<Tests::PumpedRpcServer>();
		REQUIRE(Tests::StartFakeEditor(*first));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(owner.GetProcessId(), first->GetPort(), "2026-01-01T00:00:00Z")));

		EditorConnection connection(MakeOptions(sessionDirectory));
		REQUIRE(connection.Call("rpc.ping", nlohmann::json::object(), std::chrono::milliseconds(5000)).IsSuccess());

		// The same editor process restarts its server on another port and rewrites its session file.
		first->Stop();
		first.reset();
		Tests::PumpedRpcServer second;
		REQUIRE(Tests::StartFakeEditor(second));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(owner.GetProcessId(), second.GetPort(), "2026-01-01T00:00:01Z")));

		const RpcResult result = connection.Call("rpc.ping", nlohmann::json::object(), std::chrono::milliseconds(5000));
		REQUIRE_MESSAGE(result.IsSuccess(), result.GetError().Message);
		CHECK(connection.GetEndpoint()->Port == second.GetPort());

		second.Stop();
		const RpcResult unreachable = connection.Call("rpc.ping", nlohmann::json::object(), std::chrono::milliseconds(5000));
		CHECK(unreachable.IsError());
		CHECK(unreachable.GetError().Code == JsonRpc::ErrorCode::ConnectionClosed);
		CHECK_FALSE(connection.GetLastError().empty());
	}

	TEST_CASE("Reconnects stay with the pinned editor")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("DiscoveryPinned");
		const std::filesystem::path pinnedProject = Tests::CreateTemporaryDirectory("DiscoveryPinnedProject");
		const std::filesystem::path otherProject = Tests::CreateTemporaryDirectory("DiscoveryPinnedOther");

		auto original = CreateScope<FakeEditorInstance>();
		REQUIRE(original->Start(sessionDirectory, "2026-02-01T00:00:00Z", FileSystem::ToUTF8(pinnedProject)));
		FakeEditorInstance other;
		REQUIRE(other.Start(sessionDirectory, "2026-01-01T00:00:00Z", FileSystem::ToUTF8(otherProject)));

		EditorConnection connection(MakeOptions(sessionDirectory));
		REQUIRE(connection.EnsureConnected());
		CHECK(connection.GetEndpoint()->Port == original->Server.GetPort()); // The newest session
		REQUIRE(connection.GetPinnedEditor().has_value());
		CHECK(EditorSession::IsSameProject(connection.GetPinnedEditor()->ProjectPath, pinnedProject));

		// The pinned editor goes away; the other project's editor is still running but must not take its place.
		const uint32_t originalProcess = original->Session.ProcessId;
		original.reset();
		const RpcResult disconnected = connection.Call("rpc.ping", nlohmann::json::object(), std::chrono::milliseconds(5000));
		REQUIRE(disconnected.IsError());
		CHECK(disconnected.GetError().Code == JsonRpc::ErrorCode::ConnectionClosed);
		CHECK(disconnected.GetError().Message.find("Disconnected") != std::string::npos);
		nlohmann::json status = connection.DescribeStatus();
		CHECK(status["connected"] == false);
		CHECK(status["pinnedEditor"]["ProcessId"] == originalProcess);
		CHECK(status["error"].get<std::string>().find(FileSystem::ToUTF8(pinnedProject)) != std::string::npos);

		// The same project's editor starting again (as a new process) is followed.
		FakeEditorInstance restarted;
		REQUIRE(restarted.Start(sessionDirectory, "2026-03-01T00:00:00Z", FileSystem::ToUTF8(pinnedProject)));
		REQUIRE(connection.Call("rpc.ping", nlohmann::json::object(), std::chrono::milliseconds(5000)).IsSuccess());
		CHECK(connection.GetEndpoint()->Port == restarted.Server.GetPort());
		CHECK(connection.GetPinnedEditor()->ProcessId == restarted.Session.ProcessId);

		// Switching editors takes an explicit choice.
		REQUIRE(connection.ConnectToSession(other.Session));
		CHECK(connection.GetEndpoint()->Port == other.Server.GetPort());
		CHECK(EditorSession::IsSameProject(connection.GetPinnedEditor()->ProjectPath, otherProject));
	}

	TEST_CASE("A pinned editor that opens another project is still followed")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("DiscoveryMoved");
		const std::filesystem::path firstProject = Tests::CreateTemporaryDirectory("DiscoveryMovedFirst");
		const std::filesystem::path secondProject = Tests::CreateTemporaryDirectory("DiscoveryMovedSecond");
		FakeEditorInstance editor;
		REQUIRE(editor.Start(sessionDirectory, "2026-02-01T00:00:00Z", FileSystem::ToUTF8(firstProject)));

		// Found by its first project.
		EditorConnectionOptions options = MakeOptions(sessionDirectory);
		options.ProjectDirectory = firstProject;
		EditorConnection connection(options);
		REQUIRE(connection.EnsureConnected());
		CHECK(connection.GetEndpoint()->Port == editor.Server.GetPort());

		// The editor opens the second project (its session moves along) and the connection drops: discovery by the first
		// project no longer finds it, but it is still the pinned process.
		editor.Session.ProjectPath = FileSystem::ToUTF8(secondProject);
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, editor.Session));
		connection.Disconnect();
		REQUIRE(connection.Call("rpc.ping", nlohmann::json::object(), std::chrono::milliseconds(5000)).IsSuccess());
		CHECK(connection.GetEndpoint()->Port == editor.Server.GetPort());
		REQUIRE(connection.GetPinnedEditor().has_value());
		CHECK(connection.GetPinnedEditor()->ProcessId == editor.Session.ProcessId);
		CHECK(EditorSession::IsSameProject(connection.GetPinnedEditor()->ProjectPath, secondProject));
	}

	TEST_CASE("The editor executable is resolved from the option, the environment or the default location")
	{
		Tests::ScopedEnvironmentVariable editorPath("STRATA_EDITOR_PATH", "");
		const std::filesystem::path defaultPath = ResolveEditorPath(std::nullopt);
		CHECK(defaultPath.parent_path() == Platform::GetExecutableDirectory());
		CHECK(defaultPath.stem() == "StrataEditor");

		{
			Tests::ScopedEnvironmentVariable fromEnvironment("STRATA_EDITOR_PATH", "C:/Tools/Editor \xC3\xA9.exe");
			const std::filesystem::path environmentPath = ResolveEditorPath(std::nullopt);
			CHECK(environmentPath.is_absolute());
			CHECK(environmentPath.filename() == FileSystem::FromUTF8("Editor \xC3\xA9.exe"));
			CHECK(ResolveEditorPath(std::string("D:/Explicit.exe")).filename() == "Explicit.exe");
		}

		// A relative path is made absolute, so the file that is checked is the one that is started.
		const std::filesystem::path relative = ResolveEditorPath(std::string("tools/StrataEditor.exe"));
		CHECK(relative.is_absolute());
		CHECK(relative == (std::filesystem::current_path() / "tools" / "StrataEditor.exe").lexically_normal());
		CHECK(ResolveEditorPath(std::string()) == defaultPath);
	}

	TEST_CASE("Launching validates the editor and project paths")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("LaunchValidation");

		EditorLaunchSpecification specification;
		specification.EditorPath = directory / "Missing.exe";
		specification.ProjectDirectory = directory;
		specification.SessionDirectory = directory / "Sessions";
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

	TEST_CASE("Launching starts the editor and waits until its session accepts connections")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("LaunchSessions") / "Sessions";
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("LaunchProject");
		Tests::ScopedEnvironmentVariable fakeEditor("STRATA_TEST_FAKE_EDITOR", "1");
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory));

		EditorLaunchSpecification specification;
		specification.EditorPath = Tests::GetTestExecutablePath();
		specification.ProjectDirectory = project;
		specification.Headless = true;
		specification.WaitTimeout = std::chrono::milliseconds(20000);
		EditorLaunchResult result = LaunchEditor(specification);
		REQUIRE_MESSAGE(result.Success, result.Error);
		REQUIRE(result.EditorProcess != nullptr);
		CHECK(result.Session.ProcessId == result.EditorProcess->GetProcessID());
		CHECK(result.Session.Headless);
		CHECK(EditorSession::IsSameProject(result.Session.ProjectPath, project));
		CHECK(EditorSession::ReadProjectSession(project).has_value());

		RpcClient client;
		REQUIRE_MESSAGE(client.Connect("127.0.0.1", result.Session.Port, result.Session.Token, std::chrono::milliseconds(3000)), client.GetLastError());
		const RpcResult info = client.Call("editor.info", nlohmann::json::object(), std::chrono::milliseconds(5000));
		REQUIRE(info.IsSuccess());
		CHECK(info.GetValue()["Headless"] == true);
		CHECK(client.Call("editor.quit", nlohmann::json::object(), std::chrono::milliseconds(5000)).IsSuccess());
		client.Close();

		const std::optional<int> exitCode = result.EditorProcess->Wait(std::chrono::milliseconds(10000));
		REQUIRE(exitCode.has_value());
		CHECK(*exitCode == 0);
		CHECK(EditorSession::FindSessions().empty());
		CHECK_FALSE(FileSystem::Exists(EditorSession::GetProjectSessionFilePath(project)));
	}

	TEST_CASE("Launching without a project starts an editor without one")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("LaunchEmptySessions") / "Sessions";
		Tests::ScopedEnvironmentVariable fakeEditor("STRATA_TEST_FAKE_EDITOR", "1");
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory));

		EditorLaunchSpecification specification;
		specification.EditorPath = Tests::GetTestExecutablePath();
		specification.NoGpu = true;
		specification.WaitTimeout = std::chrono::milliseconds(20000);
		EditorLaunchResult result = LaunchEditor(specification);
		REQUIRE_MESSAGE(result.Success, result.Error);
		CHECK(result.Session.ProjectPath.empty());
		CHECK(result.Session.Headless);

		RpcClient client;
		REQUIRE_MESSAGE(client.Connect("127.0.0.1", result.Session.Port, result.Session.Token, std::chrono::milliseconds(3000)), client.GetLastError());
		const RpcResult info = client.Call("editor.info", nlohmann::json::object(), std::chrono::milliseconds(5000));
		REQUIRE(info.IsSuccess());
		CHECK(info.GetValue()["NoGpu"] == true);
		CHECK(info.GetValue()["Project"] == "");
		CHECK(client.Call("editor.quit", nlohmann::json::object(), std::chrono::milliseconds(5000)).IsSuccess());
		client.Close();
		REQUIRE(result.EditorProcess->Wait(std::chrono::milliseconds(10000)).has_value());
	}

	TEST_CASE("Waiting for a launched editor's session")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("LaunchWait");
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));

		SUBCASE("The session is found once it is written and accepts connections")
		{
			// The editor writes its session file a little after starting, first with a port that is not up yet.
			Tests::RefusingPort notListeningYet;
			std::thread writer([&]()
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(777, notListeningYet.GetPort(), "2026-01-01T00:00:00Z"));
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

		SUBCASE("An unbounded wait does not overflow its deadline")
		{
			// milliseconds::max() is far beyond what steady_clock can add; the wait must still run until the
			// editor exits rather than time out at once.
			std::atomic<int> checks = 0;
			std::string error;
			CHECK_FALSE(WaitForEditorSession(782, sessionDirectory, std::chrono::milliseconds::max(), [&checks]() { return ++checks < 3; }, &error).has_value());
			CHECK(checks == 3);
			CHECK(error.find("exited") != std::string::npos);
		}
	}
}

#endif
