#include "Panels/InspectorPanel.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/PropertyEdit.h"
#include "Editor/ScriptEdit.h"
#include "Panels/SceneHierarchyPanel.h"
#include "UI/PropertyWidgets.h"

#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/ComponentAccess.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scripting/ScriptSystem.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	namespace
	{

		void DrawComponentProperties(EditorContext& context, Entity entity, const ComponentInfo& info)
		{
			if (!ImGui::BeginTable("Properties", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
				return;
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.4f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
			for (const PropertyInfo& property : info.Properties)
			{
				if (property.IsHidden() || property.IsTransient())
					continue;
				std::optional<PropertyValue> value = ComponentAccess::GetProperty(entity, info, property);
				if (!value)
					continue;

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(property.DisplayName.c_str());
				if (!property.Tooltip.empty() && ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", property.Tooltip.c_str());

				ImGui::TableSetColumnIndex(1);
				ImGui::BeginDisabled(property.IsReadOnly());
				const PropertyEditResult result = DrawPropertyWidget(property, *value, *entity.GetScene());
				ImGui::EndDisabled();
				if (result.Changed)
				{
					std::string error;
					if (!SetPropertyWithUndo(context, entity, info, property, *value, &error))
						ST_WARN("{}.{}: {}", info.Name, property.Name, error);
				}
				if (result.Finished)
					context.GetUndoStack().BreakMerge();
			}
			ImGui::EndTable();
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

		void DrawScriptFields(EditorContext& context, Entity entity, const ScriptEntry& entry, const ScriptClassInfo& info)
		{
			if (info.Fields.empty())
			{
				ImGui::TextDisabled("No fields");
				return;
			}
			if (!ImGui::BeginTable("Fields", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
				return;
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.4f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
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

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(property.DisplayName.c_str());
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("%s (%s), default %s%s", field.Name.c_str(), PropertyTypeToString(field.Type),
						JsonUtils::Dump(ScriptEdit::FieldValueToJson(field.DefaultValue, field.Type)).c_str(), overridden ? "; overridden on this entity" : "");
				}

				ImGui::TableSetColumnIndex(1);
				const PropertyEditResult result = DrawPropertyWidget(property, value, *entity.GetScene());
				std::string error;
				if (result.Changed && !ScriptEdit::SetField(context, entity, entry.ClassName, field, value, &error))
					ST_WARN("{}.{}: {}", entry.ClassName, field.Name, error);
				if (result.Finished)
					context.GetUndoStack().BreakMerge();

				ImGui::TableSetColumnIndex(2);
				ImGui::BeginDisabled(!overridden);
				if (ImGui::SmallButton("R"))
				{
					context.GetUndoStack().BreakMerge();
					if (!ScriptEdit::SetField(context, entity, entry.ClassName, field, std::nullopt, &error))
						ST_WARN("{}.{}: {}", entry.ClassName, field.Name, error);
					context.GetUndoStack().BreakMerge();
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip(overridden ? "Reset to the class default" : "Uses the class default");
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

	}

	void InspectorPanel::DrawScripts(EditorContext& context, const EditorCommandRegistry& commands, Entity entity)
	{
		const Ref<ScriptEngine>& engine = context.GetScriptEngine();
		const bool moduleLoaded = engine && engine->IsModuleLoaded();
		if (!moduleLoaded)
			ImGui::TextWrapped("No script module is loaded: build the scripts (Scripts > Build Scripts) to edit their fields.");

		// A copy: removing a script changes the component while it is drawn.
		const std::vector<ScriptEntry> entries = entity.GetComponent<ScriptComponent>().Scripts;
		const std::string entityID = UUIDToJson(entity.GetUUID()).get<std::string>();
		for (const ScriptEntry& entry : entries)
		{
			ImGui::PushID(entry.ClassName.c_str());
			const float headerRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
			const bool open = ImGui::TreeNodeEx("##Script", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth,
				"%s", entry.ClassName.c_str());
			ImGui::SameLine(headerRight - ImGui::GetFrameHeight());
			const bool remove = ImGui::SmallButton("x");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Remove the script %s", entry.ClassName.c_str());

			if (open)
			{
				const ScriptClassInfo* info = moduleLoaded ? engine->FindClass(entry.ClassName) : nullptr;
				if (info)
				{
					DrawScriptFields(context, entity, entry, *info);
				}
				else
				{
					if (moduleLoaded)
						ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "The script module has no class '%s'", entry.ClassName.c_str());
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

	void InspectorPanel::DrawAddComponentPopup(EditorContext& context, const EditorCommandRegistry& commands, Entity entity)
	{
		if (!ImGui::BeginPopup("AddComponent"))
			return;
		if (ImGui::IsWindowAppearing())
		{
			m_ComponentFilter.clear();
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::InputTextWithHint("##Filter", "Search", &m_ComponentFilter);
		const std::string filter = StringUtils::ToLower(m_ComponentFilter);

		// Grouped by category, in registration order within each.
		std::map<std::string, std::vector<const ComponentInfo*>> categories;
		entt::registry& registry = entity.GetScene()->GetRegistry();
		for (const ComponentInfo* info : ComponentRegistry::GetAll())
		{
			if (!info->IsAddable() || info->Has(registry, entity.GetHandle()))
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
				if (ImGui::Selectable(info->DisplayName.c_str()))
				{
					RunEditorCommand(context, commands, "component.add", { { "entity", UUIDToJson(entity.GetUUID()) }, { "component", info->Name } });
					ImGui::CloseCurrentPopup();
				}
				if (!info->Description.empty() && ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", info->Description.c_str());
			}
		}
		if (categories.empty())
			ImGui::TextDisabled("No components match");
		ImGui::EndPopup();
	}

	void InspectorPanel::OnImGuiRender(EditorContext& context, const EditorCommandRegistry& commands)
	{
		if (!ImGui::Begin("Inspector"))
		{
			ImGui::End();
			return;
		}

		Entity entity = context.GetPrimarySelection();
		if (!entity)
		{
			ImGui::TextDisabled("Select an entity to edit it");
			ImGui::End();
			return;
		}
		if (context.GetSelection().size() > 1)
			ImGui::TextDisabled("%zu entities selected; editing the last one", context.GetSelection().size());

		bool active = entity.IsActive();
		if (ImGui::Checkbox("##Active", &active))
			RunEditorCommand(context, commands, "entity.setActive", { { "entity", UUIDToJson(entity.GetUUID()) }, { "active", active } });
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Active");
		ImGui::SameLine();
		ImGui::TextDisabled("ID %s", entity.GetUUID().ToString().c_str());

		entt::registry& registry = entity.GetScene()->GetRegistry();
		for (const ComponentInfo* info : ComponentRegistry::GetAll())
		{
			if (info->IsHidden() || HasFlag(info->Flags, ComponentFlags::NoSerialize) || !info->Has(registry, entity.GetHandle()))
				continue;
			// The entity may lose components below (removal); stop drawing it then.
			if (!entity)
				break;

			ImGui::PushID(info->Name.c_str());
			const float headerRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
			const bool open = ImGui::CollapsingHeader(info->DisplayName.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
			if (!info->Description.empty() && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", info->Description.c_str());
			bool remove = false;
			if (info->IsRemovable())
			{
				ImGui::SameLine(headerRight - ImGui::GetFrameHeight());
				if (ImGui::SmallButton("x"))
					remove = true;
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Remove %s", info->DisplayName.c_str());
			}

			if (open && !remove)
			{
				if (info->TypeId == entt::type_id<ScriptComponent>().hash())
					DrawScripts(context, commands, entity);
				else if (info->Properties.empty())
					ImGui::TextDisabled(info->SerializeExtra ? "Edited in its own section" : "No properties");
				else
					DrawComponentProperties(context, entity, *info);
			}
			ImGui::PopID();

			if (remove)
				RunEditorCommand(context, commands, "component.remove", { { "entity", UUIDToJson(entity.GetUUID()) }, { "component", info->Name } });
		}

		ImGui::Spacing();
		const float buttonWidth = 200.0f;
		ImGui::SetCursorPosX(std::max((ImGui::GetContentRegionAvail().x - buttonWidth) * 0.5f, 0.0f) + ImGui::GetCursorPosX());
		if (ImGui::Button("Add Component", ImVec2(buttonWidth, 0.0f)))
			ImGui::OpenPopup("AddComponent");
		DrawAddComponentPopup(context, commands, entity);
		ImGui::End();
	}

}
