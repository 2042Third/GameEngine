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
#include <cstddef>
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
	// largest), so the sweep keeps evicting. The sweep loads whole textures (mip streaming off): it is the gate of the
	// residency budgets and of eviction; mip streaming has its own sweep (the Mips metrics below).
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
	// load brings in every texture the world uses at its mip tail before it starts (all of them fit the budgets) and they
	// stay, raised to the levels the view needs. A maximum by nature, one run per attempt.
	constexpr const char* c_RuntimePeakPrivateMemory = "PerfGPU.Streaming.RuntimePeakPrivateMemory";
	// The same with the whole world in view: the game started in the top-down scene, whose camera sees every district (the
	// material of every district, so every texture, is drawn), for 300 frames, its textures at the mip levels the screen
	// needs (mip streaming), checked at the end: every texture at its wanted level, nothing loading or evicted. Texture
	// cooking's criterion (650 MB, then with every texture resident) stays the budget; mip streaming's 550 MB is its target.
	// A maximum by nature, one run per attempt.
	constexpr const char* c_RuntimePeakPrivateMemoryAllInView = "PerfGPU.Streaming.RuntimePeakPrivateMemoryAllInView";

	// Mip streaming's sweep: a flight 3 units above the ground of the flight scene (its terrain textured, tiled
	// c_StressTerrainTiling times, so the ground under the camera needs the finest level of its texture, and objects near the
	// camera fine levels of theirs) at 1920x1080 with a texture budget of 96 MB, run by a helper process with mip streaming,
	// again with mip streaming within a budget its needs exceed (c_MipsPressureBudget, so levels also go for the budget),
	// and with whole textures (the baseline: SceneRendererSpecification::TextureStreaming false, the manager's partial loads
	// off), one of each per attempt. The metrics, maxima by nature, of both streaming runs: their peak private memory (the
	// 550 MB acceptance limit), the most bytes the asset manager uploaded in a frame (24 MB) and the longest its upload steps
	// took (2 ms), and their longest frame to the baseline's (the CPU time of the manager's update and the rendering; a
	// ratio, as budgets are not negative: the acceptance limit, "the longest frame at most 2 ms over the baseline's", is a
	// ratio of 1.8 to 1.4 for the baseline's 2.5 to 4.5 ms, above the budget; the longest frames are among the first, which
	// load the world). The flight within 96 MB must reach the finest level and show the world like the baseline in its last
	// frame, near the camera too (checks).
	constexpr uint64_t c_MipsTextureBudget = 96 * c_MB;
	constexpr uint64_t c_MipsPressureBudget = 8 * c_MB;
	constexpr const char* c_MipsPeakPrivateMemory = "PerfGPU.Streaming.Mips.PeakPrivateMemory";
	constexpr const char* c_MipsMaxUploadBytesPerFrame = "PerfGPU.Streaming.Mips.MaxUploadBytesPerFrame";
	constexpr const char* c_MipsMaxUploadStageMs = "PerfGPU.Streaming.Mips.MaxUploadStageMs";
	constexpr const char* c_MipsMaxFrameRatio = "PerfGPU.Streaming.Mips.MaxFrameRatio";
	// The streaming run's last frame against the baseline's, in dB (peak signal-to-noise ratio over RGB).
	constexpr double c_MinFinalFramePSNR = 40.0;
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

	// The peak signal-to-noise ratio of two RGBA8 images of the same size, over their color channels, in dB (100 when they are
	// equal).
	double ComputePSNR(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
	{
		REQUIRE(a.size() == b.size());
		REQUIRE(a.size() % 4 == 0);
		double squaredError = 0.0;
		for (size_t index = 0; index < a.size(); index++)
		{
			if (index % 4 == 3)
				continue;
			const double difference = static_cast<double>(a[index]) - static_cast<double>(b[index]);
			squaredError += difference * difference;
		}
		const double meanSquaredError = squaredError / (static_cast<double>(a.size()) * 0.75);
		return meanSquaredError > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / meanSquaredError) : 100.0;
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
		double MipsBelowWanted = 0.0;    // At the end: textures below the level their views need
		double MipLevelsRaised = 0.0;    // In all
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
		const std::optional<size_t> belowWantedColumn = table->FindColumn("MipsBelowWanted");
		const std::optional<size_t> raisedColumn = table->FindColumn("MipLevelsRaised");
		REQUIRE(belowWantedColumn);
		REQUIRE(raisedColumn);
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
		result.MipsBelowWanted = table->Rows.back()[*belowWantedColumn];
		result.MipLevelsRaised = table->Rows.back()[*raisedColumn];
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
			REQUIRE_MESSAGE(WriteStressFlightScene(*project, spec, error), error); // For mip streaming's flight
			const double writeSeconds = ElapsedSeconds(start);
			REQUIRE_MESSAGE(BuildStressPack(*project, directory / "Stress.stpak", error), error);
			MESSAGE("Stress project written in ", writeSeconds, " s, imported and packed in ", ElapsedSeconds(start) - writeSeconds, " s");
		}
		REQUIRE(GetStressTextureFormat() == TextureFormat::BC7SRGB);
		CHECK(project->TextureBytes == 111848560);       // 16 x 2048^2 + 1 x 4096^2 BC7 with mips: 111.8 MB
		CHECK(project->LargestTextureBytes == 22369648); // 22.4 MB

		// The evicting sweep: 600 frames at 1280x720 with a texture budget that holds less than the stops show over time,
		// with whole textures.
		StreamingSweepSettings settings;
		settings.Pack = directory / "Stress.stpak";
		settings.Scene = project->WorldScene;
		settings.Spec = spec;
		settings.TextureBudget = c_TextureBudget;
		settings.TextureStreaming = false;
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
		// district, with a texture budget that holds every texture and whole textures (every level of every texture in view).
		// Every sweep that runs: each texture arrives and none leaves, no load fails and the device reports no error.
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

		// Mip streaming's flight: 3 units above the flight scene's textured ground over six districts, looking 20 degrees down,
		// at 1920x1080, 600 frames, with a 96 MB budget; the same within a budget its needs exceed; and the baseline, the
		// same with whole textures within 96 MB.
		StreamingSweepSettings mips = settings;
		mips.Scene = project->FlightScene;
		mips.TextureStreaming = true;
		mips.TextureBudget = c_MipsTextureBudget;
		mips.Width = 1920;
		mips.Height = 1080;
		mips.Path.Stops = 6;
		mips.Path.MoveFrames = 60;
		mips.Path.HoldFrames = 40;
		mips.Path.Height = 3.0f;
		mips.Path.PitchDegrees = 20.0f;
		REQUIRE(mips.Path.GetFrameCount() == 600);
		mips.FinalFrameFile = directory / "MipsFinalFrame.rgba";
		StreamingSweepSettings pressure = mips;
		pressure.TextureBudget = c_MipsPressureBudget;
		pressure.FinalFrameFile.clear();
		StreamingSweepSettings baseline = mips;
		baseline.TextureStreaming = false;
		baseline.FinalFrameFile = directory / "BaselineFinalFrame.rgba";
		const std::filesystem::path mipsFile = directory / "MipsSettings.json";
		const std::filesystem::path pressureFile = directory / "MipsPressureSettings.json";
		const std::filesystem::path baselineFile = directory / "BaselineSettings.json";
		REQUIRE(FileSystem::WriteText(mipsFile, JsonUtils::Dump(ToJson(mips))));
		REQUIRE(FileSystem::WriteText(pressureFile, JsonUtils::Dump(ToJson(pressure))));
		REQUIRE(FileSystem::WriteText(baselineFile, JsonUtils::Dump(ToJson(baseline))));
		// The finest level of the ground's texture alone holds this much: the flight must have had it resident.
		const uint64_t smallTextureBytes = GetStressTextureBytes(spec.SmallTextureSize);
		uint32_t mipSweeps = 0;
		Perf::CheckBudgetsConfirmed({ c_MipsPeakPrivateMemory, c_MipsMaxUploadBytesPerFrame, c_MipsMaxUploadStageMs, c_MipsMaxFrameRatio }, [&]
		{
			const uint32_t sweep = ++mipSweeps;
			CAPTURE(sweep);
			const auto start = std::chrono::steady_clock::now();
			const StreamingSweepResult streamed = RunSweepProcess(mipsFile, directory / ("MipsResult" + std::to_string(sweep) + ".json"));
			const StreamingSweepResult pressed = RunSweepProcess(pressureFile, directory / ("MipsPressureResult" + std::to_string(sweep) + ".json"));
			const StreamingSweepResult whole = RunSweepProcess(baselineFile, directory / ("BaselineResult" + std::to_string(sweep) + ".json"));
			REQUIRE(streamed.FrameMs.size() == streamed.Frames);
			REQUIRE(pressed.FrameMs.size() == streamed.FrameMs.size());
			REQUIRE(whole.FrameMs.size() == streamed.FrameMs.size());
			// The runs load at different frames, so the longest frames compare, not frames of the same number. The first frame
			// creates the renderer's pipelines and targets, the same work in every run that takes far longer than any other
			// frame (and varies by tens of milliseconds): it would hide what streaming adds.
			REQUIRE(streamed.FrameMs.size() > 1);
			const double streamedLongest = std::max(*std::max_element(streamed.FrameMs.begin() + 1, streamed.FrameMs.end()),
				*std::max_element(pressed.FrameMs.begin() + 1, pressed.FrameMs.end()));
			const double wholeLongest = *std::max_element(whole.FrameMs.begin() + 1, whole.FrameMs.end());
			REQUIRE(wholeLongest > 0.0);
			const std::optional<std::vector<uint8_t>> streamedFrame = FileSystem::ReadBytes(mips.FinalFrameFile);
			const std::optional<std::vector<uint8_t>> wholeFrame = FileSystem::ReadBytes(baseline.FinalFrameFile);
			REQUIRE(streamedFrame);
			REQUIRE(wholeFrame);
			REQUIRE(streamedFrame->size() == static_cast<size_t>(mips.Width) * mips.Height * 4);
			const double psnr = ComputePSNR(*streamedFrame, *wholeFrame);
			// The ground just ahead of the camera: the bottom third of the view, which shows the finest levels.
			const size_t nearOffset = static_cast<size_t>(mips.Width) * (mips.Height - mips.Height / 3) * 4;
			const double nearPSNR = ComputePSNR(std::vector<uint8_t>(streamedFrame->begin() + static_cast<std::ptrdiff_t>(nearOffset), streamedFrame->end()),
				std::vector<uint8_t>(wholeFrame->begin() + static_cast<std::ptrdiff_t>(nearOffset), wholeFrame->end()));
			const auto describeStreaming = [](const StreamingSweepResult& result)
			{
				return fmt::format("{}; upload steps at most {} ms; {} levels raised, {} dropped ({} for the budget), {} MB read for them, finest level {}",
					DescribeSweep(result), result.MaxUploadMs, result.LevelsRaised, result.LevelsDropped, result.LevelsDroppedForBudget,
					result.BytesStreamed / c_MBDouble, result.FinestStreamedMip);
			};
			MESSAGE("Mip streaming, sweep ", sweep, " (", ElapsedSeconds(start), " s): ", describeStreaming(streamed), "; longest frame ", streamedLongest,
				" ms (of both streaming runs), the baseline's ", wholeLongest, " ms (", streamedLongest - wholeLongest, " ms over it); last frame ", psnr,
				" dB against the baseline's, its bottom third ", nearPSNR, " dB. Within ", c_MipsPressureBudget / c_MBDouble, " MB: ", describeStreaming(pressed),
				". Baseline: ", DescribeSweep(whole), "; upload steps at most ", whole.MaxUploadMs, " ms");

			// Every run streams correctly; streaming raises and drops levels, reaches the finest level of the ground's texture
			// and holds less than whole textures; its last frame shows the world like the baseline's, near the camera too.
			for (const StreamingSweepResult* run : { &streamed, &pressed })
			{
				CHECK(run->NewErrors == 0);
				CHECK(run->FailedAssets == 0);
				CHECK(std::find(run->FramesToSettle.begin(), run->FramesToSettle.end(), c_StopNeverSettled) == run->FramesToSettle.end());
				CHECK(run->LevelsRaised > 0);
				CHECK(run->LevelsDropped > 0);
			}
			CHECK(whole.NewErrors == 0);
			CHECK(whole.LevelsRaised == 0);
			CHECK(streamed.FinestStreamedMip == 0);
			CHECK(streamed.MaxResidentTextureBytes > smallTextureBytes);
			CHECK(streamed.MaxResidentTextureBytes < whole.MaxResidentTextureBytes);
			// The budget's pressure took levels, and residency stayed within it.
			CHECK(pressed.LevelsDroppedForBudget > 0);
			CHECK(pressed.MaxResidentTextureBytes <= c_MipsPressureBudget);
			// The baseline's last frame is the fully resident render: its last stop settled.
			REQUIRE_FALSE(whole.FramesToSettle.empty());
			CHECK(whole.FramesToSettle.back() != c_StopNeverSettled);
			CHECK(psnr >= c_MinFinalFramePSNR);
			CHECK(nearPSNR >= c_MinFinalFramePSNR);
			REQUIRE(streamed.PeakPrivateBytes > 0);
			REQUIRE(pressed.PeakPrivateBytes > 0);
			return Perf::MetricValues {
				{ c_MipsPeakPrivateMemory, static_cast<double>(std::max(streamed.PeakPrivateBytes, pressed.PeakPrivateBytes)) / c_MBDouble },
				{ c_MipsMaxUploadBytesPerFrame, static_cast<double>(std::max(streamed.MaxUploadedBytes, pressed.MaxUploadedBytes)) / c_MBDouble },
				{ c_MipsMaxUploadStageMs, static_cast<double>(std::max(streamed.MaxUploadMs, pressed.MaxUploadMs)) },
				{ c_MipsMaxFrameRatio, streamedLongest / wholeLongest } };
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
		// Every texture at the level the screen needs at the end (none below it, none evicted or still loading): the run
		// measured the criterion.
		Perf::CheckBudgetConfirmed(c_RuntimePeakPrivateMemoryAllInView, [&]
		{
			const uint32_t run = ++runtimeRuns;
			CAPTURE(run);
			const RuntimePerfLogResult result = RunExportedGame(topDownManifestPath, directory / ("RuntimePerfLog" + std::to_string(run) + ".csv"),
				c_RuntimeAllResidentFrames, false);
			MESSAGE("Exported stress game with the whole world in view, run ", run, ": peak private memory ", result.PeakPrivateMB, " MB, ",
				result.ResidentTexturesMB, " MB of textures resident at the end (", result.MipLevelsRaised, " mip levels raised)");
			CHECK(result.ResidentTexturesMB > 0.0);
			CHECK(result.ResidentTexturesMB * c_MBDouble < static_cast<double>(project->TextureBytes));
			CHECK(result.MipsBelowWanted == 0.0);
			CHECK(result.Evictions == 0.0);
			CHECK(result.LoadingAssets == 0.0);
			return result.PeakPrivateMB;
		});
	}
}
