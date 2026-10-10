#include "UI/EditorPanelRegistry.h"

#include <imgui_internal.h>

#include <charconv>
#include <cstring>
#include <map>
#include <system_error>

namespace Strata
{

	namespace
	{

		bool IsValidId(std::string_view id)
		{
			// Ids are keys of "Id=1" lines in imgui.ini and follow "###" in window names.
			return !id.empty() && id.find_first_of("=#[]\r\n") == std::string_view::npos;
		}

	}

	EditorPanelRegistry::~EditorPanelRegistry()
	{
		RemoveSettingsHandler();
	}

	bool EditorPanelRegistry::Register(EditorPanelDescriptor descriptor, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		if (!IsValidId(descriptor.Id))
			return fail("A panel id must not be empty or contain '=', '#', '[', ']' or line breaks");
		if (FindEntry(descriptor.Id))
			return fail("A panel with the id '" + descriptor.Id + "' is registered already");
		if (descriptor.Title.empty())
			return fail("The panel '" + descriptor.Id + "' has no title");
		if (!descriptor.Create)
			return fail("The panel '" + descriptor.Id + "' has no factory");

		Entry entry;
		entry.Panel = descriptor.Create();
		if (!entry.Panel)
			return fail("The factory of the panel '" + descriptor.Id + "' created nothing");
		entry.WindowName = (descriptor.Icon ? std::string(descriptor.Icon) + "  " : std::string()) + descriptor.Title + "###" + descriptor.Id;
		const auto saved = m_SavedOpenStates.find(descriptor.Id);
		entry.Open = saved != m_SavedOpenStates.end() ? saved->second : descriptor.OpenByDefault;
		entry.Descriptor = std::move(descriptor);
		m_Entries.push_back(std::move(entry));
		return true;
	}

	EditorPanelRegistry::Entry* EditorPanelRegistry::FindEntry(std::string_view id)
	{
		for (Entry& entry : m_Entries)
		{
			if (entry.Descriptor.Id == id)
				return &entry;
		}
		return nullptr;
	}

	const EditorPanelRegistry::Entry* EditorPanelRegistry::FindEntry(std::string_view id) const
	{
		for (const Entry& entry : m_Entries)
		{
			if (entry.Descriptor.Id == id)
				return &entry;
		}
		return nullptr;
	}

	EditorPanel* EditorPanelRegistry::Find(std::string_view id) const
	{
		const Entry* entry = FindEntry(id);
		return entry ? entry->Panel.get() : nullptr;
	}

	const EditorPanelDescriptor* EditorPanelRegistry::FindDescriptor(std::string_view id) const
	{
		const Entry* entry = FindEntry(id);
		return entry ? &entry->Descriptor : nullptr;
	}

	bool EditorPanelRegistry::IsOpen(std::string_view id) const
	{
		const Entry* entry = FindEntry(id);
		return entry && entry->Open;
	}

	void EditorPanelRegistry::SetOpen(std::string_view id, bool open)
	{
		Entry* entry = FindEntry(id);
		if (!entry || entry->Open == open)
			return;
		entry->Open = open;
		if (ImGui::GetCurrentContext())
			ImGui::MarkIniSettingsDirty();
	}

	void EditorPanelRegistry::ResetOpenStates()
	{
		for (Entry& entry : m_Entries)
			SetOpen(entry.Descriptor.Id, entry.Descriptor.OpenByDefault);
	}

	void EditorPanelRegistry::SetLayoutVersion(int version)
	{
		if (m_LayoutVersion == version)
			return;
		m_LayoutVersion = version;
		if (ImGui::GetCurrentContext())
			ImGui::MarkIniSettingsDirty();
	}

	void EditorPanelRegistry::Focus(std::string_view id)
	{
		Entry* entry = FindEntry(id);
		if (!entry)
			return;
		SetOpen(id, true);
		entry->FocusRequested = true;
	}

	std::string EditorPanelRegistry::GetWindowName(std::string_view id) const
	{
		const Entry* entry = FindEntry(id);
		return entry ? entry->WindowName : std::string();
	}

	void EditorPanelRegistry::OnUpdate(EditorPanelContext& context)
	{
		for (Entry& entry : m_Entries)
			entry.Panel->OnUpdate(context);
	}

	void EditorPanelRegistry::OnImGuiRender(EditorPanelContext& context)
	{
		for (Entry& entry : m_Entries)
		{
			if (!entry.Open)
			{
				entry.Panel->OnHidden(context);
				continue;
			}

			const EditorPanelWindowOptions options = entry.Panel->GetWindowOptions(context);
			if (entry.FocusRequested)
			{
				ImGui::SetNextWindowFocus();
				entry.FocusRequested = false;
			}
			if (options.NoPadding)
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
			bool open = true;
			const bool visible = ImGui::Begin(entry.WindowName.c_str(), &open, options.Flags);
			if (options.NoPadding)
				ImGui::PopStyleVar();
			if (visible)
				entry.Panel->OnImGuiRender(context);
			else
				entry.Panel->OnHidden(context);
			ImGui::End();

			// Closed with the tab's close button.
			if (!open)
			{
				SetOpen(entry.Descriptor.Id, false);
				entry.Panel->OnHidden(context);
			}
		}
	}

