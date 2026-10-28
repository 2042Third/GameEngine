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

		SettingsField FloatField(const char* name, const char* title, std::string description, double max, double defaultValue)
		{
			SettingsField field;
			field.Name = name;
			field.Title = title;
			field.Description = std::move(description);
			field.Type = SettingsFieldType::Float;
			field.Min = 0.0;
			field.Max = max;
			field.Default = defaultValue;
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
			section.Fields.push_back(FloatField("ContactRecycleDistance", "Contact recycle distance",
				"A distance in meters (5 cm by default, box3d's; not a fraction), 0 to 1; 0 turns contact recycling off. While two touching "
				"bodies have moved less than this relative to each other since their contact was computed (and turned less than 10 degrees), "
				"box3d reuses the contact instead of computing it again: less narrow-phase work and steadier resting contacts. Larger values "
				"recycle more, but a recycled contact keeps its old points and normal, so a body can seem supported a little past an edge, "
				"bump at seams between tiles, or keep its old contact when moved or teleported by up to this distance. Lower it when bodies "
				"bump over seams or hang past edges. Character controllers never recycle contacts.",
				1.0, c_DefaultContactRecycleDistance));
			section.Fields.push_back(FloatField("SleepThreshold", "Sleep threshold",
				"Speed in m/s, 0 to 1 (0.05 by default, box3d's): a body slower than this for half a second, with every body it touches, falls "
				"asleep and costs nothing until something wakes it. Larger values put piles to sleep sooner but stop slow bodies (a ball "
				"rolling to a halt, a slowly sliding crate) early; 0 keeps every body awake.",
				1.0, c_DefaultSleepThreshold));
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
