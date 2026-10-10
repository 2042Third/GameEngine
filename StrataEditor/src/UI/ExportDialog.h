#pragma once

#include "UI/EditorPanelRegistry.h"

#include <filesystem>
#include <string>

namespace Strata
{

	// File > Export Game... and the main toolbar's Export button: exports the open project as a playable game. The dialog
	// has the fields of project.export (the directory, with Browse; the window's width, height and fullscreen; whether to
	// copy the runtime and the script symbols; whether to prune unused assets) and runs the command through the command
	// runner, so the editor keeps running while the export works on an I/O thread. Meanwhile it shows the export's progress
	// as a strata-band bar with its stage and a Cancel button; afterwards what was written, with Open Folder, or why it
	// failed. Closing the dialog leaves an export running; opening it again shows it. Drawn by EditorLayer. Main thread only.
	class ExportDialog
	{
	public:
		static constexpr const char* c_Popup = "Export Game";

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
	private:
		bool m_OpenRequested = false;
		bool m_Open = false;
		bool m_FolderPickerAvailable = false;

		// The fields (UTF-8).
		std::string m_Directory;
		int m_Width = 1280;
		int m_Height = 720;
		bool m_Fullscreen = false;
		bool m_IncludeRuntime = true;
		bool m_IncludeScriptSymbols = true;
		bool m_Prune = false;

		// The export this dialog runs or ran.
		bool m_Exporting = false;
		bool m_Finished = false;
		bool m_Succeeded = false;
		std::string m_Outcome; // What was written, or why not
		std::filesystem::path m_ExportedDirectory;
	};

}
