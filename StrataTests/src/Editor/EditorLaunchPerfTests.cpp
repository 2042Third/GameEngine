#if defined(STRATA_TESTS_HAVE_CLI)

#include <doctest/doctest.h>

#include "Network/NetworkTestHelpers.h"
#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Perf/WorldFixture.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Platform.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Core/Process.h>
#include <Strata/Network/JsonRpc.h>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

// The editor's start on a large project and its open of the realistic 1M world, as an agent drives them (CTest
// StrataTests.PerfGPU: Release and Dist, with the built StrataEditor and StrataCLI next to the test executable). A fresh copy of the 30,000-material project of
// Perf/WorldFixture.h (its committed files, no cache) per attempt: `StrataCLI launch --headless --no-wait-scan`, then
// `StrataCLI call editor.status`, timed from the launch to the answer, which comes while the assets are still scanned
// (the editor publishes its session within its startup). The full launch, which waits for the scan and the start scene,
// must succeed too; its time is reported.
//
// The world open: a headless editor on a fresh project with the realistic 1M world (Perf/WorldFixture.h, its file as the
// fixture writes it: compact, read as a stream) opens it with scene.open: the open's time and its frames' longest, and the
// editor's peak private memory afterwards (editor.status). Then the world saved by the editor (the writer's layout, read in
// parallel) is opened by a fresh editor the same way.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr const char* c_Launch30kStatus = "PerfGPU.Editor.Launch30kStatus";
	constexpr const char* c_WorldOpen1MPeakPrivate = "PerfGPU.Editor.WorldOpen1MPeakPrivate";
	constexpr const char* c_WorldOpen1MMaxFrame = "PerfGPU.Editor.WorldOpen1MMaxFrame";
	constexpr const char* c_WorldOpen1MSeconds = "PerfGPU.Editor.WorldOpen1MSeconds";
	constexpr const char* c_WorldOpen1MSavedPeakPrivate = "PerfGPU.Editor.WorldOpen1MSavedPeakPrivate";
	constexpr const char* c_WorldOpen1MSavedSeconds = "PerfGPU.Editor.WorldOpen1MSavedSeconds";
	constexpr uint32_t c_LaunchMaterials = 30'000;
	constexpr std::chrono::minutes c_ProcessTimeout { 10 };

	std::filesystem::path GetCliPath()
	{
		if (const std::optional<std::string> path = Platform::GetEnvVar("STRATA_TEST_CLI_PATH"); path && !path->empty())
			return FileSystem::FromUTF8(*path);
#if defined(ST_PLATFORM_WINDOWS)
		return Platform::GetExecutableDirectory() / "StrataCLI.exe";
#else
		return Platform::GetExecutableDirectory() / "StrataCLI";
#endif
	}

	Process::RunResult RunCli(const std::vector<std::string>& arguments)
	{
		ProcessSpecification specification;
		specification.Executable = GetCliPath();
		specification.Arguments = arguments;
		specification.Output = ProcessOutputMode::CaptureSeparate;
		Process::RunResult result = Process::Run(specification, c_ProcessTimeout);
		CHECK_MESSAGE(result.Started, result.Error);
		CHECK_FALSE(result.TimedOut);
		return result;
	}

	// A fresh copy of the project, as a clone has it.
	std::filesystem::path CopyProject(const std::filesystem::path& from, const std::filesystem::path& to)
	{
		std::error_code error;
		std::filesystem::copy(from, to, std::filesystem::copy_options::recursive, error);
		REQUIRE_MESSAGE(!error, error.message());
		std::filesystem::remove_all(to / ".strata", error);
		return to;
	}

	void Quit(const std::filesystem::path& project)
	{
		const Process::RunResult quit = RunCli({ "call", "editor.quit", "{\"force\": true}", "--project", FileSystem::ToUTF8(project) });
		CHECK_MESSAGE(quit.ExitCode == 0, quit.ErrorOutput);
	}

	nlohmann::json Call(const std::filesystem::path& project, const std::string& method, const nlohmann::json& parameters = nlohmann::json::object())
	{
		const Process::RunResult result = RunCli({ "call", method, parameters.dump(), "--project", FileSystem::ToUTF8(project) });
		REQUIRE_MESSAGE(result.ExitCode == 0, result.ErrorOutput);
		const std::optional<nlohmann::json> value = JsonRpc::Parse(result.Output);
		REQUIRE(value.has_value());
		return *value;
	}

	struct WorldOpen
	{
		double Seconds = 0.0;
		double MaxFrameMs = 0.0;
		double PeakPrivateMB = 0.0;
	};

	// A headless editor on the project (its session state removed first: a fresh start) opens the scene, and quits after
	// `then` (when given) ran on it.
	WorldOpen OpenWorld(const std::filesystem::path& project, AssetHandle scene, const std::function<void()>& then = {})
	{
		std::error_code error;
		std::filesystem::remove_all(project / ".strata", error);
		const Process::RunResult launched = RunCli({ "launch", "--headless", "--project", FileSystem::ToUTF8(project) });
		REQUIRE_MESSAGE(launched.ExitCode == 0, launched.ErrorOutput);
		const nlohmann::json opened = Call(project, "scene.open", { { "scene", UUIDToJson(scene) } });
		const nlohmann::json status = Call(project, "editor.status");
		WorldOpen open;
		open.Seconds = opened.value("seconds", 0.0);
		open.MaxFrameMs = opened["frameTimes"].value("maxMs", 0.0);
		open.PeakPrivateMB = status["memory"].value("peakPrivateMB", 0.0);
		CHECK(open.Seconds > 0.0);
		CHECK(open.PeakPrivateMB > 0.0);
		if (then)
			then();
		Quit(project);
		return open;
	}

}

