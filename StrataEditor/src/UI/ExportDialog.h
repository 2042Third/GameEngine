#pragma once

#include "UI/EditorPanelRegistry.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace Strata
{

	class GameExport;

	// File > Export Game... and the main toolbar's Export button: exports the open project as a playable game. The dialog
	// has the fields of project.export (the directory, with Browse; the window's width, height and fullscreen; whether to
	// copy the runtime and the script symbols; whether to prune unused assets), which start as the command's defaults, and
	// runs the command through the command runner, so the editor keeps running while the export works on an I/O thread.
	// Meanwhile it shows the export's progress as a strata-band bar with its stage and a Cancel button; afterwards what was
	// written, with Open Folder, or why it failed. Exports that a command or an agent started show the same way while the
	// dialog is open (the toolbar's "Exporting" button opens it). Closing the dialog leaves an export running; opening it
	// again shows it. Drawn by EditorLayer. Main thread only.
	class ExportDialog
	{
	public:
		static constexpr const char* c_Popup = "Export Game";

		ExportDialog();

		// Opens the dialog at the next Draw. The directory starts as the last one used, else "<project folder> Build" next
		// to the project.
		void Open(EditorPanelContext& context);
		// Every frame, inside the ImGui frame.
		void Draw(EditorPanelContext& context);
		bool IsOpen() const { return m_Open; }
		// Whether an export this dialog started still runs.
		bool IsExporting() const { return m_Exporting; }

		// Whether Browse may open the system's folder picker (FileDialogs, which needs a window).
		void SetFolderPickerAvailable(bool available) { m_FolderPickerAvailable = available; }
	private:
		void DrawFields(EditorPanelContext& context, float width);
		void DrawProgress(EditorPanelContext& context, float width);
		void DrawOutcome(float width);
		void StartExport(EditorPanelContext& context);
		// Follows the running export the dialog did not start (shown while the dialog is open) and takes its outcome.
		void WatchExports(EditorPanelContext& context);
		// Why the directory cannot receive the export (CheckExportDirectory), or empty. Checked when the text, or the project,
		// changed since the last check and when the dialog opens, not every frame: the check asks the file system, which can
		// block for a long time on a drive that went away.
		const std::string& GetDirectoryProblem(EditorPanelContext& context);
	private:
		bool m_OpenRequested = false;
		bool m_Open = false;
		bool m_FolderPickerAvailable = false;

		// The fields (UTF-8), initialized from GameExportOptions.
		std::string m_Directory;
		int m_Width;
		int m_Height;
		bool m_Fullscreen;
		bool m_IncludeRuntime;
		bool m_IncludeScriptSymbols; // Off in Dist editors, like project.export's default
		bool m_Prune;

		// What the directory check found for the text m_CheckedDirectory, in the project m_CheckedProject.
		std::optional<std::string> m_CheckedDirectory;
		std::filesystem::path m_CheckedProject;
		std::string m_DirectoryProblem;

		// The export this dialog runs or ran.
		bool m_Exporting = false;
		bool m_Finished = false;
		bool m_Succeeded = false;
		std::string m_Outcome; // What was written, or why not
		std::filesystem::path m_ExportedDirectory;
		// A running export that a command or an agent started, shown until it ends (EditorContext keeps it alive).
		std::weak_ptr<GameExport> m_Watched;
	};

}
