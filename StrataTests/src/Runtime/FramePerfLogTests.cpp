#include <doctest/doctest.h>

#include "Runtime/PerfLogReader.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/FrameProfiler.h"
#include "Strata/Runtime/FramePerfLog.h"
#include "TestHelpers.h"

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
		assets.Resident = AssetMemoryUsage { 5 * 1024 * 1024, 64 * 1024 * 1024, 0 };
		assets.Evictions = 7;
		bool withAssets = false;
		const uint32_t listener = profiler.AddFrameListener([&](const FrameSample& frame)
		{
			CHECK(log->WriteFrame(frame, withAssets ? &assets : nullptr));
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
		const std::optional<size_t> texturesColumn = table->FindColumn("ResidentGpuTexturesMB");
		const std::optional<size_t> evictionsColumn = table->FindColumn("Evictions");
		const std::optional<size_t> privateColumn = table->FindColumn("PrivateMB");
		const std::optional<size_t> peakPrivateColumn = table->FindColumn("PeakPrivateMB");
		REQUIRE((customColumn && finalizeColumn && workColumn && intervalColumn && uploadedColumn && queuedColumn && texturesColumn && evictionsColumn));
		REQUIRE((privateColumn && peakPrivateColumn));
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
			}
			else
			{
				CHECK(values[*uploadedColumn] == doctest::Approx(1.0));
				CHECK(values[*queuedColumn] == 7.0);
				CHECK(values[*texturesColumn] == doctest::Approx(64.0));
				CHECK(values[*evictionsColumn] == 7.0);
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
