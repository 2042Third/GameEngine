#pragma once

#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Physics/PhysicsTypes.h"

#include <cstdint>
#include <string_view>

namespace Strata
{

	class IProjectSettings;

	// The project's physics tuning: the settings section "Physics.Simulation" (DeveloperGuide "Physics simulation"), whose
	// fields are PhysicsSettings' ContactRecycleDistance, SleepThreshold, ContactHertz, ContactDampingRatio and
	// ContactPushSpeed. Main thread only, like IProjectSettings.
	namespace PhysicsSimulationSettings
	{

		constexpr std::string_view c_SectionId = "Physics.Simulation";

		// The section (simple: its fields default to PhysicsSettings' defaults).
		SettingsSection MakeSection();

		// Where the values a world uses come from.
		enum class Source : uint8_t
		{
			Defaults = 0, // The process defaults (PhysicsSystem::GetDefaultSettings): no project, or one without the section
			Project       // The project's section
		};

		const char* SourceToString(Source source);

		// `defaults` with the project's section applied over its tuning fields, when the program provides IProjectSettings and
		// the project stores the section; `outSource` says which. Reads the provider each call: callers keep the result with
		// the provider's version (GetProviderVersion).
		PhysicsSettings Apply(const PhysicsSettings& defaults, Source* outSource = nullptr);
		// The provider's version (IProjectSettings::GetVersion), or 0 without a provider.
		uint64_t GetProviderVersion();

	}

}
