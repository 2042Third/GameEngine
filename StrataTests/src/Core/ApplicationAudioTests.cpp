#include <doctest/doctest.h>

#include "Strata/Audio/AudioClip.h"
#include "Strata/Audio/AudioEngine.h"
#include "Strata/Audio/AudioSource.h"
#include "Strata/Core/Application.h"
#include "Strata/Core/Layer.h"
#include "TestHelpers.h"

#include <chrono>
#include <thread>

using namespace Strata;

namespace
{

	struct AudioPumpResult
	{
		bool NullDevice = false;
		bool StartedPlaying = false;
		bool Finished = false;
	};

	// Plays a short clip and lets frames of a few milliseconds pass until it has finished (or the frame limit ends the run).
	class AudioPumpLayer : public Layer
	{
	public:
		explicit AudioPumpLayer(AudioPumpResult& result)
			: Layer("AudioPump"), m_Result(result)
		{
		}

		void OnAttach() override
		{
			m_Result.NullDevice = AudioEngine::IsNullDevice();
			m_Source = CreateScope<AudioSource>();
			if (m_Source->SetClip(AudioClip::LoadFromMemory(Tests::CreateSineWav(0.05f), "Pump")))
			{
				m_Source->Play();
				m_Result.StartedPlaying = m_Source->IsPlaying();
			}
		}

		void OnDetach() override
		{
			m_Source.reset();
		}

		void OnUpdate(Timestep) override
		{
			if (m_Result.StartedPlaying && !m_Source->IsPlaying())
			{
				m_Result.Finished = true;
				Application::Get().Close();
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	private:
		AudioPumpResult& m_Result;
		Scope<AudioSource> m_Source;
	};

	class AudioPumpApplication : public Application
	{
	public:
		AudioPumpApplication(const ApplicationSpecification& specification, AudioPumpResult& result)
			: Application(specification)
		{
			if (IsRunning())
				PushLayer(new AudioPumpLayer(result));
		}
	};

}

TEST_SUITE("Core.Application")
{
	TEST_CASE("Headless applications advance the null audio device with the frame time")
	{
		REQUIRE_FALSE(AudioEngine::IsInitialized());
		ApplicationSpecification specification;
		specification.Name = "AudioPumpTest";
		specification.Headless = true;
		specification.EnableRenderer = false;
		specification.Jobs.WorkerThreadCount = 1;
		specification.Jobs.IOThreadCount = 1;
		// At 5 ms or more per frame, 400 frames take at least two seconds: far more than the 50 ms clip needs.
		specification.MaxFrames = 400;

		AudioPumpResult result;
		{
			AudioPumpApplication application(specification, result);
			application.Run();
		}
		CHECK(result.NullDevice);
		CHECK(result.StartedPlaying);
		CHECK(result.Finished);
		CHECK_FALSE(AudioEngine::IsInitialized()); // Shut down with the application
	}
}
