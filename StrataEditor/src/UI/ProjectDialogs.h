#pragma once

#include "UI/EditorPanelRegistry.h"

#include <filesystem>
#include <string>
#include <vector>

struct ImGuiContext;
struct ImGuiSettingsHandler;
struct ImGuiTextBuffer;

namespace Strata
{

	// The dialogs that start a project, drawn by EditorLayer over the launcher or the editor:
	// - New Project: a template (cards from project.templates), a name and a location, then project.create;
	// - Open Sample: where the copy of a sample (project.samples) goes, then project.openSample.
	// Both suggest the location of the last project they made, remembered in imgui.ini (section "StrataLauncher"), first
	// <home>/StrataProjects, whose short paths keep script builds clear of path length limits. Creating a project replaces
	// the edited scene: open the dialogs through EditorShell::RequestDiscardChanges. Main thread only.
	class ProjectDialogs
	{
	public:
		static constexpr const char* c_NewProjectPopup = "New Project";
		static constexpr const char* c_OpenSamplePopup = "Open Sample";
		static constexpr const char* c_SettingsName = "StrataLauncher";
		static constexpr const char* c_DefaultLocationName = "StrataProjects";
		static constexpr const char* c_DefaultProjectName = "My Project";

		ProjectDialogs() = default;
		~ProjectDialogs();

		ProjectDialogs(const ProjectDialogs&) = delete;
		ProjectDialogs& operator=(const ProjectDialogs&) = delete;

		// Opens the dialog at the next Draw. The templates and samples come from their commands; a sample that is not
		// offered fails (false, with the reason) without opening anything. The New Project dialog chooses the given template,
		// or basic3d for an empty or unknown id.
		void OpenNewProject(EditorPanelContext& context, const std::string& templateId = {});
		bool OpenSample(EditorPanelContext& context, const std::string& sampleId, std::string* outError = nullptr);
		// Every frame, inside the ImGui frame: the open dialog.
		void Draw(EditorPanelContext& context);
		bool IsOpen() const { return m_Dialog != Dialog::None; }

		// Whether Browse buttons may open the system's folder picker (FileDialogs, which needs a window).
		void SetFolderPickerAvailable(bool available) { m_FolderPickerAvailable = available; }

		// Where new projects and sample copies go: the remembered location, else GetDefaultLocation.
		std::filesystem::path GetLocation() const;
		void SetLocation(const std::filesystem::path& location);
		// <home>/StrataProjects, else (without a home directory) <user data>/Strata/Projects.
		static std::filesystem::path GetDefaultLocation();

		// Registers the "StrataLauncher" settings handler with the current ImGui context (before it loads its settings),
		// which keeps the remembered location; remove it before the dialogs go away while the context lives on.
		void InstallSettingsHandler();
		void RemoveSettingsHandler();
	private:
		enum class Dialog : uint8_t
		{
			None = 0,
			NewProject,
			OpenSample
		};

		struct TemplateCard
		{
			std::string Id;
			std::string Name;
			std::string Description;
		};

		void DrawNewProject(EditorPanelContext& context);
		void DrawOpenSample(EditorPanelContext& context);
		// The location field with its Browse button, at the given width. Returns true when Enter was pressed in the field.
		bool DrawLocationField(float width);
		// The directory the dialog would create from its fields, or why it cannot (outProblem). Checks the file system only
		// when a field changed.
		std::filesystem::path GetTarget(const std::string& folderName, std::string& outProblem);

		static void* SettingsReadOpen(ImGuiContext* context, ImGuiSettingsHandler* handler, const char* name);
		static void SettingsReadLine(ImGuiContext* context, ImGuiSettingsHandler* handler, void* entry, const char* line);
		static void SettingsWriteAll(ImGuiContext* context, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buffer);
	private:
		Dialog m_Dialog = Dialog::None;
		bool m_OpenRequested = false;
		bool m_FolderPickerAvailable = false;
		std::filesystem::path m_RememberedLocation; // Empty: none yet

		// The fields of the open dialog (UTF-8).
		std::string m_Name;
		std::string m_LocationText;
		std::string m_Error; // Why the last attempt failed
		std::vector<TemplateCard> m_Templates;
		std::string m_TemplateId;
		std::string m_SampleId;
		std::string m_SampleName;
		std::string m_SampleDescription;

		// The last target check: the fields it was made for and its result.
		std::string m_CheckedName;
		std::string m_CheckedLocation;
		std::filesystem::path m_CheckedTarget;
		std::string m_CheckedProblem;

		ImGuiContext* m_SettingsContext = nullptr;
	};

}
