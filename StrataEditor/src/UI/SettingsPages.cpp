#include "UI/SettingsPages.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorPreferences.h"
#include "UI/EditorActions.h"
#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/PropertyWidgets.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/Property.h>
#include <Strata/Reflection/PropertyJson.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace Strata
{

	namespace
	{

		// The list of pages, in text heights.
		constexpr float c_ListWidthInFontSizes = 11.0f;

		void Caption(std::string_view text, const ImVec4& color, float width)
		{
			UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
			ImGui::TextUnformatted(text.data(), text.data() + text.size());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}

		// What a page reads its values with: the project's settings or the preferences, with the version they change with.
		struct SectionSource
		{
			const nlohmann::json* Value = nullptr; // Null: the default applies
			bool Stored = false;
			uint64_t Version = 0;
		};

		SectionSource ReadSection(const EditorContext& context, SettingsWindowKind kind, std::string_view id)
		{
			SectionSource source;
			if (kind == SettingsWindowKind::Preferences)
			{
				const EditorPreferences& preferences = context.GetPreferences();
				source.Value = preferences.FindSection(id);
				source.Stored = preferences.IsStored(id);
				source.Version = preferences.GetVersion();
			}
			else if (context.HasProject())
			{
				const ProjectSettingsValues& values = context.GetProject()->GetSettings();
				source.Value = values.FindSection(id);
				source.Stored = values.IsStored(id);
				source.Version = values.GetVersion();
			}
			return source;
		}

		// A simple section's field as the property widgets draw it.
		PropertyInfo MakeProperty(const SettingsField& field)
		{
			PropertyInfo property;
			property.Name = field.Name;
			property.DisplayName = field.Title;
			property.Tooltip = field.Description;
			switch (field.Type)
			{
				case SettingsFieldType::Bool:
					property.Type = PropertyType::Bool;
					break;
				case SettingsFieldType::Int:
					property.Type = PropertyType::Int;
					// The widget edits 32-bit numbers: a wider range is cut to theirs (the section still validates the value).
					property.Min = static_cast<float>(std::max(field.Min, static_cast<double>(std::numeric_limits<int32_t>::min())));
					property.Max = static_cast<float>(std::min(field.Max, static_cast<double>(std::numeric_limits<int32_t>::max())));
					break;
				case SettingsFieldType::Float:
					property.Type = PropertyType::Float;
					property.Min = static_cast<float>(field.Min);
					property.Max = static_cast<float>(field.Max);
					break;
				case SettingsFieldType::String:
					property.Type = PropertyType::String;
					break;
				case SettingsFieldType::Enum:
					property.Type = PropertyType::Enum;
					for (size_t index = 0; index < field.Options.size(); index++)
						property.EnumValues.push_back({ field.Options[index], static_cast<int32_t>(index), {} });
					break;
				case SettingsFieldType::Asset:
					property.Type = PropertyType::Asset;
					property.AssetFilter = AssetTypeFromString(field.AssetType).value_or(AssetType::None);
					break;
			}
			return property;
		}

		// A field's value of a validated section value as its widget edits it.
		PropertyValue ToPropertyValue(const SettingsField& field, const PropertyInfo& property, const nlohmann::json& value)
		{
			if (field.Type == SettingsFieldType::Asset && value.is_null())
				return UUID::Null();
			if (field.Type == SettingsFieldType::Int && value.is_number())
			{
				const double number = value.get<double>();
				return static_cast<int32_t>(std::clamp(number, static_cast<double>(std::numeric_limits<int32_t>::min()),
					static_cast<double>(std::numeric_limits<int32_t>::max())));
			}
			if (std::optional<PropertyValue> parsed = PropertyValueFromJson(property, value))
				return std::move(*parsed);
			return GetDefaultPropertyValue(property.Type);
		}

		nlohmann::json ToJsonValue(const SettingsField& field, const PropertyInfo& property, const PropertyValue& value)
		{
			if (field.Type == SettingsFieldType::Asset)
			{
				const UUID handle = std::get<UUID>(value);
				return handle.IsValid() ? UUIDToJson(handle) : nlohmann::json(nullptr);
			}
			return PropertyValueToJson(property, value);
		}

		// The page of a simple section: its fields in a property table, each edit running the section's command with the whole
		// value (the others as they are).
		class GenericSettingsPage final : public SettingsPage
		{
		public:
			GenericSettingsPage(const SettingsSection& section, SettingsWindowKind kind)
				: m_Section(section), m_Kind(kind)
			{
				for (const SettingsField& field : section.Fields)
					m_Properties.push_back(MakeProperty(field));
			}

			void Draw(EditorPanelContext& context, float width) override
			{
				Refresh(context.Context);
				const UI::ThemeColors& colors = UI::GetThemeColors();
				const std::string prefix = fmt::format("{}.{}", SettingsBrowser::GetProbePrefix(m_Kind), m_Section.Id);
				if (!m_Problem.empty())
					Caption(fmt::format("The stored value is not used (its default applies until it is changed): {}", m_Problem), colors.Warning, width);

				std::optional<size_t> edited;
				if (UI::BeginPropertyTable("Fields"))
				{
					for (size_t index = 0; index < m_Section.Fields.size(); index++)
					{
						const std::string probe = fmt::format("{}.{}", prefix, m_Section.Fields[index].Name);
						UI::PropertyLabel(m_Properties[index].DisplayName, m_Properties[index].Tooltip, probe);
						PropertyWidgetOptions options;
						options.ProbeKey = probe;
						ImGui::PushID(static_cast<int>(index));
						const PropertyEditResult result = DrawPropertyWidget(m_Properties[index], m_Values[index], *context.Context.GetActiveScene(), options);
						ImGui::PopID();
						// A drag or a text edit shows its value as it goes and runs the command once, when it ends.
						if (result.Finished)
							edited = index;
					}
					UI::EndPropertyTable();
				}
				if (edited)
				{
					nlohmann::json value = m_Value;
					value[m_Section.Fields[*edited].Name] = ToJsonValue(m_Section.Fields[*edited], m_Properties[*edited], m_Values[*edited]);
					Apply(context, { { "section", m_Section.Id }, { "value", std::move(value) } });
				}

				ImGui::Spacing();
				if (UI::DialogButton(fmt::format("{}.Reset", prefix).c_str(), "Reset to Defaults", false, m_Stored))
					Apply(context, { { "section", m_Section.Id }, { "reset", true } });
			}
		private:
			void Refresh(const EditorContext& context)
			{
				const SectionSource source = ReadSection(context, m_Kind, m_Section.Id);
				if (m_Version && *m_Version == source.Version)
					return;
				m_Version = source.Version;
				m_Stored = source.Stored;
				m_Value = source.Value ? *source.Value : m_Section.GetDefault();
				m_Problem.clear();
				if (source.Stored && !source.Value)
					m_Problem = FindProblem(context);
				m_Values.clear();
				for (size_t index = 0; index < m_Section.Fields.size(); index++)
				{
					const nlohmann::json* member = JsonUtils::Find(m_Value, m_Section.Fields[index].Name);
					m_Values.push_back(ToPropertyValue(m_Section.Fields[index], m_Properties[index], member ? *member : m_Section.Fields[index].Default));
				}
			}

			// Why the stored value is refused (read when the values change, not per frame).
			std::string FindProblem(const EditorContext& context) const
			{
				if (m_Kind == SettingsWindowKind::Preferences)
					return context.GetPreferences().FindProblem(m_Section.Id).value_or(std::string());
				const nlohmann::json entry = context.GetProject()->GetSettings().GetStoredEntry(m_Section.Id);
				std::string error;
				if (entry.is_object() && entry.contains("Value") && entry.contains("Version") && entry["Version"].is_number_unsigned())
					m_Section.Validate(entry["Value"], entry["Version"].get<uint32_t>(), &error);
				return error;
			}

			void Apply(EditorPanelContext& context, const nlohmann::json& parameters)
			{
				const char* command = m_Kind == SettingsWindowKind::Project ? "project.setSettings" : "preferences.set";
				RunEditorCommand(context.Context, context.Commands, command, parameters);
				// Read again: the value the command applied, or the one that stayed when it refused.
				m_Version.reset();
			}
		private:
			const SettingsSection& m_Section;
			SettingsWindowKind m_Kind;
			std::vector<PropertyInfo> m_Properties;
			std::vector<PropertyValue> m_Values; // One per field, as the widgets edit them
			nlohmann::json m_Value;              // The section's value they were read from
			std::optional<uint64_t> m_Version;   // Of the values m_Value was read from
			bool m_Stored = false;
			std::string m_Problem;
		};

		// The project's own fields: the scene the game starts with and the one it plays while that loads.
		class GameSettingsPage final : public SettingsPage
		{
		public:
			GameSettingsPage()
			{
				m_StartScene.Name = "StartScene";
				m_StartScene.DisplayName = "Start Scene";
				m_StartScene.Tooltip = "The scene the game starts with (project.setStartScene)";
				m_StartScene.Type = PropertyType::Asset;
				m_StartScene.AssetFilter = AssetType::Scene;
				m_LoadingScene = m_StartScene;
				m_LoadingScene.Name = "LoadingScene";
				m_LoadingScene.DisplayName = "Loading Scene";
				m_LoadingScene.Tooltip = "A scene the exported game plays while its start scene loads; none: a cleared frame meanwhile "
					"(project.setExportSettings)";
			}

			void Draw(EditorPanelContext& context, float width) override
			{
				EditorContext& editor = context.Context;
				if (!editor.HasProject())
					return;
				const ProjectConfig& config = editor.GetProject()->GetConfig();
				PropertyValue startScene = config.StartScene;
				PropertyValue loadingScene = config.Export.LoadingScene;
				Scene& scene = *editor.GetActiveScene();
				if (UI::BeginPropertyTable("Game"))
				{
					UI::PropertyLabel(m_StartScene.DisplayName, m_StartScene.Tooltip, "ProjectSettings.Game.StartScene");
					PropertyWidgetOptions options;
					options.ProbeKey = "ProjectSettings.Game.StartScene";
					const bool startChanged = DrawPropertyWidget(m_StartScene, startScene, scene, options).Changed;
					UI::PropertyLabel(m_LoadingScene.DisplayName, m_LoadingScene.Tooltip, "ProjectSettings.Game.LoadingScene");
					options.ProbeKey = "ProjectSettings.Game.LoadingScene";
					const bool loadingChanged = DrawPropertyWidget(m_LoadingScene, loadingScene, scene, options).Changed;
					UI::EndPropertyTable();
					// A game needs a start scene: the picker's None changes nothing.
					const UUID start = std::get<UUID>(startScene);
					if (startChanged && start.IsValid())
						RunEditorCommand(editor, context.Commands, "project.setStartScene", { { "scene", UUIDToJson(start) } });
					const UUID loading = std::get<UUID>(loadingScene);
					if (loadingChanged)
					{
						RunEditorCommand(editor, context.Commands, "project.setExportSettings",
							{ { "loadingScene", loading.IsValid() ? UUIDToJson(loading) : nlohmann::json(nullptr) } });
					}
				}
				ImGui::Spacing();
				Caption("The game's start scene opens first when the project opens and when the exported game starts. The loading scene "
					"plays while the start scene loads in the exported game.", UI::GetThemeColors().TextSecondary, width);
			}
		private:
			PropertyInfo m_StartScene;
			PropertyInfo m_LoadingScene;
		};

		// What pruned exports keep although no scene uses it: Export.AlwaysInclude.
		class ExportSettingsPage final : public SettingsPage
		{
		public:
			void Draw(EditorPanelContext& context, float width) override
			{
				EditorContext& editor = context.Context;
				if (!editor.HasProject())
					return;
				const UI::ThemeColors& colors = UI::GetThemeColors();
				const std::vector<std::string>& included = editor.GetProject()->GetConfig().Export.AlwaysInclude;
				Caption("Pruned exports pack only what the scenes use. Assets and folders listed here go in too, with what they use: assets "
					"scripts load by path.", colors.TextSecondary, width);
				ImGui::Spacing();
				std::optional<size_t> removed;
				for (size_t index = 0; index < included.size(); index++)
				{
					ImGui::PushID(static_cast<int>(index));
					if (UI::IconButton("Remove", Icons::X, "Remove from the list"))
						removed = index;
					UI::ItemProbe::Record(UI::NumberedProbeKey("ProjectSettings.Export.Remove.", index));
					ImGui::SameLine();
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(included[index].c_str());
					UI::ItemProbe::Record(UI::NumberedProbeKey("ProjectSettings.Export.AlwaysInclude.", index), true, 0, included[index]);
					ImGui::PopID();
				}
				if (included.empty())
					Caption("Nothing is listed.", colors.TextDisabled, width);
				ImGui::Spacing();
				const float addWidth = UI::DialogButtonWidth("Add");
				UI::TextField("ProjectSettings.Export.Path", "Asset or folder", m_Path, "Prefabs/Enemy.stprefab",
					std::max(width - addWidth - ImGui::GetStyle().ItemSpacing.x, 1.0f));
				const bool submitted = ImGui::IsItemDeactivatedAfterEdit() && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
				ImGui::SameLine();
				const bool add = UI::DialogButton("ProjectSettings.Export.Add", "Add", false, !m_Path.empty()) || (submitted && !m_Path.empty());

				std::optional<std::vector<std::string>> list;
				if (removed)
				{
					list = included;
					list->erase(list->begin() + static_cast<std::ptrdiff_t>(*removed));
				}
				else if (add)
				{
					list = included;
					list->push_back(m_Path);
				}
				// The command checks each path (an asset or a folder of assets); a refused one stays in the field.
				if (list && !RunEditorCommand(editor, context.Commands, "project.setExportSettings", { { "alwaysInclude", *list } }).is_null() && add)
					m_Path.clear();
			}
		private:
			std::string m_Path;
		};

	}

	////////////////////////////////////////////////////////////////////////////////
	// EditorSettingsPageRegistry
	////////////////////////////////////////////////////////////////////////////////

	bool EditorSettingsPageRegistry::Register(std::string sectionId, Factory create)
	{
		if (!create || sectionId.empty())
		{
			ST_ERROR("A settings page needs a section id and a factory ('{}')", sectionId);
			return false;
		}
		if (m_Factories.contains(sectionId))
		{
			ST_ERROR("The settings section '{}' has a page already", sectionId);
			return false;
		}
		m_Factories.emplace(std::move(sectionId), std::move(create));
		return true;
	}

	bool EditorSettingsPageRegistry::Has(std::string_view sectionId) const
	{
		return m_Factories.find(sectionId) != m_Factories.end();
	}

	Scope<SettingsPage> EditorSettingsPageRegistry::CreatePage(EditorContext& context, const SettingsSection& section, SettingsWindowKind window) const
	{
		if (const auto it = m_Factories.find(section.Id); it != m_Factories.end())
			return it->second(context, section, window);
		if (section.IsSimple())
			return CreateScope<GenericSettingsPage>(section, window);
		return nullptr;
	}

	////////////////////////////////////////////////////////////////////////////////
	// SettingsBrowser
	////////////////////////////////////////////////////////////////////////////////

	SettingsBrowser::SettingsBrowser(SettingsWindowKind kind)
		: m_Kind(kind)
	{
	}

	SettingsBrowser::~SettingsBrowser() = default;

	const char* SettingsBrowser::GetProbePrefix(SettingsWindowKind kind)
	{
		return kind == SettingsWindowKind::Project ? "ProjectSettings" : "Preferences";
	}

	void SettingsBrowser::Select(std::string id)
	{
		m_Selected = std::move(id);
	}

	void SettingsBrowser::BuildEntries(const SettingsRegistry& registry)
	{
		m_Registry = &registry;
		m_Entries.clear();
		if (m_Kind == SettingsWindowKind::Project)
		{
			m_Entries.push_back({ c_GamePage, "Game", "Project", "The scene the game starts with and the one it plays while that loads.", {}, nullptr });
			m_Entries.push_back({ c_ExportPage, "Export", "Project", "What pruned exports keep although no scene uses it.", {}, nullptr });
		}
		const SettingsScope scope = m_Kind == SettingsWindowKind::Project ? SettingsScope::Project : SettingsScope::User;
		// Grouped by category, the categories in the order their first section registered.
		std::vector<std::string> categories;
		for (const SettingsSection& section : registry.GetAll())
		{
			if (section.Scope == scope && std::find(categories.begin(), categories.end(), section.Category) == categories.end())
				categories.push_back(section.Category);
		}
		for (const std::string& category : categories)
		{
			for (const SettingsSection& section : registry.GetAll())
			{
				if (section.Scope == scope && section.Category == category)
					m_Entries.push_back({ section.Id, section.Title, section.Category, section.Description, {}, &section });
			}
		}
		for (Entry& entry : m_Entries)
		{
			entry.SearchText = StringUtils::ToLower(fmt::format("{} {}", entry.Title, entry.Category));
			if (entry.Section)
			{
				for (const SettingsField& field : entry.Section->Fields)
					entry.SearchText += " " + StringUtils::ToLower(field.Title);
			}
		}
	}

	void SettingsBrowser::Draw(EditorPanelContext& context, const EditorSettingsPageRegistry& pages, const ImVec2& size)
	{
		const SettingsRegistry& registry = context.Context.GetSettingsRegistry();
		if (m_Registry != &registry)
			BuildEntries(registry);
		if (m_Entries.empty())
		{
			Caption("Nothing to set: no module registered settings of this kind.", UI::GetThemeColors().TextSecondary, size.x);
			return;
		}
		if (std::none_of(m_Entries.begin(), m_Entries.end(), [this](const Entry& entry) { return entry.Id == m_Selected; }))
			m_Selected = m_Entries.front().Id;

		DrawList(size.y);
		ImGui::SameLine();
		const ImGuiStyle& style = ImGui::GetStyle();
		// Sized explicitly: the Preferences dialog sizes itself to its content.
		const float pageWidth = std::max(size.x - ImGui::GetFontSize() * c_ListWidthInFontSizes - style.ItemSpacing.x, 1.0f);
		if (ImGui::BeginChild("##Page", ImVec2(pageWidth, size.y), ImGuiChildFlags_AlwaysUseWindowPadding))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, style.ItemSpacing.y * 1.5f));
			DrawPage(context, pages);
			ImGui::PopStyleVar();
		}
		ImGui::EndChild();
	}

	void SettingsBrowser::DrawList(float height)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const char* prefix = GetProbePrefix(m_Kind);
		const float width = ImGui::GetFontSize() * c_ListWidthInFontSizes;
		if (ImGui::BeginChild("##Pages", ImVec2(width, height), ImGuiChildFlags_None))
		{
			UI::SearchField(fmt::format("{}.Search", prefix).c_str(), m_Search, "Search", -1.0f);
			const std::string search = StringUtils::ToLower(m_Search);
			const std::string* category = nullptr;
			for (const Entry& entry : m_Entries)
			{
				if (!search.empty() && entry.SearchText.find(search) == std::string::npos)
					continue;
				if (!category || *category != entry.Category)
				{
					category = &entry.Category;
					ImGui::Spacing();
					ImGui::PushStyleColor(ImGuiCol_Text, colors.TextSecondary);
					ImGui::TextUnformatted(entry.Category.c_str());
					ImGui::PopStyleColor();
				}
				const bool selected = entry.Id == m_Selected;
				ImGui::PushID(entry.Id.c_str());
				UI::PushSelectionColors(selected);
				if (ImGui::Selectable(entry.Title.c_str(), selected, ImGuiSelectableFlags_NoAutoClosePopups))
					m_Selected = entry.Id;
				UI::PopSelectionColors();
				ImGui::PopID();
				UI::ItemProbe::Record(fmt::format("{}.Page.{}", prefix, entry.Id), true, 0, entry.Title);
			}
		}
		ImGui::EndChild();
	}

	void SettingsBrowser::DrawPage(EditorPanelContext& context, const EditorSettingsPageRegistry& pages)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const auto entry = std::find_if(m_Entries.begin(), m_Entries.end(), [this](const Entry& candidate) { return candidate.Id == m_Selected; });
		if (entry == m_Entries.end())
			return;
		const uint64_t generation = context.Context.GetProjectGeneration();
		if (!m_Page || m_PageId != entry->Id || m_PageGeneration != generation)
		{
			m_PageId = entry->Id;
			m_PageGeneration = generation;
			if (entry->Id == c_GamePage)
				m_Page = CreateScope<GameSettingsPage>();
			else if (entry->Id == c_ExportPage)
				m_Page = CreateScope<ExportSettingsPage>();
			else
				m_Page = pages.CreatePage(context.Context, *entry->Section, m_Kind);
		}

		const float width = ImGui::GetContentRegionAvail().x;
		UI::Heading(entry->Title);
		UI::ItemProbe::Record(fmt::format("{}.Title", GetProbePrefix(m_Kind)), true, 0, entry->Title);
		Caption(entry->Description, colors.TextSecondary, width);
		ImGui::Spacing();
		if (m_Kind == SettingsWindowKind::Project && !context.Context.HasProject())
		{
			Caption("No project is open: project settings belong to a project.", colors.TextSecondary, width);
			return;
		}
		if (!m_Page)
		{
			Caption("This section has no page in this editor: set it with project.setSettings or preferences.set.", colors.Warning, width);
			return;
		}
		ImGui::PushID(entry->Id.c_str());
		m_Page->Draw(context, width);
		ImGui::PopID();
	}

}
