#include "stpch.h"
#include "Strata/Scene/ProjectCollisionLayers.h"

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Strata::ProjectCollisionLayers
{

	namespace
	{

		// The table parsed from the provider's value, kept until the provider's version changes (versions are unique across
		// providers, IProjectSettings::GetVersion).
		struct Cache
		{
			bool Valid = false;
			uint64_t SettingsVersion = 0;
			Table Parsed;
			uint64_t NextTableVersion = 1;
		};
		Cache s_Cache;

		nlohmann::json MakeSchema()
		{
			return {
				{ "type", "object" },
				{ "properties", {
					{ "Names", {
						{ "type", "object" },
						{ "description", "Names of layers by index (\"0\" to \"31\", no leading zeros), e.g. {\"1\": \"Player\"}: unique ignoring case, at most "
							"64 characters, no control characters, no leading or trailing space, and not another layer's \"Layer <index>\"" },
						{ "propertyNames", { { "pattern", "^([0-9]|[1-2][0-9]|3[0-1])$" } } },
						{ "additionalProperties", { { "type", "string" }, { "maxLength", PhysicsLayerSettings::c_MaxNameLength } } } } },
					{ "NoCollisions", {
						{ "type", "array" },
						{ "description", "The pairs of layers that do not collide, as two layer indices; every other pair collides" },
						{ "items", {
							{ "type", "array" },
							{ "minItems", 2 },
							{ "maxItems", 2 },
							{ "items", { { "type", "integer" }, { "minimum", 0 }, { "maximum", c_PhysicsLayerCount - 1 } } } } } } } } },
				{ "additionalProperties", false } };
		}

	}

	SettingsSection MakeSection()
	{
		SettingsSection section;
		section.Id = c_SectionId;
		section.Title = "Collision Layers";
		section.Category = "Physics";
		section.Description = "The project's collision layers: names for the 32 layers rigid bodies and character controllers are on (their Layer "
			"property and scripts' Physics::GetLayerIndex accept them) and the pairs of layers that do not collide. Every scene and prefab uses this "
			"table; set it with project.setCollisionLayers.";
		section.Schema = MakeSchema();
		section.MakeDefault = []() { return PhysicsLayerSettings().ToJson(); };
		section.Read = [](const nlohmann::json& value, uint32_t, std::string* outError) -> std::optional<nlohmann::json>
		{
			auto fail = [outError](std::string message) -> std::optional<nlohmann::json>
			{
				if (outError)
					*outError = std::move(message);
				return std::nullopt;
			};
			if (value.is_object())
			{
				for (const auto& [key, member] : value.items())
				{
					if (key != "Names" && key != "NoCollisions")
						return fail(fmt::format("'{}' is not a member of the collision layers (they have Names and NoCollisions)", key));
				}
			}
			std::vector<std::string> problems;
			const PhysicsLayerSettings layers = PhysicsLayerSettings::FromJson(value, &problems);
			if (!problems.empty())
				return fail(fmt::format("Invalid collision layers: {}", problems.front()));
			return layers.ToJson();
		};
		return section;
	}

	const Table* Find()
	{
		const IProjectSettings* settings = EngineServices::Get<IProjectSettings>();
		if (!settings)
			return nullptr;
		const uint64_t version = settings->GetVersion();
		if (s_Cache.Valid && s_Cache.SettingsVersion == version)
			return &s_Cache.Parsed;

		// The provider holds validated values only (a value its section refuses is not provided: the default applies).
		PhysicsLayerSettings layers;
		if (const nlohmann::json* value = settings->FindSection(c_SectionId))
			layers = PhysicsLayerSettings::FromJson(*value);
		if (!s_Cache.Valid || layers != s_Cache.Parsed.Layers)
		{
			s_Cache.Parsed.Layers = layers;
			s_Cache.Parsed.Version = s_Cache.NextTableVersion++;
		}
		s_Cache.Valid = true;
		s_Cache.SettingsVersion = version;
		return &s_Cache.Parsed;
	}

}
