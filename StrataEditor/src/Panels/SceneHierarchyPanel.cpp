#include "Panels/SceneHierarchyPanel.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "UI/PropertyWidgets.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/Log.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>

#include <imgui.h>

#include <algorithm>
#include <utility>

namespace Strata
{

	nlohmann::json RunEditorCommand(EditorContext& context, const EditorCommandRegistry& commands, std::string_view name, const nlohmann::json& parameters)
	{
		EditorCommandResult result = commands.Execute(context, name, parameters);
		if (!result.Success)
		{
			ST_ERROR("{}: {}", name, result.Error);
			return nullptr;
		}
		if (result.Value.is_object() && result.Value.contains("warning"))
			ST_WARN("{}: {}", name, result.Value["warning"].get<std::string>());
		return result.Value;
	}

	namespace
	{

		struct EntityPreset
		{
			const char* Label;
			const char* Name;
			nlohmann::json Components;
		};

		std::vector<EntityPreset> GetPresets()
		{
			return {
				{ "Empty", "Entity", nlohmann::json::object() },
				{ "Cube", "Cube", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } },
				{ "Sphere", "Sphere", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::SphereMesh) } } } } },
				{ "Plane", "Plane", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::PlaneMesh) } } } } },
				{ "Camera", "Camera", { { "Camera", nlohmann::json::object() } } },
				{ "Directional Light", "Directional Light", { { "DirectionalLight", nlohmann::json::object() } } },
				{ "Point Light", "Point Light", { { "PointLight", nlohmann::json::object() } } },
				{ "Spot Light", "Spot Light", { { "SpotLight", nlohmann::json::object() } } },
				{ "Sky Light", "Sky Light", { { "SkyLight", nlohmann::json::object() } } },
				{ "Post Process", "Post Process", { { "PostProcess", nlohmann::json::object() } } },
				{ "Text", "Text", { { "Text", nlohmann::json::object() } } },
				{ "Audio Source", "Audio Source", { { "AudioSource", nlohmann::json::object() } } } };
		}

		void* ToImGuiID(UUID id)
		{
			return reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint64_t>(id)));
		}

	}

	void SceneHierarchyPanel::DrawCreateMenu(EditorContext& context, const EditorCommandRegistry& commands, UUID parent,
		const std::function<void(std::function<void()>)>& schedule)
	{
		for (const EntityPreset& preset : GetPresets())
		{
			if (!ImGui::MenuItem(preset.Label))
				continue;
			nlohmann::json parameters = { { "name", preset.Name }, { "components", preset.Components } };
			if (parent.IsValid())
				parameters["parent"] = UUIDToJson(parent);
			auto action = [&context, &commands, parameters = std::move(parameters)]()
			{
				const nlohmann::json created = RunEditorCommand(context, commands, "entity.create", parameters);
				if (created.is_object())
					context.Select(*UUIDFromJson(created["id"]));
			};
			if (schedule)
				schedule(std::move(action));
			else
				action();
		}
	}

	void SceneHierarchyPanel::DrawEntityContextMenu(EditorContext& context, const EditorCommandRegistry& commands, UUID entity)
	{
		if (!ImGui::BeginPopupContextItem())
			return;
		if (!m_Selected.contains(entity))
			m_Deferred.push_back([&context, entity]() { context.Select(entity); });
		if (ImGui::BeginMenu("Create Child"))
		{
			DrawCreateMenu(context, commands, entity, [this](std::function<void()> action) { m_Deferred.push_back(std::move(action)); });
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
		{
			m_Deferred.push_back([&context, &commands, entity]()
			{
				const nlohmann::json copy = RunEditorCommand(context, commands, "entity.duplicate", { { "entity", UUIDToJson(entity) } });
				if (copy.is_object())
					context.Select(*UUIDFromJson(copy["id"]));
			});
		}
		if (ImGui::MenuItem("Unparent", nullptr, false, context.GetActiveScene()->GetEntityByUUID(entity).GetParent().IsValid()))
			m_Deferred.push_back([&context, &commands, entity]() { RunEditorCommand(context, commands, "entity.setParent", { { "entity", UUIDToJson(entity) } }); });
		ImGui::Separator();
		if (ImGui::MenuItem("Delete", "Del"))
		{
			// The clicked entity and, when it is selected, the rest of the selection.
			nlohmann::json ids = nlohmann::json::array({ UUIDToJson(entity) });
			if (m_Selected.contains(entity))
			{
				for (UUID selected : context.GetSelection())
				{
					if (selected != entity)
						ids.push_back(UUIDToJson(selected));
				}
			}
			m_Deferred.push_back([&context, &commands, ids]() { RunEditorCommand(context, commands, "entity.delete", { { "entities", ids } }); });
		}
		ImGui::EndPopup();
	}

	void SceneHierarchyPanel::AcceptDrops(EditorContext& context, const EditorCommandRegistry& commands, UUID target)
	{
		if (!ImGui::BeginDragDropTarget())
			return;
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Entity))
		{
			const UUID child = *static_cast<const UUID*>(payload->Data);
			if (child != target)
			{
				nlohmann::json parameters = { { "entity", UUIDToJson(child) } };
				if (target.IsValid())
					parameters["parent"] = UUIDToJson(target);
				m_Deferred.push_back([&context, &commands, parameters]() { RunEditorCommand(context, commands, "entity.setParent", parameters); });
			}
		}
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Asset))
		{
			const AssetHandle asset = *static_cast<const AssetHandle*>(payload->Data);
			const AssetType type = AssetManager::GetAssetType(asset);
			if (type == AssetType::Prefab || type == AssetType::Model)
			{
				nlohmann::json parameters = { { "prefab", UUIDToJson(asset) } };
				if (target.IsValid())
					parameters["parent"] = UUIDToJson(target);
				m_Deferred.push_back([&context, &commands, parameters]()
				{
					const nlohmann::json created = RunEditorCommand(context, commands, "prefab.instantiate", parameters);
					if (created.is_object() && !created["entities"].empty())
						context.Select(*UUIDFromJson(created["entities"][0]));
				});
			}
			else
			{
				ST_WARN("Only prefabs and models can be dropped into the hierarchy ({} is a {})", asset.ToString(), AssetTypeToString(type));
			}
		}
		ImGui::EndDragDropTarget();
	}

	void SceneHierarchyPanel::OnImGuiRender(EditorContext& context, const EditorCommandRegistry& commands)
	{
		m_Deferred.clear();
		if (!ImGui::Begin("Hierarchy"))
		{
			ImGui::End();
			return;
		}

		Scene& scene = *context.GetActiveScene();
		m_Selected = std::unordered_set<UUID>(context.GetSelection().begin(), context.GetSelection().end());
		const float indent = ImGui::GetStyle().IndentSpacing;

		// Depth-first with an explicit stack: hierarchies can be deeper than any recursion should go. Children of
		// collapsed nodes are never visited.
		std::vector<std::pair<UUID, int>> stack;
		const std::vector<UUID>& roots = scene.GetRootEntities();
		for (auto it = roots.rbegin(); it != roots.rend(); ++it)
			stack.emplace_back(*it, 0);

		while (!stack.empty())
		{
			const auto [id, depth] = stack.back();
			stack.pop_back();
			Entity entity = scene.GetEntityByUUID(id);
			if (!entity)
				continue;

			// A copy: nothing below may hold references into component storage.
			const std::vector<UUID> children = entity.GetComponent<RelationshipComponent>().Children;
			const std::string name = entity.GetName().empty() ? std::string("(unnamed)") : entity.GetName();
			const bool active = entity.IsActive();

			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth
				| ImGuiTreeNodeFlags_NoTreePushOnOpen;
			if (children.empty())
				flags |= ImGuiTreeNodeFlags_Leaf;
			if (m_Selected.contains(id))
				flags |= ImGuiTreeNodeFlags_Selected;

			if (depth > 0)
				ImGui::Indent(indent * static_cast<float>(depth));
			if (!active)
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			const bool open = ImGui::TreeNodeEx(ToImGuiID(id), flags, "%s", name.c_str());
			if (!active)
				ImGui::PopStyleColor();
			if (depth > 0)
				ImGui::Unindent(indent * static_cast<float>(depth));

			if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
			{
				const bool additive = ImGui::GetIO().KeyCtrl;
				m_Deferred.push_back([&context, id, additive, wasSelected = m_Selected.contains(id)]()
				{
					if (additive && wasSelected)
						context.Deselect(id);
					else
						context.Select(id, additive);
				});
			}
			if (ImGui::BeginDragDropSource())
			{
				ImGui::SetDragDropPayload(DragDrop::c_Entity, &id, sizeof(UUID));
				ImGui::TextUnformatted(name.c_str());
				ImGui::EndDragDropSource();
			}
			AcceptDrops(context, commands, id);
			ImGui::PushID(ToImGuiID(id));
			DrawEntityContextMenu(context, commands, id);
			ImGui::PopID();

			if (open)
			{
				for (auto it = children.rbegin(); it != children.rend(); ++it)
					stack.emplace_back(*it, depth + 1);
			}
		}

		// The empty area below the tree: drop to move to the top level, click to deselect, right-click to create.
		ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFrameHeight())));
		AcceptDrops(context, commands, UUID::Null());
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
			m_Deferred.push_back([&context]() { context.ClearSelection(); });
		if (ImGui::BeginPopupContextItem("HierarchyBackground"))
		{
			DrawCreateMenu(context, commands, UUID::Null(), [this](std::function<void()> action) { m_Deferred.push_back(std::move(action)); });
			ImGui::EndPopup();
		}
		ImGui::End();

		// Applied after drawing, so the tree is never changed while it is being walked.
		std::vector<std::function<void()>> deferred = std::move(m_Deferred);
		m_Deferred.clear();
		for (const std::function<void()>& action : deferred)
			action();
	}

}
