#pragma once

#include <Strata/Renderer/GraphicsDevice.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

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
		// saved or discarded: at once when there are none, later (or never, when cancelled) after asking.
		virtual void RequestDiscardChanges(std::function<void()> action) = 0;
		// The New Project dialog: template, name and location, then project.create. An empty template id chooses the
		// default one (basic3d).
		virtual void ShowNewProjectDialog(const std::string& templateId) = 0;
		// Asks where to copy a sample (project.samples) and opens the copy (project.openSample).
		virtual void ShowOpenSampleDialog(const std::string& sampleId) = 0;
		// Picks a project file with the system's file dialog and opens it.
		virtual void ShowOpenProjectDialog() = 0;
		virtual void ShowAboutDialog() = 0;
		// Leaves the launcher for the editor without opening a project (an untitled scene with the built-in assets).
		virtual void DismissLauncher() = 0;

		virtual EditorEnvironment GetEnvironment() const = 0;
		virtual EditorAutomationState GetAutomationState() const = 0;
	};

}