TEST_SUITE("PerfGPU.Editor")
{
	TEST_CASE("A headless editor on a fresh copy of 30,000 assets answers editor.status within its startup")
	{
		REQUIRE_MESSAGE(FileSystem::IsRegularFile(GetCliPath()), "The test needs the built StrataCLI (and StrataEditor next to it)");
		const std::filesystem::path root = CreateTemporaryDirectory("PerfLaunch30k");
		{
			const Perf::ScopedApplicationJobSystem jobs;
			std::string error;
			REQUIRE_MESSAGE(WriteManyAssetsProject(root / "Template", c_LaunchMaterials, error).has_value(), error);
		}
		ScopedEnvironmentVariable sessions("STRATA_SESSION_DIR", FileSystem::ToUTF8(root / "Sessions"));
		ScopedEnvironmentVariable editorPath("STRATA_EDITOR_PATH", "");

		uint32_t attempt = 0;
		Perf::CheckBudgetConfirmed(c_Launch30kStatus, [&]()
		{
			const std::filesystem::path project = CopyProject(root / "Template", root / fmt::format("Copy{}", attempt++));
			const auto start = std::chrono::steady_clock::now();
			const Process::RunResult launched = RunCli({ "launch", "--headless", "--no-wait-scan", "--project", FileSystem::ToUTF8(project) });
			REQUIRE_MESSAGE(launched.ExitCode == 0, launched.ErrorOutput);
			const Process::RunResult status = RunCli({ "call", "editor.status", "--project", FileSystem::ToUTF8(project) });
			const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			REQUIRE_MESSAGE(status.ExitCode == 0, status.ErrorOutput);
			const std::optional<nlohmann::json> value = JsonRpc::Parse(status.Output);
			REQUIRE(value.has_value());
			MESSAGE(fmt::format("editor.status answered {:.0f} ms after the launch began; the scan: {}", milliseconds, (*value)["assets"]["scanning"].dump()));
			Quit(project);
			return milliseconds;
		});

		// The full launch waits for the scan and the start scene, however long they take while they progress.
		const std::filesystem::path project = CopyProject(root / "Template", root / "Full");
		const auto start = std::chrono::steady_clock::now();
		const Process::RunResult launched = RunCli({ "launch", "--headless", "--project", FileSystem::ToUTF8(project) });
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		REQUIRE_MESSAGE(launched.ExitCode == 0, launched.ErrorOutput);
		MESSAGE(fmt::format("The full launch (the scan of 30,000 assets and the start scene) took {:.2f} s", seconds));
		const Process::RunResult status = RunCli({ "call", "editor.status", "--project", FileSystem::ToUTF8(project) });
		REQUIRE(status.ExitCode == 0);
		const std::optional<nlohmann::json> value = JsonRpc::Parse(status.Output);
		REQUIRE(value.has_value());
		CHECK((*value)["assets"]["scanning"].is_null());
		CHECK((*value)["sceneLoad"].is_null());
		Quit(project);
	}

	TEST_CASE("A headless editor opens the realistic 1M world within seconds, holding no frame and little memory, also once the editor saved it")
	{
		REQUIRE_MESSAGE(FileSystem::IsRegularFile(GetCliPath()), "The test needs the built StrataCLI (and StrataEditor next to it)");
		const std::filesystem::path root = CreateTemporaryDirectory("PerfWorldOpen1M");
		std::optional<WorldProject> project;
		{
			const Perf::ScopedApplicationJobSystem jobs;
			std::string error;
			project = WriteWorldProject(root / "World", WorldProjectSpec { { c_World1M } }, error);
			REQUIRE_MESSAGE(project.has_value(), error);
		}
		ScopedEnvironmentVariable sessions("STRATA_SESSION_DIR", FileSystem::ToUTF8(root / "Sessions"));
		ScopedEnvironmentVariable editorPath("STRATA_EDITOR_PATH", "");
		const AssetHandle world = project->Scenes.front().Handle;

		// The maximum frame and the peak memory are maxima by nature; the seconds are one open's.
		Perf::CheckBudgetsConfirmed({ c_WorldOpen1MPeakPrivate, c_WorldOpen1MMaxFrame, c_WorldOpen1MSeconds }, [&]()
		{
			const WorldOpen open = OpenWorld(project->Directory, world);
			MESSAGE(fmt::format("The 1M world as written: open {:.2f} s, longest frame {:.1f} ms, peak {:.0f} MB", open.Seconds, open.MaxFrameMs, open.PeakPrivateMB));
			return Perf::MetricValues { { c_WorldOpen1MPeakPrivate, open.PeakPrivateMB }, { c_WorldOpen1MMaxFrame, open.MaxFrameMs },
				{ c_WorldOpen1MSeconds, open.Seconds * 1000.0 } };
		});

		// Saved by the editor: the writer's layout.
		OpenWorld(project->Directory, world, [&]()
		{
			Call(project->Directory, "scene.save", { { "wait", true } });
		});
		const std::optional<std::string> head = [&]() -> std::optional<std::string>
		{
			const std::optional<std::string> text = FileSystem::ReadText(project->Scenes.front().Path);
			return text ? std::optional<std::string>(text->substr(0, text->find('\n'))) : std::nullopt;
		}();
		REQUIRE(head.has_value());
		CHECK(head->ends_with("\"Entities\":["));
		Perf::CheckBudgetsConfirmed({ c_WorldOpen1MSavedPeakPrivate, c_WorldOpen1MSavedSeconds }, [&]()
		{
			const WorldOpen open = OpenWorld(project->Directory, world);
			MESSAGE(fmt::format("The 1M world as the editor saves it: open {:.2f} s, longest frame {:.1f} ms, peak {:.0f} MB", open.Seconds, open.MaxFrameMs,
				open.PeakPrivateMB));
			return Perf::MetricValues { { c_WorldOpen1MSavedPeakPrivate, open.PeakPrivateMB }, { c_WorldOpen1MSavedSeconds, open.Seconds * 1000.0 } };
		});
	}
}

#endif
