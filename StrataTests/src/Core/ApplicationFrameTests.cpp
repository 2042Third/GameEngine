#include <doctest/doctest.h>

#include "Strata/Core/Application.h"
#include "Strata/Core/FrameProfiler.h"
#include "Strata/Core/Layer.h"
#include "Strata/Core/Profiling.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;

namespace
{

	constexpr std::chrono::milliseconds c_FrameWork(5);
	// The frame profiler stage of the busy layer's work.
	constexpr const char* c_BusyStage = "Test.BusyLayer";

	// Works for c_FrameWork in every update (busy, so the time is exact) and records what the application reports about
	// the frame before. With a display wait, it then sleeps that long in the Present stage, as a frame waits for the
	// display's image where a driver hands it out at its first use (Renderer::PrepareBackBuffer).
	class BusyLayer : public Layer
	{
	public:
		BusyLayer(std::vector<double>& workTimes, std::chrono::milliseconds displayWait)
			: Layer("Busy"), m_WorkTimes(workTimes), m_DisplayWait(displayWait)
		{
		}

		void OnUpdate(Timestep) override
		{
			Application& application = Application::Get();
			if (application.GetFrameCount() > 0)
				m_WorkTimes.push_back(application.GetLastFrameWorkTime());
			ST_PROFILE_STAGE(c_BusyStage);
			const auto end = std::chrono::steady_clock::now() + c_FrameWork;
			while (std::chrono::steady_clock::now() < end)
			{
			}
			if (m_DisplayWait.count() > 0)
			{
				ST_PROFILE_STAGE("Present");
				std::this_thread::sleep_for(m_DisplayWait);
			}
		}
	private:
		std::vector<double>& m_WorkTimes;
		std::chrono::milliseconds m_DisplayWait;
	};

	class BusyApplication : public Application
	{
	public:
		BusyApplication(const ApplicationSpecification& specification, std::vector<double>& workTimes,
			std::chrono::milliseconds displayWait = std::chrono::milliseconds(0))
			: Application(specification)
		{
			if (IsRunning())
				PushLayer(new BusyLayer(workTimes, displayWait));
		}
	};

	// Posts work for the main thread in every update, as a layer does whose answer comes a frame later (a screenshot
	// without a graphics device), and journals when it runs and when the layer goes.
	class PostingLayer : public Layer
	{
	public:
		explicit PostingLayer(std::vector<std::string>& journal)
			: Layer("Posting"), m_Journal(journal)
		{
		}

		void OnUpdate(Timestep) override
		{
			// The journal, not the layer: without the drain after the last frame this would run after the layer is gone.
			Application::Get().SubmitToMainThread([&journal = m_Journal]()
			{
				journal.push_back("Queued");
				Application::Get().SetExitCode(3);
			});
		}

		void OnDetach() override { m_Journal.push_back("Detach"); }
	private:
		std::vector<std::string>& m_Journal;
	};

	class PostingApplication : public Application
	{
	public:
		PostingApplication(const ApplicationSpecification& specification, std::vector<std::string>& journal)
			: Application(specification)
		{
			if (IsRunning())
				PushLayer(new PostingLayer(journal));
		}
	};

}

