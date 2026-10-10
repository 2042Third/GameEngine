#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Perf/PerfUtils.h"
#include "Perf/StressProject.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Process.h"
#include "TestHelpers.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Megabytes here are decimal, like the streaming audit's numbers (the stress project's textures are 447.4 MB of
	// RGBA8, the largest 89.5 MB).
	constexpr uint64_t c_MB = 1000 * 1000;
	constexpr double c_MBDouble = 1000.0 * 1000.0;

	// The acceptance criteria of the residency work, for the full-size stress project.
	constexpr uint64_t c_TextureBudget = 128 * c_MB;
	constexpr uint64_t c_MaxPeakPrivateBytes = 750 * c_MB; // The exported runtime peaked at 1034-1055 MiB before
	constexpr float c_MaxFinalizeMs = 8.0f;
	// What a camera stop shows has arrived within this many frames at rest (FramesHolding below it).
	constexpr uint32_t c_MaxFramesToSettle = 60;

	// The sweep runs this many times. Every run must meet the criteria. Peak private memory is judged by the best run: the
	// graphics driver keeps commit charge of freed device memory for a while (it counts in private bytes on Windows), by
	// up to about 150 MB from run to run while the engine does exactly the same, so the best run is the engine's own cost.
	// Finalization time is judged by the median run, which a spike from the machine's other work does not move.
	constexpr uint32_t c_Runs = 3;

	template<typename T>
	T Median(std::vector<T> values)
	{
		std::sort(values.begin(), values.end());
		return values[values.size() / 2];
	}

	double ElapsedSeconds(std::chrono::steady_clock::time_point start)
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	}

	// Runs the sweep in a helper process (see RunStreamingSweepProcess) and returns its result.
	StreamingSweepResult RunSweepProcess(const std::filesystem::path& settingsFile, const std::filesystem::path& resultFile)
	{
		ProcessSpecification process;
		process.Executable = Platform::GetExecutablePath();
		process.Arguments = { "--strata-test-helper=streaming-sweep", FileSystem::ToUTF8(settingsFile), FileSystem::ToUTF8(resultFile) };
		const Process::RunResult run = Process::Run(process, std::chrono::minutes(10));
		INFO("Helper output:\n", run.Output);
		REQUIRE_MESSAGE(run.Started, run.Error);
		REQUIRE_FALSE(run.TimedOut);
		REQUIRE(run.ExitCode == 0);

		std::string error;
		const std::optional<std::string> resultText = FileSystem::ReadText(resultFile);
		REQUIRE(resultText);
		const std::optional<nlohmann::json> resultJson = JsonUtils::Parse(*resultText, &error);
		REQUIRE_MESSAGE(resultJson, error);
		std::optional<StreamingSweepResult> result = StreamingSweepResultFromJson(*resultJson, error);
		REQUIRE_MESSAGE(result, error);
		REQUIRE_MESSAGE(result->Completed, result->Error);
		REQUIRE_MESSAGE(!result->ValidationEnabled, "the helper's device validates: run PerfGPU with STRATA_TEST_GPU_VALIDATION=0 (ctest -L perf does)");
		return std::move(*result);
	}

}

