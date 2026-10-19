#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Strata/Core/FrameProfiler.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/Log.h"
#include "TestHelpers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr int64_t c_Millisecond = 1'000'000; // Nanoseconds

	// A profiler on a clock the test sets, so stage times are exact.
	struct ManualProfiler
	{
		int64_t Time = 0;
		FrameProfiler Profiler;

		explicit ManualProfiler(uint32_t capacity = FrameProfiler::c_DefaultCapacity)
			: Profiler(MakeSpecification(capacity, Time))
		{
		}

		static FrameProfilerSpecification MakeSpecification(uint32_t capacity, int64_t& time)
		{
			FrameProfilerSpecification specification;
			specification.Capacity = capacity;
			specification.Clock = [&time]() { return time; };
			specification.SampleJobs = false;
			return specification;
		}

		void AdvanceMs(double milliseconds) { Time += static_cast<int64_t>(milliseconds * static_cast<double>(c_Millisecond)); }
	};

	float StageMs(const FrameSample& frame, FrameStageID stage)
	{
		REQUIRE(stage < frame.StageMs.size());
		return frame.StageMs[stage];
	}

	FrameStageID Builtin(const FrameProfiler& profiler, std::string_view name)
	{
		const FrameStageID stage = profiler.FindStage(name);
		REQUIRE(stage != c_InvalidFrameStage);
		return stage;
	}

}

