#include "Panels/SceneHierarchyPanel.h"

#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorPlacement.h"
#include "Editor/PrefabSync.h"
#include "UI/EditorActions.h"
#include "UI/EditorFonts.h"
#include "UI/EntityIcons.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/PropertyWidgets.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/PrefabInstances.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneHierarchy.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>

namespace Strata
{

	namespace
	{

		struct EntityPreset
		{
			const char* Icon;
			const char* Label;
			const char* Name;
			nlohmann::json Components;
		};

		std::vector<EntityPreset> GetPresets()
		{
			return {
				{ Icons::SquareDashed, "Empty", "Entity", nlohmann::json::object() },
				{ Icons::Box, "Cube", "Cube", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } },
				{ Icons::Globe, "Sphere", "Sphere", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::SphereMesh) } } } } },
				{ Icons::Square, "Plane", "Plane", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::PlaneMesh) } } } } },
				{ Icons::Cylinder, "Cylinder", "Cylinder", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CylinderMesh) } } } } },
				{ Icons::Pill, "Capsule", "Capsule", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CapsuleMesh) } } } } },
				{ Icons::Cone, "Cone", "Cone", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::ConeMesh) } } } } },
				{ Icons::Torus, "Torus", "Torus", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::TorusMesh) } } } } },
				{ Icons::Video, "Camera", "Camera", { { "Camera", nlohmann::json::object() } } },
				{ Icons::Sun, "Directional Light", "Directional Light", { { "DirectionalLight", nlohmann::json::object() } } },
				{ Icons::Lightbulb, "Point Light", "Point Light", { { "PointLight", nlohmann::json::object() } } },
				{ Icons::Flashlight, "Spot Light", "Spot Light", { { "SpotLight", nlohmann::json::object() } } },
				{ Icons::CloudSun, "Sky Light", "Sky Light", { { "SkyLight", nlohmann::json::object() } } },
				{ Icons::Aperture, "Post Process", "Post Process", { { "PostProcess", nlohmann::json::object() } } },
				{ Icons::Type, "Text", "Text", { { "Text", nlohmann::json::object() } } },
				{ Icons::Volume2, "Audio Source", "Audio Source", { { "AudioSource", nlohmann::json::object() } } } };
		}

		// Where a new top-level entity of a preset goes: at the view's pivot, resting on the surface under it (its mesh's
		// bounds touching the surface).
		glm::vec3 PlacePreset(EditorContext& context, const EntityPreset& preset)
		{
			const EditorPlacement::Surface surface = EditorPlacement::FindViewSurface(*context.GetActiveScene(), context.GetViewport().GetCamera());
			AABB bounds;
			const nlohmann::json* renderer = JsonUtils::Find(preset.Components, "MeshRenderer");
			const nlohmann::json* mesh = renderer ? JsonUtils::Find(*renderer, "Mesh") : nullptr;
			if (const std::optional<UUID> handle = mesh ? UUIDFromJson(*mesh) : std::nullopt)
				bounds = EditorPlacement::GetMeshBounds(*handle);
			return EditorPlacement::RestOn(surface, bounds);
		}

		// Creates an entity of a preset (entity.create) and selects it: under the parent at its origin, or at the top level
		// where the view looks (PlacePreset).
		void CreatePreset(EditorContext& context, const EditorCommandRegistry& commands, const EntityPreset& preset, UUID parent)
		{
			nlohmann::json parameters = { { "name", preset.Name }, { "components", preset.Components } };
			if (parent.IsValid())
			{
				parameters["parent"] = UUIDToJson(parent);
			}
			else
			{
				const glm::vec3 translation = PlacePreset(context, preset);
				parameters["components"]["Transform"] = { { "Translation", { translation.x, translation.y, translation.z } } };
			}
			const nlohmann::json created = RunEditorCommand(context, commands, "entity.create", parameters);
			if (created.is_object())
			{
				if (const std::optional<UUID> id = UUIDFromJson(created["id"]))
					context.Select(*id);
			}
		}

		const EntityPreset* FindPreset(const std::vector<EntityPreset>& presets, std::string_view label)
		{
			for (const EntityPreset& preset : presets)
			{
				if (label == preset.Label)
					return &preset;
			}
			return nullptr;
		}

		const ImVec4& GetPlayStateColor(const EditorContext& context)
		{
			const UI::ThemeColors::PlayStateColors& colors = UI::GetThemeColors().PlayState;
			if (context.IsPaused())
				return colors.Paused;
			return context.GetSceneState() == SceneState::Simulate ? colors.Simulate : colors.Play;
		}

		nlohmann::json ToJson(const std::vector<UUID>& ids)
		{
			nlohmann::json array = nlohmann::json::array();
			for (const UUID id : ids)
				array.push_back(UUIDToJson(id));
			return array;
		}

		// Whether the entity is the root of a prefab instance: it comes from a prefab, its parent does not (from that one).
		bool IsPrefabRoot(const entt::registry& registry, entt::entity entity)
		{
			const PrefabInstanceComponent* prefab = registry.try_get<PrefabInstanceComponent>(entity);
			if (!prefab)
				return false;
			const entt::entity parent = registry.get<HierarchyComponent>(entity).Parent;
			const PrefabInstanceComponent* parentPrefab = parent != entt::null ? registry.try_get<PrefabInstanceComponent>(parent) : nullptr;
			return !parentPrefab || parentPrefab->Prefab != prefab->Prefab;
		}

		std::string GetAssetName(AssetHandle handle)
		{
			if (AssetManager::HasActive())
			{
				if (const std::optional<AssetMetadata> metadata = AssetManager::GetActive()->GetMetadata(handle))
					return metadata->Name.empty() ? metadata->Path : metadata->Name;
			}
			return handle.ToString();
		}

		// A row's selection data in the multi-select scope: its entity's handle, which (unlike its row index) still names the
		// same entity when rows appear or disappear above it between the two clicks of a range. Never
		// ImGuiSelectionUserData_Invalid (-1): handles are unsigned 32-bit.
		ImGuiSelectionUserData ToSelectionData(entt::entity handle)
		{
			return static_cast<ImGuiSelectionUserData>(entt::to_integral(handle));
		}

		// A small icon button on a row (the eye, the prefab badge) without a probe key: rows come and go, the probe keeps a
		// fixed set of keys. Returns true when clicked.
		bool RowButton(const char* id, const char* icon, const ImVec4& color, const ImVec2& position, float size, const char* tooltip)
		{
			ImGui::SetCursorScreenPos(position);
			const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
			const bool hovered = ImGui::IsItemHovered();
			const ImVec2 iconSize = ImGui::CalcTextSize(icon);
			ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + (size - iconSize.x) * 0.5f, position.y + (size - iconSize.y) * 0.5f),
				ImGui::GetColorU32(hovered ? UI::GetThemeColors().Text : color), icon);
			if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("%s", tooltip);
			return pressed;
		}

	}

	void SceneHierarchyPanel::DrawCreateMenu(EditorContext& context, const EditorCommandRegistry& commands, UUID parent,
		const std::function<void(std::function<void()>)>& schedule)
	{
		for (const EntityPreset& preset : GetPresets())
		{
			const std::string label = fmt::format("{}  {}", preset.Icon, preset.Label);
			const bool chosen = ImGui::MenuItem(label.c_str());
			// One creation menu is open at a time: its items are recorded as "Create.<Preset>".
			UI::ItemProbe::Record(fmt::format("Create.{}", preset.Label));
			if (!chosen)
				continue;
			auto action = [&context, &commands, preset, parent]() { CreatePreset(context, commands, preset, parent); };
			if (schedule)
				schedule(std::move(action));
			else
				action();
		}
	}

	void SceneHierarchyPanel::BeginRename(UUID entity)
	{
		m_Rename = RenameState();
		m_Rename.Entity = entity;
		m_Rename.Focus = true;
	}

	std::optional<SceneHierarchyPanel::RowRect> SceneHierarchyPanel::FindRowRect(UUID entity) const
	{
		for (const VisibleRow& row : m_VisibleRows)
		{
			if (row.Entity == entity)
				return row.Rect;
		}
		return std::nullopt;
	}

	void SceneHierarchyPanel::OnHidden(EditorPanelContext&)
	{
		m_Rename = RenameState();
		m_Dragged.clear();
		m_VisibleRows.clear();
	}

	void SceneHierarchyPanel::OnImGuiRender(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		const EditorCommandRegistry& commands = panelContext.Commands;
		m_Deferred.clear();
		m_VisibleRows.clear();
		Scene& scene = *context.GetActiveScene();
		// Rows are rebuilt only when the hierarchy, the expansion or the filter changed.
		m_View.Update(context.GetActiveScene());
		UpdateReveal(context);

		if (context.IsPlaying())
		{
			const bool paused = context.IsPaused();
			UI::Banner("Hierarchy.PlayBanner", paused ? Icons::Pause : Icons::Play,
				paused ? "Paused: changes are discarded when play stops" : "Playing: changes are discarded when play stops", GetPlayStateColor(context));
			ImGui::Spacing();
		}
		DrawHeader(context, commands);

		// While the panel (or its list) has the keyboard focus.
		EditorActions::HandleSelectionShortcuts(context, commands, false);
		if (context.AcceptsEditShortcuts() && !ImGui::GetTopMostPopupModal() && ImGui::Shortcut(ImGuiKey_F2, ImGuiInputFlags_RouteFocused))
		{
			if (const Entity primary = context.GetPrimarySelection())
				BeginRename(primary.GetUUID());
		}

		if (m_View.GetRows().empty() && !m_View.IsFiltering())
			DrawEmptyScene(context, commands);
		else
			DrawRows(context, commands, scene);

		// Applied after drawing, so the rows are never changed while they are drawn.
		std::vector<std::function<void()>> deferred = std::move(m_Deferred);
		m_Deferred.clear();
		for (const std::function<void()>& action : deferred)
			action();
	}

	void SceneHierarchyPanel::UpdateReveal(EditorContext& context)
	{
		const Ref<Scene>& scene = context.GetActiveScene();
		// A rename ends with its entity.
		if (m_Rename.Entity.IsValid() && !scene->GetEntityByUUID(m_Rename.Entity))
			m_Rename = RenameState();

		// An entity selected elsewhere (a viewport pick, a tool) is revealed; the panel's own clicks are in view already.
		const Entity primary = context.GetPrimarySelection();
		const UUID primaryId = primary ? primary.GetUUID() : UUID::Null();
		if (primaryId != m_LastPrimary)
		{
			if (primaryId.IsValid() && !m_SelectionFromPanel)
			{
				if (const HierarchyView::RevealResult reveal = m_View.Reveal(scene, primaryId); reveal.Row)
					m_ScrollToRow = reveal.Row;
			}
			m_LastPrimary = primaryId;
		}
		m_SelectionFromPanel = false;

		if (m_Rename.Entity.IsValid() && m_Rename.Focus)
		{
			if (const HierarchyView::RevealResult reveal = m_View.Reveal(scene, m_Rename.Entity); reveal.Row)
				m_ScrollToRow = reveal.Row;
		}
	}

	void SceneHierarchyPanel::DrawHeader(EditorContext& context, const EditorCommandRegistry& commands)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		if (UI::ToolbarButton("Hierarchy.Create", Icons::Plus, nullptr, "Create an entity"))
			ImGui::OpenPopup("CreateEntity");
		if (ImGui::BeginPopup("CreateEntity"))
		{
			DrawCreateMenu(context, commands, UUID::Null(), [this](std::function<void()> action) { m_Deferred.push_back(std::move(action)); });
			ImGui::EndPopup();
		}
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		const float collapseWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x;
		if (UI::SearchField("Hierarchy.Search", m_FilterText, "Search", std::max(ImGui::GetContentRegionAvail().x - collapseWidth, ImGui::GetFrameHeight()),
			"Search by name; \"c:Light\" finds entities with a component, \"t:Enemy\" with a tag. Terms combine."))
		{
			m_View.SetFilter(m_FilterText);
		}
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		if (UI::IconButton("Hierarchy.CollapseAll", Icons::ChevronsDownUp, "Collapse all", !m_View.IsFiltering()))
			m_View.CollapseAll();
		for (const std::string& unknown : m_View.GetUnknownComponents())
		{
			UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
			ImGui::TextColored(UI::GetThemeColors().Warning, "No component is named like \"%s\"", unknown.c_str());
			ImGui::PopFont();
		}
	}

	void SceneHierarchyPanel::DrawEmptyScene(EditorContext& context, const EditorCommandRegistry& commands)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize()));
		UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Display);
		const float iconWidth = ImGui::CalcTextSize(Icons::Layers).x;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max((ImGui::GetContentRegionAvail().x - iconWidth) * 0.5f, 0.0f));
		ImGui::TextColored(colors.TextDisabled, "%s", Icons::Layers);
		ImGui::PopFont();
		const char* title = "The scene is empty";
		UI::PushFont(UI::EditorFont::SemiBold, UI::TextSize::Body);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title).x) * 0.5f, 0.0f));
		ImGui::TextUnformatted(title);
		ImGui::PopFont();
		const char* hint = "Add something to start with:";
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(hint).x) * 0.5f, 0.0f));
		ImGui::TextColored(colors.TextSecondary, "%s", hint);
		ImGui::Spacing();

		// Cube, Light and Camera, centered as a group.
		const std::vector<EntityPreset> presets = GetPresets();
		struct QuickAdd
		{
			const char* Id;
			const char* Preset;
			const char* Label;
		};
		constexpr QuickAdd c_QuickAdds[] = { { "Hierarchy.AddCube", "Cube", "Cube" }, { "Hierarchy.AddLight", "Directional Light", "Light" },
			{ "Hierarchy.AddCamera", "Camera", "Camera" } };
		float total = 0.0f;
		for (const QuickAdd& add : c_QuickAdds)
		{
			const EntityPreset* preset = FindPreset(presets, add.Preset);
			total += style.FramePadding.x * 2.0f + ImGui::CalcTextSize(preset ? preset->Icon : "").x + style.ItemInnerSpacing.x + ImGui::CalcTextSize(add.Label).x;
		}
		total += style.ItemSpacing.x * 2.0f;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max((ImGui::GetContentRegionAvail().x - total) * 0.5f, 0.0f));
		bool first = true;
		for (const QuickAdd& add : c_QuickAdds)
		{
			const EntityPreset* preset = FindPreset(presets, add.Preset);
			if (!preset)
				continue;
			if (!first)
				ImGui::SameLine();
			first = false;
			const std::string tooltip = fmt::format("Create a {}", add.Preset);
			UI::ButtonStyle buttonStyle;
			buttonStyle.Active = true;
			if (UI::ToolbarButton(add.Id, preset->Icon, add.Label, tooltip.c_str(), buttonStyle))
			{
				const EntityPreset chosen = *preset;
				m_Deferred.push_back([&context, &commands, chosen]() { CreatePreset(context, commands, chosen, UUID::Null()); });
			}
		}

		// The rest of the panel takes drops and opens the creation menu.
		const ImVec2 available = ImGui::GetContentRegionAvail();
		ImGui::Dummy(ImVec2(std::max(available.x, 1.0f), std::max(available.y, ImGui::GetFrameHeight())));
		AcceptBackgroundDrops(context, commands, *context.GetActiveScene());
		if (ImGui::BeginPopupContextWindow("HierarchyBackground", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			DrawCreateMenu(context, commands, UUID::Null(), [this](std::function<void()> action) { m_Deferred.push_back(std::move(action)); });
			ImGui::EndPopup();
		}
	}

	void SceneHierarchyPanel::DrawRows(EditorContext& context, const EditorCommandRegistry& commands, Scene& scene)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		if (!ImGui::BeginChild("Rows", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_None))
		{
			ImGui::EndChild();
			return;
		}
		// A drag that ended leaves nothing to carry.
		if (!ImGui::GetDragDropPayload())
			m_Dragged.clear();

		const std::vector<HierarchyRow>& rows = m_View.GetRows();
		const float rowHeight = ImGui::GetFrameHeight();
		if (m_ScrollToRow && *m_ScrollToRow < rows.size())
		{
			// Into view, centered when it has to move.
			const float y = static_cast<float>(*m_ScrollToRow) * rowHeight;
			const float height = ImGui::GetWindowHeight();
			if (y < ImGui::GetScrollY() || y + rowHeight > ImGui::GetScrollY() + height)
				ImGui::SetScrollY(std::max(y - (height - rowHeight) * 0.5f, 0.0f));
		}
		m_ScrollToRow.reset();

		if (rows.empty())
		{
			ImGui::TextColored(UI::GetThemeColors().TextSecondary, "Nothing matches \"%s\"", m_FilterText.c_str());
		}
		else
		{
			// Rows touch: their highlights form one column.
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
			// Clicks select on release, so a row (selected or not) can be dragged; a selection box starts in the empty space.
			const ImGuiMultiSelectFlags flags = ImGuiMultiSelectFlags_ClearOnEscape | ImGuiMultiSelectFlags_ClearOnClickVoid | ImGuiMultiSelectFlags_BoxSelect1d
				| ImGuiMultiSelectFlags_SelectOnClickRelease;
			// The selection has a focus scope of its own (its ID): the rename field after it is outside the selection.
			ImGui::PushID("Selection");
			ImGuiMultiSelectIO* io = ImGui::BeginMultiSelect(flags, static_cast<int>(context.GetSelection().size()), static_cast<int>(rows.size()));
			ApplySelectionRequests(context, scene, *io);

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(rows.size()), rowHeight);
			// The row a range selection starts from and the row being renamed are drawn even when out of view.
			if (const std::optional<size_t> source = FindSelectionRow(scene, io->RangeSrcItem))
				clipper.IncludeItemByIndex(static_cast<int>(*source));
			if (m_Rename.Entity.IsValid())
			{
				// An entity the rows do not show (the filter hides it) is not renamed.
				if (const std::optional<size_t> renamed = m_View.FindRow(m_Rename.Entity))
					clipper.IncludeItemByIndex(static_cast<int>(*renamed));
				else
					m_Rename = RenameState();
			}
			while (clipper.Step())
			{
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
					DrawRow(context, commands, scene, static_cast<size_t>(row));
			}
			io = ImGui::EndMultiSelect();
			ApplySelectionRequests(context, scene, *io);
			ImGui::PopID();
			ImGui::PopStyleVar();
			DrawRenameField(context, commands);
		}

		// The space below the rows: drops go to the end of the top level, a right click creates entities, a click (no item)
		// clears the selection and a drag draws a selection box.
		const ImVec2 available = ImGui::GetContentRegionAvail();
		ImGui::Dummy(ImVec2(std::max(available.x, 1.0f), std::max(available.y, rowHeight)));
		AcceptBackgroundDrops(context, commands, scene);
		if (ImGui::BeginPopupContextWindow("HierarchyBackground", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			DrawCreateMenu(context, commands, UUID::Null(), [this](std::function<void()> action) { m_Deferred.push_back(std::move(action)); });
			ImGui::EndPopup();
		}
		ImGui::EndChild();
	}

	void SceneHierarchyPanel::DrawRow(EditorContext& context, const EditorCommandRegistry& commands, Scene& scene, size_t index)
	{
		const HierarchyRow& row = m_View.GetRows()[index];
		const entt::registry& registry = scene.GetRegistry();
		if (!registry.valid(row.Handle))
			return;
		const ImGuiStyle& style = ImGui::GetStyle();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const UUID id = row.ID;
		ImGui::PushID(reinterpret_cast<const char*>(&id), reinterpret_cast<const char*>(&id) + sizeof(UUID));

		const bool selected = context.IsSelected(id);
		const bool filtering = m_View.IsFiltering();
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + style.IndentSpacing * static_cast<float>(row.Depth));
		const ImVec2 itemPosition = ImGui::GetCursorScreenPos();
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_OpenOnArrow
			| ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		// While filtering, the rows are the matches with their ancestors: nothing to open or close.
		if (!row.HasChildren() || filtering)
			flags |= ImGuiTreeNodeFlags_Leaf;
		if (selected)
			flags |= ImGuiTreeNodeFlags_Selected;
		ImGui::SetNextItemSelectionUserData(ToSelectionData(row.Handle));
		if (row.HasChildren() && !filtering)
			ImGui::SetNextItemOpen(row.IsExpanded());
		UI::PushSelectionColors(selected);
		const bool open = ImGui::TreeNodeEx("##Row", flags);
		UI::PopSelectionColors();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		m_VisibleRows.push_back({ id, { min, max } });
		const float textX = itemPosition.x + ImGui::GetFontSize() + style.FramePadding.x * 2.0f;

		// Opened or closed with the arrow (Alt: the whole subtree), or the keyboard.
		const bool toggled = ImGui::IsItemToggledOpen();
		if (toggled && !filtering)
		{
			if (ImGui::GetIO().KeyAlt)
				m_View.SetSubtreeExpanded(scene, id, open);
			else
				m_View.SetExpanded(id, open);
		}
		if (!toggled && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && ImGui::GetIO().MousePos.x >= textX)
			BeginRename(id);

		// Dragged rows: the selection when a selected row is dragged, else the row.
		if (ImGui::BeginDragDropSource())
		{
			if (m_Dragged.empty())
				m_Dragged = selected ? HierarchyView::GetTopmostInHierarchyOrder(scene, context.GetSelection()) : std::vector<UUID> { id };
			ImGui::SetDragDropPayload(DragDrop::c_Entities, m_Dragged.data(), m_Dragged.size() * sizeof(UUID), ImGuiCond_Once);
			if (m_Dragged.size() == 1)
				ImGui::Text("%s  %s", UI::GetEntityIcon(registry, row.Handle), registry.get<NameComponent>(row.Handle).Name.c_str());
			else
				ImGui::Text("%s  %zu entities", Icons::Layers, m_Dragged.size());
			ImGui::EndDragDropSource();
		}
		AcceptRowDrops(context, commands, scene, index, min, max);
		DrawRowContextMenu(context, commands, scene, row);

		// The eye at the right end, the prefab badge before it, then the name in what is left.
		const float button = ImGui::GetFrameHeight();
		const bool ownActive = !registry.all_of<InactiveComponent>(row.Handle);
		if (RowButton("##Eye", ownActive ? Icons::Eye : Icons::EyeOff, ownActive ? colors.TextDisabled : colors.TextSecondary, ImVec2(max.x - button, min.y), button,
			ownActive ? "Deactivate: neither simulated nor drawn, with its children" : "Activate"))
		{
			const nlohmann::json parameters = { { "entities", nlohmann::json::array({ UUIDToJson(id) }) }, { "active", !ownActive } };
			m_Deferred.push_back([&context, &commands, parameters]() { RunEditorCommand(context, commands, "entity.setActive", parameters); });
		}
		float right = max.x - button;
		if (IsPrefabRoot(registry, row.Handle))
		{
			// The badge leads to the prefab (or model) the entity was made from: the Asset tab shows it. An instance whose
			// values differ from its prefab's is marked (PrefabSync caches that until the edits or the prefab change).
			const AssetHandle prefab = registry.get<PrefabInstanceComponent>(row.Handle).Prefab;
			const bool modified = !context.IsPlaying() && context.GetPrefabSync().IsModified(context, Entity(row.Handle, &scene));
			const std::string tooltip = modified
				? fmt::format("An instance of {}, modified: its values differ from the prefab's (Apply or Revert in its menu). Click to show the prefab.",
					GetAssetName(prefab))
				: fmt::format("An instance of {}: click to show it in the Asset tab", GetAssetName(prefab));
			if (RowButton("##Prefab", Icons::Package, modified ? colors.Warning : colors.Info, ImVec2(right - button, min.y), button, tooltip.c_str()))
			{
				m_Deferred.push_back([&context, prefab]()
				{
					if (!context.SetInspectedAsset(prefab))
						ST_WARN("The prefab this entity was made from ({}) is not an asset of the project", prefab.ToString());
				});
			}
			right -= button;
		}
		DrawRowName(scene, row, textX, right - style.ItemInnerSpacing.x);
		ImGui::PopID();
	}

	void SceneHierarchyPanel::DrawRowName(Scene& scene, const HierarchyRow& row, float textX, float right)
	{
		const entt::registry& registry = scene.GetRegistry();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImGuiStyle& style = ImGui::GetStyle();
		const ImVec2 rowMin = m_VisibleRows.back().Rect.Min;
		const std::string& name = registry.get<NameComponent>(row.Handle).Name;

		if (m_Rename.Entity == row.ID)
		{
			// The field itself is drawn after the rows (DrawRenameField), outside the multi-selection, whose clicks and
			// keyboard moves it must not take part in.
			if (m_Rename.Focus)
				m_Rename.Text = name;
			m_RenameField = RowRect { ImVec2(textX, rowMin.y), ImVec2(std::max(right, textX + ImGui::GetFrameHeight() * 2.0f), rowMin.y + ImGui::GetFrameHeight()) };
			return;
		}

		// The icon and the name, clipped before the buttons at the right.
		const bool activeInHierarchy = registry.get<WorldTransformComponent>(row.Handle).ActiveInHierarchy;
		const bool prefab = registry.all_of<PrefabInstanceComponent>(row.Handle);
		ImVec4 textColor = colors.Text;
		if (!activeInHierarchy)
			textColor = colors.TextDisabled;
		else if (m_View.IsFiltering() && !row.IsMatch())
			textColor = colors.TextSecondary;
		else if (prefab)
			textColor = colors.Info;
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const float textY = rowMin.y + style.FramePadding.y;
		const char* icon = UI::GetEntityIcon(registry, row.Handle);
		drawList->PushClipRect(ImVec2(textX, rowMin.y), ImVec2(std::max(right, textX), rowMin.y + ImGui::GetFrameHeight()), true);
		drawList->AddText(ImVec2(textX, textY), ImGui::GetColorU32(activeInHierarchy ? (prefab ? colors.Info : colors.TextSecondary) : colors.TextDisabled), icon);
		const float nameX = textX + ImGui::CalcTextSize(icon).x + style.ItemInnerSpacing.x * 1.5f;
		const char* shown = name.empty() ? "(unnamed)" : name.c_str();
		drawList->AddText(ImVec2(nameX, textY), ImGui::GetColorU32(name.empty() ? colors.TextSecondary : textColor), shown);
		drawList->PopClipRect();
	}

	void SceneHierarchyPanel::DrawRenameField(EditorContext& context, const EditorCommandRegistry& commands)
	{
		if (!m_RenameField)
			return;
		const RowRect field = *m_RenameField;
		m_RenameField.reset();
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(field.Min);
		ImGui::SetNextItemWidth(field.Max.x - field.Min.x);
		// Activated (on the next frame) rather than given the keyboard focus through navigation: a navigation move out of
		// the selection's scope makes ImGui clear the selection.
		if (m_Rename.Focus)
		{
			ImGui::ActivateItemByID(ImGui::GetID("##Rename"));
			m_Rename.Focus = false;
		}
		const bool entered = ImGui::InputText("##Rename", &m_Rename.Text, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		UI::ItemProbe::Record("Hierarchy.Rename");
		if (ImGui::IsItemActive())
			m_Rename.Activated = true;
		if (entered)
			EndRename(context, commands, true);
		else if (m_Rename.Activated && ImGui::IsItemDeactivated())
			EndRename(context, commands, !ImGui::IsKeyPressed(ImGuiKey_Escape, false));
		ImGui::SetCursorScreenPos(cursor);
	}

	void SceneHierarchyPanel::EndRename(EditorContext& context, const EditorCommandRegistry& commands, bool commit)
	{
		const RenameState rename = std::move(m_Rename);
		m_Rename = RenameState();
		if (!commit)
			return;
		const Entity entity = context.GetActiveScene()->GetEntityByUUID(rename.Entity);
		if (!entity || entity.GetName() == rename.Text)
			return;
		const nlohmann::json parameters = { { "entity", UUIDToJson(rename.Entity) }, { "name", rename.Text } };
		m_Deferred.push_back([&context, &commands, parameters]() { RunEditorCommand(context, commands, "entity.rename", parameters); });
	}

	void SceneHierarchyPanel::DrawRowContextMenu(EditorContext& context, const EditorCommandRegistry& commands, Scene& scene, const HierarchyRow& row)
	{
		if (!ImGui::BeginPopupContextItem("RowMenu"))
			return;
		const UUID id = row.ID;
		auto schedule = [this](std::function<void()> action) { m_Deferred.push_back(std::move(action)); };
		if (ImGui::BeginMenu("Create Child"))
		{
			DrawCreateMenu(context, commands, id, schedule);
			ImGui::EndMenu();
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Rename", "F2"))
			BeginRename(id);
		if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
			schedule([&context, &commands]() { EditorActions::DuplicateSelection(context, commands); });
		if (ImGui::MenuItem("Copy", "Ctrl+C"))
			schedule([&context, &commands]() { EditorActions::CopySelection(context, commands); });
		if (ImGui::MenuItem("Cut", "Ctrl+X"))
			schedule([&context, &commands]() { EditorActions::CutSelection(context, commands); });
		if (ImGui::MenuItem("Paste", "Ctrl+V", false, EditorActions::CanPaste()))
			schedule([&context, &commands]() { EditorActions::Paste(context, commands); });
		ImGui::Separator();
		DrawPrefabMenuItems(context, commands, scene, row);
		ImGui::Separator();
		const bool hasParent = scene.GetRegistry().get<HierarchyComponent>(row.Handle).Parent != entt::null;
		if (ImGui::MenuItem("Unparent", nullptr, false, hasParent))
		{
			const nlohmann::json entities = ToJson(HierarchyView::GetTopmostInHierarchyOrder(scene, context.GetSelection()));
			schedule([&context, &commands, entities]() { RunEditorCommand(context, commands, "entity.setParent", { { "entities", entities }, { "parent", nullptr } }); });
		}
		if (row.HasChildren() && !m_View.IsFiltering())
		{
			if (ImGui::MenuItem("Expand All"))
				m_View.SetSubtreeExpanded(scene, id, true);
			if (ImGui::MenuItem("Collapse All"))
				m_View.SetSubtreeExpanded(scene, id, false);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Delete", "Del"))
			schedule([&context, &commands]() { EditorActions::DeleteSelection(context, commands); });
		ImGui::EndPopup();
	}

	void SceneHierarchyPanel::DrawPrefabMenuItems(EditorContext& context, const EditorCommandRegistry& commands, Scene& scene, const HierarchyRow& row)
	{
		auto schedule = [this](std::function<void()> action) { m_Deferred.push_back(std::move(action)); };
		const bool playing = context.IsPlaying();
		const EditorAssetManager* assets = context.GetAssetManager();
		if (ImGui::MenuItem("Create Prefab", nullptr, false, assets && !playing))
		{
			// The selected entities (those below another selected one go with it), or the row's, as a prefab in Prefabs/ that
			// they become an instance of.
			const std::vector<UUID> roots = context.IsSelected(row.ID) ? HierarchyView::GetTopmostInHierarchyOrder(scene, context.GetSelection())
				: std::vector<UUID> { row.ID };
			const std::string& name = scene.GetRegistry().get<NameComponent>(row.Handle).Name;
			const std::string path = CommandUtils::MakeUniqueAssetPath(*assets, "Prefabs", name, ".stprefab", "Prefab");
			schedule([&context, &commands, roots, path]()
			{
				const nlohmann::json created = RunEditorCommand(context, commands, "prefab.create", { { "entities", ToJson(roots) }, { "path", path } });
				if (!created.is_object())
					return;
				if (const std::optional<UUID> prefab = UUIDFromJson(created.value("asset", nlohmann::json())))
					context.SetInspectedAsset(*prefab);
			});
		}

		const Entity root = PrefabInstances::FindInstanceRoot(Entity(row.Handle, &scene));
		if (!root)
			return;
		const nlohmann::json instance = { { "entity", UUIDToJson(row.ID) } };
		const bool isPrefab = AssetManager::GetAssetType(PrefabInstances::GetPrefab(root)) == AssetType::Prefab;
		if (ImGui::MenuItem("Apply to Prefab", nullptr, false, isPrefab && !playing))
			schedule([&context, &commands, instance]() { RunEditorCommand(context, commands, "prefab.apply", instance); });
		if (ImGui::MenuItem("Revert to Prefab"))
			schedule([&context, &commands, instance]() { RunEditorCommand(context, commands, "prefab.revert", instance); });
		if (ImGui::MenuItem("Unpack Prefab"))
			schedule([&context, &commands, instance]() { RunEditorCommand(context, commands, "prefab.unpack", instance); });
		if (ImGui::MenuItem("Select Prefab Root", nullptr, false, root.GetUUID() != row.ID))
			schedule([&context, &commands, instance]() { RunEditorCommand(context, commands, "prefab.select", instance); });
		if (ImGui::MenuItem("Show Prefab Asset"))
		{
			nlohmann::json parameters = instance;
			parameters["asset"] = true;
			schedule([&context, &commands, parameters]() { RunEditorCommand(context, commands, "prefab.select", parameters); });
		}
	}

	void SceneHierarchyPanel::AcceptRowDrops(EditorContext& context, const EditorCommandRegistry& commands, Scene& scene, size_t index, const ImVec2& min, const ImVec2& max)
	{
		if (!ImGui::BeginDragDropTarget())
			return;
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImGuiStyle& style = ImGui::GetStyle();
		const HierarchyRow& row = m_View.GetRows()[index];
		const ImGuiDragDropFlags flags = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Entities, flags))
		{
			std::vector<UUID> dragged(static_cast<size_t>(payload->DataSize) / sizeof(UUID));
			if (!dragged.empty())
				std::memcpy(dragged.data(), payload->Data, dragged.size() * sizeof(UUID));
			const float fraction = (ImGui::GetIO().MousePos.y - min.y) / std::max(max.y - min.y, 1.0f);
			const std::optional<HierarchyDropTarget> target = m_View.GetDropTarget(scene, index, fraction, dragged);
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const float thickness = std::max(1.0f, std::round(style.FramePadding.y * 0.5f));
			if (!target)
			{
				// Onto itself or one of its descendants.
				ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed);
			}
			else if (target->Zone == HierarchyDropZone::Inside)
			{
				drawList->AddRectFilled(min, max, ImGui::GetColorU32(UI::WithAlpha(colors.Accent, 0.18f)));
				drawList->AddRect(min, max, ImGui::GetColorU32(colors.Accent), 0.0f, thickness);
			}
			else
			{
				// A line where the entities go, indented to the depth they will have.
				const bool intoRow = target->Parent == row.ID;
				const float x = min.x + style.IndentSpacing * static_cast<float>(row.Depth + (intoRow ? 1u : 0u)) + style.FramePadding.x;
				const float y = target->Zone == HierarchyDropZone::Before ? min.y : max.y;
				drawList->AddLine(ImVec2(x, y), ImVec2(max.x, y), ImGui::GetColorU32(colors.Accent), thickness * 2.0f);
				drawList->AddCircleFilled(ImVec2(x, y), thickness * 2.0f, ImGui::GetColorU32(colors.Accent));
			}
			if (target && payload->IsDelivery())
			{
				const nlohmann::json parameters = { { "entities", ToJson(dragged) },
					{ "parent", target->Parent.IsValid() ? UUIDToJson(target->Parent) : nlohmann::json(nullptr) }, { "index", target->Index } };
				// Dropped into a collapsed entity: open it, so the entities stay in view.
				if (target->Parent.IsValid())
					m_View.SetExpanded(target->Parent, true);
				m_Deferred.push_back([&context, &commands, parameters]() { RunEditorCommand(context, commands, "entity.setParent", parameters); });
			}
		}
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Asset); payload && payload->DataSize == static_cast<int>(sizeof(AssetHandle)))
		{
			AssetHandle asset = UUID::Null();
			std::memcpy(&asset, payload->Data, sizeof(AssetHandle));
			const AssetType type = AssetManager::GetAssetType(asset);
			if (type == AssetType::Prefab || type == AssetType::Model)
			{
				const nlohmann::json parameters = { { "prefab", UUIDToJson(asset) }, { "parent", UUIDToJson(row.ID) } };
				m_View.SetExpanded(row.ID, true);
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

	void SceneHierarchyPanel::AcceptBackgroundDrops(EditorContext& context, const EditorCommandRegistry& commands, Scene& scene)
	{
		if (!ImGui::BeginDragDropTarget())
			return;
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Entities))
		{
			std::vector<UUID> dragged(static_cast<size_t>(payload->DataSize) / sizeof(UUID));
			if (!dragged.empty())
				std::memcpy(dragged.data(), payload->Data, dragged.size() * sizeof(UUID));
			// To the end of the top level, in hierarchy order.
			const std::vector<UUID> moving = HierarchyView::GetTopmostInHierarchyOrder(scene, dragged);
			if (!moving.empty())
			{
				const nlohmann::json parameters = { { "entities", ToJson(moving) }, { "parent", nullptr } };
				m_Deferred.push_back([&context, &commands, parameters]() { RunEditorCommand(context, commands, "entity.setParent", parameters); });
			}
		}
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Asset); payload && payload->DataSize == static_cast<int>(sizeof(AssetHandle)))
		{
			AssetHandle asset = UUID::Null();
			std::memcpy(&asset, payload->Data, sizeof(AssetHandle));
			const AssetType type = AssetManager::GetAssetType(asset);
			if (type == AssetType::Prefab || type == AssetType::Model)
			{
				const nlohmann::json parameters = { { "prefab", UUIDToJson(asset) } };
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

	std::optional<size_t> SceneHierarchyPanel::FindSelectionRow(const Scene& scene, ImGuiSelectionUserData data) const
	{
		if (data < 0 || data > static_cast<ImGuiSelectionUserData>(std::numeric_limits<entt::id_type>::max()))
			return std::nullopt;
		const entt::entity handle = static_cast<entt::entity>(static_cast<entt::id_type>(data));
		const entt::registry& registry = scene.GetRegistry();
		const IDComponent* id = registry.valid(handle) ? registry.try_get<IDComponent>(handle) : nullptr;
		return id ? m_View.FindRow(id->ID) : std::nullopt;
	}

	void SceneHierarchyPanel::ApplySelectionRequests(EditorContext& context, const Scene& scene, const ImGuiMultiSelectIO& io)
	{
		const std::vector<HierarchyRow>& rows = m_View.GetRows();
		if (io.Requests.Size == 0 || rows.empty())
			return;
		// While filtering, the ancestors shown dimmed for context are not what was searched for: selecting all rows or a range
		// of rows (Ctrl+A, Shift+click, a box) takes the matches only, so a search by component selects exactly the entities
		// to edit together (and Delete never takes a context row with all its descendants). A context row clicked on its own
		// is selected.
		const bool filtering = m_View.IsFiltering();
		std::vector<UUID> selection = context.GetSelection();
		for (const ImGuiSelectionRequest& request : io.Requests)
		{
			if (request.Type == ImGuiSelectionRequestType_SetAll)
			{
				selection.clear();
				if (request.Selected)
				{
					for (const HierarchyRow& row : rows)
					{
						if (!filtering || row.IsMatch())
							selection.push_back(row.ID);
					}
				}
				continue;
			}
			if (request.Type != ImGuiSelectionRequestType_SetRange)
				continue;
			// The ends are entities: their rows now (rows may have appeared above the first since it was clicked). An end
			// whose row is gone (collapsed away, deleted) leaves the range at the other one.
			std::optional<size_t> first = FindSelectionRow(scene, request.RangeFirstItem);
			std::optional<size_t> last = FindSelectionRow(scene, request.RangeLastItem);
			if (!first && !last)
				continue;
			if (!first)
				first = last;
			if (!last)
				last = first;
			const size_t begin = std::min(*first, *last);
			const size_t end = std::max(*first, *last);
			std::vector<UUID> range;
			range.reserve(end - begin + 1);
			for (size_t row = begin; row <= end; row++)
			{
				if (!filtering || rows[row].IsMatch() || begin == end)
					range.push_back(rows[row].ID);
			}
			// Selected rows move to the end in the order the range was made, so the row clicked last is the primary selection.
			if (request.RangeDirection < 0)
				std::reverse(range.begin(), range.end());
			const std::unordered_set<UUID> inRange(range.begin(), range.end());
			std::erase_if(selection, [&inRange](UUID id) { return inRange.contains(id); });
			if (request.Selected)
				selection.insert(selection.end(), range.begin(), range.end());
		}
		context.SetSelection(std::move(selection));
		m_SelectionFromPanel = true;
	}

}
