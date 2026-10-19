#include "Panels/InspectorPanel.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/PropertyEdit.h"
#include "Editor/SceneEdit.h"
#include "Editor/ScriptEdit.h"
#include "UI/ComponentDrawers.h"
#include "UI/EditorActions.h"
#include "UI/EditorFonts.h"
#include "UI/EditorShell.h"
#include "UI/EntityIcons.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/PropertyWidgets.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Animation/AnimationComponents.h>
#include <Strata/Animation/AnimationSystem.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Scene/ComponentAccess.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/PrefabInstances.h>
#include <Strata/Scene/PrefabOverrides.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scripting/ScriptSystem.h>
#include <Strata/Scripting/ScriptingComponents.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Strata
{

	namespace
	{

		constexpr float c_AddComponentWidthInFontSizes = 14.0f;
		constexpr float c_AddComponentListHeightInFontSizes = 20.0f;
		// The widest a row label of the collision layer matrix gets, in text heights (longer names are elided).
		constexpr float c_LayerLabelMaxWidthInFontSizes = 10.0f;

		const ImVec4& GetPlayStateColor(const EditorContext& context)
		{
			const UI::ThemeColors::PlayStateColors& colors = UI::GetThemeColors().PlayState;
			if (context.IsPaused())
				return colors.Paused;
			return context.GetSceneState() == SceneState::Simulate ? colors.Simulate : colors.Play;
		}

		nlohmann::json ToJson(std::span<const Entity> entities)
		{
			nlohmann::json ids = nlohmann::json::array();
			for (const Entity entity : entities)
				ids.push_back(UUIDToJson(entity.GetUUID()));
			return ids;
		}

		bool HasComponentOnAll(std::span<const Entity> entities, const ComponentInfo& info)
		{
			for (const Entity entity : entities)
			{
				if (!info.Has(entity.GetScene()->GetRegistry(), entity.GetHandle()))
					return false;
			}
			return true;
		}

		// The value a field shows: the live instance's while the scene plays, else the stored override or the default.
		PropertyValue GetShownFieldValue(Entity entity, const ScriptEntry& entry, const ScriptFieldInfo& field)
		{
			Scene* scene = entity.GetScene();
			if (const ScriptSystem* system = scene && scene->IsRunning() ? scene->GetSystem<ScriptSystem>() : nullptr)
			{
				if (std::optional<PropertyValue> live = system->GetFieldValue(entity, entry.ClassName, field.Name))
					return *live;
			}
			const ScriptFieldValue* stored = entry.FindField(field.Name);
			if (stored && stored->Value.index() == GetPropertyValueIndex(field.Type))
				return stored->Value;
			return field.DefaultValue;
		}

		// A script field of a prefab instance's entity that overrides its prefab ("Script", "<Class>.<Field>"): marked like
		// other overridden properties, Apply to Prefab and Revert on a right-click of its label.
		struct ScriptFieldOverrides
		{
			std::function<bool(const std::string& property)> IsOverridden;
			std::function<void(const std::string& property, const char* command)> Run; // prefab.apply or prefab.revert
			bool Model = false; // Imported models offer Revert only
		};

		void DrawScriptFields(EditorContext& context, Entity entity, const ScriptEntry& entry, const ScriptClassInfo& info,
			const ScriptFieldOverrides* overrides)
		{
			if (info.Fields.empty())
			{
				ImGui::TextDisabled("No fields");
				return;
			}
			// Laid out like the property tables (UI::BeginPropertyTable), with a column for the reset buttons.
			if (!ImGui::BeginTable("Fields", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV
				| ImGuiTableFlags_NoBordersInBodyUntilResize))
				return;
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
			ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
			for (const ScriptFieldInfo& field : info.Fields)
			{
				ImGui::PushID(field.Name.c_str());
				const bool overridden = entry.FindField(field.Name) != nullptr;
				PropertyInfo property;
				property.Name = field.Name;
				property.DisplayName = Utils::PascalCaseToDisplayName(field.Name);
				property.Type = field.Type;
				PropertyValue value = GetShownFieldValue(entity, entry, field);

				const std::string tooltip = fmt::format("{} ({}), default {}{}", field.Name, PropertyTypeToString(field.Type),
					JsonUtils::Dump(ScriptEdit::FieldValueToJson(field.DefaultValue, field.Type)), overridden ? "; overridden on this entity" : "");
				const std::string prefabProperty = entry.ClassName + "." + field.Name;
				const std::string probe = fmt::format("Inspector.Script.{}", prefabProperty);
				UI::PropertyLabelOptions labelOptions;
				labelOptions.Overridden = overrides && overrides->IsOverridden(prefabProperty);
				UI::PropertyLabel(property.DisplayName, tooltip, probe, labelOptions);
				if (labelOptions.Overridden && ImGui::BeginPopupContextItem("FieldOverride"))
				{
					if (!overrides->Model && ImGui::MenuItem("Apply to Prefab"))
						overrides->Run(prefabProperty, "prefab.apply");
					UI::ItemProbe::Record(probe + ".Apply", !overrides->Model);
					if (ImGui::MenuItem("Revert"))
						overrides->Run(prefabProperty, "prefab.revert");
					UI::ItemProbe::Record(probe + ".Revert");
					ImGui::EndPopup();
				}
				const PropertyEditResult result = DrawPropertyWidget(property, value, *entity.GetScene());
				std::string error;
				if (result.Changed && !ScriptEdit::SetField(context, entity, entry.ClassName, field, value, &error))
					ST_WARN("{}.{}: {}", entry.ClassName, field.Name, error);
				if (result.Finished)
					context.GetUndoStack().BreakMerge();

				ImGui::TableSetColumnIndex(2);
				if (UI::IconButton("ResetField", Icons::RotateCcw, overridden ? "Reset to the class default" : "Uses the class default", overridden))
				{
					context.GetUndoStack().BreakMerge();
					if (!ScriptEdit::SetField(context, entity, entry.ClassName, field, std::nullopt, &error))
						ST_WARN("{}.{}: {}", entry.ClassName, field.Name, error);
					context.GetUndoStack().BreakMerge();
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		// Without the class: the stored overrides as text.
		void DrawStoredOverrides(const ScriptEntry& entry)
		{
			if (entry.Fields.empty())
				ImGui::TextDisabled("No field overrides");
			for (const ScriptFieldValue& field : entry.Fields)
				ImGui::TextDisabled("%s = %s", field.Name.c_str(), JsonUtils::Dump(ScriptEdit::FieldValueToJson(field.Value, field.Type)).c_str());
		}

		// A file name made of a name an entity has: letters, digits, spaces, '-' and '_' only.
		std::string ToFileName(const std::string& name)
		{
			std::string result;
			for (const char character : name)
			{
				const unsigned char byte = static_cast<unsigned char>(character);
				result += std::isalnum(byte) || character == ' ' || character == '-' || character == '_' ? character : '_';
			}
			const size_t first = result.find_first_not_of(" _");
			return first == std::string::npos ? std::string("Material") : result.substr(first);
		}

		// An editable setting of the scene, as a property the property widgets draw, with its probe key
		// ("Inspector.Scene.<Name>").
		struct SceneSettingRow
		{
			PropertyInfo Property;
			std::string ProbeKey;
		};

		struct SceneSettingProperties
		{
			SceneSettingRow Gravity;
			SceneSettingRow FixedTimestep;
			SceneSettingRow MaxFixedSteps;
			// The physics world's capacities (SceneSettings::PhysicsCapacity): 0 sizes them automatically.
			SceneSettingRow MaxBodies;
			SceneSettingRow MaxBodyPairs;
			SceneSettingRow MaxContactConstraints;
			// Sub-scenes (SceneSettings::MaxEntitiesInstantiatedPerFrame).
			SceneSettingRow MaxEntitiesInstantiated;
		};

		SceneSettingRow MakeSceneSettingRow(PropertyInfo property)
		{
			std::string probeKey = "Inspector.Scene." + property.Name;
			return SceneSettingRow { std::move(property), std::move(probeKey) };
		}

		const char* GetPhysicsHeader()
		{
			static const std::string s_Header = std::string(Icons::Orbit) + "  Physics";
			return s_Header.c_str();
		}

		const char* GetStreamingHeader()
		{
			// Not the Layers icon of the Collision Layers section above it.
			static const std::string s_Header = std::string(Icons::Download) + "  Streaming";
			return s_Header.c_str();
		}

		// The streaming section's summary ("5000 entities per frame").
		std::string DescribeStreaming(const SceneSettings& settings)
		{
			return fmt::format("{} entities per frame", settings.MaxEntitiesInstantiatedPerFrame);
		}

		const char* GetLayersHeader()
		{
			static const std::string s_Header = std::string(Icons::Layers) + "  Collision Layers";
			return s_Header.c_str();
		}

		// The collision layers section's summary ("2 named · 1 pair passes through") and the layers its matrix shows.
		std::string DescribeLayers(const PhysicsLayerSettings& layers, std::vector<uint32_t>& outShown)
		{
			outShown.clear();
			size_t named = 0;
			size_t apart = 0;
			for (uint32_t layer = 0; layer < c_PhysicsLayerCount; layer++)
			{
				named += layers.Names[layer].empty() ? 0 : 1;
				for (uint32_t other = layer; other < c_PhysicsLayerCount; other++)
					apart += layers.DoCollide(layer, other) ? 0 : 1;
				if (layer == 0 || !layers.Names[layer].empty() || !layers.CollidesWithAll(layer))
					outShown.push_back(layer);
			}
			const std::string names = named == 0 ? std::string("No names") : fmt::format("{} named", named);
			if (apart == 0)
				return names + " \xC2\xB7 all collide";
			return fmt::format("{} \xC2\xB7 {} {} through", names, apart, apart == 1 ? "pair passes" : "pairs pass");
		}

		// A name for a newly named layer that no layer has: "New Layer", else "New Layer 2" and so on.
		std::string MakeNewLayerName(const PhysicsLayerSettings& layers, uint32_t layer)
		{
			for (uint32_t number = 1; number <= c_PhysicsLayerCount + 1; number++)
			{
				std::string name = number == 1 ? std::string("New Layer") : fmt::format("New Layer {}", number);
				if (!layers.ValidateName(layer, name))
					return name;
			}
			return {};
		}

		const char* GetSkyHeader()
		{
			static const std::string s_Header = std::string(Icons::CloudSun) + "  Sky";
			return s_Header.c_str();
		}

		// The physics section's summary: the gravity's strength and the fixed steps per second ("Gravity 9.81 m/s² · 60 steps/s").
		std::string DescribePhysics(const SceneSettings& settings)
		{
			const float gravity = glm::length(settings.Gravity);
			const std::string strength = gravity > 0.0f ? fmt::format("Gravity {:.3g} m/s\xC2\xB2", gravity) : std::string("No gravity");
			const float stepsPerSecond = settings.FixedTimestep > 0.0f ? 1.0f / settings.FixedTimestep : 0.0f;
			return fmt::format("{} \xC2\xB7 {:.4g} steps/s", strength, stepsPerSecond);
		}

		// The sky section's summary: where the sky's light comes from.
		const char* DescribeSky(Entity sky)
		{
			if (!sky)
				return "None";
			const SkyLightComponent& light = sky.GetComponent<SkyLightComponent>();
			if (light.Source == SkyLightSource::Procedural)
				return "Procedural";
			// Without a map, the sky light adds its ambient color.
			return light.EnvironmentMap.IsValid() ? "Environment map" : "Ambient color";
		}

		// What a capacity of 0 (sized automatically when play starts) reads as.
		constexpr std::string_view c_AutomaticCapacity = "Auto";

		PropertyInfo MakeCapacityProperty(const char* name, const char* displayName, const char* tooltip, uint32_t limit)
		{
			PropertyInfo property;
			property.Name = name;
			property.DisplayName = displayName;
			property.Tooltip = tooltip;
			property.Type = PropertyType::UInt;
			property.Min = 0.0f;
			property.Max = static_cast<float>(limit); // Powers of two: exact as floats
			property.Speed = 8.0f;
			return property;
		}

		const SceneSettingProperties& GetSceneSettingProperties()
		{
			static const SceneSettingProperties s_Properties = []()
			{
				PropertyInfo gravity;
				gravity.Name = "Gravity";
				gravity.DisplayName = "Gravity";
				gravity.Tooltip = "Acceleration of rigid bodies in m/s\xC2\xB2";
				gravity.Type = PropertyType::Vec3;
				gravity.Speed = 0.05f;
				PropertyInfo fixedTimestep;
				fixedTimestep.Name = "FixedTimestep";
				fixedTimestep.DisplayName = "Fixed Timestep";
				fixedTimestep.Tooltip = "Seconds per physics step (1/1000 to 1/10)";
				fixedTimestep.Type = PropertyType::Float;
				fixedTimestep.Min = 0.001f;
				fixedTimestep.Max = 0.1f;
				fixedTimestep.Speed = 0.0005f;
				PropertyInfo maxFixedSteps;
				maxFixedSteps.Name = "MaxFixedStepsPerFrame";
				maxFixedSteps.DisplayName = "Max Steps per Frame";
				maxFixedSteps.Tooltip = "Physics steps one frame may run at most (a slow frame drops the rest)";
				maxFixedSteps.Type = PropertyType::UInt;
				maxFixedSteps.Min = 1.0f;
				maxFixedSteps.Max = 64.0f;

				SceneSettingProperties properties;
				properties.Gravity = MakeSceneSettingRow(std::move(gravity));
				properties.FixedTimestep = MakeSceneSettingRow(std::move(fixedTimestep));
				properties.MaxFixedSteps = MakeSceneSettingRow(std::move(maxFixedSteps));
				properties.MaxBodies = MakeSceneSettingRow(MakeCapacityProperty("MaxBodies", "Max Bodies",
					"Bodies the physics world holds, reserved when play starts (about 80 bytes each); Auto (0) sizes it from the bodies the "
					"scene starts with. Raise it for scenes that create many bodies while they play",
					PhysicsCapacitySettings::c_MaxBodiesLimit));
				properties.MaxBodyPairs = MakeSceneSettingRow(MakeCapacityProperty("MaxBodyPairs", "Max Body Pairs",
					"Pairs of bodies with overlapping bounds per step (about 90 bytes each); Auto (0): sized from the bodies",
					PhysicsCapacitySettings::c_MaxBodyPairsLimit));
				properties.MaxContactConstraints = MakeSceneSettingRow(MakeCapacityProperty("MaxContactConstraints", "Max Contacts",
					"Contacts solved per step (about 830 bytes each); Auto (0): sized from the bodies", PhysicsCapacitySettings::c_MaxContactConstraintsLimit));
				PropertyInfo maxEntitiesInstantiated;
				maxEntitiesInstantiated.Name = "MaxEntitiesInstantiatedPerFrame";
				maxEntitiesInstantiated.DisplayName = "Entities per Frame";
				maxEntitiesInstantiated.Tooltip = "Entities sub-scenes loading into the scene while it plays may create per frame, all together; a frame also "
					"stops creating after about 2 ms. Lower it if streamed parts of a level make frames long";
				maxEntitiesInstantiated.Type = PropertyType::UInt;
				maxEntitiesInstantiated.Min = 1.0f;
				maxEntitiesInstantiated.Max = static_cast<float>(SceneSettings::c_MaxEntitiesInstantiatedLimit); // A power of two: exact as a float
				maxEntitiesInstantiated.Speed = 50.0f;
				properties.MaxEntitiesInstantiated = MakeSceneSettingRow(std::move(maxEntitiesInstantiated));
				return properties;
			}();
			return s_Properties;
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// Panel
	////////////////////////////////////////////////////////////////////////////////

	void InspectorPanel::OnImGuiRender(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		if (context.IsPlaying())
		{
			const bool paused = context.IsPaused();
			UI::Banner("Inspector.PlayBanner", paused ? Icons::Pause : Icons::Play,
				paused ? "Paused: changes are discarded when play stops" : "Playing: changes are discarded when play stops", GetPlayStateColor(context));
			ImGui::Spacing();
		}
		// While a scene is being opened, the scene shown cannot change: its values show, disabled.
		const bool readOnly = DrawSceneLoadBanner("Inspector.SceneLoadBanner", context);
		ImGui::BeginDisabled(readOnly);
		DrawSelection(panelContext);
		ImGui::EndDisabled();
	}

	void InspectorPanel::DrawSelection(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		const EditorCommandRegistry& commands = panelContext.Commands;
		// The selection in selection order (the last is the primary one).
		std::vector<Entity> entities;
		entities.reserve(context.GetSelection().size());
		for (const UUID id : context.GetSelection())
		{
			if (Entity entity = context.GetActiveScene()->GetEntityByUUID(id))
				entities.push_back(entity);
		}
		if (entities.empty())
		{
			m_MaterialEditor.CommitPending(context);
			DrawSceneSettings(context, commands);
			return;
		}

		DrawHeader(context, commands, entities);
		const std::optional<PrefabView> prefab = FindPrefabView(entities);
		if (prefab)
			DrawPrefabBar(panelContext, *prefab);
		for (const ComponentInfo* info : ComponentRegistry::GetAll())
		{
			// The name is the header's, the prefab link the prefab bar's; hidden and internal components are not shown.
			if (info->IsHidden() || HasFlag(info->Flags, ComponentFlags::NoSerialize) || info->TypeId == entt::type_id<NameComponent>().hash()
				|| info->TypeId == entt::type_id<PrefabInstanceComponent>().hash())
				continue;
			if (!HasComponentOnAll(entities, *info))
				continue;
			DrawComponent(panelContext, entities, *info, prefab ? &*prefab : nullptr);
		}
		if (entities.size() > 1)
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, UI::GetThemeColors().TextSecondary);
			ImGui::TextWrapped("Components that only some of the selected entities have are not shown.");
			ImGui::PopStyleColor();
		}

		ImGui::Spacing();
		const float buttonWidth = ImGui::GetFontSize() * c_AddComponentWidthInFontSizes;
		ImGui::SetCursorPosX(std::max((ImGui::GetContentRegionAvail().x - buttonWidth) * 0.5f, 0.0f) + ImGui::GetCursorPosX());
		if (UI::ActionButton("Inspector.AddComponent", Icons::Plus, "Add Component", "Add a component to the selected entities", {}, buttonWidth))
			ImGui::OpenPopup("AddComponent");
		DrawAddComponentPopup(context, commands, entities);
		if (std::optional<PendingCommand> pending = std::exchange(m_PendingPrefabCommand, std::nullopt))
			RunEditorCommand(context, commands, pending->Name, pending->Parameters);
	}

	void InspectorPanel::OnHidden(EditorPanelContext& context)
	{
		m_MaterialEditor.CommitPending(context.Context);
		m_SettingsDraft.reset();
		m_RotationEdit.reset();
	}

	const std::vector<std::optional<PropertyValue>>& InspectorPanel::GetDefaults(const ComponentInfo& info)
	{
		auto it = m_Defaults.find(&info);
		if (it == m_Defaults.end())
			it = m_Defaults.emplace(&info, GetDefaultPropertyValues(info)).first;
		return it->second;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Header
	////////////////////////////////////////////////////////////////////////////////

	void InspectorPanel::DrawHeader(EditorContext& context, const EditorCommandRegistry& commands, std::span<const Entity> entities)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImGuiStyle& style = ImGui::GetStyle();
		const Entity primary = entities.back();
		Scene& scene = *primary.GetScene();
		const bool single = entities.size() == 1;

		// The icon, the name (or the count) and the active toggle on one line.
		UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Title);
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(colors.Accent, "%s", single ? UI::GetEntityIcon(scene.GetRegistry(), primary.GetHandle()) : Icons::Layers);
		ImGui::PopFont();
		ImGui::SameLine();
		const float toggleWidth = ImGui::GetFrameHeight();
		const float nameWidth = std::max(ImGui::GetContentRegionAvail().x - toggleWidth - style.ItemSpacing.x, ImGui::GetFrameHeight());
		if (single)
		{
			// The text typed stays while the field is active; otherwise it shows the entity's name.
			const ImGuiID nameId = ImGui::GetID("##Name");
			if (ImGui::GetActiveID() != nameId || m_NameEntity != primary.GetUUID())
			{
				m_NameEntity = primary.GetUUID();
				m_NameText = primary.GetName();
			}
			ImGui::SetNextItemWidth(nameWidth);
			UI::PushFont(UI::EditorFont::SemiBold, UI::TextSize::Body);
			const bool entered = ImGui::InputTextWithHint("##Name", "Name", &m_NameText, ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::PopFont();
			UI::ItemProbe::Record("Inspector.Name");
			if ((entered || ImGui::IsItemDeactivatedAfterEdit()) && m_NameText != primary.GetName())
				RunEditorCommand(context, commands, "entity.rename", { { "entity", UUIDToJson(primary.GetUUID()) }, { "name", m_NameText } });
		}
		else
		{
			ImGui::AlignTextToFramePadding();
			UI::PushFont(UI::EditorFont::SemiBold, UI::TextSize::Body);
			ImGui::Text("%zu entities", entities.size());
			ImGui::PopFont();
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::Dummy(ImVec2(std::max(nameWidth - ImGui::GetItemRectSize().x, 0.0f), ImGui::GetFrameHeight()));
		}

		ImGui::SameLine();
		bool active = primary.IsActive();
		bool mixed = false;
		for (const Entity entity : entities)
			mixed |= entity.IsActive() != active;
		ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, mixed);
		if (ImGui::Checkbox("##Active", &active))
			RunEditorCommand(context, commands, "entity.setActive", { { "entities", ToJson(entities) }, { "active", mixed ? true : active } });
		ImGui::PopItemFlag();
		UI::ItemProbe::Record("Inspector.Active");
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
		{
			ImGui::SetTooltip("%s", mixed ? "Some of the entities are inactive: click to activate them all"
				: "Active: inactive entities and their children are neither simulated nor drawn");
		}

		// The ID (in the form commands take) with a copy button, and the prefab the entity comes from.
		if (single)
		{
			const std::string id = UUIDToJson(primary.GetUUID()).get<std::string>();
			UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(colors.TextSecondary, "ID %s", id.c_str());
			ImGui::PopFont();
			ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
			if (UI::IconButton("Inspector.CopyID", Icons::Copy, "Copy the ID (as commands and scripts take it)"))
				ImGui::SetClipboardText(id.c_str());
		}
		ImGui::Spacing();
	}

	bool InspectorPanel::PrefabView::IsOverridden(std::string_view component, std::string_view property) const
	{
		return PrefabOverrides::IsOverridden(Overrides, PrefabEntity, component, property);
	}

	std::optional<InspectorPanel::PrefabView> InspectorPanel::FindPrefabView(std::span<const Entity> entities) const
	{
		if (entities.size() != 1)
			return std::nullopt;
		const Entity entity = entities.front();
		const PrefabInstanceComponent* link = entity.TryGetComponent<PrefabInstanceComponent>();
		const Entity root = PrefabInstances::FindInstanceRoot(entity);
		if (!link || !root)
			return std::nullopt;
		PrefabView view;
		view.Root = root.GetUUID();
		view.SceneEntity = entity.GetUUID();
		view.PrefabEntity = link->PrefabEntityID;
		view.Prefab = link->Prefab;
		view.Overrides = root.GetComponent<PrefabInstanceComponent>().Overrides;
		view.Model = AssetManager::HasActive() && AssetManager::GetActive()->GetAssetType(link->Prefab) == AssetType::Model;
		return view;
	}

	void InspectorPanel::SchedulePrefabCommand(std::string name, nlohmann::json parameters)
	{
		m_PendingPrefabCommand = PendingCommand { std::move(name), std::move(parameters) };
	}

	const std::vector<InspectorPanel::OverrideRow>& InspectorPanel::GetOverrideRows(EditorContext& context, const PrefabView& view)
	{
		const Scene* scene = context.GetActiveScene().get();
		const uint64_t undoVersion = context.GetUndoStack().GetVersion();
		OverrideRows& cache = m_OverrideRows;
		if (cache.ForScene == scene && cache.Root == view.Root && cache.UndoVersion == undoVersion && scene
			&& cache.HierarchyVersion == scene->GetHierarchyVersion() && cache.Overrides == view.Overrides)
			return cache.Rows;
		cache = OverrideRows { scene, view.Root, undoVersion, scene ? scene->GetHierarchyVersion() : 0, view.Overrides, {} };
		m_OverrideRowsBuilds++;
		// The instance's linked entities by their prefab entity ids, in one walk.
		std::unordered_map<UUID, Entity> linked;
		const Entity root = scene ? scene->GetEntityByUUID(view.Root) : Entity();
		std::vector<Entity> stack;
		if (root)
			stack.push_back(root);
		while (!stack.empty())
		{
			const Entity current = stack.back();
			stack.pop_back();
			const PrefabInstanceComponent* link = current.TryGetComponent<PrefabInstanceComponent>();
			if (link && link->Prefab == view.Prefab)
				linked.try_emplace(link->PrefabEntityID, current);
			const std::vector<Entity> children = current.GetChildren();
			stack.insert(stack.end(), children.rbegin(), children.rend());
		}
		for (const PrefabOverride& entry : view.Overrides)
		{
			const auto found = linked.find(entry.Entity);
			const Entity target = found != linked.end() ? found->second : Entity();
			const std::string what = entry.Kind == PrefabOverrideKind::RemovedEntity ? "Removed entity"
				: entry.Kind == PrefabOverrideKind::AddedComponent ? fmt::format("Added {}{}{}", entry.Component, entry.Property.empty() ? "" : " ", entry.Property)
				: entry.Kind == PrefabOverrideKind::RemovedComponent ? fmt::format("Removed {}{}{}", entry.Component, entry.Property.empty() ? "" : " ", entry.Property)
				: fmt::format("{}.{}", entry.Component, entry.Property);
			OverrideRow row;
			row.Target = target ? target.GetUUID() : UUID::Null();
			row.Label = fmt::format("{} \xC2\xB7 {}", target ? target.GetName() : std::string("(removed)"), what);
			row.Parameters = { { "entity", UUIDToJson(row.Target) }, { "component", entry.Component },
				{ "property", entry.Kind == PrefabOverrideKind::Property || PrefabOverrides::c_ScriptComponent == entry.Component ? entry.Property : std::string() } };
			cache.Rows.push_back(std::move(row));
		}
		return cache.Rows;
	}

	void InspectorPanel::DrawPrefabBar(EditorPanelContext& panelContext, const PrefabView& view)
	{
		EditorContext& context = panelContext.Context;
		const EditorCommandRegistry& commands = panelContext.Commands;
		const ImGuiStyle& style = ImGui::GetStyle();
		std::string name = view.Model ? "Model" : "Prefab";
		if (AssetManager::HasActive())
		{
			if (const std::optional<AssetMetadata> metadata = AssetManager::GetActive()->GetMetadata(view.Prefab))
				name = metadata->Name.empty() ? metadata->Path : metadata->Name;
		}
		const nlohmann::json entity = UUIDToJson(view.SceneEntity);
		const nlohmann::json root = UUIDToJson(view.Root);

		ImGui::AlignTextToFramePadding();
		if (UI::LinkButton("Inspector.Prefab.Asset", name, view.Model ? Icons::Shapes : Icons::Package,
			view.Model ? "Show the model this instance comes from" : "Show the prefab this instance comes from") && panelContext.Shell)
			panelContext.Shell->ShowAsset(view.Prefab);
		ImGui::SameLine();
		if (UI::Chip("Inspector.Prefab.Select", Icons::MousePointer2, "Select", "Select the instance's root"))
			RunEditorCommand(context, commands, "prefab.select", { { "entity", entity } });
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		if (UI::Chip("Inspector.Prefab.Open", Icons::FolderOpen, "Open", "Show the prefab in the Asset tab and the Content Browser") && panelContext.Shell)
			panelContext.Shell->ShowAsset(view.Prefab);
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		const size_t count = view.Overrides.size();
		const std::string overrides = fmt::format("Overrides ({})", count);
		if (UI::Chip("Inspector.Prefab.Overrides", Icons::Layers, overrides.c_str(), "What this instance changed of its prefab: apply it to the prefab or revert it"))
			ImGui::OpenPopup("PrefabOverrides");

		if (ImGui::BeginPopup("PrefabOverrides"))
		{
			if (!view.Model && ImGui::MenuItem("Apply All to Prefab", nullptr, false, count > 0))
				SchedulePrefabCommand("prefab.apply", { { "entity", root }, { "force", true } });
			UI::ItemProbe::Record("Inspector.Prefab.ApplyAll", count > 0);
			if (ImGui::MenuItem("Revert All", nullptr, false, count > 0))
				SchedulePrefabCommand("prefab.revert", { { "entity", root } });
			UI::ItemProbe::Record("Inspector.Prefab.RevertAll", count > 0);
			if (count > 0)
				ImGui::Separator();
			// Each override, under its entity and component: Apply and Revert (an entity the instance removed: Revert All).
			const std::vector<OverrideRow>& rows = GetOverrideRows(context, view);
			for (size_t index = 0; index < rows.size(); index++)
			{
				const OverrideRow& row = rows[index];
				const std::string label = fmt::format("{}###Override{}", row.Label, index);
				const bool open = ImGui::BeginMenu(label.c_str(), row.Target.IsValid());
				UI::ItemProbe::Record(fmt::format("Inspector.Prefab.Override.{}", index), row.Target.IsValid(), 0, row.Label);
				if (!open)
					continue;
				if (!view.Model && ImGui::MenuItem("Apply to Prefab"))
					SchedulePrefabCommand("prefab.apply", row.Parameters);
				if (ImGui::MenuItem("Revert"))
					SchedulePrefabCommand("prefab.revert", row.Parameters);
				ImGui::EndMenu();
			}
			ImGui::EndPopup();
		}
		ImGui::Spacing();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Components
	////////////////////////////////////////////////////////////////////////////////

	void InspectorPanel::DrawComponent(EditorPanelContext& panelContext, std::span<const Entity> entities, const ComponentInfo& info, const PrefabView* prefab)
	{
		EditorContext& context = panelContext.Context;
		const EditorCommandRegistry& commands = panelContext.Commands;
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::PushID(info.Name.c_str());
		const float headerRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		const std::string label = fmt::format("{}  {}", UI::GetComponentIcon(info), info.DisplayName);
		// The menu (Reset, Copy and Paste Values for components with settable properties, Remove for those people may remove:
		// not those the engine adds, such as prefab links), and Remove as a button. A component with neither, such as a prefab
		// link, has no menu.
		const bool removable = info.IsRemovable() && !HasFlag(info.Flags, ComponentFlags::EngineAdded);
		const bool settable = HasSettableProperties(info);
		// Overridden in its prefab instance: marked, with Apply and Revert in its menu.
		const bool overridden = prefab && prefab->IsOverridden(info.Name);
		const bool hasMenu = settable || removable || overridden;
		const bool open = UI::SectionHeader("Component", label.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
		if (overridden)
		{
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			const float bar = std::max(1.0f, std::floor(ImGui::GetFontSize() * 0.15f));
			ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + bar, max.y), ImGui::GetColorU32(UI::GetThemeColors().Accent));
			UI::ItemProbe::RecordRect(fmt::format("Inspector.{}.Overridden", info.Name), min, max, true, ImGui::GetColorU32(UI::GetThemeColors().Accent));
		}
		if (hasMenu && ImGui::IsItemClicked(ImGuiMouseButton_Right))
			ImGui::OpenPopup("ComponentMenu");
		if (!info.Description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
			ImGui::SetTooltip("%s", info.Description.c_str());

		const float buttons = ImGui::GetFrameHeight() * ((hasMenu ? 1.0f : 0.0f) + (removable ? 1.0f : 0.0f)) + (hasMenu && removable ? style.ItemInnerSpacing.x : 0.0f);
		if (hasMenu)
		{
			ImGui::SameLine(headerRight - buttons);
			const std::string menuId = fmt::format("Inspector.{}.Menu", info.Name);
			if (UI::IconButton(menuId.c_str(), Icons::EllipsisVertical, settable ? "Reset, copy, paste or remove" : "Remove"))
				ImGui::OpenPopup("ComponentMenu");
		}
		bool remove = false;
		if (removable)
		{
			ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
			const std::string removeId = fmt::format("Inspector.{}.Remove", info.Name);
			const std::string tooltip = "Remove " + info.DisplayName;
			remove = UI::IconButton(removeId.c_str(), Icons::X, tooltip.c_str());
		}
		DrawComponentMenu(context, commands, entities, info, prefab);

		if (remove)
			RunEditorCommand(context, commands, "component.remove", { { "entities", ToJson(entities) }, { "component", info.Name } });
		// The menu may have removed it too.
		if (open && HasComponentOnAll(entities, info))
		{
			DrawMissingRequirements(context, commands, entities, info);
			if (info.TypeId == entt::type_id<ScriptComponent>().hash())
			{
				if (entities.size() == 1)
					DrawScripts(context, commands, entities.front(), prefab);
				else
					ImGui::TextDisabled("Select one entity to edit its scripts");
			}
			else if (info.Properties.empty())
			{
				ImGui::TextDisabled(info.SerializeExtra ? "Edited in its own section" : "No properties");
			}
			else
			{
				// What the generic rows cannot show (a UIRect's anchor presets and margins), above them; they leave out what
				// the drawer showed in its own way.
				UI::ComponentDrawResult drawn;
				std::optional<UI::ComponentDrawOverrides> overrides;
				if (prefab)
				{
					overrides.emplace();
					overrides->IsOverridden = [prefab, &info](std::string_view property) { return prefab->IsOverridden(info.Name, property); };
					overrides->DrawMenu = [this, prefab, &info](std::span<const std::string_view> properties, std::string_view probeKey)
						{ DrawPropertyOverrideMenu(*prefab, info, properties, probeKey); };
				}
				UI::ComponentDrawers::Draw(context, entities, info, overrides ? &*overrides : nullptr, drawn);
				DrawProperties(context, entities, info, prefab, &drawn);
			}
			if (info.TypeId == entt::type_id<MeshRendererComponent>().hash())
				DrawMaterialSection(panelContext, entities);
			if (info.TypeId == entt::type_id<AnimatorComponent>().hash() && entities.size() == 1)
				DrawAnimatorState(context, entities.front());
			ImGui::Spacing();
		}
		ImGui::PopID();
	}

	void InspectorPanel::DrawMissingRequirements(EditorContext& context, const EditorCommandRegistry& commands, std::span<const Entity> entities,
		const ComponentInfo& info)
	{
		// Removing a component another one requires is allowed (ComponentInfo::Requires), which leaves that one without effect:
		// a UI Image without its UI Rect draws nothing. The section says so, with a button that adds it to those without it.
		for (const std::string& requiredName : info.Requires)
		{
			const ComponentInfo* required = ComponentRegistry::Find(requiredName);
			if (!required)
				continue;
			std::vector<Entity> missing;
			for (const Entity entity : entities)
			{
				if (!required->Has(entity.GetScene()->GetRegistry(), entity.GetHandle()))
					missing.push_back(entity);
			}
			if (missing.empty())
				continue;
			const std::string probe = fmt::format("Inspector.{}.Needs{}", info.Name, required->Name);
			const std::string text = entities.size() == 1
				? fmt::format("Needs a {}: without it this component has no effect.", required->DisplayName)
				: fmt::format("{} of the selected entities have no {}: without it this component has no effect there.", missing.size(), required->DisplayName);
			UI::Banner(probe.c_str(), Icons::TriangleAlert, text, UI::GetThemeColors().Warning);
			const std::string label = fmt::format("{}  Add {}", Icons::Plus, required->DisplayName);
			const bool add = ImGui::Button(label.c_str());
			UI::ItemProbe::Record(probe + ".Add", true, 0, label);
			if (add)
				RunEditorCommand(context, commands, "component.add", { { "entities", ToJson(missing) }, { "component", required->Name } });
		}
	}

	void InspectorPanel::DrawComponentMenu(EditorContext& context, const EditorCommandRegistry& commands, std::span<const Entity> entities, const ComponentInfo& info,
		const PrefabView* prefab)
	{
		if (!ImGui::BeginPopup("ComponentMenu"))
			return;
		const nlohmann::json ids = ToJson(entities);
		const std::string probe = fmt::format("Inspector.{}.Menu.", info.Name);
		if (prefab && prefab->IsOverridden(info.Name))
		{
			const nlohmann::json parameters = { { "entity", UUIDToJson(prefab->SceneEntity) }, { "component", info.Name } };
			if (!prefab->Model && ImGui::MenuItem("Apply to Prefab"))
				SchedulePrefabCommand("prefab.apply", parameters);
			UI::ItemProbe::Record(probe + "Apply", !prefab->Model);
			if (ImGui::MenuItem("Revert to Prefab"))
				SchedulePrefabCommand("prefab.revert", parameters);
			UI::ItemProbe::Record(probe + "Revert");
			ImGui::Separator();
		}
		// Reset, Copy and Paste work on the properties component.set can set; a component without any has none of them.
		const bool settable = HasSettableProperties(info);
		if (settable)
		{
			if (ImGui::MenuItem("Reset"))
				RunEditorCommand(context, commands, "component.set", { { "entities", ids }, { "component", info.Name }, { "values", GetDefaultComponentValues(info) } });
			UI::ItemProbe::Record(probe + "Reset");
			if (ImGui::MenuItem("Copy Values"))
			{
				// The primary entity's.
				const nlohmann::json values = RunEditorCommand(context, commands, "component.get",
					{ { "entity", UUIDToJson(entities.back().GetUUID()) }, { "component", info.Name } });
				if (values.is_object() && values.contains("values"))
					EditorActions::CopyComponentValues(info, values["values"]);
			}
			UI::ItemProbe::Record(probe + "Copy");
			const std::optional<nlohmann::json> clipboard = EditorActions::ReadComponentValues(info);
			const std::string pasteLabel = entities.size() == 1 ? std::string("Paste Values") : fmt::format("Paste Values to {} Entities", entities.size());
			if (ImGui::MenuItem(pasteLabel.c_str(), nullptr, false, clipboard.has_value()))
				RunEditorCommand(context, commands, "component.set", { { "entities", ids }, { "component", info.Name }, { "values", *clipboard } });
			UI::ItemProbe::Record(probe + "Paste", clipboard.has_value());
		}
		if (info.IsRemovable() && !HasFlag(info.Flags, ComponentFlags::EngineAdded))
		{
			if (settable)
				ImGui::Separator();
			if (ImGui::MenuItem("Remove"))
				RunEditorCommand(context, commands, "component.remove", { { "entities", ids }, { "component", info.Name } });
			UI::ItemProbe::Record(probe + "Remove");
		}
		ImGui::EndPopup();
	}

	void InspectorPanel::DrawPropertyOverrideMenu(const PrefabView& prefab, const ComponentInfo& info, std::span<const std::string_view> properties,
		std::string_view probeKey)
	{
		ImGui::PushID(probeKey.data(), probeKey.data() + probeKey.size());
		if (ImGui::BeginPopupContextItem("PropertyOverride"))
		{
			const bool several = properties.size() > 1;
			for (const std::string_view property : properties)
			{
				const nlohmann::json parameters = { { "entity", UUIDToJson(prefab.SceneEntity) }, { "component", info.Name }, { "property", property } };
				// With several properties each item names its own.
				const PropertyInfo* found = info.FindProperty(property);
				const std::string_view name = found ? std::string_view(found->DisplayName) : property;
				const std::string apply = several ? fmt::format("Apply {} to Prefab", name) : std::string("Apply to Prefab");
				const std::string revert = several ? fmt::format("Revert {}", name) : std::string("Revert");
				const std::string suffix = several ? fmt::format(".{}", property) : std::string();
				if (!prefab.Model && ImGui::MenuItem(apply.c_str()))
					SchedulePrefabCommand("prefab.apply", parameters);
				UI::ItemProbe::Record(fmt::format("{}.Apply{}", probeKey, suffix), !prefab.Model);
				if (ImGui::MenuItem(revert.c_str()))
					SchedulePrefabCommand("prefab.revert", parameters);
				UI::ItemProbe::Record(fmt::format("{}.Revert{}", probeKey, suffix));
			}
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}

	void InspectorPanel::DrawProperties(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info, const PrefabView* prefab,
		const UI::ComponentDrawResult* drawn)
	{
		if (!UI::BeginPropertyTable("Properties"))
			return;
		const std::vector<std::optional<PropertyValue>>& defaults = GetDefaults(info);
		Scene& scene = *entities.front().GetScene();
		for (size_t index = 0; index < info.Properties.size(); index++)
		{
			const PropertyInfo& property = info.Properties[index];
			if (property.IsHidden() || property.IsTransient() || (drawn && drawn->Replaces(property.Name)))
				continue;
			const std::optional<SharedPropertyValue> shared = GetSharedProperty(entities, info, property);
			if (!shared)
				continue;

			const std::string probe = fmt::format("Inspector.{}.{}", info.Name, property.Name);
			const std::string inactive = GetSharedInactiveReason(entities, info, property);
			UI::PropertyLabelOptions labelOptions;
			labelOptions.Overridden = prefab && prefab->IsOverridden(info.Name, property.Name);
			labelOptions.InactiveReason = inactive;
			UI::PropertyLabel(property.DisplayName, property.Tooltip, probe, labelOptions);
			if (labelOptions.Overridden)
			{
				const std::string_view name = property.Name;
				DrawPropertyOverrideMenu(*prefab, info, std::span(&name, 1), probe);
			}
			PropertyWidgetOptions options;
			options.Mixed = shared->Mixed;
			options.MixedComponents = shared->MixedComponents;
			options.DefaultValue = index < defaults.size() && defaults[index] ? &*defaults[index] : nullptr;
			options.ProbeKey = probe;
			PropertyValue value = shared->Value;
			// An inactive property shows its value disabled: changing it would change nothing (its label says why).
			ImGui::BeginDisabled(property.IsReadOnly() || !inactive.empty());
			const PropertyEditResult result = DrawPropertyWidget(property, value, scene, options);
			ImGui::EndDisabled();
			if (result.Changed)
			{
				// Several entities: the components (axes) the edit changed are written, each entity keeps its own others.
				std::string error;
				const bool partial = std::any_of(result.ChangedComponents.begin(), result.ChangedComponents.end(), [](bool changed) { return changed; });
				bool set = false;
				if (entities.size() == 1)
					set = SetPropertyWithUndo(context, entities.front(), info, property, value, &error);
				else if (partial && property.Type == PropertyType::Quat)
					set = UpdateRotationEdit(context, entities, info, property, result, &error);
				else if (partial)
					set = SetPropertyComponentsWithUndo(context, entities, info, property, value, result.ChangedComponents, &error);
				else
					set = SetPropertyWithUndo(context, entities, info, property, shared->Value, value, &error);
				if (!set)
					ST_WARN("{}.{}: {}", info.Name, property.Name, error);
			}
			if (result.Finished)
			{
				context.GetUndoStack().BreakMerge();
				// The next edit reads the entities' angles again, as the fields show them then.
				m_RotationEdit.reset();
			}
		}
		UI::EndPropertyTable();
	}

	bool InspectorPanel::UpdateRotationEdit(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& info, const PropertyInfo& property,
		const PropertyEditResult& result, std::string* outError)
	{
		if (!m_RotationEdit || !m_RotationEdit->Continues(entities, info, property))
			m_RotationEdit = RotationAnglesEdit::Begin(entities, info, property, result.EulerAnglesBefore, outError);
		return m_RotationEdit && m_RotationEdit->Update(context, entities, info, property, result.EulerAngles, result.ChangedComponents, outError);
	}

	void InspectorPanel::DrawMaterialSection(EditorPanelContext& panelContext, std::span<const Entity> entities)
	{
		EditorContext& context = panelContext.Context;
		const EditorCommandRegistry& commands = panelContext.Commands;
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ComponentInfo* info = ComponentRegistry::Find<MeshRendererComponent>();
		const PropertyInfo* property = info ? info->FindProperty("Material") : nullptr;
		const std::optional<SharedPropertyValue> shared = property ? GetSharedProperty(entities, *info, *property) : std::nullopt;
		if (!shared)
			return;
		const AssetHandle material = std::get<UUID>(shared->Value);

		// A new material of the project for the selected entities: the file (material.create), then the component (one undo
		// step of the scene).
		const bool canCreate = context.HasProject() && !context.IsPlaying();
		UI::ButtonStyle style;
		style.Enabled = canCreate;
		ImGui::Indent(ImGui::GetStyle().IndentSpacing * 0.5f);
		if (UI::ToolbarButton("Inspector.MeshRenderer.NewMaterial", Icons::Plus, "New Material",
			canCreate ? "Create a material of the project (Materials/) and give it to the selected entities"
					  : (context.IsPlaying() ? "Stop playing to create materials" : "Open a project to create materials"), style))
		{
			const std::filesystem::path assets = context.GetProject()->GetAssetDirectory();
			const std::string name = ToFileName(entities.back().GetName());
			const std::filesystem::path file = FileSystem::GetUniquePath(assets / "Materials" / FileSystem::FromUTF8(name + ".stmat"));
			std::string relative = FileSystem::ToUTF8(FileSystem::GetRelativePath(file, assets));
			std::replace(relative.begin(), relative.end(), '\\', '/');
			const nlohmann::json created = RunEditorCommand(context, commands, "material.create", { { "path", relative } });
			if (created.is_object() && created.contains("asset"))
			{
				RunEditorCommand(context, commands, "component.set", { { "entities", ToJson(entities) }, { "component", info->Name },
					{ "values", { { "Material", created["asset"] } } } });
			}
		}

		if (shared->Mixed)
		{
			ImGui::TextColored(colors.TextSecondary, "The selected entities use different materials");
		}
		else if (material.IsValid())
		{
			std::string name = material.ToString();
			if (AssetManager::HasActive())
			{
				if (const std::optional<AssetMetadata> metadata = AssetManager::GetActive()->GetMetadata(material))
					name = metadata->Name.empty() ? metadata->Path : metadata->Name;
			}
			const bool open = ImGui::TreeNodeEx("EditMaterial", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding, "%s  Edit material (shared)",
				Icons::Palette);
			UI::ItemProbe::Record("Inspector.MeshRenderer.EditMaterial");
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("%s: changes apply to every entity that uses this material", name.c_str());
			if (open)
			{
				m_MaterialEditor.Draw(panelContext, material, "Inspector.Material");
				ImGui::TreePop();
			}
		}
		ImGui::Unindent(ImGui::GetStyle().IndentSpacing * 0.5f);
	}

	void InspectorPanel::DrawAnimatorState(EditorContext& context, Entity entity)
	{
		const Ref<Scene>& scene = context.GetActiveScene();
		AnimationSystem* system = scene && scene->IsRunning() ? scene->GetSystem<AnimationSystem>() : nullptr;
		if (!system)
			return;
		const UI::ThemeColors& colors = UI::GetThemeColors();
		ImGui::Spacing();
		if (!system->FindInstance(entity))
		{
			ImGui::TextColored(colors.TextSecondary, "%s  Not animating: the entity is inactive", Icons::Bone);
			return;
		}
		const std::optional<std::string> state = system->GetCurrentState(entity);
		const std::optional<std::string> blendSource = system->GetBlendSource(entity);
		const std::vector<AnimatorParameterValue> parameters = system->GetParameters(entity);
		if (!UI::BeginPropertyTable("Inspector.Animator.Live"))
			return;
		UI::PropertyLabel("State", "The state the animator is in (a clip played directly has none); while a cross-fade runs, the state it comes "
			"from and the state it goes to", "Inspector.Animator.State");
		const auto describe = [](const std::string& name) { return name.empty() ? std::string("(clip)") : name; };
		std::string stateText = state && !state->empty() ? *state : std::string(system->FindInstance(entity)->IsPlaying() ? "(clip)" : "(not playing)");
		if (blendSource)
			stateText = fmt::format("{}  {}  {}", describe(*blendSource), Icons::ArrowRight, stateText);
		ImGui::TextUnformatted(stateText.c_str());
		UI::ItemProbe::Record("Inspector.Animator.State", true, 0, stateText);
		UI::PropertyLabel("Time", "The state's time over its clip's duration, counting loops", "Inspector.Animator.Time");
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Body);
		ImGui::Text("%.2f", system->GetNormalizedTime(entity));
		ImGui::PopFont();
		UI::EndPropertyTable();
		if (parameters.empty())
			return;
		// The controller's parameters under a heading of their own: the component's properties above have names such as Speed too.
		ImGui::Spacing();
		ImGui::TextColored(colors.TextSecondary, "Parameters");
		UI::ItemProbe::Record("Inspector.Animator.Parameters");
		if (!UI::BeginPropertyTable("Inspector.Animator.LiveParameters"))
			return;
		for (const AnimatorParameterValue& parameter : parameters)
		{
			UI::PropertyLabel(parameter.Name, fmt::format("{} parameter of the controller", AnimationParameterTypeToString(parameter.Type)));
			UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Body);
			if (parameter.Type == AnimationParameterType::Float)
				ImGui::Text("%.3f", parameter.Value);
			else
				ImGui::TextColored(parameter.Value != 0.0f ? colors.Text : colors.TextSecondary, "%s", parameter.Value != 0.0f ? "true" : "false");
			ImGui::PopFont();
		}
		UI::EndPropertyTable();
	}

	void InspectorPanel::DrawAddComponentPopup(EditorContext& context, const EditorCommandRegistry& commands, std::span<const Entity> entities)
	{
		// Constraints only for the popup's window: set while no other window could take them.
		if (ImGui::IsPopupOpen("AddComponent"))
		{
			ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * c_AddComponentWidthInFontSizes, 0.0f),
				ImVec2(FLT_MAX, ImGui::GetFontSize() * c_AddComponentListHeightInFontSizes));
		}
		if (!ImGui::BeginPopup("AddComponent"))
			return;
		if (ImGui::IsWindowAppearing())
		{
			m_ComponentFilter.clear();
			ImGui::SetKeyboardFocusHere();
		}
		UI::SearchField("Inspector.AddComponent.Search", m_ComponentFilter);
		const std::string filter = StringUtils::ToLower(m_ComponentFilter);

		// Grouped by category, in registration order within each: those that some selected entity lacks.
		std::map<std::string, std::vector<const ComponentInfo*>> categories;
		for (const ComponentInfo* info : ComponentRegistry::GetAll())
		{
			if (!info->IsAddable() || HasComponentOnAll(entities, *info))
				continue;
			if (!filter.empty() && StringUtils::ToLower(info->DisplayName).find(filter) == std::string::npos)
				continue;
			categories[info->Category.empty() ? "General" : info->Category].push_back(info);
		}

		for (const auto& [category, infos] : categories)
		{
			ImGui::SeparatorText(category.c_str());
			for (const ComponentInfo* info : infos)
			{
				const std::string label = fmt::format("{}  {}", UI::GetComponentIcon(*info), info->DisplayName);
				if (ImGui::Selectable(label.c_str()))
				{
					RunEditorCommand(context, commands, "component.add", { { "entities", ToJson(entities) }, { "component", info->Name } });
					ImGui::CloseCurrentPopup();
				}
				if (!info->Description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
					ImGui::SetTooltip("%s", info->Description.c_str());
			}
		}
		if (categories.empty())
			ImGui::TextDisabled("No components match");
		ImGui::EndPopup();
	}

	void InspectorPanel::DrawScripts(EditorContext& context, const EditorCommandRegistry& commands, Entity entity, const PrefabView* prefab)
	{
		std::optional<ScriptFieldOverrides> fieldOverrides;
		if (prefab)
		{
			fieldOverrides = ScriptFieldOverrides {
				[prefab](const std::string& property) { return prefab->IsOverridden(PrefabOverrides::c_ScriptComponent, property); },
				[this, entity = prefab->SceneEntity](const std::string& property, const char* command)
				{
					SchedulePrefabCommand(command, { { "entity", UUIDToJson(entity) }, { "component", PrefabOverrides::c_ScriptComponent },
						{ "property", property } });
				},
				prefab->Model };
		}
		const Ref<ScriptEngine>& engine = context.GetScriptEngine();
		const bool moduleLoaded = engine && engine->IsModuleLoaded();
		if (!moduleLoaded)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, UI::GetThemeColors().TextSecondary);
			ImGui::TextWrapped("No script module is loaded: build the scripts (Scripts > Build Scripts) to edit their fields.");
			ImGui::PopStyleColor();
		}

		// A copy: removing a script changes the component while it is drawn.
		const std::vector<ScriptEntry> entries = entity.GetComponent<ScriptComponent>().Scripts;
		const std::string entityID = UUIDToJson(entity.GetUUID()).get<std::string>();
		for (const ScriptEntry& entry : entries)
		{
			ImGui::PushID(entry.ClassName.c_str());
			const float headerRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
			const bool open = ImGui::TreeNodeEx("##Script", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth
				| ImGuiTreeNodeFlags_FramePadding, "%s  %s", Icons::FileCode, entry.ClassName.c_str());
			ImGui::SameLine(headerRight - ImGui::GetFrameHeight());
			const std::string removeTooltip = "Remove the script " + entry.ClassName;
			const bool remove = UI::IconButton("RemoveScript", Icons::X, removeTooltip.c_str());

			if (open)
			{
				const ScriptClassInfo* info = moduleLoaded ? engine->FindClass(entry.ClassName) : nullptr;
				if (info)
				{
					DrawScriptFields(context, entity, entry, *info, fieldOverrides ? &*fieldOverrides : nullptr);
				}
				else
				{
					if (moduleLoaded)
						ImGui::TextColored(UI::GetThemeColors().Warning, "The script module has no class '%s'", entry.ClassName.c_str());
					DrawStoredOverrides(entry);
				}
				ImGui::TreePop();
			}
			ImGui::PopID();

			if (remove)
			{
				RunEditorCommand(context, commands, "script.remove", { { "entity", entityID }, { "class", entry.ClassName } });
				return;
			}
		}

		if (!moduleLoaded)
			return;
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::BeginCombo("##AddScript", "Add Script..."))
		{
			bool any = false;
			for (const ScriptClassInfo& info : engine->GetClasses())
			{
				const bool attached = std::any_of(entries.begin(), entries.end(), [&](const ScriptEntry& entry) { return entry.ClassName == info.Name; });
				if (attached)
					continue;
				any = true;
				if (ImGui::Selectable(info.Name.c_str()))
					RunEditorCommand(context, commands, "script.add", { { "entity", entityID }, { "class", info.Name } });
			}
			if (!any)
				ImGui::TextDisabled("Every script class is attached");
			ImGui::EndCombo();
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Scene settings (nothing selected)
	////////////////////////////////////////////////////////////////////////////////

	void InspectorPanel::DrawSceneSettings(EditorContext& context, const EditorCommandRegistry& commands)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		Scene& scene = *context.GetActiveScene();
		UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Title);
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(colors.Accent, "%s", Icons::Clapperboard);
		ImGui::PopFont();
		ImGui::SameLine();
		ImGui::BeginGroup();
		UI::Heading(scene.GetName(), UI::TextSize::Body);
		UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
		ImGui::TextColored(colors.TextSecondary, "Scene \xC2\xB7 %zu %s \xC2\xB7 select an entity to edit it", scene.GetEntityCount(),
			scene.GetEntityCount() == 1 ? "entity" : "entities");
		ImGui::PopFont();
		ImGui::EndGroup();
		UI::ItemProbe::Record("Inspector.SceneSettings");
		ImGui::Spacing();

		// The sections start closed, their headers summing up what they hold: their rows are live widgets, which cost frame
		// time while they are drawn, and they are opened to be edited (each keeps its state while the editor runs).
		SceneSettings shown = m_SettingsDraft.value_or(scene.GetSettings());
		if (!m_PhysicsSummary || m_PhysicsSummary->Gravity != shown.Gravity || m_PhysicsSummary->FixedTimestep != shown.FixedTimestep)
			m_PhysicsSummary = PhysicsSummary { shown.Gravity, shown.FixedTimestep, DescribePhysics(shown) };
		if (!m_StreamingSummary || m_StreamingSummary->MaxEntitiesInstantiatedPerFrame != shown.MaxEntitiesInstantiatedPerFrame)
			m_StreamingSummary = StreamingSummary { shown.MaxEntitiesInstantiatedPerFrame, DescribeStreaming(shown) };
		const SceneSettingProperties& properties = GetSceneSettingProperties();
		const SceneSettings defaults;
		bool changed = false;
		bool finished = false;
		auto draw = [&](const SceneSettingRow& row, PropertyValue value, const PropertyValue& fallback, std::string_view zeroLabel, auto&& apply)
		{
			UI::PropertyLabel(row.Property.DisplayName, row.Property.Tooltip, row.ProbeKey);
			PropertyWidgetOptions options;
			options.DefaultValue = &fallback;
			options.ProbeKey = row.ProbeKey;
			options.ZeroLabel = zeroLabel;
			const PropertyEditResult result = DrawPropertyWidget(row.Property, value, scene, options);
			if (result.Changed)
				apply(value);
			changed |= result.Changed;
			finished |= result.Finished;
		};
		if (UI::SectionHeader("Inspector.Physics", GetPhysicsHeader(), ImGuiTreeNodeFlags_None, m_PhysicsSummary->Text))
		{
			if (UI::BeginPropertyTable("Settings"))
			{
				draw(properties.Gravity, shown.Gravity, PropertyValue(defaults.Gravity), {}, [&](const PropertyValue& value) { shown.Gravity = std::get<glm::vec3>(value); });
				draw(properties.FixedTimestep, shown.FixedTimestep, PropertyValue(defaults.FixedTimestep), {},
					[&](const PropertyValue& value) { shown.FixedTimestep = std::get<float>(value); });
				draw(properties.MaxFixedSteps, shown.MaxFixedStepsPerFrame, PropertyValue(defaults.MaxFixedStepsPerFrame), {},
					[&](const PropertyValue& value) { shown.MaxFixedStepsPerFrame = std::get<uint32_t>(value); });
				// Capacities take effect when a physics world is created (the next play start); 0, the default, sizes them from the
				// scene (shown as "Auto"). A typed value is not clamped by the field, and scene.setSettings refuses one over the
				// limit: it becomes the limit.
				PhysicsCapacitySettings& capacity = shown.PhysicsCapacity;
				draw(properties.MaxBodies, capacity.MaxBodies, PropertyValue(defaults.PhysicsCapacity.MaxBodies), c_AutomaticCapacity, [&](const PropertyValue& value)
					{ capacity.MaxBodies = std::min(std::get<uint32_t>(value), PhysicsCapacitySettings::c_MaxBodiesLimit); });
				draw(properties.MaxBodyPairs, capacity.MaxBodyPairs, PropertyValue(defaults.PhysicsCapacity.MaxBodyPairs), c_AutomaticCapacity, [&](const PropertyValue& value)
					{ capacity.MaxBodyPairs = std::min(std::get<uint32_t>(value), PhysicsCapacitySettings::c_MaxBodyPairsLimit); });
				draw(properties.MaxContactConstraints, capacity.MaxContactConstraints, PropertyValue(defaults.PhysicsCapacity.MaxContactConstraints), c_AutomaticCapacity,
					[&](const PropertyValue& value)
					{ capacity.MaxContactConstraints = std::min(std::get<uint32_t>(value), PhysicsCapacitySettings::c_MaxContactConstraintsLimit); });
				UI::EndPropertyTable();
			}
		}
		// Physics settings too, but edited by commands of their own (the draft below never carries layers).
		DrawCollisionLayers(context, commands);
		if (UI::SectionHeader("Inspector.Streaming", GetStreamingHeader(), ImGuiTreeNodeFlags_None, m_StreamingSummary->Text))
		{
			if (UI::BeginPropertyTable("StreamingSettings"))
			{
				// A typed value is not clamped by the field, and scene.setSettings refuses one out of range: it is clamped here.
				draw(properties.MaxEntitiesInstantiated, shown.MaxEntitiesInstantiatedPerFrame, PropertyValue(defaults.MaxEntitiesInstantiatedPerFrame), {},
					[&](const PropertyValue& value)
					{ shown.MaxEntitiesInstantiatedPerFrame = std::clamp(std::get<uint32_t>(value), 1u, SceneSettings::c_MaxEntitiesInstantiatedLimit); });
				UI::EndPropertyTable();
			}
		}
		// A drag shows its value here and applies it once, when it ends: one scene.setSettings, one undo step.
		if (changed)
			m_SettingsDraft = shown;
		if (finished && m_SettingsDraft)
		{
			const SceneSettings settings = *m_SettingsDraft;
			m_SettingsDraft.reset();
			const PhysicsCapacitySettings& capacity = settings.PhysicsCapacity;
			RunEditorCommand(context, commands, "scene.setSettings", {
				{ "gravity", { settings.Gravity.x, settings.Gravity.y, settings.Gravity.z } },
				{ "fixedTimestep", settings.FixedTimestep },
				{ "maxFixedStepsPerFrame", settings.MaxFixedStepsPerFrame },
				{ "maxEntitiesInstantiatedPerFrame", settings.MaxEntitiesInstantiatedPerFrame },
				{ "physicsCapacity", { { "maxBodies", capacity.MaxBodies }, { "maxBodyPairs", capacity.MaxBodyPairs },
					{ "maxContactConstraints", capacity.MaxContactConstraints } } } });
		}
		DrawSky(context, commands);
	}

	void InspectorPanel::DrawCollisionLayers(EditorContext& context, const EditorCommandRegistry& commands)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImGuiStyle& style = ImGui::GetStyle();
		Scene& scene = *context.GetActiveScene();
		const PhysicsLayerSettings& layers = scene.GetSettings().PhysicsLayers;
		if (!m_LayersSummary || m_LayersSummary->Layers != layers)
		{
			std::vector<uint32_t> shown;
			std::string text = DescribeLayers(layers, shown);
			m_LayersSummary = LayersSummary { layers, std::move(text), std::move(shown) };
		}
		if (!UI::SectionHeader("Inspector.Layers", GetLayersHeader(), ImGuiTreeNodeFlags_None, m_LayersSummary->Text))
			return;
		// Copies: a command run below replaces the settings (and the summary with them).
		const PhysicsLayerSettings current = layers;
		const std::vector<uint32_t> shown = m_LayersSummary->Shown;

		// The names, edited in place: a name applies when its field is left or Enter is pressed, an empty one clears it.
		if (UI::BeginPropertyTable("LayerNames"))
		{
			for (uint32_t layer = 0; layer < c_PhysicsLayerCount; layer++)
			{
				if (current.Names[layer].empty())
					continue;
				const std::string key = fmt::format("Inspector.Layers.Name.{}", layer);
				UI::PropertyLabel(fmt::format("Layer {}", layer), "Bodies and character controllers choose their layer by this name", key);
				ImGui::PushID(static_cast<int>(layer));
				std::string local;
				std::string* text = &local;
				if (m_LayerNameLayer == layer)
					text = &m_LayerNameText;
				else
					local = current.Names[layer];
				ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - style.ItemSpacing.x, ImGui::GetFrameHeight()));
				const bool entered = ImGui::InputTextWithHint("##Name", "Name", text, ImGuiInputTextFlags_EnterReturnsTrue);
				UI::ItemProbe::Record(key, true, 0, *text);
				if (ImGui::IsItemActivated())
				{
					m_LayerNameLayer = layer;
					m_LayerNameText = *text;
				}
				const std::string typed = *text;
				const bool commit = (entered || ImGui::IsItemDeactivatedAfterEdit()) && typed != current.Names[layer];
				if (!ImGui::IsItemActive() && m_LayerNameLayer == layer)
					m_LayerNameLayer = c_PhysicsLayerCount;
				ImGui::SameLine();
				const std::string clearKey = fmt::format("Inspector.Layers.Clear.{}", layer);
				const bool clear = UI::IconButton(clearKey.c_str(), Icons::X, "Remove the name (what is on the layer stays on it)");
				ImGui::PopID();
				if (commit)
					RunEditorCommand(context, commands, "scene.setSettings", { { "layerNames", { { std::to_string(layer), typed } } } });
				else if (clear)
					RunEditorCommand(context, commands, "scene.setSettings", { { "layerNames", { { std::to_string(layer), "" } } } });
			}
			UI::EndPropertyTable();
		}
		// Naming a layer starts with the first one without a name after the default layer (0, where everything starts).
		std::optional<uint32_t> unnamed;
		for (uint32_t offset = 0; offset < c_PhysicsLayerCount && !unnamed; offset++)
		{
			const uint32_t layer = (offset + 1) % c_PhysicsLayerCount;
			if (current.Names[layer].empty())
				unnamed = layer;
		}
		UI::ButtonStyle addStyle;
		addStyle.Enabled = unnamed.has_value();
		if (UI::ToolbarButton("Inspector.Layers.Add", Icons::LayersPlus, "Name a Layer", "Give the next layer without a name one, to choose it by", addStyle) && unnamed)
		{
			RunEditorCommand(context, commands, "scene.setSettings",
				{ { "layerNames", { { std::to_string(*unnamed), MakeNewLayerName(current, *unnamed) } } } });
		}

		// The matrix: one row per shown layer with a box for each shown layer up to it (the pairs once each); a cleared box
		// lets the pair pass through each other. Compact, so the layers a game names fit an Inspector of a laptop's width:
		// boxes of the text's height in tight cells, and row labels as wide as the longest (at most a few words, the rest
		// elided, whole in their tooltips).
		ImGui::Spacing();
		UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
		ImGui::TextColored(colors.TextSecondary, "Which layers collide");
		ImGui::PopFont();
		std::vector<std::string> labels;
		labels.reserve(shown.size());
		float labelWidth = 0.0f;
		for (const uint32_t layer : shown)
		{
			labels.push_back(fmt::format("{} \xC2\xB7 {}", layer, current.GetDisplayName(layer)));
			labelWidth = std::max(labelWidth, ImGui::CalcTextSize(labels.back().c_str()).x);
		}
		labelWidth = std::min(labelWidth, ImGui::GetFontSize() * c_LayerLabelMaxWidthInFontSizes);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x * 0.5f, style.CellPadding.y * 0.5f));
		const float cellWidth = ImGui::GetFrameHeight();
		const float rowHeight = ImGui::GetFrameHeight() + style.CellPadding.y * 2.0f;
		const float contentWidth = labelWidth + style.CellPadding.x * 2.0f + (cellWidth + style.CellPadding.x * 2.0f) * static_cast<float>(shown.size());
		const bool scroll = contentWidth > ImGui::GetContentRegionAvail().x;
		const float height = rowHeight * static_cast<float>(shown.size() + 1) + (scroll ? style.ScrollbarSize : 0.0f) + style.CellPadding.y;
		const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX | (scroll ? ImGuiTableFlags_ScrollX : ImGuiTableFlags_None);
		if (!ImGui::BeginTable("LayerMatrix", static_cast<int>(shown.size()) + 1, flags, ImVec2(scroll ? 0.0f : contentWidth, scroll ? height : 0.0f)))
		{
			ImGui::PopStyleVar(2);
			return;
		}
		ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthFixed, labelWidth);
		for (size_t column = 0; column < shown.size(); column++)
			ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, cellWidth);
		ImGui::TableNextRow();
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		for (size_t column = 0; column < shown.size(); column++)
		{
			ImGui::TableSetColumnIndex(static_cast<int>(column) + 1);
			const std::string index = std::to_string(shown[column]);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cellWidth - ImGui::CalcTextSize(index.c_str()).x) * 0.5f);
			ImGui::TextColored(colors.TextSecondary, "%s", index.c_str());
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("%s", current.GetDisplayName(shown[column]).c_str());
		}
		ImGui::PopFont();
		std::optional<std::pair<std::pair<uint32_t, uint32_t>, bool>> toggled;
		for (size_t row = 0; row < shown.size(); row++)
		{
			const uint32_t layer = shown[row];
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			const std::string shownLabel = UI::ElideEnd(labels[row], labelWidth);
			ImGui::TextUnformatted(shownLabel.c_str());
			UI::ItemProbe::Record(fmt::format("Inspector.Layers.Label.{}", layer), true, 0, shownLabel);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("%s", labels[row].c_str());
			for (size_t column = 0; column <= row; column++)
			{
				const uint32_t other = shown[column];
				ImGui::TableSetColumnIndex(static_cast<int>(column) + 1);
				ImGui::PushID(static_cast<int>(layer * c_PhysicsLayerCount + other));
				bool collide = current.DoCollide(layer, other);
				if (ImGui::Checkbox("##Collide", &collide))
					toggled = std::make_pair(std::make_pair(other, layer), collide);
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				{
					const std::string pair = layer == other ? fmt::format("Bodies on {}", current.GetDisplayName(layer))
						: fmt::format("{} and {}", current.GetDisplayName(other), current.GetDisplayName(layer));
					ImGui::SetTooltip("%s %s", pair.c_str(), collide ? "collide" : "pass through each other");
				}
				UI::ItemProbe::Record(fmt::format("Inspector.Layers.Cell.{}.{}", other, layer), true, 0, collide ? "on" : "off");
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
		ImGui::PopStyleVar(2);
		if (toggled)
		{
			const auto& [pair, collide] = *toggled;
			RunEditorCommand(context, commands, "scene.setSettings",
				{ { "layerCollisions", nlohmann::json::array({ { { "layers", { pair.first, pair.second } }, { "collide", collide } } }) } });
		}
	}

	void InspectorPanel::DrawSky(EditorContext& context, const EditorCommandRegistry& commands)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		Scene& scene = *context.GetActiveScene();
		// The sky the renderer uses: the first active sky light.
		Entity sky;
		for (auto [entity, light, world] : scene.GetRegistry().view<SkyLightComponent, WorldTransformComponent>().each())
		{
			if (world.ActiveInHierarchy)
			{
				sky = Entity(entity, &scene);
				break;
			}
		}
		if (!UI::SectionHeader("Inspector.Sky", GetSkyHeader(), ImGuiTreeNodeFlags_None, DescribeSky(sky)))
			return;
		if (!sky)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, colors.TextSecondary);
			ImGui::TextWrapped("The scene has no sky light: only its lights light it, and the background is black.");
			ImGui::PopStyleColor();
			// The scene stays shown (nothing is selected): the new sky appears here.
			if (UI::ToolbarButton("Inspector.AddSky", Icons::CloudSun, "Add Procedural Sky", "Create an entity with a procedural sky light"))
				RunEditorCommand(context, commands, "entity.create", { { "name", "Sky" }, { "components", { { "SkyLight", { { "Source", "Procedural" } } } } } });
			return;
		}
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(colors.TextSecondary, "From '%s'", sky.GetName().c_str());
		ImGui::SameLine();
		if (UI::ToolbarButton("Inspector.SelectSky", Icons::MousePointerClick, "Select", "Select the entity that holds the sky"))
		{
			context.Select(sky.GetUUID());
			return;
		}
		const ComponentInfo* info = ComponentRegistry::Find<SkyLightComponent>();
		if (!info)
			return;
		const Entity entities[] = { sky };
		ImGui::PushID("Sky");
		DrawProperties(context, entities, *info, nullptr);
		ImGui::PopID();
	}

}
