#include "Panels/InspectorPanel.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/PropertyEdit.h"
#include "Panels/SceneHierarchyPanel.h"
#include "UI/PropertyWidgets.h"

#include <Strata/Core/Log.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/ComponentAccess.h>
#include <Strata/Scene/Components.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <map>

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
				if (info->Properties.empty())
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
