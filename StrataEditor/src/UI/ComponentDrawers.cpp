#include "UI/ComponentDrawers.h"

#include "UI/EditorActions.h"
#include "UI/PropertyWidgets.h"

#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Scene/Scene.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace Strata::UI
{

	namespace
	{

		std::unordered_map<entt::id_type, ComponentDrawer> MakeBuiltinDrawers()
		{
			std::unordered_map<entt::id_type, ComponentDrawer> drawers;
			drawers[entt::type_id<UIRectComponent>().hash()] = &DrawUIRect;
			return drawers;
		}

		std::unordered_map<entt::id_type, ComponentDrawer>& GetDrawers()
		{
			static std::unordered_map<entt::id_type, ComponentDrawer> s_Drawers = MakeBuiltinDrawers();
			return s_Drawers;
		}

		// By component type, then property name (looked up by string_view: the Inspector asks for every row it draws).
		using PropertyDrawerMap = std::unordered_map<entt::id_type, std::map<std::string, PropertyDrawer, std::less<>>>;

		PropertyDrawerMap MakeBuiltinPropertyDrawers()
		{
			PropertyDrawerMap drawers;
			drawers[entt::type_id<PostProcessComponent>().hash()]["TAAPreset"] = &DrawTemporalAntiAliasingPreset;
			return drawers;
		}

		PropertyDrawerMap& GetPropertyDrawers()
		{
			static PropertyDrawerMap s_Drawers = MakeBuiltinPropertyDrawers();
			return s_Drawers;
		}

		// The TAA's tunables, as the registry names them (PostProcessComponent), with the condition under which a start writes
		// them: ClipWidth only for tight clipping and the threshold only with rejection, as SetTemporalAntiAliasingSettings does.
		struct TemporalTunable
		{
			const char* Property;
			bool (*Written)(const TemporalAntiAliasingSettings& settings);
		};

		constexpr TemporalTunable c_TemporalTunables[] = {
			{ "TAAHistoryFrames", nullptr },
			{ "TAAMotionResponse", nullptr },
			{ "TAAMaxMotionWeight", nullptr },
			{ "TAAResampleWeight", nullptr },
			{ "TAAHistoryClamp", nullptr },
			{ "TAAClipWidth", [](const TemporalAntiAliasingSettings& settings) { return settings.HistoryClamp == TemporalHistoryClamp::Variance; } },
			{ "TAATransparentWeight", nullptr },
			{ "TAADisocclusionRejection", nullptr },
			{ "TAADisocclusionThreshold", [](const TemporalAntiAliasingSettings& settings) { return settings.DisocclusionRejection; } },
			{ "TAASharpening", nullptr },
		};

		// The values component.set writes for the choice of `preset`: the preset, and for Custom the start every selected entity
		// shares (none when they differ).
		nlohmann::json GetTemporalAntiAliasingPresetValues(std::span<const Entity> entities, const ComponentInfo& info, const PropertyInfo& presetProperty,
			TemporalAntiAliasingPreset preset)
		{
			nlohmann::json values = nlohmann::json::object();
			values[presetProperty.Name] = PropertyValueToJson(presetProperty, PropertyValue(static_cast<int32_t>(preset)));
			if (preset != TemporalAntiAliasingPreset::Custom)
				return values;
			std::optional<TemporalAntiAliasingSettings> start;
			for (const Entity entity : entities)
			{
				const TemporalAntiAliasingSettings entityStart = GetTemporalAntiAliasingCustomStart(entity.GetComponent<PostProcessComponent>());
				if (start && *start != entityStart)
					return values;
				start = entityStart;
			}
			if (!start)
				return values;
			PostProcessComponent copy = entities.front().GetComponent<PostProcessComponent>();
			SetTemporalAntiAliasingSettings(copy, *start);
			for (const TemporalTunable& tunable : c_TemporalTunables)
			{
				const PropertyInfo* property = info.FindProperty(tunable.Property);
				if (property && (!tunable.Written || tunable.Written(*start)))
					values[property->Name] = PropertyValueToJson(*property, property->GetValue(&copy));
			}
			return values;
		}

		nlohmann::json EntitiesToJson(std::span<const Entity> entities)
		{
			nlohmann::json ids = nlohmann::json::array();
			for (const Entity entity : entities)
				ids.push_back(UUIDToJson(entity.GetUUID()));
			return ids;
		}

	}

	void PropertyDrawers::Register(entt::id_type type, std::string_view property, PropertyDrawer drawer)
	{
		GetPropertyDrawers()[type][std::string(property)] = std::move(drawer);
	}

	const PropertyDrawer* PropertyDrawers::Find(entt::id_type type, std::string_view property)
	{
		const PropertyDrawerMap& drawers = GetPropertyDrawers();
		const auto component = drawers.find(type);
		if (component == drawers.end())
			return nullptr;
		const auto it = component->second.find(property);
		return it == component->second.end() ? nullptr : &it->second;
	}

	void DrawTemporalAntiAliasingPreset(EditorContext& context, const PropertyDrawContext& row)
	{
		if (row.Entities.empty())
			return;
		PropertyWidgetOptions options;
		options.Mixed = row.Mixed;
		options.ProbeKey = row.ProbeKey;
		PropertyValue value = row.Value;
		ImGui::BeginDisabled(row.Disabled);
		const PropertyEditResult result = DrawPropertyWidget(row.Property, value, *row.Entities.front().GetScene(), options);
		ImGui::EndDisabled();
		if (!result.Changed || !std::holds_alternative<int32_t>(value))
			return;
		const auto preset = static_cast<TemporalAntiAliasingPreset>(std::get<int32_t>(value));
		RunEditorCommand(context, row.Commands, "component.set", { { "entities", EntitiesToJson(row.Entities) }, { "component", row.Info.Name },
			{ "values", GetTemporalAntiAliasingPresetValues(row.Entities, row.Info, row.Property, preset) } });
	}

	void ComponentDrawResult::Replace(std::string_view property)
	{
		if (!Replaces(property) && ReplacedCount < c_MaxReplaced)
			Replaced[ReplacedCount++] = property;
	}

	bool ComponentDrawResult::Replaces(std::string_view property) const
	{
		for (uint32_t index = 0; index < ReplacedCount; index++)
		{
			if (Replaced[index] == property)
				return true;
		}
		return false;
	}

	void ComponentDrawers::Register(entt::id_type type, ComponentDrawer drawer)
	{
		GetDrawers()[type] = std::move(drawer);
	}

	bool ComponentDrawers::Draw(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info, const ComponentDrawOverrides* overrides,
		ComponentDrawResult& result)
	{
		const auto it = GetDrawers().find(info.TypeId);
		if (it == GetDrawers().end() || entities.empty())
			return false;
		it->second(context, entities, info, overrides, result);
		return true;
	}

}
