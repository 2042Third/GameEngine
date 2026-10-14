#if defined(STRATA_TESTS_HAVE_CLI)

#include <doctest/doctest.h>

#include "Network/NetworkTestHelpers.h"
#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Perf/WorldFixture.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Process.h>
#include <Strata/Network/JsonRpc.h>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

// The editor's start on a large project, as an agent starts it (CTest StrataTests.PerfGPU: Release and Dist, with the
// built StrataEditor and StrataCLI next to the test executable). A fresh copy of the 30,000-material project of
// Perf/WorldFixture.h (its committed files, no cache) per attempt: `StrataCLI launch --headless --no-wait-scan`, then
// `StrataCLI call editor.status`, timed from the launch to the answer, which comes while the assets are still scanned
// (the editor publishes its session within its startup). The full launch, which waits for the scan and the start scene,
// must succeed too; its time is reported.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr const char* c_Launch30kStatus = "PerfGPU.Editor.Launch30kStatus";
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
}

#endif
