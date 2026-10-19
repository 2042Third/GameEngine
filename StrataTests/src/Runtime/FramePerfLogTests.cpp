#include <doctest/doctest.h>

#include "Runtime/PerfLogReader.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/FrameProfiler.h"
#include "Strata/Renderer/GPUUploadQueue.h"
#include "Strata/Runtime/FramePerfLog.h"
#include "TestHelpers.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

TEST_SUITE("Runtime.PerfLog")
{
	TEST_CASE("The perf log has a row per frame with the stages and the streaming numbers")
	{
		int64_t time = 0;
		FrameProfilerSpecification specification;
		specification.Clock = [&time]() { return time; };
		specification.SampleJobs = false;
		FrameProfiler profiler(specification);
		// A stage whose name needs quoting in CSV.
		const FrameStageID custom = profiler.RegisterStage("Game.AI, \"planning\"");
		const FrameStageID finalize = profiler.FindStage("Assets.Finalize");

		const std::filesystem::path path = CreateTemporaryDirectory("PerfLog") / "Logs" / "Frames.csv";
		std::string error;
		Scope<FramePerfLog> log = FramePerfLog::Create(path, profiler, &error);
		REQUIRE_MESSAGE(log, error);
		CHECK(log->GetColumns().front() == "Frame");

		AssetManagerStats assets;
		assets.LoadingAssets = 3;
		assets.QueuedLoads = { 1, 2, 4 };
		assets.InFlightLoads = 2;
		assets.InFlightBytes = 3 * 1024 * 1024;
		assets.UploadedBytesLastFrame = 1024 * 1024;
		assets.FinalizeMsLastFrame = 2.5f;
		assets.UploadMsLastFrame = 1.25f;
		assets.Resident = AssetMemoryUsage { 5 * 1024 * 1024, 64 * 1024 * 1024, 0 };
		assets.Evictions = 7;
		assets.Detail.BytesStreamedLastFrame = 2 * 1024 * 1024;
		assets.Detail.LevelsRaised = 9;
		assets.Detail.LevelsDropped = 4;
		assets.Detail.AssetsBelowWanted = 3;
		GPUUploadQueueStats uploads;
		uploads.FrameBytes = 3 * 1024 * 1024;
		uploads.FrameMs = 0.75f;
		uploads.FrameSteps = 5;
		FramePerfDeviceMemory device;
		device.DeviceBytes = 300 * 1024 * 1024;
		device.StagingBytes = 16 * 1024 * 1024;
		bool withAssets = false;
		const uint32_t listener = profiler.AddFrameListener([&](const FrameSample& frame)
		{
			CHECK(log->WriteFrame(frame, withAssets ? &assets : nullptr, withAssets ? &uploads : nullptr, withAssets ? &device : nullptr));
		});
		for (int frame = 0; frame < 6; frame++)
		{
			withAssets = frame >= 3;
			profiler.BeginFrame();
			{
				const FrameStageScope stage(profiler, finalize);
				time += 1'500'000; // 1.5 ms
			}
			{
				const FrameStageScope stage(profiler, custom);
				time += 250'000;
			}
			time += 250'000;
			profiler.EndFrame(0.002);
			time += 14'000'000;
		}
		profiler.RemoveFrameListener(listener);
		// Registered after the header: no column.
		profiler.RegisterStage("Late.Stage");
		CHECK(log->GetRowCount() == 6);
		REQUIRE_MESSAGE(log->Close(&error), error);
		CHECK_FALSE(log->WriteFrame(profiler.GetFrame(0), nullptr)); // Closed

		const std::optional<PerfLogTable> table = ReadPerfLog(path, error);
		REQUIRE_MESSAGE(table, error);
		REQUIRE_MESSAGE(ValidatePerfLog(*table, 6, error), error);
		CHECK_FALSE(table->FindColumn("Late.Stage"));
		const std::optional<size_t> customColumn = table->FindColumn("Game.AI, \"planning\"");
		const std::optional<size_t> finalizeColumn = table->FindColumn("Assets.Finalize");
		const std::optional<size_t> workColumn = table->FindColumn("WorkMs");
		const std::optional<size_t> intervalColumn = table->FindColumn("IntervalMs");
		const std::optional<size_t> uploadedColumn = table->FindColumn("UploadedMB");
		const std::optional<size_t> queuedColumn = table->FindColumn("QueuedLoads");
		const std::optional<size_t> uploadMsColumn = table->FindColumn("UploadMs");
		const std::optional<size_t> streamedColumn = table->FindColumn("StreamedMB");
		const std::optional<size_t> raisedColumn = table->FindColumn("MipLevelsRaised");
		const std::optional<size_t> droppedColumn = table->FindColumn("MipLevelsDropped");
		const std::optional<size_t> belowColumn = table->FindColumn("MipsBelowWanted");
		const std::optional<size_t> queueBytesColumn = table->FindColumn("UploadQueueMB");
		const std::optional<size_t> queueMsColumn = table->FindColumn("UploadQueueMs");
		const std::optional<size_t> queueStepsColumn = table->FindColumn("UploadQueueSteps");
		const std::optional<size_t> deviceColumn = table->FindColumn("DeviceMB");
		const std::optional<size_t> stagingColumn = table->FindColumn("StagingMB");
		const std::optional<size_t> texturesColumn = table->FindColumn("ResidentGpuTexturesMB");
		const std::optional<size_t> evictionsColumn = table->FindColumn("Evictions");
		const std::optional<size_t> privateColumn = table->FindColumn("PrivateMB");
		const std::optional<size_t> peakPrivateColumn = table->FindColumn("PeakPrivateMB");
		REQUIRE((customColumn && finalizeColumn && workColumn && intervalColumn && uploadedColumn && queuedColumn && texturesColumn && evictionsColumn));
		REQUIRE((privateColumn && peakPrivateColumn));
		REQUIRE((uploadMsColumn && queueBytesColumn && queueMsColumn && queueStepsColumn));
		REQUIRE((streamedColumn && raisedColumn && droppedColumn && belowColumn));
		REQUIRE((deviceColumn && stagingColumn));
		for (size_t row = 0; row < table->Rows.size(); row++)
		{
			CAPTURE(row);
			const std::vector<double>& values = table->Rows[row];
			CHECK(values[0] == static_cast<double>(row));
			CHECK(values[*workColumn] == doctest::Approx(2.0));
			CHECK(values[*intervalColumn] == doctest::Approx(row == 0 ? 0.0 : 16.0));
			CHECK(values[*finalizeColumn] == doctest::Approx(1.5));
			CHECK(values[*customColumn] == doctest::Approx(0.25));
			// This process's memory, every row (a peak never goes down and is at least what is held).
			CHECK(values[*privateColumn] > 0.0);
			CHECK(values[*peakPrivateColumn] >= values[*privateColumn]);
			if (row > 0)
				CHECK(values[*peakPrivateColumn] >= table->Rows[row - 1][*peakPrivateColumn]);
			if (row < 3)
			{
				CHECK(std::isnan(values[*uploadedColumn])); // No asset manager: empty
				CHECK(std::isnan(values[*queueBytesColumn])); // No renderer: empty
				CHECK(std::isnan(values[*deviceColumn]));
				CHECK(std::isnan(values[*stagingColumn]));
			}
			else
			{
				CHECK(values[*uploadedColumn] == doctest::Approx(1.0));
				CHECK(values[*queuedColumn] == 7.0);
				CHECK(values[*texturesColumn] == doctest::Approx(64.0));
				CHECK(values[*evictionsColumn] == 7.0);
				CHECK(values[*uploadMsColumn] == doctest::Approx(1.25));
				CHECK(values[*streamedColumn] == doctest::Approx(2.0));
				CHECK(values[*raisedColumn] == 9.0);
				CHECK(values[*droppedColumn] == 4.0);
				CHECK(values[*belowColumn] == 3.0);
				CHECK(values[*deviceColumn] == doctest::Approx(300.0));
				CHECK(values[*stagingColumn] == doctest::Approx(16.0));
				CHECK(values[*queueBytesColumn] == doctest::Approx(3.0));
				CHECK(values[*queueMsColumn] == doctest::Approx(0.75));
				CHECK(values[*queueStepsColumn] == 5.0);
			}
		}
	}

	TEST_CASE("A perf log that cannot be written is refused")
	{
		FrameProfiler profiler(FrameProfilerSpecification { 4, nullptr, false });
		const std::filesystem::path directory = CreateTemporaryDirectory("PerfLogFolder");
		std::string error;
		// A folder is no file.
		CHECK_FALSE(FramePerfLog::Create(directory, profiler, &error));
		CHECK(error.find("cannot create") != std::string::npos);
	}

	TEST_CASE("Perf logs are read strictly")
	{
		std::string error;
		CHECK_FALSE(ParsePerfLog("", error));
		CHECK_FALSE(ParsePerfLog("Frame,WorkMs\n1\n", error));
		CHECK(error.find("1 fields for 2 columns") != std::string::npos);
		CHECK_FALSE(ParsePerfLog("Frame,WorkMs\n1,abc\n", error));
		CHECK_FALSE(ParsePerfLog("\"Frame,WorkMs\n", error));
		const std::optional<PerfLogTable> table = ParsePerfLog("Frame,\"A,\"\"B\"\"\"\r\n0,1.5\r\n1,\r\n", error);
		REQUIRE_MESSAGE(table, error);
		REQUIRE(table->Columns.size() == 2);
		CHECK(table->Columns[1] == "A,\"B\"");
		REQUIRE(table->Rows.size() == 2);
		CHECK(table->Rows[0][1] == 1.5);
		CHECK(std::isnan(table->Rows[1][1]));
		// Without the frame columns and stages it is no perf log.
		CHECK_FALSE(ValidatePerfLog(*table, 2, error));
		CHECK(error.find("no column") != std::string::npos);
	}
}

