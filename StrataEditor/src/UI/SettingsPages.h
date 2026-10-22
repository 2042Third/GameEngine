#pragma once

#include "UI/EditorPanelRegistry.h"

#include <Strata/Core/Base.h>

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class EditorContext;
	class SettingsRegistry;
	struct SettingsSection;

	// The two settings windows. Both draw a SettingsBrowser: the Project Settings panel (ProjectSettingsPanel) the project's own
	// pages and the registered project sections, the Preferences dialog (PreferencesDialog) the registered user sections.
	enum class SettingsWindowKind : uint8_t
	{
		Project,
		Preferences
	};

	// A page of a settings window: what it draws beside the list of pages. Every edit runs a command (project sections:
	// project.setSettings; preferences: preferences.set; the project's own fields: their commands), with
	// CommandOrigin { CommandSource::UI }. Pages draw from copies of the values they refresh when the values' version
	// changes, never parsing them per frame.
	class SettingsPage
	{
	public:
		virtual ~SettingsPage() = default;

		// Draws the page in `width`, every frame while it shows.
		virtual void Draw(EditorPanelContext& context, float width) = 0;
	};

	// The pages of complex settings sections (those without fields, which no generic page can draw), keyed by section id: the
	// editor UI registers them (EditorLayer: External Editor; Collision Layers and Input Actions with their sections).
	// Simple sections need none: a generic page draws their fields. Editor.UI.ProjectSettings checks that every complex
	// section of the process's registry has one. Main thread only.
	class EditorSettingsPageRegistry
	{
	public:
		using Factory = std::function<Scope<SettingsPage>(EditorContext& context, const SettingsSection& section, SettingsWindowKind window)>;

		// False (with the reason logged) for an empty factory or an id that has one.
		bool Register(std::string sectionId, Factory create);
		bool Has(std::string_view sectionId) const;
		// A new page for the section: its registered page, the generic page of a simple section, or null for a complex
		// section without a registered page.
		Scope<SettingsPage> CreatePage(EditorContext& context, const SettingsSection& section, SettingsWindowKind window) const;
	private:
		std::map<std::string, Factory, std::less<>> m_Factories;
	};

	// What both settings windows draw: a list of their pages on the left (grouped by category, with a search field) and the
	// selected page beside it. The Project Settings window lists the project's own pages first (Game: the start and loading
	// scenes; Export: what pruned exports always keep), then the registry's project sections; the Preferences window its
	// user sections. The registry is the editor's (EditorContext::GetSettingsRegistry). Probe keys: "<Window>.Page.<Id>"
	// for the list's entries and "<Window>.<Id>.<Field>" for fields, <Window> being ProjectSettings or Preferences.
	class SettingsBrowser
	{
	public:
		static constexpr const char* c_GamePage = "Game";
		static constexpr const char* c_ExportPage = "Export";

		explicit SettingsBrowser(SettingsWindowKind kind);
		~SettingsBrowser();

		SettingsBrowser(const SettingsBrowser&) = delete;
		SettingsBrowser& operator=(const SettingsBrowser&) = delete;

		// Draws the list and the selected page in a region of `size` at the cursor.
		void Draw(EditorPanelContext& context, const EditorSettingsPageRegistry& pages, const ImVec2& size);
		// Shows the page of a section, or a built-in page, from the next Draw on; an id the window has no page for selects its
		// first page then.
		void Select(std::string id);
		const std::string& GetSelected() const { return m_Selected; }
		// The page drawn last (null before the first Draw), e.g. for tests.
		SettingsPage* GetPage() const { return m_Page.get(); }

		// The probe key prefix of a window's items.
		static const char* GetProbePrefix(SettingsWindowKind kind);
	private:
		struct Entry
		{
			std::string Id;
			std::string Title;
			std::string Category;
			std::string Description;
			std::string SearchText;                 // What the search field matches, in lower case
			const SettingsSection* Section = nullptr; // Null for the built-in pages
		};

		void BuildEntries(const SettingsRegistry& registry);
		void DrawList(float height);
		void DrawPage(EditorPanelContext& context, const EditorSettingsPageRegistry& pages);
	private:
		SettingsWindowKind m_Kind;
		const SettingsRegistry* m_Registry = nullptr; // The registry m_Entries were built from
		std::vector<Entry> m_Entries;
		std::string m_Search;
		std::string m_Selected;
		// The page drawn: made for m_PageId, within the project of m_PageGeneration (EditorContext::GetProjectGeneration).
		Scope<SettingsPage> m_Page;
		std::string m_PageId;
		uint64_t m_PageGeneration = 0;
	};

}
