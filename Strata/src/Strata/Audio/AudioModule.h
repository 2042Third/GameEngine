#pragma once

#include "Strata/Core/EngineModule.h"

#include <string_view>

namespace Strata
{

	struct AudioModuleSpecification
	{
		// Mix without an output device, advanced by the frame time so that playback still progresses (headless programs,
		// servers, tests).
		bool NullDevice = false;
	};

	// The engine module of the AudioEngine: initializes it at startup (a program whose machine has no output device, or
	// whose mixer cannot be made, runs without sound), pulls the null device's mix by the frame time and reclaims finished
	// one-shots every frame, and shuts it down once the job system stopped.
	class AudioModule final : public EngineModule
	{
	public:
		static constexpr std::string_view c_Name = "Audio";

		explicit AudioModule(AudioModuleSpecification specification = {});

		static EngineModuleFactory Factory(AudioModuleSpecification specification = {});

		std::string_view GetName() const override { return c_Name; }
		[[nodiscard]] bool OnInit(EngineContext& context) override;
		void OnUpdate(EngineFrame& frame) override;
		void OnShutdown() override;
	private:
		AudioModuleSpecification m_Specification;
	};

}
