#pragma once

#include "Editor/ScriptSources.h"
#include "UI/EditorPanelRegistry.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strata
{

	// A view of the project's script sources (the code and CMake files of its script directory) as a table of files with
	// the script classes each registers, for a panel to draw where people look for files: the Content Browser's Scripts
	// root. The files are listed on an I/O thread (ScriptSourceList) when the view is first drawn and every
	// c_RefreshSeconds while it is drawn, never while it is not, so a panel that does not show it pays nothing; the rows
	// are rebuilt only when the listing or the filter changed. A double click opens a file in the person's code editor
	// (EditorContext::OpenScriptSource, like script.open); its context menu opens it, shows its folder or copies its path.
	// The actions for the panel's toolbar (New Script... through the editor's dialog, Open in IDE for the script
	// directory) are drawn by DrawActions. Main thread only.
	class ScriptBrowser
	{
	public:
		static constexpr double c_RefreshSeconds = 1.0;
		// Rows recorded in the item probe ("ScriptBrowser.Row.<n>", with the file's path as text): the first ones, for tests.
		static constexpr size_t c_ProbedRows = 8;

		// The toolbar's buttons: New Script... and Open in IDE (disabled without a project).
		void DrawActions(EditorPanelContext& context);
		// The width DrawActions takes in the current font, to place the buttons before drawing them (e.g. right-aligned).
		static float CalcActionsWidth();
		// The files whose path or classes contain every word of the filter (ignoring case), in the rest of the window.
		void Draw(EditorPanelContext& context, std::string_view filter);
		// Lists the files again when next drawn (after an action that changed them).
		void RequestRefresh() { m_RefreshRequested = true; }

		const ScriptSourceList& GetList() const { return m_List; }
		// The files shown (indices into the listing), in their order.
		const std::vector<size_t>& GetRows() const { return m_Rows; }
		// Opens a file of the listing (as a double click does). False, with the reason in GetLastOpenError, when it cannot.
		bool OpenFile(EditorPanelContext& context, std::string_view path);
		const std::string& GetLastOpenError() const { return m_LastOpenError; }

		// Where a file's row was drawn in the last frame (screen coordinates), when it was in view: for tests.
		struct FileRect
		{
			ImVec2 Min;
			ImVec2 Max;
		};
		std::optional<FileRect> FindFileRect(std::string_view path) const;
	private:
		void RebuildRows(std::string_view filter);
		void DrawFileMenu(EditorPanelContext& context, const ScriptSourceFile& file);
	private:
		ScriptSourceList m_List;
		double m_LastRefreshTime = -1.0;
		bool m_RefreshRequested = true;

		// The rows and what each shows, made when the listing or the filter changed (not every frame).
		std::vector<size_t> m_Rows;
		std::vector<std::string> m_ClassTexts; // Per file of the listing: its classes, comma-separated
		uint64_t m_RowsVersion = UINT64_MAX;
		std::string m_RowsFilter;

		std::string m_Selected; // The path of the selected file
		// The rows drawn in the last frame: indices into the listing and their rectangles.
		std::vector<std::pair<size_t, FileRect>> m_Rects;
		std::string m_LastOpenError;
	};

}
