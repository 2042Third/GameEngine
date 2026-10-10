#pragma once

#include "UI/EditorShell.h"
#include "UI/Markdown.h"

#include <string>
#include <utility>
#include <vector>

namespace Strata
{

	// Help > About Strata: the mark, the version and the commit of the build, the GPU (adapter, driver, graphics API
	// version), the measured startup time and the third-party notices (ThirdPartyNotices.md, compiled into the editor),
	// with a button that copies the details for a bug report. Drawn by EditorLayer. Main thread only.
	class AboutDialog
	{
	public:
		static constexpr const char* c_Popup = "About Strata";

		// Opens the dialog at the next Draw.
		void Open() { m_OpenRequested = true; }
		// Every frame, inside the ImGui frame.
		void Draw(const EditorEnvironment& environment);
		bool IsOpen() const { return m_Open; }

		// The facts the dialog lists, as (label, value) rows: Version, Commit, Platform, GPU, Driver, Graphics API, Startup,
		// UI scale.
		static std::vector<std::pair<std::string, std::string>> DescribeEnvironment(const EditorEnvironment& environment);
		// The rows as text, one "Label: value" per line (what Copy Details puts on the clipboard).
		static std::string FormatDetails(const EditorEnvironment& environment);
		// ThirdPartyNotices.md as compiled into the editor.
		static std::string_view GetThirdPartyNotices();
	private:
		bool m_OpenRequested = false;
		bool m_Open = false;
		std::vector<UI::MarkdownBlock> m_Notices; // Parsed when the dialog first opens
	};

}
