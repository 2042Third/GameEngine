#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Perf/PerfUtils.h"
#include "Perf/StressProject.h"
#include "Runtime/PerfLogReader.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Process.h"
#include "Strata/Project/GameManifest.h"
#include "TestHelpers.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Megabytes here are decimal, like the streaming audit's numbers (the stress project's textures cook to 111.8 MB of
	// BC7, the largest 22.4 MB; they were 447.4 MB and 89.5 MB of RGBA8 before texture cooking v2).
	constexpr uint64_t c_MB = 1000 * 1000;
	constexpr double c_MBDouble = 1000.0 * 1000.0;

	// The residency budget the evicting sweep streams within: far less than the stress project's textures. It holds the
	// same share of the textures as the residency work's 128 MB held of their RGBA8 form (about 29%, 1.4 times the
	// largest), so the sweep keeps evicting.
	constexpr uint64_t c_TextureBudget = 32 * c_MB;

	// The metrics of one evicting sweep, checked against their budgets (with their reasons below):
	// - PeakPrivateMemory: the helper process's peak private bytes, a maximum by nature (what the process needs at once).
	//   The graphics driver keeps commit charge of freed device memory for a while (it counts in private bytes on Windows),
	//   by up to about 150 MB from sweep to sweep while the engine does exactly the same, and the confirmation's best
	//   attempt is the engine's own cost. Since textures are block-compressed the budget is below the residency work's
	//   target (750 MB) and below texture cooking's limit (650 MB, see the all-resident sweep).
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

	// The metrics of the all-resident sweep (the stress game at 1920x1080 over 300 frames with all of its textures resident),
	// one sweep per attempt, maxima by nature like the evicting sweep's: its peak private memory, whose budget is texture
	// cooking's acceptance limit (650 MB; the exported runtime peaked at 1034-1055 MiB with RGBA8 textures), and its longest
	// frame finalization (below the streaming workstream's limit of 8 ms per frame).
	constexpr const char* c_PeakPrivateMemoryAllResident = "PerfGPU.Streaming.PeakPrivateMemoryAllResident";
	constexpr const char* c_MaxFinalizeTimeAllResident = "PerfGPU.Streaming.MaxFinalizeTimeAllResident";
	// The exported game's slowest frame finalization (the Assets.Finalize stage of its perf log), one run per attempt: a
	// maximum by nature too (below the same limit of 8 ms).
	constexpr const char* c_RuntimeMaxFinalizeStage = "PerfGPU.Streaming.RuntimeMaxFinalizeStage";
	// The exported game's peak private memory (the PeakPrivateMB its perf log ends with), run with the game's own budgets
	// (nothing evicted) from the world scene's overview camera, which shows about two thirds of the textures: the scene's
	// load brings in every texture the world uses before it starts (all of them fit the budgets) and they stay. A maximum
	// by nature, one run per attempt.
	constexpr const char* c_RuntimePeakPrivateMemory = "PerfGPU.Streaming.RuntimePeakPrivateMemory";
	// The same with every texture of the pack resident: the game started in the top-down scene, whose camera sees the
	// whole world (the material of every district, so every texture, is drawn and stays), for 300 frames. Texture
	// cooking's acceptance criterion ("the exported stress game at 1920x1080 over 300 frames, with all textures resident,
	// peaks at 650 MB private or less"), whose limit is the budget; the sweeps above only stand in for the game. A maximum
	// by nature, one run per attempt.
	constexpr const char* c_RuntimePeakPrivateMemoryAllResident = "PerfGPU.Streaming.RuntimePeakPrivateMemoryAllResident";
	// The all-resident sweep's texture budget: far more than the 111.8 MB of textures, so nothing is evicted.
	constexpr uint64_t c_AllResidentTextureBudget = 1000 * c_MB;

	double ElapsedSeconds(std::chrono::steady_clock::time_point start)
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	}

	// Frames the exported stress game runs for its perf log: ten seconds at 60 frames per second, the initial load included;
	// with every texture resident, the 300 frames of the acceptance criterion.
	constexpr uint32_t c_RuntimeFrames = 600;
	constexpr uint32_t c_RuntimeAllResidentFrames = 300;
	// The pixels the exported game renders (StrataRuntime --resolution): those of the acceptance criterion, whatever the
	// display's scale.
	constexpr uint32_t c_RuntimeWidth = 1920;
	constexpr uint32_t c_RuntimeHeight = 1080;

	// The StrataRuntime next to the test executable, or the one STRATA_TEST_RUNTIME_PATH names.
	std::filesystem::path GetRuntimePath()
	{
		if (const std::optional<std::string> path = Platform::GetEnvVar("STRATA_TEST_RUNTIME_PATH"); path && !path->empty())
			return FileSystem::FromUTF8(*path);
#if defined(ST_PLATFORM_WINDOWS)
		return Platform::GetExecutableDirectory() / "StrataRuntime.exe";
#else
		return Platform::GetExecutableDirectory() / "StrataRuntime";
#endif
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

	// What a sweep measured, for the test's log: the finalization of every frame that finalized (median and 95th
	// percentile next to the maximum the budget judges) and the frames each camera stop took to settle.
	std::string DescribeSweep(const StreamingSweepResult& result)
	{
		std::string settled;
		for (uint32_t frames : result.FramesToSettle)
			settled += frames == c_StopNeverSettled ? std::string(" never") : " " + std::to_string(frames);
		std::vector<double> finalizingFrames;
		for (const float milliseconds : result.FinalizeMs)
		{
			if (milliseconds > 0.0f)
				finalizingFrames.push_back(milliseconds);
		}
		const Perf::Measurement finalize = Perf::Summarize(finalizingFrames);
		return fmt::format("resident GPU textures at most {} MB (budget {} MB); peak private {} MB, peak working set {} MB; {} frames finalized, median {} ms, "
			"p95 {} ms, at most {} ms and {} MB per frame; at most {} MB in flight; {} loads, {} evictions; frames until each stop settled:{}",
			result.MaxResidentTextureBytes / c_MBDouble, result.TextureBudget / c_MBDouble, result.PeakPrivateBytes / c_MBDouble,
			result.PeakWorkingSetBytes / c_MBDouble, finalize.Iterations, finalize.MedianMs, finalize.P95Ms, result.MaxFinalizeMs,
			result.MaxUploadedBytes / c_MBDouble, result.MaxInFlightBytes / c_MBDouble, result.LoadsCompleted, result.Evictions, settled);
	}

	// What a run of the exported game logged: its frames, the slowest Assets.Finalize stage, the bytes it uploaded, and its
	// memory and streaming state at the end.
	struct RuntimePerfLogResult
	{
		size_t Frames = 0;
		double MaxFinalizeMs = 0.0;
		size_t SlowestFrame = 0;
		double UploadedMB = 0.0;
		double PeakPrivateMB = 0.0;      // Of 10^6 bytes, like the sweeps' memory metrics
		double ResidentTexturesMB = 0.0; // At the end, of 10^6 bytes
		double LoadingAssets = 0.0;      // At the end
		double Evictions = 0.0;          // In all
	};

	// Runs the exported game for `frames` frames in a window of c_RuntimeWidth x c_RuntimeHeight pixels (--resolution), with
	// the stress test's texture budget (or the game's own budgets), and reads its perf log, which must have a row per frame.
	RuntimePerfLogResult RunExportedGame(const std::filesystem::path& manifest, const std::filesystem::path& perfLog, uint32_t frames, bool textureBudget)
	{
		ProcessSpecification runtime;
		runtime.Executable = GetRuntimePath();
		runtime.Arguments = { "--game", FileSystem::ToUTF8(manifest), "--resolution", fmt::format("{}x{}", c_RuntimeWidth, c_RuntimeHeight), "--frames",
			std::to_string(frames), "--perf-log", FileSystem::ToUTF8(perfLog) };
		if (textureBudget)
		{
			runtime.Arguments.push_back("--asset-budget-mb");
			runtime.Arguments.push_back(std::to_string(c_TextureBudget / (1024 * 1024)));
		}
		const Process::RunResult run = Process::Run(runtime, std::chrono::minutes(5));
		INFO("Runtime output:\n", run.Output);
		REQUIRE_MESSAGE(run.Started, run.Error);
		REQUIRE_FALSE(run.TimedOut);
		REQUIRE(run.ExitCode == 0);
		// It rendered the pixels asked for, whatever the display's scale.
		REQUIRE(run.Output.find(fmt::format("Swapchain: {} x {}", c_RuntimeWidth, c_RuntimeHeight)) != std::string::npos);

		std::string error;
		const std::optional<PerfLogTable> table = ReadPerfLog(perfLog, error);
		REQUIRE_MESSAGE(table, error);
		REQUIRE_MESSAGE(ValidatePerfLog(*table, frames, error), error); // One row per frame
		const std::optional<size_t> finalizeColumn = table->FindColumn("Assets.Finalize");
		const std::optional<size_t> uploadedColumn = table->FindColumn("UploadedMB");
		const std::optional<size_t> peakPrivateColumn = table->FindColumn("PeakPrivateMB");
		const std::optional<size_t> texturesColumn = table->FindColumn("ResidentGpuTexturesMB");
		const std::optional<size_t> loadingColumn = table->FindColumn("LoadingAssets");
		const std::optional<size_t> evictionsColumn = table->FindColumn("Evictions");
		REQUIRE(finalizeColumn);
		REQUIRE(uploadedColumn);
		REQUIRE(peakPrivateColumn);
		REQUIRE(texturesColumn);
		REQUIRE(loadingColumn);
		REQUIRE(evictionsColumn);
		RuntimePerfLogResult result;
		result.Frames = table->Rows.size();
		for (size_t row = 0; row < table->Rows.size(); row++)
		{
			const double finalizeMs = table->Rows[row][*finalizeColumn];
			if (finalizeMs > result.MaxFinalizeMs)
			{
				result.MaxFinalizeMs = finalizeMs;
				result.SlowestFrame = row;
			}
			result.UploadedMB += table->Rows[row][*uploadedColumn];
		}
		// The log's MB are 2^20 bytes.
		result.PeakPrivateMB = table->Rows.back()[*peakPrivateColumn] * 1024.0 * 1024.0 / c_MBDouble;
		result.ResidentTexturesMB = table->Rows.back()[*texturesColumn] * 1024.0 * 1024.0 / c_MBDouble;
		result.LoadingAssets = table->Rows.back()[*loadingColumn];
		result.Evictions = table->Rows.back()[*evictionsColumn];
		return result;
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
			REQUIRE_MESSAGE(WriteStressTopDownScene(*project, spec, error), error); // For the exported game with every texture resident
			const double writeSeconds = ElapsedSeconds(start);
			REQUIRE_MESSAGE(BuildStressPack(*project, directory / "Stress.stpak", error), error);
			MESSAGE("Stress project written in ", writeSeconds, " s, imported and packed in ", ElapsedSeconds(start) - writeSeconds, " s");
		}
		REQUIRE(GetStressTextureFormat() == TextureFormat::BC7SRGB);
		CHECK(project->TextureBytes == 111848560);       // 16 x 2048^2 + 1 x 4096^2 BC7 with mips: 111.8 MB
		CHECK(project->LargestTextureBytes == 22369648); // 22.4 MB

		// The evicting sweep: 600 frames at 1280x720 with a texture budget that holds less than the stops show over time.
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
			uint32_t framesToSettle = 0;
			for (uint32_t frames : result.FramesToSettle)
				framesToSettle = std::max(framesToSettle, std::min(frames, settings.Path.HoldFrames));
			MESSAGE("Evicting sweep ", sweep, " (", ElapsedSeconds(start), " s): ", DescribeSweep(result));

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

		// All textures resident: the stress game at 1920x1080 for 300 frames, over twelve stops whose views take in every
		// district, with a texture budget that holds every texture. Every sweep that runs: each texture arrives and none
		// leaves, no load fails and the device reports no error.
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
		uint32_t allResidentSweeps = 0;
		Perf::CheckBudgetsConfirmed({ c_PeakPrivateMemoryAllResident, c_MaxFinalizeTimeAllResident }, [&]
		{
			const uint32_t sweep = ++allResidentSweeps;
			CAPTURE(sweep);
			const auto start = std::chrono::steady_clock::now();
			const StreamingSweepResult result = RunSweepProcess(allResidentFile, directory / ("AllResidentResult" + std::to_string(sweep) + ".json"));
			MESSAGE("All resident, sweep ", sweep, " (", ElapsedSeconds(start), " s): ", DescribeSweep(result));
			CHECK(result.NewErrors == 0);
			CHECK(result.FailedAssets == 0);
			CHECK(result.Evictions == 0);
			CHECK(result.MaxResidentTextureBytes == project->TextureBytes);
			REQUIRE(result.PeakPrivateBytes > 0);
			return Perf::MetricValues { { c_PeakPrivateMemoryAllResident, static_cast<double>(result.PeakPrivateBytes) / c_MBDouble },
				{ c_MaxFinalizeTimeAllResident, result.MaxFinalizeMs } };
		});

		// The exported game, in a window of 1920x1080 pixels, with its perf log: the runtime's own frames, from the world's
		// overview camera, then from the top-down scene's. Every run renders those pixels, streams and logs every frame.
		const auto saveManifest = [&](AssetHandle startScene, const char* file)
		{
			GameManifest manifest;
			manifest.Name = "StrataStressWorld";
			manifest.AssetPack = "Stress.stpak";
			manifest.StartScene = startScene;
			const std::filesystem::path path = directory / file;
			REQUIRE_MESSAGE(manifest.Save(path, &error), error);
			return path;
		};
		const std::filesystem::path manifestPath = saveManifest(project->WorldScene, "Stress.stgame");
		const std::filesystem::path topDownManifestPath = saveManifest(project->TopDownScene, "StressTopDown.stgame");
		uint32_t runtimeRuns = 0;
		Perf::CheckBudgetConfirmed(c_RuntimeMaxFinalizeStage, [&]
		{
			const uint32_t run = ++runtimeRuns;
			CAPTURE(run);
			const RuntimePerfLogResult result = RunExportedGame(manifestPath, directory / ("RuntimePerfLog" + std::to_string(run) + ".csv"), c_RuntimeFrames, true);
			MESSAGE("Exported stress game, run ", run, ": ", result.Frames, " frames logged, Assets.Finalize at most ", result.MaxFinalizeMs,
				" ms (frame ", result.SlowestFrame, "), ", result.UploadedMB, " MB uploaded");
			CHECK(result.UploadedMB > 0.0); // It streamed
			return result.MaxFinalizeMs;
		});
		Perf::CheckBudgetConfirmed(c_RuntimePeakPrivateMemory, [&]
		{
			const uint32_t run = ++runtimeRuns;
			CAPTURE(run);
			const RuntimePerfLogResult result = RunExportedGame(manifestPath, directory / ("RuntimePerfLog" + std::to_string(run) + ".csv"), c_RuntimeFrames, false);
			MESSAGE("Exported stress game with its own budgets, run ", run, ": peak private memory ", result.PeakPrivateMB, " MB, ",
				result.ResidentTexturesMB, " MB of textures resident at the end");
			CHECK(result.ResidentTexturesMB > 0.0);
			return result.PeakPrivateMB;
		});
		// Every texture of the pack resident at the end (and none evicted or still loading): the run measured the criterion.
		Perf::CheckBudgetConfirmed(c_RuntimePeakPrivateMemoryAllResident, [&]
		{
			const uint32_t run = ++runtimeRuns;
			CAPTURE(run);
			const RuntimePerfLogResult result = RunExportedGame(topDownManifestPath, directory / ("RuntimePerfLog" + std::to_string(run) + ".csv"),
				c_RuntimeAllResidentFrames, false);
			MESSAGE("Exported stress game with every texture resident, run ", run, ": peak private memory ", result.PeakPrivateMB, " MB, ",
				result.ResidentTexturesMB, " MB of textures resident at the end");
			// The log has three decimals of 2^20 bytes.
			CHECK(std::abs(result.ResidentTexturesMB * c_MBDouble - static_cast<double>(project->TextureBytes)) <= 0.001 * 1024.0 * 1024.0);
			CHECK(result.Evictions == 0.0);
			CHECK(result.LoadingAssets == 0.0);
			return result.PeakPrivateMB;
		});
	}
}
