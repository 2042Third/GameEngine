#include "UI/GraphicsQualityPage.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorViewport.h"
#include "Editor/ViewportRenderer.h"
#include "UI/EditorActions.h"
#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/PropertyWidgets.h"
#include "UI/SettingsFieldProperty.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/FrameProfiler.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/Property.h>
#include <Strata/Renderer/SceneRenderer.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strata
{

	namespace
	{

		// The table's columns after the setting's: the four presets, then Custom.
		constexpr size_t c_PresetColumns = c_RendererQualityPresetCount;
		constexpr size_t c_Columns = c_PresetColumns + 1;
		constexpr const char* c_ColumnNames[c_Columns] = { "Low", "Medium", "High", "Ultra", "Custom" };
		constexpr const char* c_ColumnIcons[c_Columns] = { Icons::SignalLow, Icons::SignalMedium, Icons::SignalHigh, Icons::Signal,
			Icons::SlidersHorizontal };
		constexpr float c_ColumnWidthInFontSizes = 7.0f;
		// The GPU line's times change every frame: they are formatted again at most this often.
		constexpr double c_GPULineSeconds = 0.5;
		constexpr glm::uvec2 c_Output1080 = { 1920, 1080 };
		constexpr glm::uvec2 c_Output2160 = { 3840, 2160 };

		constexpr const char* c_Caption = "Every renderer of the project follows this: the editor's views (unless a view chooses its own quality), "
			"captures and the exported game. Players can change it in the game's options (Graphics). Medium has Strata 0.6.0's costs; High is "
			"the default. Nothing here lowers the rendering resolution.";
		constexpr const char* c_MemoryCaption = "Shadow maps and atlas of one view. Each renderer holds its own: the editor's Scene and Game views and "
			"its captures, the exported game one. Shadow maps exist only while a light casts shadows.";
		constexpr const char* c_ChoicesCaption = "The presets leave these alone: the game's look keeps them, and players may turn them off.";

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

		std::string FormatBytes(uint64_t bytes)
		{
			constexpr uint64_t c_GiB = uint64_t { 1 } << 30;
			if (bytes < c_GiB)
				return fmt::format("{} MiB", bytes >> 20);
			// Up to two decimals, at least one: "1.0 GiB", "1.25 GiB".
			std::string text = fmt::format("{:.2f}", static_cast<double>(bytes) / static_cast<double>(c_GiB));
			if (text.back() == '0')
				text.pop_back();
			return text + " GiB";
		}

		// A row's value as its preset cell shows it: an option's name (the automatic shadow resolutions with the sizes they
		// resolve to, by step 0's rule), a number (up to two decimals), On or Off.
		std::string FormatValue(const SettingsField& field, const nlohmann::json& value, bool engineModel)
		{
			if (value.is_boolean())
				return value.get<bool>() ? "On" : "Off";
			if (value.is_number_integer() || value.is_number_unsigned())
				return value.dump();
			if (value.is_number())
			{
				std::string text = fmt::format("{:.2f}", value.get<double>());
				while (text.back() == '0')
					text.pop_back();
				if (text.back() == '.')
					text.pop_back();
				return text;
			}
			if (!value.is_string())
				return value.dump();
			const std::string& text = value.get_ref<const std::string&>();
			if (engineModel && field.Name == "ShadowResolution")
			{
				if (const std::optional<ShadowMapResolution> resolution = RendererQuality::ShadowResolutionFromString(text))
				{
					const uint32_t below = RendererQuality::ResolveShadowMapSize(*resolution, RendererQuality::c_HighResolutionRows - 1);
					const uint32_t above = RendererQuality::ResolveShadowMapSize(*resolution, RendererQuality::c_HighResolutionRows);
					if (*resolution == ShadowMapResolution::Auto)
						return fmt::format("Auto ({} / {})", below, above);
					if (*resolution == ShadowMapResolution::AutoFine)
						return fmt::format("Auto fine ({} / {})", below, above);
				}
			}
			return text;
		}

		// Why a stored value is refused (read when the values change, not per frame).
		std::string FindProblem(const EditorContext& context, const SettingsSection& section)
		{
			if (!context.HasProject())
				return {};
			const nlohmann::json entry = context.GetProject()->GetSettings().GetStoredEntry(section.Id);
			std::string error;
			if (entry.is_object() && entry.contains("Value") && entry.contains("Version") && entry["Version"].is_number_unsigned())
				section.Validate(entry["Value"], entry["Version"].get<uint32_t>(), &error);
			return error;
		}

	}

	struct GraphicsQualityPage::Data
	{
		// A row of the table (or of the players' choices).
		struct Row
		{
			const SettingsField* Field = nullptr;
			const RendererQuality::RowTraits* Traits = nullptr;
			PropertyInfo Property;
			PropertyValue Value; // Custom's column for a row that follows the preset, the choice for one that does not
			std::array<std::string, c_PresetColumns> Cells;
			std::array<std::string, c_Columns> CellTooltips; // Empty for rows without memory of their own
			std::string Tooltip;                             // The description and the chosen column's memory
		};

		RendererQuality::OptionModel Model;
		bool EngineModel = false;
		std::string Prefix; // "ProjectSettings.<section id>."
		std::vector<Row> Rows;    // That follow the preset, in the model's order
		std::vector<Row> Choices; // That do not

		std::optional<uint64_t> Version; // Of the project's settings the rest was read from
		bool Stored = false;
		nlohmann::json Value;       // The section's value: the stored one, or the default
		nlohmann::json CustomValue; // What Custom's column shows: the stored value under Custom, else MakeCustomStart's
		RendererQualityPreset Preset = RendererQualityPreset::High;
		std::string Problem;
		std::string CustomTooltip;
		std::array<std::string, c_Columns> Memory1080;
		std::array<std::string, c_Columns> Memory2160;

		std::string GPULine;
		double GPULineTime = -std::numeric_limits<double>::infinity();
		std::vector<std::pair<std::string, float>> StageTimes; // Of the stages the rows name, as the GPU line shows them
	};

	GraphicsQualityPage::GraphicsQualityPage(const SettingsSection& section)
		: GraphicsQualityPage(RendererQuality::GetOptionModel(section))
	{
	}

	GraphicsQualityPage::GraphicsQualityPage(const RendererQuality::OptionModel& model)
		: m_Data(CreateScope<Data>())
	{
		Data& data = *m_Data;
		data.Model = model;
		data.EngineModel = model.Section && model.Section->Id == RendererQuality::c_SectionId;
		data.Prefix = fmt::format("{}.{}.", SettingsBrowser::GetProbePrefix(SettingsWindowKind::Project), model.Section ? model.Section->Id : std::string());
		if (!model.Section)
			return;
		for (const SettingsField& field : model.Section->Fields)
		{
			const RendererQuality::RowTraits* traits = RendererQuality::FindRow(field.Name, model);
			if (!traits)
				continue; // Preset
			Data::Row row;
			row.Field = &field;
			row.Traits = traits;
			row.Property = UI::MakeFieldProperty(field);
			(traits->FollowsPreset ? data.Rows : data.Choices).push_back(std::move(row));
		}
	}

	GraphicsQualityPage::~GraphicsQualityPage() = default;

	void GraphicsQualityPage::Refresh(const EditorContext& context)
	{
		Data& data = *m_Data;
		const SettingsSection& section = *data.Model.Section;
		const ProjectSettingsValues* values = context.HasProject() ? &context.GetProject()->GetSettings() : nullptr;
		const uint64_t version = values ? values->GetVersion() : 0;
		if (data.Version && *data.Version == version)
			return;
		data.Version = version;
		const nlohmann::json* value = values ? values->FindSection(section.Id) : nullptr;
		data.Stored = values && values->IsStored(section.Id);
		// As stored (validated: every field), not normalized: a named preset's choice keeps the rows' stored values, which a
		// switch back to Custom shows again.
		data.Value = value ? *value : section.GetDefault();
		data.Problem = data.Stored && !value ? FindProblem(context, section) : std::string();

		data.Preset = RendererQualityPreset::High;
		if (const nlohmann::json* preset = JsonUtils::Find(data.Value, "Preset"); preset && preset->is_string())
			data.Preset = RendererQuality::PresetFromString(preset->get_ref<const std::string&>()).value_or(RendererQualityPreset::High);
		const bool custom = data.Preset == RendererQualityPreset::Custom;
		data.CustomValue = custom ? RendererQuality::Normalize(data.Value, data.Model) : RendererQuality::MakeCustomStart(data.Value, data.Model);
		data.CustomTooltip = custom ? std::string()
			: fmt::format("Editing a value switches to Custom, starting from {}'s values", RendererQuality::PresetToString(data.Preset));

		// The settings of each column, for the memory the engine's rows take (a test model's rows have none).
		std::array<RendererQualitySettings, c_Columns> settings;
		for (size_t column = 0; column < c_PresetColumns; column++)
			settings[column] = RendererQuality::GetPreset(static_cast<RendererQualityPreset>(column));
		settings[c_PresetColumns] = RendererQuality::FromSectionValue(&data.CustomValue);
		const size_t chosen = static_cast<size_t>(data.Preset);

		for (Data::Row& row : data.Rows)
		{
			const std::string& name = row.Field->Name;
			for (size_t column = 0; column < c_PresetColumns; column++)
			{
				const nlohmann::json* member = column < data.Model.PresetValues.size() ? JsonUtils::Find(data.Model.PresetValues[column], name) : nullptr;
				row.Cells[column] = member ? FormatValue(*row.Field, *member, data.EngineModel) : std::string("-");
			}
			const nlohmann::json* member = JsonUtils::Find(data.CustomValue, name);
			row.Value = UI::FieldToPropertyValue(*row.Field, row.Property, member ? *member : row.Field->Default);
			row.Tooltip = row.Field->Description;
			for (std::string& tooltip : row.CellTooltips)
				tooltip.clear();
			if (data.EngineModel && row.Traits->MemoryBytes)
			{
				for (size_t column = 0; column < c_Columns; column++)
				{
					row.CellTooltips[column] = fmt::format("Memory of one view: {} at 1920 x 1080, {} at 3840 x 2160",
						FormatBytes(row.Traits->MemoryBytes(settings[column], c_Output1080)), FormatBytes(row.Traits->MemoryBytes(settings[column], c_Output2160)));
				}
				row.Tooltip += fmt::format("\n\n{} ({})", row.CellTooltips[chosen], c_ColumnNames[chosen]);
			}
		}
		for (Data::Row& row : data.Choices)
		{
			const nlohmann::json* member = JsonUtils::Find(data.Value, row.Field->Name);
			row.Value = UI::FieldToPropertyValue(*row.Field, row.Property, member ? *member : row.Field->Default);
		}

		for (size_t column = 0; column < c_Columns; column++)
		{
			uint64_t bytes1080 = 0;
			uint64_t bytes2160 = 0;
			for (const RendererQuality::RowTraits& row : data.Model.Rows)
			{
				if (!row.MemoryBytes)
					continue;
				bytes1080 += row.MemoryBytes(settings[column], c_Output1080);
				bytes2160 += row.MemoryBytes(settings[column], c_Output2160);
			}
			data.Memory1080[column] = FormatBytes(bytes1080);
			data.Memory2160[column] = FormatBytes(bytes2160);
		}
	}

	void GraphicsQualityPage::Draw(EditorPanelContext& context, float width)
	{
		Data& data = *m_Data;
		if (!data.Model.Section)
			return;
		Refresh(context.Context);
		const UI::ThemeColors& colors = UI::GetThemeColors();
		Caption(c_Caption, colors.TextSecondary, width);
		if (!data.Problem.empty())
		{
			const std::string text = fmt::format("The stored value is not used (its default applies until it is changed): {}", data.Problem);
			Caption(text, colors.Warning, width);
			UI::ItemProbe::Record(data.Prefix + "Problem", true, 0, text);
		}
		ImGui::Spacing();
		DrawPresets(context);
		ImGui::Spacing();
		DrawTable(context, width);
		if (data.EngineModel)
			Caption(c_MemoryCaption, colors.TextSecondary, width);
		DrawChoices(context, width);
		ImGui::Spacing();
		DrawGPU(context, width);
		ImGui::Spacing();
		if (UI::DialogButton((data.Prefix + "Reset").c_str(), "Reset to Defaults", false, data.Stored))
			Apply(context, { { "section", data.Model.Section->Id }, { "reset", true } });
	}

	void GraphicsQualityPage::DrawPresets(EditorPanelContext& context)
	{
		Data& data = *m_Data;
		for (size_t column = 0; column < c_Columns; column++)
		{
			if (column > 0)
				ImGui::SameLine();
			const RendererQualityPreset preset = static_cast<RendererQualityPreset>(column);
			bool chosen = data.Preset == preset;
			const std::string key = data.Prefix + "Preset." + c_ColumnNames[column];
			const char* tooltip = preset == RendererQualityPreset::Custom ? "Custom: the values of the table's Custom column"
				: preset == RendererQualityPreset::Medium ? "Medium: Strata 0.6.0's costs"
				: preset == RendererQualityPreset::High ? "High: the default" : "";
			if (UI::ToggleChip(key.c_str(), c_ColumnIcons[column], c_ColumnNames[column], &chosen, *tooltip ? tooltip : nullptr)
				&& data.Preset != preset)
			{
				// A named preset keeps the rows' stored values, so Custom's tuning comes back when Custom is chosen again.
				nlohmann::json value = preset == RendererQualityPreset::Custom ? data.CustomValue : data.Value;
				value["Preset"] = c_ColumnNames[column];
				Apply(context, { { "section", data.Model.Section->Id }, { "value", std::move(value) } });
			}
		}
	}

	void GraphicsQualityPage::DrawTable(EditorPanelContext& context, float width)
	{
		Data& data = *m_Data;
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const size_t chosen = static_cast<size_t>(data.Preset);
		const ImU32 chosenBackground = ImGui::ColorConvertFloat4ToU32(colors.AccentMuted);
		const float columnWidth = c_ColumnWidthInFontSizes * ImGui::GetFontSize();
		if (!ImGui::BeginTable("Rows", static_cast<int>(c_Columns) + 1, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV,
			ImVec2(width, 0.0f)))
		{
			return;
		}
		ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch);
		for (const char* name : c_ColumnNames)
			ImGui::TableSetupColumn(name, ImGuiTableColumnFlags_WidthFixed, columnWidth);

		// The header: the presets, the chosen one in the accent on its column's highlight.
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		ImGui::TableSetColumnIndex(0);
		ImGui::TextColored(colors.TextSecondary, "Setting");
		for (size_t column = 0; column < c_Columns; column++)
		{
			ImGui::TableSetColumnIndex(static_cast<int>(column) + 1);
			const bool isChosen = column == chosen;
			if (isChosen)
				ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, chosenBackground);
			UI::PushFont(UI::EditorFont::SemiBold);
			ImGui::TextColored(isChosen ? colors.Accent : colors.TextSecondary, "%s", c_ColumnNames[column]);
			ImGui::PopFont();
			UI::ItemProbe::Record(data.Prefix + "Header." + c_ColumnNames[column], true, isChosen ? chosenBackground : 0, c_ColumnNames[column]);
		}

		std::optional<size_t> edited;
		for (size_t index = 0; index < data.Rows.size(); index++)
		{
			Data::Row& row = data.Rows[index];
			const std::string& name = row.Field->Name;
			ImGui::PushID(static_cast<int>(index));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(row.Field->Title.c_str());
			UI::ItemProbe::Record(data.Prefix + "Row." + name, true, 0, row.Field->Title);
			if (UI::IsTooltipDue())
			{
				std::string tooltip = row.Tooltip;
				for (const auto& [stage, milliseconds] : data.StageTimes)
				{
					if (stage == row.Traits->GPUStage)
						tooltip += fmt::format("\n\n{}: {:.2f} ms now in the Scene view", stage, milliseconds);
				}
				ImGui::SetTooltip("%s", tooltip.c_str());
			}

			for (size_t column = 0; column < c_PresetColumns; column++)
			{
				ImGui::TableSetColumnIndex(static_cast<int>(column) + 1);
				const bool isChosen = column == chosen;
				if (isChosen)
					ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, chosenBackground);
				ImGui::AlignTextToFramePadding();
				ImGui::TextColored(isChosen ? colors.Text : colors.TextSecondary, "%s", row.Cells[column].c_str());
				UI::ItemProbe::Record(fmt::format("{}Cell.{}.{}", data.Prefix, name, c_ColumnNames[column]), true, isChosen ? chosenBackground : 0,
					row.Cells[column]);
				if (!row.CellTooltips[column].empty() && UI::IsTooltipDue())
					ImGui::SetTooltip("%s", row.CellTooltips[column].c_str());
			}

			// Custom's cell edits the value; under a named preset it shows where a switch to Custom starts, in secondary text.
			ImGui::TableSetColumnIndex(static_cast<int>(c_Columns));
			const bool customChosen = chosen == c_PresetColumns;
			if (customChosen)
				ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, chosenBackground);
			if (!customChosen)
				ImGui::PushStyleColor(ImGuiCol_Text, colors.TextSecondary);
			const std::string customKey = data.Prefix + "Custom." + name;
			PropertyWidgetOptions options;
			options.ProbeKey = customKey;
			const PropertyEditResult result = DrawPropertyWidget(row.Property, row.Value, *context.Context.GetActiveScene(), options);
			if (!customChosen)
				ImGui::PopStyleColor();
			if (!data.CustomTooltip.empty() && UI::IsTooltipDue())
				ImGui::SetTooltip("%s", data.CustomTooltip.c_str());
			// A drag or a text edit shows its value as it goes and runs the command once, when it ends.
			if (result.Finished)
				edited = index;
			ImGui::PopID();
		}

		// The memory the rows take, per column.
		if (data.EngineModel)
		{
			for (int line = 0; line < 2; line++)
			{
				const std::array<std::string, c_Columns>& memory = line == 0 ? data.Memory1080 : data.Memory2160;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextColored(colors.TextSecondary, "%s", line == 0 ? "Memory at 1920 x 1080" : "at 3840 x 2160");
				for (size_t column = 0; column < c_Columns; column++)
				{
					ImGui::TableSetColumnIndex(static_cast<int>(column) + 1);
					if (column == chosen)
						ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, chosenBackground);
					UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
					ImGui::TextColored(column == chosen ? colors.Text : colors.TextSecondary, "%s", memory[column].c_str());
					ImGui::PopFont();
					UI::ItemProbe::Record(fmt::format("{}{}.{}", data.Prefix, line == 0 ? "Memory1080" : "Memory2160", c_ColumnNames[column]), true,
						column == chosen ? chosenBackground : 0, memory[column]);
				}
			}
		}
		ImGui::EndTable();

		if (edited)
		{
			const Data::Row& row = data.Rows[*edited];
			// Custom's value with the field set: from a named preset, the switch to Custom and the edit are one step.
			nlohmann::json value = data.CustomValue;
			value[row.Field->Name] = UI::PropertyToFieldValue(*row.Field, row.Property, row.Value);
			Apply(context, { { "section", data.Model.Section->Id }, { "value", std::move(value) } });
		}
	}

	void GraphicsQualityPage::DrawChoices(EditorPanelContext& context, float width)
	{
		Data& data = *m_Data;
		if (data.Choices.empty())
			return;
		const UI::ThemeColors& colors = UI::GetThemeColors();
		ImGui::Spacing();
		UI::Heading("Players' choices", UI::TextSize::Caption);
		Caption(c_ChoicesCaption, colors.TextSecondary, width);
		std::optional<size_t> edited;
		if (UI::BeginPropertyTable("Choices"))
		{
			for (size_t index = 0; index < data.Choices.size(); index++)
			{
				Data::Row& row = data.Choices[index];
				const std::string key = data.Prefix + "Choice." + row.Field->Name;
				UI::PropertyLabel(row.Property.DisplayName, row.Property.Tooltip, key);
				PropertyWidgetOptions options;
				options.ProbeKey = key;
				ImGui::PushID(static_cast<int>(index));
				const PropertyEditResult result = DrawPropertyWidget(row.Property, row.Value, *context.Context.GetActiveScene(), options);
				ImGui::PopID();
				if (result.Finished)
					edited = index;
			}
			UI::EndPropertyTable();
		}
		if (edited)
		{
			// The field set, the preset and the other rows as stored.
			const Data::Row& row = data.Choices[*edited];
			nlohmann::json value = data.Value;
			value[row.Field->Name] = UI::PropertyToFieldValue(*row.Field, row.Property, row.Value);
			Apply(context, { { "section", data.Model.Section->Id }, { "value", std::move(value) } });
		}
	}

	void GraphicsQualityPage::DrawGPU(EditorPanelContext& context, float width)
	{
		Data& data = *m_Data;
		const double now = ImGui::GetTime();
		if (now - data.GPULineTime >= c_GPULineSeconds || now < data.GPULineTime)
		{
			data.GPULineTime = now;
			data.StageTimes.clear();
			const ViewportRenderer* renderer = context.Context.GetViewport().FindRenderer();
			const SceneRendererGPUStats* gpu = renderer ? &renderer->GetGPUStats() : nullptr;
			if (!gpu || !gpu->HasTimes)
			{
				data.GPULine = "GPU times appear while the Scene view renders";
			}
			else
			{
				// The stages the rows name, once each, in the rows' order.
				for (const RendererQuality::RowTraits& row : data.Model.Rows)
				{
					if (row.GPUStage.empty())
						continue;
					bool listed = false;
					for (const auto& [stage, milliseconds] : data.StageTimes)
						listed = listed || stage == row.GPUStage;
					if (listed)
						continue;
					float milliseconds = 0.0f;
					for (const GPUStageTime& time : gpu->StageMs)
					{
						if (FrameProfiler::Get().GetStageName(time.Stage) == row.GPUStage)
							milliseconds += time.Milliseconds;
					}
					data.StageTimes.emplace_back(std::string(row.GPUStage), milliseconds);
				}
				const glm::uvec2 size = renderer->GetSize();
				std::string line = fmt::format("Now in the Scene view ({}, {} x {}):", RendererQuality::PresetToString(renderer->GetStats().Quality.Preset),
					size.x, size.y);
				for (size_t index = 0; index < data.StageTimes.size(); index++)
					line += fmt::format("{} {} {:.2f} ms", index == 0 ? "" : ",", data.StageTimes[index].first, data.StageTimes[index].second);
				line += fmt::format("{} all stages {:.2f} ms", data.StageTimes.empty() ? "" : ",", gpu->BusyMs);
				data.GPULine = std::move(line);
			}
		}
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		ImGui::PushStyleColor(ImGuiCol_Text, UI::GetThemeColors().TextSecondary);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
		ImGui::TextUnformatted(data.GPULine.c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::PopFont();
		UI::ItemProbe::Record(data.Prefix + "GPU", true, 0, data.GPULine);
	}

	void GraphicsQualityPage::Apply(EditorPanelContext& context, const nlohmann::json& parameters)
	{
		RunEditorCommand(context.Context, context.Commands, "project.setSettings", parameters);
		// Read again: the value the command applied, or the one that stayed when it refused.
		m_Data->Version.reset();
	}

}