	void EditorPanelRegistry::DrawMenuItems()
	{
		// Panels without a menu path first, then one submenu per path (sorted), each in registration order.
		std::map<std::string, std::vector<Entry*>> submenus;
		for (Entry& entry : m_Entries)
		{
			if (!entry.Descriptor.MenuPath.empty())
			{
				submenus[entry.Descriptor.MenuPath].push_back(&entry);
				continue;
			}
			if (ImGui::MenuItem(entry.Descriptor.Title.c_str(), nullptr, entry.Open))
				SetOpen(entry.Descriptor.Id, !entry.Open);
		}
		for (const auto& [path, entries] : submenus)
		{
			if (!ImGui::BeginMenu(path.c_str()))
				continue;
			for (Entry* entry : entries)
			{
				if (ImGui::MenuItem(entry->Descriptor.Title.c_str(), nullptr, entry->Open))
					SetOpen(entry->Descriptor.Id, !entry->Open);
			}
			ImGui::EndMenu();
		}
	}

	void EditorPanelRegistry::OnDetach(EditorPanelContext& context)
	{
		for (Entry& entry : m_Entries)
			entry.Panel->OnDetach(context);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Settings (imgui.ini)
	////////////////////////////////////////////////////////////////////////////////

	void EditorPanelRegistry::InstallSettingsHandler()
	{
		ImGuiContext* context = ImGui::GetCurrentContext();
		if (!context)
			return;
		ImGui::RemoveSettingsHandler(c_SettingsName);
		ImGuiSettingsHandler handler;
		handler.TypeName = c_SettingsName;
		handler.TypeHash = ImHashStr(c_SettingsName);
		handler.ReadOpenFn = &EditorPanelRegistry::SettingsReadOpen;
		handler.ReadLineFn = &EditorPanelRegistry::SettingsReadLine;
		handler.WriteAllFn = &EditorPanelRegistry::SettingsWriteAll;
		handler.UserData = this;
		ImGui::AddSettingsHandler(&handler);
		m_SettingsContext = context;
	}

	void EditorPanelRegistry::RemoveSettingsHandler()
	{
		// Only this registry's handler, and only while its context is current (it may be gone already, e.g. when the
		// application destroyed ImGui before the editor detached).
		if (m_SettingsContext && ImGui::GetCurrentContext() == m_SettingsContext)
		{
			const ImGuiSettingsHandler* handler = ImGui::FindSettingsHandler(c_SettingsName);
			if (handler && handler->UserData == this)
				ImGui::RemoveSettingsHandler(c_SettingsName);
		}
		m_SettingsContext = nullptr;
	}

	void EditorPanelRegistry::ReadSetting(std::string_view id, bool open)
	{
		m_SavedOpenStates[std::string(id)] = open;
		if (Entry* entry = FindEntry(id))
			entry->Open = open;
	}

	void EditorPanelRegistry::WriteSettings(ImGuiTextBuffer& buffer) const
	{
		if (const int version = m_LayoutVersion != 0 ? m_LayoutVersion : m_SavedLayoutVersion; version != 0)
			buffer.appendf("[%s][Layout]\nVersion=%d\n\n", c_SettingsName, version);
		buffer.appendf("[%s][Open]\n", c_SettingsName);
		// Panels this run did not register keep their saved state.
		std::map<std::string, bool> states(m_SavedOpenStates.begin(), m_SavedOpenStates.end());
		for (const Entry& entry : m_Entries)
			states[entry.Descriptor.Id] = entry.Open;
		for (const auto& [id, open] : states)
			buffer.appendf("%s=%d\n", id.c_str(), open ? 1 : 0);
		buffer.append("\n");
	}

	void* EditorPanelRegistry::SettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler* handler, const char* name)
	{
		// Two sections: "[StrataPanels][Open]" (the registry itself as the entry) and "[StrataPanels][Layout]" (its saved
		// layout version). ImGui skips the lines of sections it gets no entry for.
		EditorPanelRegistry* registry = static_cast<EditorPanelRegistry*>(handler->UserData);
		if (std::strcmp(name, "Open") == 0)
			return registry;
		if (std::strcmp(name, "Layout") == 0)
			return &registry->m_SavedLayoutVersion;
		return nullptr;
	}

	void EditorPanelRegistry::SettingsReadLine(ImGuiContext*, ImGuiSettingsHandler* handler, void* entry, const char* line)
	{
		EditorPanelRegistry* registry = static_cast<EditorPanelRegistry*>(handler->UserData);
		const std::string_view text(line);
		const size_t equals = text.find('=');
		if (equals == std::string_view::npos)
			return;
		const std::string_view key = text.substr(0, equals);
		const std::string_view value = text.substr(equals + 1);
		if (entry == &registry->m_SavedLayoutVersion)
		{
			int version = 0;
			const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), version);
			if (key == "Version" && error == std::errc() && end == value.data() + value.size() && version > 0)
				registry->m_SavedLayoutVersion = version;
			return;
		}
		if (!IsValidId(key) || (value != "0" && value != "1"))
			return;
		registry->ReadSetting(key, value == "1");
	}

	void EditorPanelRegistry::SettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buffer)
	{
		static_cast<const EditorPanelRegistry*>(handler->UserData)->WriteSettings(*buffer);
	}

}
