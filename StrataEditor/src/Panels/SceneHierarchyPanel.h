#pragma once

#include "UI/EditorPanelRegistry.h"

#include <Strata/Core/UUID.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Strata
{

	class EditorCommandRegistry;
	class EditorContext;

	// Tree of the active scene's entities: selection, drag-and-drop reparenting, prefab and model drops, and creation
	// menus. Every change goes through editor commands, so it is undoable and identical to what automation can do.
	class SceneHierarchyPanel : public EditorPanel
	{
	public:
		void OnImGuiRender(EditorPanelContext& context) override;

		// Menu items creating entity presets (entity.create) under `parent` (null: top level). With `schedule`, the
		// chosen action is handed to it instead of running immediately (callers that are iterating the scene).
		static void DrawCreateMenu(EditorContext& context, const EditorCommandRegistry& commands, UUID parent,
			const std::function<void(std::function<void()>)>& schedule = {});
	private:
		void DrawEntityContextMenu(EditorContext& context, const EditorCommandRegistry& commands, UUID entity);
		void AcceptDrops(EditorContext& context, const EditorCommandRegistry& commands, UUID target);
	private:
		// Actions chosen while the tree is drawn run after it, so the scene never changes during the walk.
		std::vector<std::function<void()>> m_Deferred;
		std::unordered_set<UUID> m_Selected; // Snapshot of the selection for this frame
	};

	// Runs a command from the UI and logs failures (the UI shows no modal errors for routine operations). Returns the
	// result value, or null on failure.
	nlohmann::json RunEditorCommand(EditorContext& context, const EditorCommandRegistry& commands, std::string_view name, const nlohmann::json& parameters = nlohmann::json::object());

}
