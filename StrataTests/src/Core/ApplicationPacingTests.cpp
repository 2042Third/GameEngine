#include <doctest/doctest.h>

#include "Strata/Core/Application.h"
#include "Strata/Core/Layer.h"

#include <chrono>
#include <vector>

using namespace Strata;

namespace
{

	// What the layer saw of a frame: when it ran, and what the application reported about the frame before it.
	struct PacedFrame
	{
		std::chrono::steady_clock::time_point Time;
		double PreviousWorkTime = 0.0;
	};

	// Records when each frame ran and changes the application's frame rate as it goes: unlimited, then capped from the
	// third frame, then unlimited again from the eighth.
	class PacingLayer : public Layer
	{
	public:
		explicit PacingLayer(std::vector<PacedFrame>& frames)
			: Layer("Pacing"), m_Frames(frames)
		{
		}

		void OnUpdate(Timestep) override
		{
			Application& application = Application::Get();
			m_Frames.push_back({ std::chrono::steady_clock::now(), application.GetLastFrameWorkTime() });
			if (m_Frames.size() == 3)
				application.SetMaxFrameRate(25);
			else if (m_Frames.size() == 8)
				application.SetMaxFrameRate(0);
		}
	private:
		std::vector<PacedFrame>& m_Frames;
	};

	class PacingApplication : public Application
	{
	public:
		PacingApplication(const ApplicationSpecification& specification, std::vector<PacedFrame>& frames)
			: Application(specification)
		{
			if (IsRunning())
				PushLayer(new PacingLayer(frames));
		}
	};

}

TEST_SUITE("Core.Application")
{
	TEST_CASE("The application's frame rate cap can change while it runs")
	{
		ApplicationSpecification specification;
		specification.Name = "PacingTest";
		specification.Headless = true;
		specification.EnableRenderer = false;
		specification.EnableAudio = false;
		specification.MaxFrames = 30;
		std::vector<PacedFrame> frames;
		{
			PacingApplication application(specification, frames);
			CHECK(application.GetMaxFrameRate() == 0);
			CHECK(application.GetLastFrameWorkTime() == 0.0);
			application.Run();
			CHECK(application.GetMaxFrameRate() == 0);
		}
		REQUIRE(frames.size() == 30);
		// Capped at 25 frames per second after the third frame: frames 4 to 8 are at least 40 ms apart.
		const auto capped = frames[7].Time - frames[2].Time;
		CHECK(capped >= std::chrono::milliseconds(195));
		// Unlimited again after the eighth: the last 21 frames take far less than 21 periods.
		const auto unlimited = frames[29].Time - frames[8].Time;
		CHECK(unlimited < std::chrono::milliseconds(400));

		// The work time of a frame leaves the pacer's wait out: the capped frames 4 to 7 (reported by the frames after
		// them) took a small part of the 160 ms they were spread over.
		double cappedWork = 0.0;
		for (size_t frame = 4; frame <= 7; frame++)
		{
			CHECK(frames[frame].PreviousWorkTime > 0.0);
			cappedWork += frames[frame].PreviousWorkTime;
		}
		CHECK(cappedWork < 0.08);
	}
}
