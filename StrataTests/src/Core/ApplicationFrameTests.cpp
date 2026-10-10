#include <doctest/doctest.h>

#include "Strata/Core/Application.h"
#include "Strata/Core/Layer.h"

#include <chrono>
#include <vector>

using namespace Strata;

namespace
{

	constexpr std::chrono::milliseconds c_FrameWork(5);

	// Works for c_FrameWork in every update (busy, so the time is exact) and records what the application reports about
	// the frame before.
	class BusyLayer : public Layer
	{
	public:
		explicit BusyLayer(std::vector<double>& workTimes)
			: Layer("Busy"), m_WorkTimes(workTimes)
		{
		}

		void OnUpdate(Timestep) override
		{
			Application& application = Application::Get();
			if (application.GetFrameCount() > 0)
				m_WorkTimes.push_back(application.GetLastFrameWorkTime());
			const auto end = std::chrono::steady_clock::now() + c_FrameWork;
			while (std::chrono::steady_clock::now() < end)
			{
			}
		}
	private:
		std::vector<double>& m_WorkTimes;
	};

	class BusyApplication : public Application
	{
	public:
		BusyApplication(const ApplicationSpecification& specification, std::vector<double>& workTimes)
			: Application(specification)
		{
			if (IsRunning())
				PushLayer(new BusyLayer(workTimes));
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
		specification.EnableRenderer = false;
		specification.EnableAudio = false;
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
}
