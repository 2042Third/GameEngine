#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/UUID.h>
#include <Strata/Renderer/GraphicsDevice.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	// Facts about the running editor that the UI shows (the launcher's footer, About Strata) and editor.status reports.
	struct EditorEnvironment
	{
		std::optional<GraphicsDeviceInfo> GraphicsDevice; // nullopt without a GPU
		// From the creation of the process until its first frame was on screen; nullopt before that (or when the system
		// does not tell when the process was created).
		std::optional<double> StartupSeconds;
		float UIScale = 1.0f;
		// The automation client next to the editor (StrataCLI, also the MCP server AI agents connect through); empty when
		// it was not built.
		std::filesystem::path CLIExecutable;
	};

	// The automation server that tools and AI agents drive the editor through (EditorAutomation).
	struct EditorAutomationState
	{
		bool Enabled = false; // Asked for (not --no-automation)
		bool Running = false;
		uint16_t Port = 0;
		uint32_t Clients = 0;
		std::string Error; // Why it did not start, when it was asked for
	};

	// What panels may ask of the editor window around them (EditorLayer) beyond the commands: its dialogs, the question
	// about unsaved changes, and facts about the running editor. Panels reach it through EditorPanelContext::Shell. Main
	// thread only.
	class EditorShell
	{
	public:
		virtual ~EditorShell() = default;

		// Runs an action that replaces the edited scene (opening or creating a project or scene) once unsaved changes are
		// saved or discarded: at once when there are none, later (or never, when cancelled) after asking. The person has
		// decided by the time the action runs, so its command passes force: true (the commands refuse to discard unsaved
		// changes otherwise, see CommandUtils::CheckUnsavedChanges).
		virtual void RequestDiscardChanges(std::function<void()> action) = 0;
		// RequestDiscardChanges for an action that releases the project (opening, creating or closing a project, quitting): once
		// the scene's changes are saved or discarded, it also asks about the project's settings while its file could not be
		// written (EditorContext::HasUnwrittenProjectSettings), offering to write it again (project.save). The action's command
		// passes force: true once the person decided.
		virtual void RequestReleaseProject(std::function<void()> action) = 0;
		// RequestReleaseProject for quitting the editor: once the project's questions are answered, it also asks about the
		// preferences while their file could not be written (EditorContext::HasUnwrittenPreferences), offering to write it again
		// (preferences.save).
		virtual void RequestQuit(std::function<void()> action) = 0;
		// A modal dialog that asks (RequestDiscardChanges) while it stays open calls this between its UI::BeginModal and
		// UI::EndModal every frame: the question then shows over it. Asked from anywhere else, the question opens over the
		// whole window, which closes a modal dialog that is open.
		virtual void DrawDiscardQuestion() = 0;
		// The New Project dialog: template, name and location, then project.create. An empty template id chooses the
		// default one (basic3d).
		virtual void ShowNewProjectDialog(const std::string& templateId) = 0;
		// Asks where to copy a sample (project.samples) and opens the copy (project.openSample).
		virtual void ShowOpenSampleDialog(const std::string& sampleId) = 0;
		// Picks a project file with the system's file dialog and opens it.
		virtual void ShowOpenProjectDialog() = 0;
		virtual void ShowAboutDialog() = 0;
		// Scripts > New Script...: a class name and a template, then script.create (nothing without a project).
		virtual void ShowNewScriptDialog() = 0;
		// Leaves the launcher for the editor without opening a project (an untitled scene with the built-in assets).
		virtual void DismissLauncher() = 0;
		// Renames an entity in place in the Hierarchy (F2 in the viewport): the Hierarchy comes to the front with the entity's
		// row revealed and its name in a text field.
		virtual void RenameEntity(UUID entity) = 0;
		// Opens a docked panel by its id (EditorPanels) and brings it to the front.
		virtual void ShowPanel(std::string_view panel) = 0;
		// The Activity panel with an entry of the activity journal selected (0: none).
		virtual void ShowActivity(uint64_t entry) = 0;
		// The Recover Unsaved Changes dialog (RecoveryDialog).
		virtual void ShowRecoveryDialog() = 0;
		// An asset in the Asset tab, which comes forward, and in the Content Browser (e.g. a toast's Show Asset).
		virtual void ShowAsset(AssetHandle asset) = 0;

		virtual EditorEnvironment GetEnvironment() const = 0;
		virtual EditorAutomationState GetAutomationState() const = 0;
	};

}
