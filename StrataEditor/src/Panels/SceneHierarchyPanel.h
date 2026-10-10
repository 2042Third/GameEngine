#pragma once

#include "Editor/HierarchyView.h"
#include "UI/EditorPanelRegistry.h"

#include <Strata/Core/UUID.h>

#include <imgui.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	class EditorCommandRegistry;
	class EditorContext;
	class Scene;

	// The active scene's entities as a tree, drawn from the rows of a HierarchyView, so a frame draws only the rows in
	// view (ImGuiListClipper) however large the scene is. Every change goes through editor commands, so it is undoable and
	// identical to what automation can do.
	// - Selection through ImGui's multi-select: click, Ctrl+click toggles, Shift+click and Shift+arrows select ranges,
	//   Ctrl+A all rows, dragging in the empty space below the rows draws a selection box, Escape or a click there clears.
	//   While filtering, Ctrl+A, ranges and boxes select only the matches, not the ancestors shown dimmed for context.
	//   An entity selected elsewhere (the viewport, a tool) is revealed: its ancestors expand and the list scrolls to it.
	// - Drag and drop: the dragged entities (the selection when a selected row is dragged) go before the row under the
	//   mouse (its top quarter), after it (its bottom quarter) or into it (the middle), in one entity.setParent; dropped
	//   below the rows they go to the end of the top level. Prefabs and models dropped on a row are instantiated under it.
	// - Rows show the entity's icon, its name (dimmed while inactive in the hierarchy, in the info color for prefab
	//   instances), a badge on a prefab instance's root (a click shows the prefab in the Asset tab) and an eye that toggles
	//   the entity (entity.setActive). F2 or a double click renames in place (Enter commits, Escape cancels, clicking
	//   elsewhere commits).
	// - A '+' button and right clicks create entities; the search box filters by name, "c:<component>" and "t:<tag>"; an
	//   empty scene shows a hint with Cube, Light and Camera buttons; a banner says when play mode discards changes.
	// - Delete, Ctrl+D, Ctrl+C, Ctrl+X and Ctrl+V act on the selection while the panel has the keyboard focus
	//   (EditorActions::HandleSelectionShortcuts).
	class SceneHierarchyPanel : public EditorPanel
	{
	public:
		void OnImGuiRender(EditorPanelContext& context) override;
		void OnHidden(EditorPanelContext& context) override;

		// Menu items creating entity presets (entity.create) under `parent` (null: top level). With `schedule`, the
		// chosen action is handed to it instead of running immediately (callers that are iterating the scene).
		static void DrawCreateMenu(EditorContext& context, const EditorCommandRegistry& commands, UUID parent,
			const std::function<void(std::function<void()>)>& schedule = {});

		// Starts renaming an entity in place: its row is revealed and shows a text field from the next frame on.
		void BeginRename(UUID entity);
		bool IsRenaming() const { return m_Rename.Entity.IsValid(); }
		const HierarchyView& GetView() const { return m_View; }

		// Where a row was drawn in the last frame (screen coordinates), when it was in view: for tests.
		struct RowRect
		{
			ImVec2 Min;
			ImVec2 Max;
		};
		std::optional<RowRect> FindRowRect(UUID entity) const;
	private:
		void DrawHeader(EditorContext& context, const EditorCommandRegistry& commands);
		void DrawEmptyScene(EditorContext& context, const EditorCommandRegistry& commands);
		void DrawRows(EditorContext& context, const EditorCommandRegistry& commands, Scene& scene);
		void DrawRow(EditorContext& context, const EditorCommandRegistry& commands, Scene& scene, size_t row);
		void DrawRowName(Scene& scene, const HierarchyRow& row, float textX, float right);
		void DrawRowContextMenu(EditorContext& context, const EditorCommandRegistry& commands, Scene& scene, const HierarchyRow& row);
		void AcceptRowDrops(EditorContext& context, const EditorCommandRegistry& commands, Scene& scene, size_t row, const ImVec2& min, const ImVec2& max);
		void AcceptBackgroundDrops(EditorContext& context, const EditorCommandRegistry& commands, Scene& scene);
		void ApplySelectionRequests(EditorContext& context, const Scene& scene, const ImGuiMultiSelectIO& io);
		// The row of the entity a multi-select request names by its selection data (the entity's handle); nothing when the
		// entity is gone or has no row.
		std::optional<size_t> FindSelectionRow(const Scene& scene, ImGuiSelectionUserData data) const;
		// Reveals an entity selected outside the panel, and the entity being renamed.
		void UpdateReveal(EditorContext& context);
		// The rename field over its row (placed by DrawRowName), drawn after the rows.
		void DrawRenameField(EditorContext& context, const EditorCommandRegistry& commands);
		void EndRename(EditorContext& context, const EditorCommandRegistry& commands, bool commit);
	private:
		HierarchyView m_View;
		std::string m_FilterText;
		// Actions chosen while the rows are drawn run after them, so the scene never changes while they are drawn.
		std::vector<std::function<void()>> m_Deferred;

		// The primary selection when the panel last looked: another one (picked in the viewport, chosen by a tool) is revealed.
		UUID m_LastPrimary = UUID::Null();
		bool m_SelectionFromPanel = false; // The panel changed the selection itself (no reveal)
		std::optional<size_t> m_ScrollToRow;

		// The entities a drag carries, gathered when it starts (the topmost of the selection, in hierarchy order).
		std::vector<UUID> m_Dragged;

		struct RenameState
		{
			UUID Entity = UUID::Null();
			std::string Text;
			bool Focus = false;     // Give the field the keyboard focus when it is drawn next
			bool Activated = false; // The field has had the focus (so losing it commits)
		};
		RenameState m_Rename;
		std::optional<RowRect> m_RenameField; // Where this frame's rename field goes

		struct VisibleRow
		{
			UUID Entity;
			RowRect Rect;
		};
		std::vector<VisibleRow> m_VisibleRows;
	};

}