TEST_SUITE("Core.Application")
{
	TEST_CASE("The frame work time counts the frame's work, not the wait for the next frame")
	{
		ApplicationSpecification specification;
		specification.Name = "FrameWorkTest";
		specification.Headless = true;
		specification.Jobs.WorkerThreadCount = 1;
		specification.Jobs.IOThreadCount = 1;
		// Frames start 100 ms apart, twenty times their work: the pacer's wait must not count.
		specification.MaxFrameRate = 10;
		specification.MaxFrames = 4;

		std::vector<double> workTimes;
		double lastWorkTime = 0.0;
		const auto start = std::chrono::steady_clock::now();
		{
			BusyApplication application(specification, workTimes);
			application.Run();
			lastWorkTime = application.GetLastFrameWorkTime();
		}
		const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		CHECK(elapsed >= 0.29); // Paced: three waits of about 100 ms

		workTimes.push_back(lastWorkTime);
		REQUIRE(workTimes.size() == 4);
		const double minimum = std::chrono::duration<double>(c_FrameWork).count();
		for (const double workTime : workTimes)
		{
			CHECK(workTime >= minimum);
			CHECK(workTime < 0.06);
		}
	}

	TEST_CASE("A wait for the display inside the frame stays out of its work time")
	{
		ApplicationSpecification specification;
		specification.Name = "DisplayWaitTest";
		specification.Headless = true;
		specification.Jobs.WorkerThreadCount = 1;
		specification.Jobs.IOThreadCount = 1;
		specification.MaxFrames = 3;

		// Each update works for c_FrameWork, then waits 40 ms for the display in the Present stage.
		constexpr std::chrono::milliseconds c_DisplayWait(40);
		FrameProfiler& profiler = FrameProfiler::Get();
		std::vector<float> presentMs;
		const uint32_t listener = profiler.AddFrameListener([&](const FrameSample& frame)
		{
			const FrameStageID present = profiler.FindStage("Present");
			presentMs.push_back(present < frame.StageMs.size() ? frame.StageMs[present] : 0.0f);
		});
		std::vector<double> workTimes;
		double lastWorkTime = 0.0;
		{
			BusyApplication application(specification, workTimes, c_DisplayWait);
			application.Run();
			lastWorkTime = application.GetLastFrameWorkTime();
		}
		profiler.RemoveFrameListener(listener);

		workTimes.push_back(lastWorkTime);
		REQUIRE(workTimes.size() == 3);
		REQUIRE(presentMs.size() == 3);
		const double minimum = std::chrono::duration<double>(c_FrameWork).count();
		for (size_t frame = 0; frame < workTimes.size(); frame++)
		{
			CAPTURE(frame);
			CHECK(workTimes[frame] >= minimum);
			CHECK(workTimes[frame] < 0.03);
			CHECK(presentMs[frame] >= static_cast<float>(c_DisplayWait.count()));
		}
	}

	TEST_CASE("The frame profiler records every frame with the application's work time")
	{
		ApplicationSpecification specification;
		specification.Name = "FrameProfileTest";
		specification.Headless = true;
		specification.Jobs.WorkerThreadCount = 1;
		specification.Jobs.IOThreadCount = 1;
		specification.MaxFrameRate = 50;
		specification.MaxFrames = 5;

		FrameProfiler& profiler = FrameProfiler::Get();
		std::vector<float> workMs;
		std::vector<float> busyMs;
		std::vector<float> stageSums;
		std::vector<double> workTimes;
		const uint32_t listener = profiler.AddFrameListener([&](const FrameSample& frame)
		{
			workMs.push_back(frame.WorkMs);
			const FrameStageID busy = profiler.FindStage(c_BusyStage);
			busyMs.push_back(busy < frame.StageMs.size() ? frame.StageMs[busy] : 0.0f);
			double sum = 0.0;
			for (FrameStageID stage = 0; stage < frame.StageMs.size(); stage++)
			{
				if (profiler.IsStageInWorkTime(stage))
					sum += frame.StageMs[stage];
			}
			stageSums.push_back(static_cast<float>(sum));
		});
		double lastWorkTime = 0.0;
		{
			BusyApplication application(specification, workTimes);
			application.Run();
			lastWorkTime = application.GetLastFrameWorkTime();
		}
		profiler.RemoveFrameListener(listener);
		CHECK_FALSE(profiler.IsInFrame());

		// One profile per frame, with the work time the application reported for it (the frame before the next one's
		// update, and the last one after the run).
		workTimes.push_back(lastWorkTime);
		REQUIRE(workMs.size() == 5);
		REQUIRE(workTimes.size() == 5);
		const float minimumMs = static_cast<float>(std::chrono::duration<double, std::milli>(c_FrameWork).count());
		for (size_t frame = 0; frame < workMs.size(); frame++)
		{
			CAPTURE(frame);
			CHECK(workMs[frame] == doctest::Approx(static_cast<float>(workTimes[frame] * 1000.0)));
			// The layer's stage is the frame's work, and the stages never add up to more than the frame.
			CHECK(busyMs[frame] >= minimumMs);
			CHECK(stageSums[frame] <= workMs[frame] + 1.0e-3f);
		}
	}

	TEST_CASE("What the last frame posts for the main thread runs before the layers go and the exit code is read")
	{
		ApplicationSpecification specification;
		specification.Name = "LastFrameQueueTest";
		specification.Headless = true;
		specification.Jobs.WorkerThreadCount = 1;
		specification.Jobs.IOThreadCount = 1;
		specification.MaxFrames = 1;

		std::vector<std::string> journal;
		int exitCode = 0;
		{
			PostingApplication application(specification, journal);
			application.Run();
			// What main reads once Run returns.
			exitCode = application.GetExitCode();
		}
		CHECK(journal == std::vector<std::string> { "Queued", "Detach" });
		CHECK(exitCode == 3);
	}
}
