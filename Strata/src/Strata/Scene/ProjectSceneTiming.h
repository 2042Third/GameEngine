#pragma once

#include <cstdint>

namespace Strata
{

	struct SettingsSection;

	// The project's fixed rate and interpolation: the project settings section Scene.Timing (registered by
	// RegisterSceneModule), a simple section whose FixedRate overrides the fixed step of every scene of the project and its
	// exported game unless it is "Scene", and whose Interpolation (on by default) lets running scenes draw what their fixed
	// steps move between its last two poses (Scene::SetInterpolation). Each scene takes them on the main thread when it is
	// handed over and after every change of the project's settings (Scene::ApplyProjectTiming), as it takes the collision
	// layers. Docs/DeveloperGuide.md, "Fixed rate" and "Interpolation".
	namespace ProjectSceneTiming
	{

		inline constexpr const char* c_SectionId = "Scene.Timing";
		inline constexpr const char* c_FixedRateField = "FixedRate";
		inline constexpr const char* c_InterpolationField = "Interpolation";
		// FixedRate's first choice, the default: each scene's own SceneSettings::FixedTimestep.
		inline constexpr const char* c_SceneRate = "Scene";
		// The other choices, steps per second, stored as their names ("120").
		inline constexpr uint32_t c_Rates[] = { 30, 60, 120, 240 };
		// The most steps an update runs at a project rate is the steps of 2/15 s at that rate (GetMaxSteps), the time
		// SceneSettings::MaxFixedStepsPerFrame's default of 8 covers at 60; never more than this.
		inline constexpr uint32_t c_MaxStepsLimit = 64;

		// The section as RegisterSceneModule registers it: the Enum field FixedRate ("Scene", "30", "60", "120", "240") and the
		// Bool field Interpolation (true).
		SettingsSection MakeSection();

		struct Timing
		{
			uint32_t FixedRate = 0;     // Steps per second; 0: each scene's own step
			bool Interpolation = true;  // Scenes may draw between fixed steps (where their program turns it on)
			uint64_t Version = 0;       // Changes only when FixedRate or Interpolation does; never 0
		};

		// The project's rate and interpolation as the engine service IProjectSettings provides them (the defaults when the
		// project does not set the section), parsed again only when the provider's version changes. Null when nothing provides
		// IProjectSettings (tools, tests, games exported before 0.6.0): scenes keep their own steps. Main thread only; the
		// pointer is valid until the next call.
		const Timing* Find();

		// The most fixed steps one update runs at `stepsPerSecond`: ceil(stepsPerSecond * 2 / 15), from 1 to c_MaxStepsLimit
		// (4 at 30, 8 at 60, 16 at 120, 32 at 240).
		uint32_t GetMaxSteps(uint32_t stepsPerSecond);

	}

}