TEST_SUITE("Core.FrameProfiler")
{
	TEST_CASE("The built-in stages come first, in their order")
	{
		FrameProfiler profiler(FrameProfilerSpecification { 4, nullptr, false });
		REQUIRE(profiler.GetStageCount() == c_BuiltinFrameStages.size());
		for (size_t index = 0; index < c_BuiltinFrameStages.size(); index++)
		{
			CAPTURE(c_BuiltinFrameStages[index].Name);
			CHECK(profiler.GetStageName(static_cast<FrameStageID>(index)) == c_BuiltinFrameStages[index].Name);
			CHECK(profiler.FindStage(c_BuiltinFrameStages[index].Name) == index);
			CHECK(profiler.IsStageInWorkTime(static_cast<FrameStageID>(index)) == c_BuiltinFrameStages[index].InWorkTime);
		}
		// Present runs after the work time is measured.
		CHECK_FALSE(profiler.IsStageInWorkTime(Builtin(profiler, "Present")));
		CHECK(profiler.IsStageInWorkTime(Builtin(profiler, "Scene.Physics")));

		// A new name gets the next id, a known one its id.
		const FrameStageID custom = profiler.RegisterStage("Game.AI");
		CHECK(custom == c_BuiltinFrameStages.size());
		CHECK(profiler.RegisterStage("Game.AI") == custom);
		CHECK(profiler.RegisterStage("Scene.Physics") == Builtin(profiler, "Scene.Physics"));
		CHECK(profiler.FindStage("Unknown") == c_InvalidFrameStage);
		CHECK(profiler.GetStageName(c_InvalidFrameStage).empty());
	}

	TEST_CASE("Registering more stages than fit leaves the extra ones untimed")
	{
		FrameProfiler profiler(FrameProfilerSpecification { 4, nullptr, false });
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		while (profiler.GetStageCount() < FrameProfiler::c_MaxStages)
			CHECK(profiler.RegisterStage("Extra." + std::to_string(profiler.GetStageCount())) != c_InvalidFrameStage);
		CHECK(profiler.RegisterStage("OneTooMany") == c_InvalidFrameStage);
		CHECK(profiler.RegisterStage("TwoTooMany") == c_InvalidFrameStage);
		CHECK(CountLogMessages(logStart, "are not timed") == 1); // Warned once
		// The stages that fit still resolve.
		CHECK(profiler.RegisterStage("Extra.20") != c_InvalidFrameStage);

		// An invalid stage is not timed (its scope does nothing).
		profiler.BeginFrame();
		{
			const FrameStageScope scope(profiler, c_InvalidFrameStage);
		}
		profiler.EndFrame(0.001);
		CHECK(profiler.GetRecordedFrameCount() == 1);
	}

	TEST_CASE("Nested stages record their self time")
	{
		ManualProfiler manual;
		FrameProfiler& profiler = manual.Profiler;
		const FrameStageID update = Builtin(profiler, "Assets.Update");
		const FrameStageID finalize = Builtin(profiler, "Assets.Finalize");
		const FrameStageID transforms = Builtin(profiler, "Scene.Transforms");

		profiler.BeginFrame();
		manual.AdvanceMs(1.0); // Outside any stage
		{
			const FrameStageScope outer(profiler, update);
			manual.AdvanceMs(2.0);
			{
				const FrameStageScope inner(profiler, finalize);
				manual.AdvanceMs(3.0);
				{
					const FrameStageScope innermost(profiler, transforms);
					manual.AdvanceMs(0.5);
				}
				manual.AdvanceMs(1.0);
			}
			manual.AdvanceMs(1.5);
			{
				// The same stage again: the times add up.
				const FrameStageScope inner(profiler, finalize);
				manual.AdvanceMs(2.0);
			}
			{
				// A stage nested in itself counts once.
				const FrameStageScope again(profiler, update);
				manual.AdvanceMs(0.25);
			}
		}
		manual.AdvanceMs(1.0);
		profiler.EndFrame(0.0125);

		REQUIRE(profiler.GetRecordedFrameCount() == 1);
		const FrameSample frame = profiler.GetFrame(0);
		CHECK(frame.Index == 0);
		CHECK(frame.WorkMs == doctest::Approx(12.5f));
		CHECK(StageMs(frame, update) == doctest::Approx(2.0f + 1.5f + 0.25f));
		CHECK(StageMs(frame, finalize) == doctest::Approx(3.0f + 1.0f + 2.0f));
		CHECK(StageMs(frame, transforms) == doctest::Approx(0.5f));
		CHECK(StageMs(frame, Builtin(profiler, "Scene.Physics")) == 0.0f);

		// The stages add up to the time spent in them, never more than the frame.
		const FrameProfileSummary summary = profiler.Summarize(1);
		CHECK(summary.StageSum.Max == doctest::Approx(10.25f));
		CHECK(summary.StageSum.Max <= summary.Work.Max);
		CHECK(summary.Untimed.Max == doctest::Approx(12.5f - 10.25f)); // The rest of the frame
	}

	TEST_CASE("Stages outside the work time report their time while the frame runs, also nested in other stages")
	{
		ManualProfiler manual;
		FrameProfiler& profiler = manual.Profiler;
		const FrameStageID ui = Builtin(profiler, "Editor.UI");
		const FrameStageID present = Builtin(profiler, "Present");
		CHECK(profiler.GetOutOfWorkSeconds() == 0.0); // Outside a frame

		profiler.BeginFrame();
		{
			const FrameStageScope outer(profiler, ui);
			manual.AdvanceMs(2.0);
			{
				// The display's image, waited for at its first use while the UI draws into it.
				const FrameStageScope wait(profiler, present);
				manual.AdvanceMs(6.0);
			}
			manual.AdvanceMs(1.0);
		}
		CHECK(profiler.GetOutOfWorkSeconds() == doctest::Approx(0.006));
		profiler.EndFrame(0.003);
		const FrameSample frame = profiler.GetFrame(0);
		CHECK(StageMs(frame, ui) == doctest::Approx(3.0f));
		CHECK(StageMs(frame, present) == doctest::Approx(6.0f));

		// Every frame starts from nothing.
		profiler.BeginFrame();
		CHECK(profiler.GetOutOfWorkSeconds() == 0.0);
		profiler.EndFrame(0.0);
	}

	TEST_CASE("Stages time only the frame thread's frames")
	{
		ManualProfiler manual;
		FrameProfiler& profiler = manual.Profiler;
		const FrameStageID physics = Builtin(profiler, "Scene.Physics");
		const FrameStageID scripts = Builtin(profiler, "Scene.Scripts");

		// Before any frame: nothing is timed, and nothing is recorded.
		CHECK_FALSE(profiler.IsTimingThisThread());
		CHECK_FALSE(profiler.BeginStage(physics));
		profiler.EndFrame(0.001); // No frame to end
		CHECK(profiler.GetRecordedFrameCount() == 0);

		profiler.BeginFrame();
		CHECK(profiler.IsTimingThisThread());
		// Another thread's stages are not the frame's (e.g. jobs on workers).
		std::atomic<bool> otherThreadTimed = true;
		std::thread other([&]()
		{
			otherThreadTimed = profiler.IsTimingThisThread() || profiler.BeginStage(scripts);
		});
		other.join();
		CHECK_FALSE(otherThreadTimed.load());
		{
			const FrameStageScope scope(profiler, physics);
			manual.AdvanceMs(4.0);
		}
		profiler.EndFrame(0.005);

		// Between frames: not timed.
		CHECK_FALSE(profiler.IsTimingThisThread());
		{
			const FrameStageScope scope(profiler, physics);
			manual.AdvanceMs(100.0);
		}
		profiler.BeginFrame();
		profiler.EndFrame(0.001);

		REQUIRE(profiler.GetRecordedFrameCount() == 2);
		CHECK(StageMs(profiler.GetFrame(1), physics) == doctest::Approx(4.0f));
		CHECK(StageMs(profiler.GetFrame(1), scripts) == 0.0f);
		CHECK(StageMs(profiler.GetFrame(0), physics) == 0.0f);
	}

	TEST_CASE("A frame that is begun but not ended is discarded, and open stages continue into the next frame")
	{
		ManualProfiler manual;
		FrameProfiler& profiler = manual.Profiler;
		const FrameStageID input = Builtin(profiler, "Input");
		const FrameStageID ui = Builtin(profiler, "Editor.UI");

		// A skipped frame (the application returns before ending it, e.g. while minimized).
		profiler.BeginFrame();
		{
			const FrameStageScope scope(profiler, input);
			manual.AdvanceMs(16.0);
		}
		profiler.BeginFrame();
		manual.AdvanceMs(1.0);
		{
			// A stage that spans the end of a frame: split at the frame boundary.
			const FrameStageScope scope(profiler, ui);
			manual.AdvanceMs(2.0);
			profiler.EndFrame(0.003);
			manual.AdvanceMs(5.0); // Between the frames: in no frame
			profiler.BeginFrame();
			manual.AdvanceMs(1.5);
		}
		profiler.EndFrame(0.002);

		REQUIRE(profiler.GetRecordedFrameCount() == 2);
		CHECK(profiler.GetTotalFrameCount() == 2);
		const FrameSample first = profiler.GetFrame(1);
		CHECK(StageMs(first, input) == 0.0f); // The skipped frame's input is gone
		CHECK(StageMs(first, ui) == doctest::Approx(2.0f));
		CHECK(StageMs(profiler.GetFrame(0), ui) == doctest::Approx(1.5f));
	}

	TEST_CASE("The ring keeps the latest frames")
	{
		constexpr uint32_t c_Capacity = 8;
		ManualProfiler manual(c_Capacity);
		FrameProfiler& profiler = manual.Profiler;
		CHECK(profiler.GetCapacity() == c_Capacity);
		for (uint32_t frame = 0; frame < 21; frame++)
		{
			profiler.BeginFrame();
			manual.AdvanceMs(10.0);
			profiler.EndFrame(static_cast<double>(frame) / 1000.0);
		}
		CHECK(profiler.GetRecordedFrameCount() == c_Capacity);
		CHECK(profiler.GetTotalFrameCount() == 21);
		for (uint32_t age = 0; age < c_Capacity; age++)
		{
			CAPTURE(age);
			const FrameSample frame = profiler.GetFrame(age);
			CHECK(frame.Index == 20 - age);
			CHECK(frame.WorkMs == doctest::Approx(static_cast<float>(20 - age)));
			CHECK(frame.IntervalMs == doctest::Approx(10.0f));
		}

		// A summary covers at most the frames in the ring: work times 13 to 20.
		const FrameProfileSummary summary = profiler.Summarize(1000);
		CHECK(summary.Frames == c_Capacity);
		CHECK(summary.Work.Max == doctest::Approx(20.0f));
		CHECK(summary.Work.Average == doctest::Approx(16.5f));
		CHECK(summary.Work.P50 == doctest::Approx(16.0f));
		CHECK(summary.FramesPerSecond == doctest::Approx(100.0f));
		CHECK(profiler.Summarize(3).Work.Average == doctest::Approx(19.0f));

		profiler.Reset();
		CHECK(profiler.GetRecordedFrameCount() == 0);
		CHECK(profiler.GetTotalFrameCount() == 0);
		CHECK(profiler.Summarize(10).Frames == 0);
		profiler.BeginFrame();
		profiler.EndFrame(0.001);
		CHECK(profiler.GetFrame(0).Index == 0);
		CHECK(profiler.GetFrame(0).IntervalMs == 0.0f); // The first frame after a reset has no interval
	}

	TEST_CASE("Percentiles are exact samples by nearest rank")
	{
		// 1 to 100 in a shuffled order.
		std::vector<float> samples(100);
		std::iota(samples.begin(), samples.end(), 1.0f);
		std::mt19937 random(7);
		std::shuffle(samples.begin(), samples.end(), random);
		FrameTimeStats stats = FrameProfiler::ComputeStats(samples);
		CHECK(stats.Average == doctest::Approx(50.5f));
		CHECK(stats.P50 == 50.0f);
		CHECK(stats.P95 == 95.0f);
		CHECK(stats.Max == 100.0f);

		// 20 samples: rank ceil(10) = 10 and ceil(19) = 19.
		std::vector<float> twenty;
		for (int value = 20; value >= 1; value--)
			twenty.push_back(static_cast<float>(value) * 0.5f);
		stats = FrameProfiler::ComputeStats(twenty);
		CHECK(stats.P50 == 5.0f);
		CHECK(stats.P95 == 9.5f);
		CHECK(stats.Max == 10.0f);
		CHECK(stats.Average == doctest::Approx(5.25f));

		// 3 samples: ranks ceil(1.5) = 2 and ceil(2.85) = 3.
		const std::vector<float> three = { 7.0f, 1.0f, 4.0f };
		stats = FrameProfiler::ComputeStats(three);
		CHECK(stats.P50 == 4.0f);
		CHECK(stats.P95 == 7.0f);

		const std::vector<float> one = { 2.5f };
		stats = FrameProfiler::ComputeStats(one);
		CHECK(stats.Average == 2.5f);
		CHECK(stats.P50 == 2.5f);
		CHECK(stats.P95 == 2.5f);
		CHECK(stats.Max == 2.5f);

		stats = FrameProfiler::ComputeStats({});
		CHECK(stats.Average == 0.0f);
		CHECK(stats.Max == 0.0f);

		// Through recorded frames: stage percentiles over synthetic stage times 1..100 ms.
		ManualProfiler manual(200);
		FrameProfiler& profiler = manual.Profiler;
		const FrameStageID physics = Builtin(profiler, "Scene.Physics");
		for (const float milliseconds : samples)
		{
			profiler.BeginFrame();
			{
				const FrameStageScope scope(profiler, physics);
				manual.AdvanceMs(milliseconds);
			}
			profiler.EndFrame(milliseconds / 1000.0 + 0.001);
		}
		const FrameProfileSummary summary = profiler.Summarize(100);
		REQUIRE(summary.Stages.size() == profiler.GetStageCount());
		const FrameStageSummary& stage = summary.Stages[physics];
		CHECK(stage.Name == "Scene.Physics");
		CHECK(stage.Time.P50 == 50.0f);
		CHECK(stage.Time.P95 == 95.0f);
		CHECK(stage.Time.Max == 100.0f);
		CHECK(stage.Time.Average == doctest::Approx(50.5f));
		CHECK(summary.Work.P95 == doctest::Approx(96.0f));
	}

	TEST_CASE("Listeners receive every recorded frame")
	{
		ManualProfiler manual;
		FrameProfiler& profiler = manual.Profiler;
		std::vector<uint64_t> seen;
		std::vector<float> work;
		const uint32_t listener = profiler.AddFrameListener([&](const FrameSample& frame)
		{
			seen.push_back(frame.Index);
			work.push_back(frame.WorkMs);
			CHECK(frame.StageMs.size() == profiler.GetStageCount());
		});
		for (int frame = 0; frame < 3; frame++)
		{
			profiler.BeginFrame();
			profiler.EndFrame(0.002 * (frame + 1));
		}
		profiler.RemoveFrameListener(listener);
		profiler.BeginFrame();
		profiler.EndFrame(0.001);
		CHECK(seen == std::vector<uint64_t> { 0, 1, 2 });
		REQUIRE(work.size() == 3);
		CHECK(work[2] == doctest::Approx(6.0f));
	}

	TEST_CASE("GPU stages stay out of the frame's work, its stage sum and its untimed time")
	{
		ManualProfiler manual;
		FrameProfiler& profiler = manual.Profiler;
		const FrameStageID gpu = profiler.RegisterStage("GPU.Test", true, FrameStageKind::GPU);
		REQUIRE(gpu != c_InvalidFrameStage);
		CHECK(profiler.GetStageKind(gpu) == FrameStageKind::GPU);
		CHECK_FALSE(profiler.IsStageInWorkTime(gpu)); // Never in the work time, whatever was asked
		CHECK(profiler.GetStageKind(Builtin(profiler, "Input")) == FrameStageKind::CPU);
		// Registering the name again returns it.
		CHECK(profiler.RegisterStage("GPU.Test", false, FrameStageKind::GPU) == gpu);
		const FrameStageID input = Builtin(profiler, "Input");

		profiler.BeginFrame();
		{
			// A GPU stage cannot be entered on the CPU: its scope does nothing, and the time counts for the stage around it.
			FrameStageScope inputScope(profiler, input);
			manual.AdvanceMs(1.0);
			FrameStageScope gpuScope(profiler, gpu);
			manual.AdvanceMs(2.0);
		}
		profiler.EndFrame(0.004);
		REQUIRE(profiler.RecordGPUFrame(0, std::vector<GPUStageTime> { { gpu, 5.0f } }));

		const FrameSample frame = profiler.GetFrame(0);
		CHECK(StageMs(frame, input) == doctest::Approx(3.0f));
		CHECK(StageMs(frame, gpu) == 0.0f);
		CHECK(frame.HasGPU);
		CHECK(frame.GPUStageMs[gpu] == doctest::Approx(5.0f));
		CHECK(frame.GPUStageMs[input] == 0.0f);
		CHECK(frame.GPUBusyMs == doctest::Approx(5.0f));

		const FrameProfileSummary summary = profiler.Summarize(1);
		CHECK(summary.Work.P50 == doctest::Approx(4.0f));
		CHECK(summary.StageSum.P50 == doctest::Approx(3.0f));
		CHECK(summary.Untimed.P50 == doctest::Approx(1.0f));
		const FrameStageSummary& gpuSummary = summary.Stages[gpu];
		CHECK(gpuSummary.Kind == FrameStageKind::GPU);
		CHECK_FALSE(gpuSummary.InWorkTime);
		CHECK(gpuSummary.Time.P50 == doctest::Approx(5.0f));
		CHECK(summary.Stages[input].Kind == FrameStageKind::CPU);
	}

	TEST_CASE("Late GPU times attach to their frame while it is in the ring")
	{
		ManualProfiler manual(4);
		FrameProfiler& profiler = manual.Profiler;
		const FrameStageID shadows = profiler.RegisterStage("GPU.Shadows", false, FrameStageKind::GPU);
		const FrameStageID opaque = profiler.RegisterStage("GPU.Opaque", false, FrameStageKind::GPU);
		const FrameStageID input = Builtin(profiler, "Input");
		for (int frame = 0; frame < 3; frame++)
		{
			profiler.BeginFrame();
			profiler.EndFrame(0.001);
		}

		// Frame 1's times arrive two frames late; a stage listed twice adds up; CPU stages and unknown ids are ignored.
		const std::vector<GPUStageTime> times = { { shadows, 1.0f }, { opaque, 2.0f }, { opaque, 0.5f }, { input, 9.0f }, { 63, 9.0f } };
		REQUIRE(profiler.RecordGPUFrame(1, times));
		const FrameSample frame1 = profiler.GetFrame(1);
		CHECK(frame1.Index == 1);
		CHECK(frame1.HasGPU);
		CHECK(frame1.GPUStageMs[shadows] == doctest::Approx(1.0f));
		CHECK(frame1.GPUStageMs[opaque] == doctest::Approx(2.5f));
		CHECK(frame1.GPUStageMs[input] == 0.0f);
		CHECK(frame1.GPUBusyMs == doctest::Approx(3.5f));
		CHECK(frame1.GPULatencyFrames == 2);
		CHECK_FALSE(profiler.GetFrame(0).HasGPU);
		CHECK_FALSE(profiler.GetFrame(2).HasGPU);

		// Frames not recorded yet, or no longer in the ring, are refused.
		CHECK_FALSE(profiler.RecordGPUFrame(3, times));
		for (int frame = 0; frame < 4; frame++)
		{
			profiler.BeginFrame();
			profiler.EndFrame(0.001);
		}
		CHECK_FALSE(profiler.RecordGPUFrame(2, times)); // Overwritten by frame 6
		CHECK(profiler.RecordGPUFrame(3, times));

		// A frame without GPU work is resolved without GPU times.
		CHECK(profiler.RecordGPUFrame(4, {}));
		CHECK_FALSE(profiler.GetFrame(2).HasGPU);

		// A reset forgets the frames: times recorded for the frames before it are refused, as their indices start again.
		const uint32_t resets = profiler.GetResetCount();
		profiler.Reset();
		CHECK(profiler.GetResetCount() == resets + 1);
		CHECK_FALSE(profiler.RecordGPUFrame(0, times));
	}

	TEST_CASE("WithGPU listeners hear of the frames in order, once their GPU times arrived or after eight frames without")
	{
		ManualProfiler manual;
		FrameProfiler& profiler = manual.Profiler;
		const FrameStageID gpu = profiler.RegisterStage("GPU.Test", false, FrameStageKind::GPU);
		std::vector<uint64_t> immediate;
		std::vector<std::pair<uint64_t, float>> withGPU;
		profiler.AddFrameListener([&](const FrameSample& frame) { immediate.push_back(frame.Index); });
		const uint32_t listener = profiler.AddFrameListener([&](const FrameSample& frame) { withGPU.emplace_back(frame.Index, frame.GPUBusyMs); },
			FrameListenerTiming::WithGPU);
		const auto runFrame = [&profiler]()
		{
			profiler.BeginFrame();
			profiler.EndFrame(0.001);
		};

		// Without GPU times expected, WithGPU listeners hear of each frame at once.
		runFrame();
		CHECK(withGPU.size() == 1);

		// Expected: frames wait for their times, in order.
		profiler.SetGPUTimesExpected(true);
		runFrame(); // 1
		runFrame(); // 2
		CHECK(withGPU.size() == 1);
		const std::vector<GPUStageTime> two = { { gpu, 2.0f } };
		REQUIRE(profiler.RecordGPUFrame(2, two)); // Frame 2's times before frame 1's: frame 2 waits for frame 1
		runFrame(); // 3
		CHECK(withGPU.size() == 1);
		const std::vector<GPUStageTime> one = { { gpu, 1.0f } };
		REQUIRE(profiler.RecordGPUFrame(1, one));
		runFrame(); // 4: frames 1 and 2 are heard of, in order
		REQUIRE(withGPU.size() == 3);
		CHECK(withGPU[1] == std::pair<uint64_t, float>(1, 1.0f));
		CHECK(withGPU[2] == std::pair<uint64_t, float>(2, 2.0f));

		// Frame 3 never gets its times: it is heard of once eight later frames were recorded, and the frames after it follow.
		for (uint64_t frame = 5; frame < 3 + FrameProfiler::c_MaxGPULatencyFrames; frame++)
		{
			runFrame();
			CHECK(withGPU.size() == 3);
		}
		runFrame(); // Frame 11: eight frames after frame 3
		REQUIRE(withGPU.size() >= 4);
		CHECK(withGPU[3] == std::pair<uint64_t, float>(3, 0.0f));
		CHECK(withGPU.size() == 4); // Frame 4 is only seven frames old

		// The end of a run: every frame left is heard of.
		profiler.FlushGPUListeners();
		REQUIRE(withGPU.size() == 12);
		for (uint64_t index = 0; index < withGPU.size(); index++)
			CHECK(withGPU[index].first == index);
		CHECK(immediate.size() == 12);

		// A frame about to leave the ring is heard of first.
		profiler.RemoveFrameListener(listener);
		ManualProfiler small(2);
		std::vector<uint64_t> heard;
		small.Profiler.AddFrameListener([&](const FrameSample& frame) { heard.push_back(frame.Index); }, FrameListenerTiming::WithGPU);
		small.Profiler.SetGPUTimesExpected(true);
		for (int frame = 0; frame < 4; frame++)
		{
			small.Profiler.BeginFrame();
			small.Profiler.EndFrame(0.001);
		}
		CHECK(heard == std::vector<uint64_t> { 0, 1 });
	}

	TEST_CASE("What bounds the frames follows the 90% rules")
	{
		// GPU: busy at least 90% of the interval and above the work time.
		CHECK(FrameProfiler::ClassifyFrameBound(9.8f, 11.8f, 12.3f) == FrameBound::GPU);
		CHECK(FrameProfiler::ClassifyFrameBound(5.0f, 9.0f, 10.0f) == FrameBound::GPU);
		// Busy enough, but the CPU's work is longer: CPU.
		CHECK(FrameProfiler::ClassifyFrameBound(9.5f, 9.2f, 10.0f) == FrameBound::CPU);
		// CPU: work at least 90% of the interval.
		CHECK(FrameProfiler::ClassifyFrameBound(9.0f, 2.0f, 10.0f) == FrameBound::CPU);
		// Neither: something else paces the frames (vsync, a cap, throttling).
		CHECK(FrameProfiler::ClassifyFrameBound(3.0f, 4.0f, 16.6f) == FrameBound::Capped);
		CHECK(FrameProfiler::ClassifyFrameBound(8.9f, 8.9f, 10.0f) == FrameBound::Capped);
		CHECK(FrameProfiler::ClassifyFrameBound(3.0f, 4.0f, 0.0f) == FrameBound::Unknown);

		// The summary classifies the medians, Unknown without GPU times.
		ManualProfiler manual;
		FrameProfiler& profiler = manual.Profiler;
		const FrameStageID gpu = profiler.RegisterStage("GPU.Test", false, FrameStageKind::GPU);
		for (int frame = 0; frame < 5; frame++)
		{
			profiler.BeginFrame();
			manual.AdvanceMs(10.0);
			profiler.EndFrame(0.002);
		}
		CHECK(profiler.Summarize(5).GPU.BoundBy == FrameBound::Unknown);
		CHECK(profiler.Summarize(5).GPU.Frames == 0);
		CHECK(profiler.Summarize(5).Interval.P50 == doctest::Approx(10.0f));
		for (uint64_t frame = 0; frame < 5; frame++)
			REQUIRE(profiler.RecordGPUFrame(frame, std::vector<GPUStageTime> { { gpu, 9.5f } }));
		const FrameProfileSummary summary = profiler.Summarize(5);
		CHECK(summary.GPU.Frames == 5);
		CHECK(summary.GPU.Busy.P50 == doctest::Approx(9.5f));
		CHECK(summary.GPU.BoundBy == FrameBound::GPU);
	}

	TEST_CASE("Frames report how busy the job system's threads were")
	{
		ScopedJobSystem jobs(2, 1);
		FrameProfiler profiler(FrameProfilerSpecification { 16, nullptr, true });
		profiler.BeginFrame();
		// Both workers spin for 30 ms; the frame waits for them. It waits only once both started: a job no worker has started
		// yet runs on the waiting thread (Wait helps), which counts for no pool, and under load (parallel test processes)
		// the workers may not have picked them up yet.
		std::atomic<int> started = 0;
		const auto spin = [&started]()
		{
			started.fetch_add(1);
			const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
			while (std::chrono::steady_clock::now() < end)
			{
			}
		};
		const JobHandle first = JobSystem::Submit(spin);
		const JobHandle second = JobSystem::Submit(spin);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (started.load() < 2 && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		first.Wait();
		second.Wait();
		profiler.EndFrame(0.03);

		const FrameSample frame = profiler.GetFrame(0);
		CHECK(started.load() == 2);
		CHECK(frame.WorkerBusyPercent > 10.0f);
		CHECK(frame.WorkerBusyPercent <= 100.0f);
		CHECK(frame.IOBusyPercent == 0.0f);
		const FrameProfileSummary summary = profiler.Summarize(1);
		CHECK(summary.WorkerThreads == 2);
		CHECK(summary.IOThreads == 1);
		CHECK(summary.WorkerBusyPercent == doctest::Approx(frame.WorkerBusyPercent));
	}
}

TEST_SUITE("Core.JobSystem")
{
	TEST_CASE("The job system counts what its threads ran and how long")
	{
		const JobSystemStats before = JobSystem::GetStats();
		CHECK(before.Workers.Threads == 0); // Not initialized

		ScopedJobSystem jobs(2, 1);
		std::atomic<bool> release = false;
		std::atomic<bool> started = false;
		const JobHandle blocking = JobSystem::SubmitIO([&]()
		{
			started = true;
			while (!release.load())
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		});
		while (!started.load())
			std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));

		// A running job counts as running, its time so far as busy.
		JobSystemStats stats = JobSystem::GetStats();
		CHECK(stats.Workers.Threads == 2);
		CHECK(stats.IO.Threads == 1);
		CHECK(stats.IO.RunningJobs == 1);
		CHECK(stats.IO.BusyTime >= std::chrono::milliseconds(20));
		CHECK(stats.IO.CompletedJobs == 0);
		const std::chrono::nanoseconds runningBusy = stats.IO.BusyTime;

		release = true;
		blocking.Wait();
		for (int job = 0; job < 8; job++)
			JobSystem::Submit([]() { std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
		// Completed jobs are counted once their thread finished them.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		do
		{
			stats = JobSystem::GetStats();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while ((stats.Workers.CompletedJobs < 8 || stats.IO.CompletedJobs < 1) && std::chrono::steady_clock::now() < deadline);
		CHECK(stats.Workers.CompletedJobs == 8);
		CHECK(stats.IO.CompletedJobs == 1);
		CHECK(stats.IO.RunningJobs == 0);
		CHECK(stats.IO.BusyTime >= runningBusy);
		CHECK(stats.Workers.BusyTime >= std::chrono::milliseconds(16));
		CHECK(stats.Workers.QueuedJobs == 0);
		// Busy time never runs ahead of the threads' wall time.
		CHECK(stats.Workers.BusyTime <= std::chrono::duration_cast<std::chrono::nanoseconds>(stats.SampleTime - before.SampleTime) * 2);
	}
}
