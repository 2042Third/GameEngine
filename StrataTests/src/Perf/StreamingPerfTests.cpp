#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Perf/PerfUtils.h"
#include "Perf/StressProject.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Process.h"
#include "TestHelpers.h"

#include <spdlog/fmt/fmt.h>

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

	// Megabytes here are decimal, like the streaming audit's numbers (the stress project's textures cook to 55.9 MB of
	// BC1, the largest 11.2 MB; they were 447.4 MB and 89.5 MB of RGBA8 before texture cooking v2).
	constexpr uint64_t c_MB = 1000 * 1000;
	constexpr double c_MBDouble = 1000.0 * 1000.0;

	// The acceptance criteria of the residency work, for the full-size stress project. The evicting sweep's texture budget
	// holds the same share of the textures as the residency work's 128 MB held of their RGBA8 form (about 29%, 1.4 times
	// the largest), so the sweep keeps evicting.
	constexpr uint64_t c_TextureBudget = 16 * c_MB;
	constexpr float c_MaxFinalizeMs = 8.0f;
	// The acceptance criterion of texture cooking: the stress game at 1920x1080 over 300 frames with all of its textures
	// resident (the all-resident sweep; the exported runtime peaked at 1034-1055 MiB with RGBA8 textures). The evicting
	// sweep, which holds less, must stay below it too.
	constexpr uint64_t c_MaxPeakPrivateBytes = 650 * c_MB;
	// The all-resident sweep's texture budget: far more than the 55.9 MB of textures, so nothing is evicted.
	constexpr uint64_t c_AllResidentTextureBudget = 1000 * c_MB;
	// What a camera stop shows has arrived within this many frames at rest (FramesHolding below it).
	constexpr uint32_t c_MaxFramesToSettle = 60;

	// Each sweep runs this many times. Every run must meet the criteria. Peak private memory is judged by the best run: the
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

	// What a run measured, for the test's log.
	std::string DescribeSweep(const StreamingSweepResult& result)
	{
		std::string settled;
		for (uint32_t frames : result.FramesToSettle)
			settled += frames == c_StopNeverSettled ? std::string(" never") : " " + std::to_string(frames);
		return fmt::format("resident GPU textures at most {} MB (budget {} MB); peak private {} MB, peak working set {} MB; finalization at most {} ms "
			"and {} MB per frame; at most {} MB in flight; {} loads, {} evictions; frames until each stop settled:{}", result.MaxResidentTextureBytes / c_MBDouble,
			result.TextureBudget / c_MBDouble, result.PeakPrivateBytes / c_MBDouble, result.PeakWorkingSetBytes / c_MBDouble, result.MaxFinalizeMs,
			result.MaxUploadedBytes / c_MBDouble, result.MaxInFlightBytes / c_MBDouble, result.LoadsCompleted, result.Evictions, settled);
	}

}

TEST_SUITE("PerfGPU.Streaming")
{
	TEST_CASE("Camera sweeps over the stress world stream within their budgets")
	{
		// The project and its pack, built in this process; the sweeps run in helper processes, whose peak memory is the
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
		REQUIRE(GetStressTextureFormat() == TextureFormat::BC1SRGB);
		CHECK(project->TextureBytes == 55924280);        // 16 x 2048^2 + 1 x 4096^2 BC1 with mips: 55.9 MB
		CHECK(project->LargestTextureBytes == 11184824); // 11.2 MB

		// The evicting sweep: 600 frames at 1280x720 with a texture budget that holds less than the stops show over time.
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
			MESSAGE("Evicting, run ", runIndex + 1, " (", ElapsedSeconds(start), " s): ", DescribeSweep(result));

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

		CHECK(bestPeakPrivateBytes <= c_MaxPeakPrivateBytes);

		// All textures resident: the stress game at 1920x1080 for 300 frames, over twelve stops whose views take in every
		// district, with a texture budget that holds every texture. Every run: each texture arrives and none leaves, no load
		// fails, the device reports no error and no frame finalizes for more than 8 ms; the best run's peak private memory
		// stays below 650 MB (texture cooking's acceptance limit) and is judged by its own budget.
		StreamingSweepSettings allResident = settings;
		allResident.TextureBudget = c_AllResidentTextureBudget;
		allResident.Width = 1920;
		allResident.Height = 1080;
		allResident.Path.Stops = 12;
		allResident.Path.MoveFrames = 10;
		allResident.Path.HoldFrames = 15;
		REQUIRE(allResident.Path.GetFrameCount() == 300);
		const std::filesystem::path allResidentFile = directory / "AllResidentSettings.json";
		REQUIRE(FileSystem::WriteText(allResidentFile, JsonUtils::Dump(ToJson(allResident))));
		uint64_t bestAllResidentPeakPrivateBytes = std::numeric_limits<uint64_t>::max();
		for (uint32_t runIndex = 0; runIndex < c_Runs; runIndex++)
		{
			CAPTURE(runIndex);
			const auto start = std::chrono::steady_clock::now();
			const StreamingSweepResult result = RunSweepProcess(allResidentFile, directory / ("AllResidentResult" + std::to_string(runIndex) + ".json"));
			MESSAGE("All resident, run ", runIndex + 1, " (", ElapsedSeconds(start), " s): ", DescribeSweep(result));
			CHECK(result.NewErrors == 0);
			CHECK(result.FailedAssets == 0);
			CHECK(result.Evictions == 0);
			CHECK(result.MaxResidentTextureBytes == project->TextureBytes);
			CHECK(result.MaxFinalizeMs <= c_MaxFinalizeMs);
			REQUIRE(result.PeakPrivateBytes > 0);
			bestAllResidentPeakPrivateBytes = std::min(bestAllResidentPeakPrivateBytes, result.PeakPrivateBytes);
		}
		CHECK(bestAllResidentPeakPrivateBytes <= c_MaxPeakPrivateBytes);

		Perf::CheckBudget("PerfGPU.Streaming.PeakPrivateMemory", static_cast<double>(bestPeakPrivateBytes) / c_MBDouble);
		Perf::CheckBudget("PerfGPU.Streaming.MaxResidentTextures", static_cast<double>(maxResidentTextureBytes) / c_MBDouble);
		Perf::CheckBudget("PerfGPU.Streaming.MaxFinalizeTime", Median(maxFinalizeMs));
		Perf::CheckBudget("PerfGPU.Streaming.PeakPrivateMemoryAllResident", static_cast<double>(bestAllResidentPeakPrivateBytes) / c_MBDouble);
	}
}
