#include "stpch.h"
#include "Strata/Audio/AudioModule.h"

#include "Strata/Audio/AudioEngine.h"

namespace Strata
{

	AudioModule::AudioModule(AudioModuleSpecification specification)
		: m_Specification(specification)
	{
	}

	EngineModuleFactory AudioModule::Factory(AudioModuleSpecification specification)
	{
		return [specification]() -> Scope<EngineModule> { return CreateScope<AudioModule>(specification); };
	}

	bool AudioModule::OnInit(EngineContext&)
	{
		AudioEngineSpecification audioSpecification;
		audioSpecification.NullDevice = m_Specification.NullDevice;
		// A program without sound still runs.
		if (!AudioEngine::Init(audioSpecification))
			ST_CORE_ERROR("Audio is unavailable; continuing without sound");
		return true;
	}

	void AudioModule::OnUpdate(EngineFrame& frame)
	{
		// Without an output device nothing pulls the mix: advance it by the frame time, so that sounds still progress and end.
		if (AudioEngine::IsNullDevice())
			AudioEngine::AdvanceNullDevice(frame.Step);
		AudioEngine::Update();
	}

	void AudioModule::OnShutdown()
	{
		AudioEngine::Shutdown();
	}

}