TEST_SUITE("Runtime.FramePerfLog")
{
	TEST_CASE("A perf log created after the modules registered has a column for every scene system and for the GPU")
	{
		// The process's profiler: the renderer module registered its GPU stages there, and no scene has run the Animation
		// system yet in this test.
		const std::filesystem::path path = CreateTemporaryDirectory("PerfLogColumns") / "Frames.csv";
		std::string error;
		Scope<FramePerfLog> log = FramePerfLog::Create(path, FrameProfiler::Get(), &error);
		REQUIRE_MESSAGE(log, error);
		const std::vector<std::string>& columns = log->GetColumns();
		const auto has = [&columns](const char* name) { return std::find(columns.begin(), columns.end(), name) != columns.end(); };
		for (const char* column : { "Scene.Scripts", "Scene.Physics", "Scene.Animation", "Scene.Audio", "GPU.Uploads", "GPU.Setup", "GPU.Shadows", "GPU.Prepass",
			"GPU.AO", "GPU.Opaque", "GPU.Transparent", "GPU.Post", "GPU.Overlays", "GPUFrame", "GPUBusyMs" })
		{
			CAPTURE(column);
			CHECK(has(column));
		}
		// The new columns come after every one that existed before them.
		const auto position = [&columns](const char* name) { return std::find(columns.begin(), columns.end(), name) - columns.begin(); };
		CHECK(position("PeakPrivateMB") + 1 == position("GPUFrame"));
		CHECK(position("GPUFrame") + 1 == position("GPUBusyMs"));
		CHECK(log->Close(&error));
	}

	TEST_CASE("Rows carry their own frame's GPU times, written once they arrived")
	{
		int64_t time = 0;
		FrameProfilerSpecification specification;
		specification.Clock = [&time]() { return time; };
		specification.SampleJobs = false;
		FrameProfiler profiler(specification);
		const FrameStageID shadows = profiler.RegisterStage("GPU.Shadows", false, FrameStageKind::GPU);
		const FrameStageID opaque = profiler.RegisterStage("GPU.Opaque", false, FrameStageKind::GPU);
		const FrameStageID input = profiler.FindStage("Input");
		profiler.SetGPUTimesExpected(true);

		const std::filesystem::path path = CreateTemporaryDirectory("PerfLogGPU") / "Frames.csv";
		std::string error;
		Scope<FramePerfLog> log = FramePerfLog::Create(path, profiler, &error);
		REQUIRE_MESSAGE(log, error);
		// Captured as frames end, written once their GPU times are known.
		AssetManagerStats assets;
		const uint32_t capture = profiler.AddFrameListener([&](const FrameSample& frame)
		{
			assets.LoadingAssets = static_cast<uint32_t>(frame.Index) + 10;
			log->CaptureFrame(frame, &assets);
		});
		const uint32_t write = profiler.AddFrameListener([&](const FrameSample& frame) { CHECK(log->WriteFrame(frame)); }, FrameListenerTiming::WithGPU);

		// Frame f's GPU times arrive two frames later: shadows f + 1 ms, opaque 2 ms; frame 2's never arrive.
		constexpr int c_Frames = 6;
		for (int frame = 0; frame < c_Frames; frame++)
		{
			profiler.BeginFrame();
			{
				const FrameStageScope stage(profiler, input);
				time += 1'000'000 * (frame + 1);
			}
			if (frame >= 2 && frame - 2 != 2)
			{
				const std::vector<GPUStageTime> times = { { shadows, static_cast<float>(frame - 2) + 1.0f }, { opaque, 2.0f } };
				REQUIRE(profiler.RecordGPUFrame(static_cast<uint64_t>(frame - 2), times));
			}
			profiler.EndFrame(0.010);
			time += 10'000'000;
		}
		CHECK(log->GetRowCount() == 2); // Frames 0 and 1; frame 2 waits for its times
		// A stage registered after the header: no column, and a warning naming it.
		profiler.RegisterStage("Late.Stage");
		profiler.FlushGPUListeners();
		CHECK(log->GetRowCount() == c_Frames);
		profiler.RemoveFrameListener(capture);
		profiler.RemoveFrameListener(write);
		REQUIRE_MESSAGE(log->Close(&error), error);

		const std::optional<PerfLogTable> table = ReadPerfLog(path, error);
		REQUIRE_MESSAGE(table, error);
		REQUIRE_MESSAGE(ValidatePerfLog(*table, c_Frames, error), error);
		const std::optional<size_t> inputColumn = table->FindColumn("Input");
		const std::optional<size_t> shadowsColumn = table->FindColumn("GPU.Shadows");
		const std::optional<size_t> opaqueColumn = table->FindColumn("GPU.Opaque");
		const std::optional<size_t> gpuFrameColumn = table->FindColumn("GPUFrame");
		const std::optional<size_t> busyColumn = table->FindColumn("GPUBusyMs");
		const std::optional<size_t> loadingColumn = table->FindColumn("LoadingAssets");
		REQUIRE((inputColumn && shadowsColumn && opaqueColumn && gpuFrameColumn && busyColumn && loadingColumn));
		CHECK_FALSE(table->FindColumn("Late.Stage"));
		for (size_t row = 0; row < table->Rows.size(); row++)
		{
			CAPTURE(row);
			const std::vector<double>& values = table->Rows[row];
			CHECK(values[0] == static_cast<double>(row));
			// The CPU columns and the captured ones are the row's own frame's.
			CHECK(values[*inputColumn] == doctest::Approx(static_cast<double>(row + 1)));
			CHECK(values[*loadingColumn] == static_cast<double>(row + 10));
			const bool hasGPU = row < 4 && row != 2; // Frames 4 and 5 were flushed before their times came
			CHECK(values[*gpuFrameColumn] == (hasGPU ? 1.0 : 0.0));
			if (hasGPU)
			{
				CHECK(values[*shadowsColumn] == doctest::Approx(static_cast<double>(row) + 1.0));
				CHECK(values[*opaqueColumn] == doctest::Approx(2.0));
				CHECK(values[*busyColumn] == doctest::Approx(static_cast<double>(row) + 3.0));
			}
			else
			{
				CHECK(std::isnan(values[*shadowsColumn]));
				CHECK(std::isnan(values[*busyColumn]));
			}
		}
	}
}
