#include "stpch.h"
#include "Strata/Physics/PhysicsSimulationSettings.h"

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/ProjectSettings.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>
#include <utility>

namespace Strata
{

	namespace
	{

		// `dragSpeed`: how much a pixel of dragging changes it on the settings page, a step its meaningful values differ by.
		SettingsField FloatField(const char* name, const char* title, std::string description, double min, double max, double defaultValue,
			double dragSpeed)
		{
			SettingsField field;
			field.Name = name;
			field.Title = title;
			field.Description = std::move(description);
			field.Type = SettingsFieldType::Float;
			field.Min = min;
			field.Max = max;
			field.DragSpeed = dragSpeed;
			// The float constants' nearest decimal (0.08, not 0.0799999982), as people and project files read it.
			field.Default = std::round(defaultValue * 1.0e6) / 1.0e6;
			return field;
		}

		// A validated value's member (the provider holds validated values only), or the fallback.
		float ReadFloat(const nlohmann::json& value, const char* name, float fallback)
		{
			const auto it = value.find(name);
			if (it == value.end() || !it->is_number())
				return fallback;
			const double number = it->get<double>();
			return std::isfinite(number) ? static_cast<float>(number) : fallback;
		}

	}

	namespace PhysicsSimulationSettings
	{

		SettingsSection MakeSection()
		{
			SettingsSection section;
			section.Id = std::string(c_SectionId);
			section.Title = "Simulation";
			section.Category = "Physics";
			section.Description = "How the physics simulation (box3d) settles bodies: how far resting contacts are reused without new collision "
				"work, and how slow a body must be to fall asleep. Applies to the running game at its next fixed step.";
			section.Scope = SettingsScope::Project;
			section.Version = 1;
			section.Fields.push_back(FloatField("ContactRecycleDistance", "Contact recycle distance (m)",
				"A distance in meters, 0 to 1 (0.05 by default: 5 cm, box3d's; not a fraction); 0 turns contact recycling off. While two "
				"touching bodies have moved less than this relative to each other since their contact was computed (and turned less than 10 "
				"degrees), box3d reuses the contact instead of computing it again: less narrow-phase work and steadier resting contacts. A "
				"recycled contact keeps its old points and normal, so a body can seem supported a little past an edge (up to 1.7 cm further "
				"at the default) or keep its old contact when moved or teleported by up to this distance, and it adds a few millimeters to "
				"the bumps at the seams of tiled floors. Lower it when bodies hang past edges. Bumps you can see at tile seams come from the "
				"tiles' edges with or without recycling (44 mm for boxes sliding at 2 m/s): make such a floor one collider or a mesh collider "
				"instead of lowering this, which removes at most the 5 mm recycling adds, while 0 lets stacks topple and costs more. The "
				"distance does not change how soon piles fall asleep (the sleep threshold does). Character controllers and kinematic "
				"bodies never recycle contacts, so contacts with them never do.",
				0.0, 1.0, c_DefaultContactRecycleDistance, 0.001));
			section.Fields.push_back(FloatField("SleepThreshold", "Sleep threshold (m/s)",
				"Speed in m/s, 0 to 1 (0.08 by default; box3d's own is 0.05, at which pits of balls took about a third longer to fall "
				"asleep): a body slower than this for half a second, with every body it touches, falls asleep and costs nothing until "
				"something wakes it. Larger values put piles to sleep sooner but stop slow bodies (a ball rolling to a halt, a slowly "
				"sliding crate) early; 0 keeps every body awake.",
				0.0, 1.0, c_DefaultSleepThreshold, 0.001));
			// The contact tuning: the sweep found each changes how long piles take to settle by more than a tenth (DeveloperGuide
			// "Contact recycling"), so projects may tune it; box3d's own values are the defaults.
			section.Fields.push_back(FloatField("ContactHertz", "Contact stiffness (Hz)",
				"Cycles per second, 1 to 240 (30 by default, box3d's): how stiffly contacts push overlapping bodies apart. box3d uses at "
				"most an eighth of its substep rate, 30 at 60 fixed steps per second (60 at 120), so higher values act as that ceiling "
				"(physics.stats settings.effectiveContactHertz shows what applies). Lower values are softer (stacks sag and towers can "
				"topple: at 20 a tower of 20 crates leaned 88 mm instead of 9).", 1.0, 240.0, c_DefaultContactHertz, 0.1));
			section.Fields.push_back(FloatField("ContactDampingRatio", "Contact damping",
				"0 to 100 (10 by default, box3d's): how much contacts damp the bounce of their push-out. Lower values let piles jiggle "
				"longer (at 5 the 2,000-ball pit did not sleep within 30 s); higher ones make contacts sluggish (at 20 towers lean "
				"more).", 0.0, 100.0, c_DefaultContactDampingRatio, 0.05));
			section.Fields.push_back(FloatField("ContactPushSpeed", "Overlap push speed (m/s)",
				"Meters per second, 0.01 to 100 (3 by default, box3d's): the fastest that overlapping bodies are pushed apart. Lower "
				"values separate spawned or teleported overlaps gently but leave piles settling longer (at 1 the ball pit slept 7 s "
				"later).", 0.01, 100.0, c_DefaultContactPushSpeed, 0.01));
			return section;
		}

		const char* SourceToString(Source source)
		{
			return source == Source::Project ? "project" : "defaults";
		}

		PhysicsSettings Apply(const PhysicsSettings& defaults, Source* outSource)
		{
			PhysicsSettings settings = defaults;
			Source source = Source::Defaults;
			const IProjectSettings* project = EngineServices::Get<IProjectSettings>();
			if (const nlohmann::json* value = project ? project->FindSection(c_SectionId) : nullptr; value && value->is_object())
			{
				settings.ContactRecycleDistance = ReadFloat(*value, "ContactRecycleDistance", defaults.ContactRecycleDistance);
				settings.SleepThreshold = ReadFloat(*value, "SleepThreshold", defaults.SleepThreshold);
				settings.ContactHertz = ReadFloat(*value, "ContactHertz", defaults.ContactHertz);
				settings.ContactDampingRatio = ReadFloat(*value, "ContactDampingRatio", defaults.ContactDampingRatio);
				settings.ContactPushSpeed = ReadFloat(*value, "ContactPushSpeed", defaults.ContactPushSpeed);
				source = Source::Project;
			}
			if (outSource)
				*outSource = source;
			return settings;
		}

		uint64_t GetProviderVersion()
		{
			const IProjectSettings* project = EngineServices::Get<IProjectSettings>();
			return project ? project->GetVersion() : 0;
		}

	}

}
