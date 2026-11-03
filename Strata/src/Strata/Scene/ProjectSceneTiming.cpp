#include "stpch.h"
#include "Strata/Scene/ProjectSceneTiming.h"

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

namespace Strata::ProjectSceneTiming
{

	namespace
	{

		// The rate parsed from the provider's value, kept until the provider's version changes (versions are unique across
		// providers, IProjectSettings::GetVersion).
		struct Cache
		{
			bool Valid = false;
			uint64_t SettingsVersion = 0;
			Timing Parsed;
			uint64_t NextVersion = 1;
		};
		Cache s_Cache;

		// Steps per second of a FixedRate choice's name; 0 for "Scene" and anything else (which the section refuses before a
		// provider holds it).
		uint32_t ParseRate(const std::string& name)
		{
			for (const uint32_t rate : c_Rates)
			{
				if (name == std::to_string(rate))
					return rate;
			}
			return 0;
		}

	}

	SettingsSection MakeSection()
	{
		SettingsField fixedRate;
		fixedRate.Name = c_FixedRateField;
		fixedRate.Title = "Fixed Rate";
		fixedRate.Description = "Fixed steps per second of every scene: physics, character controllers, scripts' OnFixedUpdate and root motion run at "
			"it whatever the frame rate. Scene (the default) keeps each scene's own Fixed Timestep (the Inspector's scene settings, scene.setSettings; "
			"60 unless changed). A rate here overrides it in every scene and in the exported game, and an update then runs at most 2/15 s of steps "
			"(4, 8, 16 or 32), dropping a longer backlog. Physics costs at least grow with the rate: 240 runs four times the steps of 60. 30 makes "
			"contacts soft: box3d stiffens contacts at most to an eighth of its substep rate, 15 Hz at 30 steps/s against 30 Hz at 60, so stacks sag "
			"and settle more slowly. With Interpolation on (the default) motion is smooth at any rate, and a higher rate halves the half step of "
			"latency interpolation adds; with it off, a higher rate halves the step-to-step motion a moving character shows above the step rate, "
			"but does not remove it.";
		fixedRate.Type = SettingsFieldType::Enum;
		fixedRate.Options.push_back(c_SceneRate);
		for (const uint32_t rate : c_Rates)
			fixedRate.Options.push_back(std::to_string(rate));
		fixedRate.Default = c_SceneRate;

		SettingsField interpolation;
		interpolation.Name = c_InterpolationField;
		interpolation.Title = "Interpolation";
		interpolation.Type = SettingsFieldType::Bool;
		interpolation.Default = true;
		interpolation.Description = "Draw what the fixed steps move (physics bodies, character controllers, entities scripts move in OnFixedUpdate, and "
			"what rides on them: children, lights, cameras) between its last two fixed poses, so that motion is smooth at any frame rate; the "
			"simulation itself is unchanged. What is drawn is then a constant fixed step behind the game's clock instead of between none and one "
			"step: half a step more on average (about 8 ms at 60 steps per second; FixedRate 120 halves it), the same at worst. Off draws each "
			"fixed step's pose as it is, which judders whenever the frame rate is above the step rate. Applies to exported games and the editor's "
			"Play and Simulate from the next frame; scripted runs (--frames, --seconds, --commands) and tests never interpolate.";

		SettingsSection section;
		section.Id = c_SectionId;
		section.Title = "Timing";
		// Beside Collision Layers and Simulation, where a person looks for the physics rate.
		section.Category = "Physics";
		section.Description = "How often the simulation steps (Physics > Timing): the project's fixed rate overrides each scene's Fixed Timestep "
			"unless it is Scene; Interpolation draws motion between the steps. Set them with project.setSettings.";
		section.Scope = SettingsScope::Project;
		section.Version = 1;
		section.Fields.push_back(std::move(fixedRate));
		section.Fields.push_back(std::move(interpolation));
		return section;
	}

	const Timing* Find()
	{
		const IProjectSettings* settings = EngineServices::Get<IProjectSettings>();
		if (!settings)
			return nullptr;
		const uint64_t version = settings->GetVersion();
		if (s_Cache.Valid && s_Cache.SettingsVersion == version)
			return &s_Cache.Parsed;

		// The provider holds validated values only (a value its section refuses is not provided: the default applies).
		uint32_t rate = 0;
		bool interpolation = true;
		if (const nlohmann::json* value = settings->FindSection(c_SectionId))
		{
			rate = ParseRate(JsonUtils::GetString(*value, c_FixedRateField, c_SceneRate));
			interpolation = JsonUtils::GetBool(*value, c_InterpolationField, true);
		}
		if (!s_Cache.Valid || rate != s_Cache.Parsed.FixedRate || interpolation != s_Cache.Parsed.Interpolation)
		{
			s_Cache.Parsed.FixedRate = rate;
			s_Cache.Parsed.Interpolation = interpolation;
			s_Cache.Parsed.Version = s_Cache.NextVersion++;
		}
		s_Cache.Valid = true;
		s_Cache.SettingsVersion = version;
		return &s_Cache.Parsed;
	}

	uint32_t GetMaxSteps(uint32_t stepsPerSecond)
	{
		// In 64 bits: no rate overflows the doubling.
		const uint64_t steps = (static_cast<uint64_t>(stepsPerSecond) * 2 + 14) / 15;
		return static_cast<uint32_t>(std::clamp<uint64_t>(steps, 1, c_MaxStepsLimit));
	}

}
