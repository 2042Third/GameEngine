#include <doctest/doctest.h>

#include "Strata/Core/Application.h"
#include "Strata/Core/Layer.h"

#include <chrono>
#include <vector>

using namespace Strata;

namespace
{

	// Records when each frame ran and changes the application's frame rate as it goes: unlimited, then capped from the
	// third frame, then unlimited again from the eighth.
	class PacingLayer : public Layer
	{
	public:
		explicit PacingLayer(std::vector<std::chrono::steady_clock::time_point>& frames)
			: Layer("Pacing"), m_Frames(frames)
		{
		}

		void OnUpdate(Timestep) override
		{
			m_Frames.push_back(std::chrono::steady_clock::now());
			Application& application = Application::Get();
			if (m_Frames.size() == 3)
				application.SetMaxFrameRate(25);
			else if (m_Frames.size() == 8)
				application.SetMaxFrameRate(0);
		}
	private:
		std::vector<std::chrono::steady_clock::time_point>& m_Frames;
	};

	class PacingApplication : public Application
	{
	public:
		PacingApplication(const ApplicationSpecification& specification, std::vector<std::chrono::steady_clock::time_point>& frames)
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
		std::vector<std::chrono::steady_clock::time_point> frames;
		{
			PacingApplication application(specification, frames);
			CHECK(application.GetMaxFrameRate() == 0);
			application.Run();
			CHECK(application.GetMaxFrameRate() == 0);
		}
		REQUIRE(frames.size() == 30);
		// Capped at 25 frames per second after the third frame: frames 4 to 8 are at least 40 ms apart.
		const auto capped = frames[7] - frames[2];
		CHECK(capped >= std::chrono::milliseconds(195));
		// Unlimited again after the eighth: the last 21 frames take far less than 21 periods.
		const auto unlimited = frames[29] - frames[8];
		CHECK(unlimited < std::chrono::milliseconds(400));
	}
}
