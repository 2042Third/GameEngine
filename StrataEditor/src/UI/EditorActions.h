#pragma once

#include "Editor/EditorPlacement.h"

#include <Strata/Core/UUID.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class EditorCommandRegistry;
	class EditorCommandRunner;
	class EditorContext;
	struct ComponentInfo;

	// Runs a command from the UI and logs failures (the UI shows no modal errors for routine operations). Returns the
	// result value, or null on failure. Only for commands that finish at once: commands that take frames go through the
	// EditorCommandRunner.
	nlohmann::json RunEditorCommand(EditorContext& context, const EditorCommandRegistry& commands, std::string_view name,
		const nlohmann::json& parameters = nlohmann::json::object());
	// Runs a command from the UI through the runner, for commands that may take frames (scene.open), and logs a failure when
	// it ends (not a cancellation, e.g. by a later scene.open). Both run it as the person's (CommandSource::UI): the
	// activity journal records it so, and toasts show its failure or warning.
	void RunEditorCommandDeferred(EditorCommandRunner& runner, EditorContext& context, const EditorCommandRegistry& commands, std::string_view name,
		const nlohmann::json& parameters = nlohmann::json::object());
	// Like RunEditorCommandDeferred, for UI that shows the failures it can (a dialog, the launcher): returns the error of a
	// command that failed at once (project.open of a file that is no project) without logging it; empty when the command
	// succeeded or is still pending. A later failure (not a cancellation) is logged.
	std::string StartEditorCommand(EditorCommandRunner& runner, EditorContext& context, const EditorCommandRegistry& commands, std::string_view name,
		const nlohmann::json& parameters = nlohmann::json::object());
	// While a scene is being opened outside play mode (EditorContext::IsSceneLoading: scene.open, a project's start scene) the
	// edited scene is read-only: draws a banner naming the scene that opens and returns true, so that the panel draws its
	// contents disabled. Draws nothing and returns false otherwise.
	bool DrawSceneLoadBanner(const char* id, const EditorContext& context);

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
		// entity.dropToGround of the selected entities (those below another selected one move with it).
		void DropSelectionToGround(EditorContext& context, const EditorCommandRegistry& commands);
		// prefab.instantiate of a prefab or model, its first root selected: under a parent where the prefab puts it, or at the
		// top level resting on a surface of the scene (EditorPlacement::RestOn with its bounds, GetTemplateBounds): the one
		// given, else the one under the view's pivot. Returns the root's id, nothing when it failed.
		std::optional<UUID> InstantiatePrefab(EditorContext& context, const EditorCommandRegistry& commands, UUID prefab, UUID parent = UUID::Null(),
			const EditorPlacement::Surface* surface = nullptr);

		// Puts a component's values (component.get's form) into the clipboard: those component.set can set (read-only and
		// transient ones are left out).
		void CopyComponentValues(const ComponentInfo& component, const nlohmann::json& values);
		// The settable values in the clipboard when they are values of this component (Paste Values); nothing when there are
		// none.
		std::optional<nlohmann::json> ReadComponentValues(const ComponentInfo& component);

		// The selection shortcuts for the current window (call between its Begin and End): Delete, Ctrl+D, Ctrl+C, Ctrl+X,
		// Ctrl+V, End (drop to the ground) and, with selectAll, Ctrl+A. They are routed to the window only while it has the keyboard focus
		// (ImGuiInputFlags_RouteFocused), and an active text field keeps its keys, so a key pressed in another panel (Delete
		// in the Content Browser) or while typing never edits the scene. Nothing happens behind a modal dialog or while the
		// running game has the input (EditorContext::AcceptsEditShortcuts).
		void HandleSelectionShortcuts(EditorContext& context, const EditorCommandRegistry& commands, bool selectAll);

	}

}
