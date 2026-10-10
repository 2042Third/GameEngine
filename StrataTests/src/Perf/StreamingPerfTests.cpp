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

	// The residency budget the sweep streams within: far less than the stress project's textures.
	constexpr uint64_t c_TextureBudget = 128 * c_MB;

	// The metrics of one sweep, checked against their budgets (with their reasons below):
	// - PeakPrivateMemory: the helper process's peak private bytes, a maximum by nature (what the process needs at once).
	//   The graphics driver keeps commit charge of freed device memory for a while (it counts in private bytes on Windows),
	//   by up to about 150 MB from sweep to sweep while the engine does exactly the same, and the confirmation's best
	//   attempt is the engine's own cost. The residency work's target (750 MB) is recorded with the budget.
	// - MaxResidentTextures: the most GPU texture bytes resident after a frame's eviction, a maximum by nature (the
	//   residency budget bounds every frame).
	// - MaxFinalizeTime: the longest a frame spent finalizing loads. Of the 600 frames about 75 to 110 finalize. Most take
	//   about 0.1 ms (the median; materials, meshes, small steps), and the few that upload a large texture's steps run until
	//   the per-frame budget (AssetResidencyBudgets::FinalizeMsPerFrame, 4 ms) and finish the step that crosses it, so the
	//   95th percentile is about 4 ms and the maximum 4 to 5.5 ms on a quiet machine. A regression that matters may show in
	//   a single frame (one large texture finalized at once is the hitch players see), which a percentile would hide, so
	//   the statistic is the maximum; a frame the machine's other work preempted is not repeated by the confirmation's
	//   next sweeps.
	// - FramesToSettle: the most frames a camera stop needed at rest until it drew without pending assets (a stop that
	//   never did counts as its whole hold), a maximum by nature (every stop must be complete soon).
	constexpr const char* c_PeakPrivateMemory = "PerfGPU.Streaming.PeakPrivateMemory";
	constexpr const char* c_MaxResidentTextures = "PerfGPU.Streaming.MaxResidentTextures";
	constexpr const char* c_MaxFinalizeTime = "PerfGPU.Streaming.MaxFinalizeTime";
	constexpr const char* c_FramesToSettle = "PerfGPU.Streaming.FramesToSettle";

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
		REQUIRE(settings.Path.HoldFrames > Perf::Budget(c_FramesToSettle));
		const std::filesystem::path settingsFile = directory / "SweepSettings.json";
		REQUIRE(FileSystem::WriteText(settingsFile, JsonUtils::Dump(ToJson(settings))));

		// Each attempt is one sweep; one within every budget passes. Every sweep that runs must also stream correctly.
		uint32_t sweeps = 0;
		Perf::CheckBudgetsConfirmed({ c_PeakPrivateMemory, c_MaxResidentTextures, c_MaxFinalizeTime, c_FramesToSettle }, [&]
		{
			const uint32_t sweep = ++sweeps;
			CAPTURE(sweep);
			const auto start = std::chrono::steady_clock::now();
			const StreamingSweepResult result = RunSweepProcess(settingsFile, directory / ("SweepResult" + std::to_string(sweep) + ".json"));
			REQUIRE(result.FramesToSettle.size() == settings.Path.Stops);
			REQUIRE(result.FinalizeMs.size() == result.Frames);
			std::string settled;
			uint32_t framesToSettle = 0;
			for (uint32_t frames : result.FramesToSettle)
			{
				settled += frames == c_StopNeverSettled ? std::string(" never") : " " + std::to_string(frames);
				framesToSettle = std::max(framesToSettle, std::min(frames, settings.Path.HoldFrames));
			}
			std::vector<double> finalizingFrames;
			for (const float milliseconds : result.FinalizeMs)
			{
				if (milliseconds > 0.0f)
					finalizingFrames.push_back(milliseconds);
			}
			const Perf::Measurement finalize = Perf::Summarize(finalizingFrames);
			MESSAGE("Sweep ", sweep, " (", ElapsedSeconds(start), " s): resident GPU textures at most ", result.MaxResidentTextureBytes / c_MBDouble,
				" MB (budget ", c_TextureBudget / c_MBDouble, " MB); peak private ", result.PeakPrivateBytes / c_MBDouble, " MB, peak working set ",
				result.PeakWorkingSetBytes / c_MBDouble, " MB; ", finalize.Iterations, " frames finalized, median ", finalize.MedianMs, " ms, p95 ",
				finalize.P95Ms, " ms, at most ", result.MaxFinalizeMs, " ms and ", result.MaxUploadedBytes / c_MBDouble, " MB per frame; at most ",
				result.MaxInFlightBytes / c_MBDouble, " MB in flight; ", result.LoadsCompleted, " loads, ", result.Evictions,
				" evictions; frames until each stop settled:", settled);

			// No load fails, the device reports no error, every stop settles, the textures stay within the budget give or take
			// the largest one (what is in view stays, also beyond the budget), and the sweep needs more than the budget holds,
			// so the residency manager evicts.
			CHECK(result.NewErrors == 0);
			CHECK(result.FailedAssets == 0);
			CHECK(std::find(result.FramesToSettle.begin(), result.FramesToSettle.end(), c_StopNeverSettled) == result.FramesToSettle.end());
			CHECK(result.MaxResidentTextureBytes <= c_TextureBudget + project->LargestTextureBytes);
			CHECK(result.Evictions > 0);
			REQUIRE(result.PeakPrivateBytes > 0);

			return Perf::MetricValues {
				{ c_PeakPrivateMemory, static_cast<double>(result.PeakPrivateBytes) / c_MBDouble },
				{ c_MaxResidentTextures, static_cast<double>(result.MaxResidentTextureBytes) / c_MBDouble },
				{ c_MaxFinalizeTime, result.MaxFinalizeMs },
				{ c_FramesToSettle, static_cast<double>(framesToSettle) } };
		});
	}
}