TEST_SUITE("PerfGPU.Streaming")
{
	TEST_CASE("A camera sweep over the stress world streams within its budgets")
	{
		// The project and its pack, built in this process; the sweep runs in a helper process, whose peak memory is the
		// streaming's alone (importing seventeen large textures in parallel peaks far higher, and a process's peak never
		// goes down).
		const std::filesystem::path directory = CreateTemporaryDirectory("StreamingPerf");
		const StressProjectSpec spec;
		std::string error;
		std::optional<StressProject> project;
		{
			ScopedJobSystem jobs(0, 2); // The default pools: a worker per hardware thread but one, two I/O threads
			const auto start = std::chrono::steady_clock::now();
			project = WriteStressProject(directory / "Stress", spec, error);
			REQUIRE_MESSAGE(project, error);
			const double writeSeconds = ElapsedSeconds(start);
			REQUIRE_MESSAGE(BuildStressPack(*project, directory / "Stress.stpak", error), error);
			MESSAGE("Stress project written in ", writeSeconds, " s, imported and packed in ", ElapsedSeconds(start) - writeSeconds, " s");
		}
		CHECK(project->TextureBytes == 447392404);       // 16 x 2048^2 + 1 x 4096^2 RGBA8 with mips: 447.4 MB
		CHECK(project->LargestTextureBytes == 89478484); // 89.5 MB

		StreamingSweepSettings settings;
		settings.Pack = directory / "Stress.stpak";
		settings.Scene = project->WorldScene;
		settings.Spec = spec;
		settings.TextureBudget = c_TextureBudget;
		REQUIRE(settings.Path.GetFrameCount() == 600);
		// Every stop holds long enough to tell a stop that settled late from one that never did.
		REQUIRE(settings.Path.HoldFrames >= c_MaxFramesToSettle);
		const std::filesystem::path settingsFile = directory / "SweepSettings.json";
		REQUIRE(FileSystem::WriteText(settingsFile, JsonUtils::Dump(ToJson(settings))));

		uint64_t bestPeakPrivateBytes = std::numeric_limits<uint64_t>::max();
		uint64_t maxResidentTextureBytes = 0;
		std::vector<float> maxFinalizeMs;
		for (uint32_t runIndex = 0; runIndex < c_Runs; runIndex++)
		{
			CAPTURE(runIndex);
			const auto start = std::chrono::steady_clock::now();
			const StreamingSweepResult result = RunSweepProcess(settingsFile, directory / ("SweepResult" + std::to_string(runIndex) + ".json"));
			REQUIRE(result.FramesToSettle.size() == settings.Path.Stops);
			std::string settled;
			for (uint32_t frames : result.FramesToSettle)
				settled += frames == c_StopNeverSettled ? std::string(" never") : " " + std::to_string(frames);
			MESSAGE("Run ", runIndex + 1, " (", ElapsedSeconds(start), " s): resident GPU textures at most ", result.MaxResidentTextureBytes / c_MBDouble,
				" MB (budget ", c_TextureBudget / c_MBDouble, " MB); peak private ", result.PeakPrivateBytes / c_MBDouble, " MB, peak working set ",
				result.PeakWorkingSetBytes / c_MBDouble, " MB; finalization at most ", result.MaxFinalizeMs, " ms and ", result.MaxUploadedBytes / c_MBDouble,
				" MB per frame; at most ", result.MaxInFlightBytes / c_MBDouble, " MB in flight; ", result.LoadsCompleted, " loads, ", result.Evictions,
				" evictions; frames until each stop settled:", settled);

			// Every run: no load fails, the device reports no error, the textures stay within the budget give or take the
			// largest one (what is in view stays, also beyond the budget), and the sweep needs more than the budget holds,
			// so the residency manager evicts. What each camera stop shows is complete within a second at rest, and no frame
			// finalizes for more than 8 ms.
			CHECK(result.NewErrors == 0);
			CHECK(result.FailedAssets == 0);
			CHECK(result.MaxResidentTextureBytes <= c_TextureBudget + project->LargestTextureBytes);
			CHECK(result.Evictions > 0);
			for (size_t stop = 0; stop < result.FramesToSettle.size(); stop++)
			{
				CAPTURE(stop);
				CHECK(result.FramesToSettle[stop] < c_MaxFramesToSettle);
			}
			CHECK(result.MaxFinalizeMs <= c_MaxFinalizeMs);
			REQUIRE(result.PeakPrivateBytes > 0);

			bestPeakPrivateBytes = std::min(bestPeakPrivateBytes, result.PeakPrivateBytes);
			maxResidentTextureBytes = std::max(maxResidentTextureBytes, result.MaxResidentTextureBytes);
			maxFinalizeMs.push_back(result.MaxFinalizeMs);
		}

		// The process stays below 750 MB of private memory.
		CHECK(bestPeakPrivateBytes <= c_MaxPeakPrivateBytes);

		Perf::CheckBudget("PerfGPU.Streaming.PeakPrivateMemory", static_cast<double>(bestPeakPrivateBytes) / c_MBDouble);
		Perf::CheckBudget("PerfGPU.Streaming.MaxResidentTextures", static_cast<double>(maxResidentTextureBytes) / c_MBDouble);
		Perf::CheckBudget("PerfGPU.Streaming.MaxFinalizeTime", Median(maxFinalizeMs));
	}
}
