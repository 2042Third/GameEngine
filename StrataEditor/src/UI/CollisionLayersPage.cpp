#include "UI/CollisionLayersPage.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/PropertyWidgets.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/ProjectSettings.h>
#include <Strata/Project/Project.h>
#include <Strata/Scene/ProjectCollisionLayers.h>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace Strata
{

	namespace
	{

		// The widest a row label of the matrix gets, in text heights (longer names are elided).
		constexpr float c_LabelMaxWidthInFontSizes = 10.0f;

		std::string ProbeKey(std::string_view item)
		{
			return std::string(CollisionLayersPage::c_ProbePrefix) + std::string(item);
		}

		// A name for a newly named layer that no layer has: "New Layer", else "New Layer 2" and so on.
		std::string MakeNewLayerName(const PhysicsLayerSettings& layers, uint32_t layer)
		{
			for (uint32_t number = 1; number <= c_PhysicsLayerCount + 1; number++)
			{
				std::string name = number == 1 ? std::string("New Layer") : fmt::format("New Layer {}", number);
				if (!layers.ValidateName(layer, name))
					return name;
			}
			return {};
		}

		void Caption(const std::string& text, const ImVec4& color, float width)
		{
			UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
			ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}

	}

	std::string CollisionLayersPage::Describe(const PhysicsLayerSettings& layers, std::vector<uint32_t>* outShown)
	{
		if (outShown)
			outShown->clear();
		size_t named = 0;
		size_t apart = 0;
		for (uint32_t layer = 0; layer < c_PhysicsLayerCount; layer++)
		{
			named += layers.Names[layer].empty() ? 0 : 1;
			for (uint32_t other = layer; other < c_PhysicsLayerCount; other++)
				apart += layers.DoCollide(layer, other) ? 0 : 1;
			if (outShown && (layer == 0 || !layers.Names[layer].empty() || !layers.CollidesWithAll(layer)))
				outShown->push_back(layer);
		}
		const std::string names = named == 0 ? std::string("No names") : fmt::format("{} named", named);
		if (apart == 0)
			return names + " \xC2\xB7 all collide";
		return fmt::format("{} \xC2\xB7 {} {} through", names, apart, apart == 1 ? "pair passes" : "pairs pass");
	}

	void CollisionLayersPage::Refresh(const EditorContext& context)
	{
		const ProjectSettingsValues& values = context.GetProject()->GetSettings();
		if (m_Read && values.GetVersion() == m_SettingsVersion)
			return;
		m_Read = true;
		m_SettingsVersion = values.GetVersion();
		const nlohmann::json* value = values.FindSection(ProjectCollisionLayers::c_SectionId);
		m_Layers = value ? PhysicsLayerSettings::FromJson(*value) : PhysicsLayerSettings();
		m_Summary = Describe(m_Layers, &m_Shown);
	}

	void CollisionLayersPage::Run(EditorPanelContext& context, const nlohmann::json& parameters)
	{
		const EditorCommandResult result = context.Commands.Execute(context.Context, "project.setCollisionLayers", parameters, CommandOrigin { CommandSource::UI });
		m_Error = result.Success ? std::string() : result.Error;
	}

	void CollisionLayersPage::Draw(EditorPanelContext& context, float width)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		if (!context.Context.HasProject())
			return;
		Refresh(context.Context);
		Caption("Rigid bodies and character controllers are on one of 32 layers and collide only when their layers do. Every scene and prefab "
			"uses these names and this matrix; scripts find layers by name (Physics::GetLayerIndex) and change the matrix of the scene they run "
			"in until the next scene starts.", colors.TextSecondary, width);
		ImGui::Spacing();
		ImGui::TextUnformatted(m_Summary.c_str());
		UI::ItemProbe::Record(ProbeKey("Summary"), true, 0, m_Summary);
		ImGui::Spacing();
		// Each part runs at most one command, which replaces the project's settings: the next frame draws the new table.
		DrawNames(context);
		DrawMatrix(context);
		if (!m_Error.empty())
			Caption(m_Error, colors.Error, width);
	}

	void CollisionLayersPage::DrawNames(EditorPanelContext& context)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		// Copies: a command run below replaces the table.
		const PhysicsLayerSettings current = m_Layers;
		std::optional<nlohmann::json> edit;
		if (UI::BeginPropertyTable("CollisionLayerNames"))
		{
			for (uint32_t layer = 0; layer < c_PhysicsLayerCount; layer++)
			{
				if (current.Names[layer].empty())
					continue;
				const std::string key = ProbeKey(fmt::format("Name.{}", layer));
				UI::PropertyLabel(fmt::format("Layer {}", layer), "Bodies and character controllers choose their layer by this name", key);
				ImGui::PushID(static_cast<int>(layer));
				std::string local;
				std::string* text = &local;
				if (m_NameLayer == layer)
					text = &m_NameText;
				else
					local = current.Names[layer];
				ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - style.ItemSpacing.x, ImGui::GetFrameHeight()));
				const bool entered = ImGui::InputTextWithHint("##Name", "Name", text, ImGuiInputTextFlags_EnterReturnsTrue);
				UI::ItemProbe::Record(key, true, 0, *text);
				if (ImGui::IsItemActivated())
				{
					m_NameLayer = layer;
					m_NameText = *text;
				}
				const std::string typed = *text;
				const bool commit = (entered || ImGui::IsItemDeactivatedAfterEdit()) && typed != current.Names[layer];
				if (!ImGui::IsItemActive() && m_NameLayer == layer)
					m_NameLayer = c_PhysicsLayerCount;
				ImGui::SameLine();
				const std::string clearKey = ProbeKey(fmt::format("Clear.{}", layer));
				const bool clear = UI::IconButton(clearKey.c_str(), Icons::X, "Remove the name (what is on the layer stays on it)");
				ImGui::PopID();
				if (commit)
					edit = nlohmann::json { { "layerNames", { { std::to_string(layer), typed } } } };
				else if (clear)
					edit = nlohmann::json { { "layerNames", { { std::to_string(layer), "" } } } };
			}
			UI::EndPropertyTable();
		}
		// Naming a layer starts with the first one without a name after the default layer (0, where everything starts).
		std::optional<uint32_t> unnamed;
		for (uint32_t offset = 0; offset < c_PhysicsLayerCount && !unnamed; offset++)
		{
			const uint32_t layer = (offset + 1) % c_PhysicsLayerCount;
			if (current.Names[layer].empty())
				unnamed = layer;
		}
		UI::ButtonStyle addStyle;
		addStyle.Enabled = unnamed.has_value();
		const std::string addKey = ProbeKey("Add");
		if (UI::ToolbarButton(addKey.c_str(), Icons::LayersPlus, "Name a Layer", "Give the next layer without a name one, to choose it by", addStyle) && unnamed)
			edit = nlohmann::json { { "layerNames", { { std::to_string(*unnamed), MakeNewLayerName(current, *unnamed) } } } };
		if (edit)
			Run(context, *edit);
	}

	void CollisionLayersPage::DrawMatrix(EditorPanelContext& context)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImGuiStyle& style = ImGui::GetStyle();
		// Copies: a command run below replaces the table.
		const PhysicsLayerSettings current = m_Layers;
		const std::vector<uint32_t> shown = m_Shown;

		// One row per shown layer with a box for each shown layer up to it (the pairs once each); a cleared box lets the pair
		// pass through each other. Compact, so the layers a game names fit a laptop's width: boxes of the text's height in
		// tight cells, and row labels as wide as the longest (at most a few words, the rest elided, whole in their tooltips).
		ImGui::Spacing();
		UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Caption);
		ImGui::TextColored(colors.TextSecondary, "Which layers collide");
		ImGui::PopFont();
		std::vector<std::string> labels;
		labels.reserve(shown.size());
		float labelWidth = 0.0f;
		for (const uint32_t layer : shown)
		{
			labels.push_back(fmt::format("{} \xC2\xB7 {}", layer, current.GetDisplayName(layer)));
			labelWidth = std::max(labelWidth, ImGui::CalcTextSize(labels.back().c_str()).x);
		}
		labelWidth = std::min(labelWidth, ImGui::GetFontSize() * c_LabelMaxWidthInFontSizes);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(style.CellPadding.x * 0.5f, style.CellPadding.y * 0.5f));
		const float cellWidth = ImGui::GetFrameHeight();
		const float rowHeight = ImGui::GetFrameHeight() + style.CellPadding.y * 2.0f;
		const float contentWidth = labelWidth + style.CellPadding.x * 2.0f + (cellWidth + style.CellPadding.x * 2.0f) * static_cast<float>(shown.size());
		const bool scroll = contentWidth > ImGui::GetContentRegionAvail().x;
		const float height = rowHeight * static_cast<float>(shown.size() + 1) + (scroll ? style.ScrollbarSize : 0.0f) + style.CellPadding.y;
		const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX | (scroll ? ImGuiTableFlags_ScrollX : ImGuiTableFlags_None);
		if (!ImGui::BeginTable("CollisionLayerMatrix", static_cast<int>(shown.size()) + 1, flags, ImVec2(scroll ? 0.0f : contentWidth, scroll ? height : 0.0f)))
		{
			ImGui::PopStyleVar(2);
			return;
		}
		ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthFixed, labelWidth);
		for (size_t column = 0; column < shown.size(); column++)
			ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, cellWidth);
		ImGui::TableNextRow();
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		for (size_t column = 0; column < shown.size(); column++)
		{
			ImGui::TableSetColumnIndex(static_cast<int>(column) + 1);
			const std::string index = std::to_string(shown[column]);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (cellWidth - ImGui::CalcTextSize(index.c_str()).x) * 0.5f);
			ImGui::TextColored(colors.TextSecondary, "%s", index.c_str());
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("%s", current.GetDisplayName(shown[column]).c_str());
		}
		ImGui::PopFont();
		std::optional<std::pair<std::pair<uint32_t, uint32_t>, bool>> toggled;
		for (size_t row = 0; row < shown.size(); row++)
		{
			const uint32_t layer = shown[row];
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			const std::string shownLabel = UI::ElideEnd(labels[row], labelWidth);
			ImGui::TextUnformatted(shownLabel.c_str());
			UI::ItemProbe::Record(ProbeKey(fmt::format("Label.{}", layer)), true, 0, shownLabel);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				ImGui::SetTooltip("%s", labels[row].c_str());
			for (size_t column = 0; column <= row; column++)
			{
				const uint32_t other = shown[column];
				ImGui::TableSetColumnIndex(static_cast<int>(column) + 1);
				ImGui::PushID(static_cast<int>(layer * c_PhysicsLayerCount + other));
				bool collide = current.DoCollide(layer, other);
				if (ImGui::Checkbox("##Collide", &collide))
					toggled = std::make_pair(std::make_pair(other, layer), collide);
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				{
					const std::string pair = layer == other ? fmt::format("Bodies on {}", current.GetDisplayName(layer))
						: fmt::format("{} and {}", current.GetDisplayName(other), current.GetDisplayName(layer));
					ImGui::SetTooltip("%s %s", pair.c_str(), collide ? "collide" : "pass through each other");
				}
				UI::ItemProbe::Record(ProbeKey(fmt::format("Cell.{}.{}", other, layer)), true, 0, collide ? "on" : "off");
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
		ImGui::PopStyleVar(2);
		if (toggled)
		{
			const auto& [pair, collide] = *toggled;
			Run(context, { { "layerCollisions", nlohmann::json::array({ { { "layers", { pair.first, pair.second } }, { "collide", collide } } }) } });
		}
	}

}
