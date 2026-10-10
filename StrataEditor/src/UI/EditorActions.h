#pragma once

#include <Strata/Core/UUID.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string_view>
#include <vector>

namespace Strata
{

	class EditorCommandRegistry;
	class EditorContext;
	struct ComponentInfo;

	// Runs a command from the UI and logs failures (the UI shows no modal errors for routine operations). Returns the
	// result value, or null on failure. Only for commands that finish at once: commands that take frames go through the
	// EditorCommandRunner.
	nlohmann::json RunEditorCommand(EditorContext& context, const EditorCommandRegistry& commands, std::string_view name,
		const nlohmann::json& parameters = nlohmann::json::object());

	// What the Edit menu and the focus-scoped shortcuts of the Hierarchy and the Viewport do with the selected entities:
	// delete, duplicate, cut, copy, paste and select all. Each runs commands, so it is one undo step and the same operation
	// automation has. The clipboard holds JSON text (ImGui's clipboard: the system's in the editor, the harness's own in
	// tests): entity.copy's data for entities, and {"Strata": {"Format": "ComponentValues"}, "Component", "Values"} for the
	// values of a component (the inspector's Copy Values and Paste Values).
	namespace EditorActions
	{

		constexpr const char* c_ComponentValuesFormat = "ComponentValues";

		// entity.delete of the selected entities (with their descendants).
		void DeleteSelection(EditorContext& context, const EditorCommandRegistry& commands);
		// entity.duplicate of the selected entities (those below another selected one are copied with it); the copies become
		// the selection.
		void DuplicateSelection(EditorContext& context, const EditorCommandRegistry& commands);
		// entity.copy of the selected entities into the clipboard. False when nothing is selected or the copy failed.
		bool CopySelection(EditorContext& context, const EditorCommandRegistry& commands);
		// Copies the selected entities, then deletes them (one undo step: copying changes nothing).
		void CutSelection(EditorContext& context, const EditorCommandRegistry& commands);
		// Whether the clipboard holds entities (entity.copy's data).
		bool CanPaste();
		// entity.paste of the clipboard's entities right after the primary selection, under its parent (at the end of the
		// top level when nothing is selected); the pasted entities become the selection.
		void Paste(EditorContext& context, const EditorCommandRegistry& commands);
		// Selects every entity of the active scene, in hierarchy order.
		void SelectAll(EditorContext& context);

		// Puts a component's values (component.get's form) into the clipboard.
		void CopyComponentValues(const ComponentInfo& component, const nlohmann::json& values);
		// The values in the clipboard when they are values of this component (Paste Values).
		std::optional<nlohmann::json> ReadComponentValues(const ComponentInfo& component);

		// The selection shortcuts for the current window (call between its Begin and End): Delete, Ctrl+D, Ctrl+C, Ctrl+X,
		// Ctrl+V and, with selectAll, Ctrl+A. They are routed to the window only while it has the keyboard focus
		// (ImGuiInputFlags_RouteFocused), and an active text field keeps its keys, so a key pressed in another panel (Delete
		// in the Content Browser) or while typing never edits the scene. Nothing happens behind a modal dialog or while the
		// running game has the input (EditorContext::AcceptsEditShortcuts).
		void HandleSelectionShortcuts(EditorContext& context, const EditorCommandRegistry& commands, bool selectAll);

	}

}
