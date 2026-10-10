#pragma once

#include <Strata/Core/Base.h>

#include <imgui.h>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ImGuiSettingsHandler;

namespace Strata
{

	class EditorCommandRegistry;
	class EditorCommandRunner;
	class EditorContext;

	// What panels work with: the editor state, the commands (every change goes through them) and the runner (commands
	// that take frames).
	struct EditorPanelContext
	{
		EditorContext& Context;
		const EditorCommandRegistry& Commands;
		EditorCommandRunner& Runner;
	};

	// How the registry begins a panel's window this frame.
	struct EditorPanelWindowOptions
	{
		ImGuiWindowFlags Flags = ImGuiWindowFlags_None;
		bool NoPadding = false; // The contents reach the window's edges (e.g. the viewport image)
	};

	// A dockable editor window. The registry owns the window: it begins it with the panel's title and open state and calls
	// the panel to draw the contents while the window is visible.
	class EditorPanel
	{
	public:
		virtual ~EditorPanel() = default;

		// Every frame before the panels are drawn, also while the panel is closed or hidden (e.g. to collect what the status
		// bar shows about it).
		virtual void OnUpdate([[maybe_unused]] EditorPanelContext& context) {}
		// Flags and options for this frame's window, asked before it begins.
		virtual EditorPanelWindowOptions GetWindowOptions([[maybe_unused]] EditorPanelContext& context) { return {}; }
		// Draws the contents of the panel's window (between ImGui::Begin and End) while it is visible.
		virtual void OnImGuiRender(EditorPanelContext& context) = 0;
		// The window is closed, collapsed or behind another tab this frame: end interactions that hold state.
		virtual void OnHidden([[maybe_unused]] EditorPanelContext& context) {}
		// The editor is closing. ImGui may be gone already: no ImGui calls.
		virtual void OnDetach([[maybe_unused]] EditorPanelContext& context) {}
		// Whether the panel needs frames at the full rate now (a drag or an animation in progress), so the editor does not
		// throttle while it lasts.
		virtual bool IsAnimating() const { return false; }
	};

	struct EditorPanelDescriptor
	{
		std::string Id;             // Stable and unique: the ImGui id of the window and the key of its open state in imgui.ini
		std::string Title;          // Shown on the tab and in the View menu
		const char* Icon = nullptr; // An Icons:: constant shown before the title (optional)
		std::string MenuPath;       // Submenu of the View menu the toggle lives in ("" for the menu itself, "Debug" for View > Debug)
		bool OpenByDefault = true;
		std::function<Scope<EditorPanel>()> Create;
	};

	// The editor's panels. The built-in panels (RegisterBuiltinPanels in EditorLayer) and others register a descriptor;
	// the registry creates the panel, draws the open ones in their windows each frame, toggles them from the View menu and
	// keeps which ones are open in imgui.ini (the "StrataPanels" settings). Main thread only.
	class EditorPanelRegistry
	{
	public:
		EditorPanelRegistry() = default;
		~EditorPanelRegistry();

		EditorPanelRegistry(const EditorPanelRegistry&) = delete;
		EditorPanelRegistry& operator=(const EditorPanelRegistry&) = delete;

		// Adds a panel and creates it. Fails (false, with the reason) for an empty or taken id, an id with characters that
		// cannot be stored in imgui.ini ('=', '#', line breaks), an empty title, a missing factory or a factory that
		// returns nothing. An open state saved in imgui.ini for the id wins over OpenByDefault.
		bool Register(EditorPanelDescriptor descriptor, std::string* outError = nullptr);

		EditorPanel* Find(std::string_view id) const;
		template<typename T>
		T* Get(std::string_view id) const
		{
			return dynamic_cast<T*>(Find(id));
		}
		const EditorPanelDescriptor* FindDescriptor(std::string_view id) const;
		size_t GetCount() const { return m_Entries.size(); }
		const EditorPanelDescriptor& GetDescriptor(size_t index) const { return m_Entries[index].Descriptor; }

		bool IsOpen(std::string_view id) const;
		void SetOpen(std::string_view id, bool open);
		// Opens the panels that are open by default and closes the others (with a layout reset).
		void ResetOpenStates();
		// Opens the panel if needed and brings its window to the front (selecting its tab) on the next frame.
		void Focus(std::string_view id);
		// Selects the panel's tab in its dock node without giving it the keyboard focus (layouts).
		void SelectTab(std::string_view id, ImGuiID dockNodeId) const;

		// The ImGui window name of a panel: its icon and title, then "###" and its id, so the window keeps its identity
		// (docking, settings) whatever its title. Empty for an unknown id.
		std::string GetWindowName(std::string_view id) const;

		// Each frame: OnUpdate of every panel, then (within an ImGui frame) the windows of the open panels.
		void OnUpdate(EditorPanelContext& context);
		void OnImGuiRender(EditorPanelContext& context);
		// One menu item per panel to show or hide it, in submenus by MenuPath (for the View menu).
		void DrawMenuItems();
		bool IsAnyAnimating() const;
		// Before the editor goes away (no ImGui calls).
		void OnDetach(EditorPanelContext& context);

		// Registers the "StrataPanels" settings handler with the current ImGui context, which then reads the saved open
		// states when it loads its settings (at its first frame) and writes them when it saves. Remove it before the
		// registry goes away while the context lives on.
		void InstallSettingsHandler();
		void RemoveSettingsHandler();
		// The version of the dock layout the settings were saved with (0 when none was saved), so an editor whose default
		// layout changed can replace an outdated one; SetLayoutVersion records the version of the layout in use.
		int GetSavedLayoutVersion() const { return m_SavedLayoutVersion; }
		void SetLayoutVersion(int version);

		static constexpr const char* c_SettingsName = "StrataPanels";
	private:
		struct Entry
		{
			EditorPanelDescriptor Descriptor;
			Scope<EditorPanel> Panel;
			std::string WindowName;
			bool Open = true;
			bool FocusRequested = false;
		};

		Entry* FindEntry(std::string_view id);
		const Entry* FindEntry(std::string_view id) const;
		void ReadSetting(std::string_view id, bool open);
		void WriteSettings(ImGuiTextBuffer& buffer) const;
		static void* SettingsReadOpen(ImGuiContext* context, ImGuiSettingsHandler* handler, const char* name);
		static void SettingsReadLine(ImGuiContext* context, ImGuiSettingsHandler* handler, void* entry, const char* line);
		static void SettingsWriteAll(ImGuiContext* context, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buffer);
	private:
		std::vector<Entry> m_Entries;
		// Open states read from imgui.ini, also for panels that register later.
		std::unordered_map<std::string, bool> m_SavedOpenStates;
		int m_SavedLayoutVersion = 0;
		int m_LayoutVersion = 0;
		ImGuiContext* m_SettingsContext = nullptr;
	};

}
