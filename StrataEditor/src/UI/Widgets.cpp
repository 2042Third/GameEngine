#include "UI/Widgets.h"

#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"

#include <imgui_internal.h>

#include <algorithm>

namespace Strata::UI
{

	namespace
	{

		// Dialog buttons are at least this many text heights wide, so a row of them looks even.
		constexpr float c_DialogButtonWidthInFontSizes = 7.0f;

		void ShowTooltip(const char* tooltip)
		{
			if (tooltip && *tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", tooltip);
		}

		// The factor ImGui applies to font sizes (UI scale and main scale), for text drawn at an explicit size.
		float GetFontScale()
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			return style.FontScaleMain * style.FontScaleDpi;
		}

		// A color for the draw list: faded like ImGui's items while disabled, unless it must stay vivid.
		ImU32 ToDrawColor(const ImVec4& color, bool vivid = false)
		{
			return vivid ? ImGui::ColorConvertFloat4ToU32(color) : ImGui::GetColorU32(color);
		}

		void AddText(ImDrawList* drawList, const ImVec2& position, const ImVec4& color, std::string_view text, bool vivid = false)
		{
			drawList->AddText(position, ToDrawColor(color, vivid), text.data(), text.data() + text.size());
		}

		bool DrawButton(const char* id, const char* icon, const char* label, const char* tooltip, const ButtonStyle& style)
		{
			const ImGuiStyle& imguiStyle = ImGui::GetStyle();
			const ThemeColors& colors = GetThemeColors();
			const float height = ImGui::GetFrameHeight();
			const bool hasLabel = label && *label;
			const ImVec2 iconSize = ImGui::CalcTextSize(icon);
			const ImVec2 labelSize = hasLabel ? ImGui::CalcTextSize(label) : ImVec2(0.0f, 0.0f);
			const float width = hasLabel ? imguiStyle.FramePadding.x * 2.0f + iconSize.x + imguiStyle.ItemInnerSpacing.x + labelSize.x : height;

			ImGui::BeginDisabled(!style.Enabled);
			const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
			const bool hovered = ImGui::IsItemHovered();
			const bool held = ImGui::IsItemActive();
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();

			ImVec4 background = WithAlpha(colors.Control, 0.0f);
			ImVec4 foreground = colors.Text;
			if (style.Primary)
			{
				background = held ? colors.AccentActive : (hovered ? colors.AccentHover : colors.Accent);
				foreground = colors.TextOnAccent;
			}
			else if (style.Active)
			{
				const ImVec4 tint = style.ActiveColor.value_or(colors.Accent);
				background = WithAlpha(tint, held ? 0.34f : (hovered ? 0.26f : 0.18f));
				foreground = tint;
			}
			else if (held)
			{
				background = colors.Border;
			}
			else if (hovered)
			{
				background = colors.Control;
			}

			// An active state stays vivid while the button cannot be clicked (e.g. Play while playing): it shows a state.
			const bool vivid = style.Active && !style.Enabled;
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			if (background.w > 0.0f)
				drawList->AddRectFilled(min, max, ToDrawColor(background, vivid), imguiStyle.FrameRounding);
			const float textY = min.y + (height - ImGui::GetFontSize()) * 0.5f;
			if (hasLabel)
			{
				const float iconX = min.x + imguiStyle.FramePadding.x;
				AddText(drawList, ImVec2(iconX, textY), foreground, icon, vivid);
				AddText(drawList, ImVec2(iconX + iconSize.x + imguiStyle.ItemInnerSpacing.x, textY), style.Primary ? foreground : colors.Text, label, vivid);
			}
			else
			{
				AddText(drawList, ImVec2(min.x + (width - iconSize.x) * 0.5f, textY), foreground, icon, vivid);
			}
			ImGui::RenderNavCursor(ImRect(min, max), ImGui::GetItemID());
			ImGui::EndDisabled();

			ShowTooltip(tooltip);
			ItemProbe::Record(id, style.Enabled);
			return pressed;
		}

		bool DrawChip(const char* id, const char* icon, const char* label, const char* tooltip, bool on)
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const ThemeColors& colors = GetThemeColors();
			const float height = ImGui::GetFrameHeight();
			const bool hasLabel = label && *label;
			const ImVec2 iconSize = ImGui::CalcTextSize(icon);
			const ImVec2 labelSize = hasLabel ? ImGui::CalcTextSize(label) : ImVec2(0.0f, 0.0f);
			const float width = style.FramePadding.x * 2.0f + iconSize.x + (hasLabel ? style.ItemInnerSpacing.x + labelSize.x : 0.0f);

			// Chips lie over what their window shows: a click on one leaves the focus where it was, so that a chip over the
			// game view does not hand the game the input.
			const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height), ImGuiButtonFlags_NoFocus | ImGuiButtonFlags_NoNavFocus);
			const bool hovered = ImGui::IsItemHovered();
			const bool held = ImGui::IsItemActive();
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();

			// Chips sit on the viewport image: their surfaces are translucent but dark enough for any scene behind them.
			ImVec4 background = colors.Overlay;
			ImVec4 border = WithAlpha(colors.Border, 0.8f);
			ImVec4 iconColor = colors.TextSecondary;
			ImVec4 labelColor = hovered ? colors.Text : colors.TextSecondary;
			if (on)
			{
				background = WithAlpha(colors.AccentMuted, held ? 1.0f : (hovered ? 0.95f : 0.85f));
				border = WithAlpha(colors.Accent, 0.6f);
				iconColor = colors.Accent;
				labelColor = colors.Text;
			}
			else if (held)
			{
				background = colors.Control;
			}
			else if (hovered)
			{
				background = WithAlpha(colors.Raised, 0.92f);
			}

			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const float rounding = height * 0.5f;
			drawList->AddRectFilled(min, max, ToDrawColor(background), rounding);
			drawList->AddRect(min, max, ToDrawColor(border), rounding);
			const float textY = min.y + (height - ImGui::GetFontSize()) * 0.5f;
			const float iconX = min.x + style.FramePadding.x;
			AddText(drawList, ImVec2(iconX, textY), iconColor, icon);
			if (hasLabel)
				AddText(drawList, ImVec2(iconX + iconSize.x + style.ItemInnerSpacing.x, textY), labelColor, label);
			ImGui::RenderNavCursor(ImRect(min, max), ImGui::GetItemID());

			ShowTooltip(tooltip);
			ItemProbe::Record(id);
			return pressed;
		}

	}

	bool IconButton(const char* id, const char* icon, const char* tooltip, bool enabled)
	{
		ButtonStyle style;
		style.Enabled = enabled;
		return DrawButton(id, icon, nullptr, tooltip, style);
	}

	bool ToolbarButton(const char* id, const char* icon, const char* label, const char* tooltip, const ButtonStyle& style)
	{
		return DrawButton(id, icon, label, tooltip, style);
	}

	bool Chip(const char* id, const char* icon, const char* label, const char* tooltip)
	{
		return DrawChip(id, icon, label, tooltip, false);
	}

	bool ToggleChip(const char* id, const char* icon, const char* label, bool* value, const char* tooltip)
	{
		if (!DrawChip(id, icon, label, tooltip, *value))
			return false;
		*value = !*value;
		return true;
	}

	bool Pill(const char* id, const char* icon, std::string_view text, const ImVec4& color, const char* tooltip)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float height = ImGui::GetFrameHeight();
		const ImVec2 iconSize = ImGui::CalcTextSize(icon);
		const ImVec2 textSize = ImGui::CalcTextSize(text.data(), text.data() + text.size());
		const float width = style.FramePadding.x * 2.0f + iconSize.x + style.ItemInnerSpacing.x + textSize.x;

		const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
		const bool hovered = ImGui::IsItemHovered();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();

		// Slimmer than a button: the pill is inset in its line.
		const float inset = style.FramePadding.y * 0.5f;
		const ImVec2 pillMin(min.x, min.y + inset);
		const ImVec2 pillMax(max.x, max.y - inset);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(pillMin, pillMax, ToDrawColor(WithAlpha(color, hovered ? 0.24f : 0.13f)), (pillMax.y - pillMin.y) * 0.5f);
		const float textY = min.y + (height - ImGui::GetFontSize()) * 0.5f;
		const float iconX = min.x + style.FramePadding.x;
		AddText(drawList, ImVec2(iconX, textY), color, icon);
		AddText(drawList, ImVec2(iconX + iconSize.x + style.ItemInnerSpacing.x, textY), color, text);

		ShowTooltip(tooltip);
		ItemProbe::Record(id, true, ToColorU32(color));
		return pressed;
	}

	void PushSelectionColors(bool selected)
	{
		// Always one color, so the pop does not depend on the row.
		const ImVec4 hovered = selected ? GetThemeColors().SelectionHovered : ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, hovered);
	}

	void PopSelectionColors()
	{
		ImGui::PopStyleColor();
	}

	bool SectionHeader(const char* id, const char* label, ImGuiTreeNodeFlags flags)
	{
		const ThemeColors& colors = GetThemeColors();
		ImGui::PushStyleColor(ImGuiCol_Header, colors.Raised);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, colors.Control);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, colors.Border);
		PushFont(EditorFont::SemiBold);
		ImGui::PushID(id);
		const bool open = ImGui::CollapsingHeader(label, flags);
		ImGui::PopID();
		ImGui::PopFont();
		ImGui::PopStyleColor(3);
		ItemProbe::Record(id);
		return open;
	}

	void Heading(std::string_view text, TextSize size)
	{
		PushFont(EditorFont::SemiBold, size);
		ImGui::TextUnformatted(text.data(), text.data() + text.size());
		ImGui::PopFont();
	}

	bool Card(const char* id, const char* icon, std::string_view title, std::string_view description, const ImVec2& size, bool selected)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const ThemeColors& colors = GetThemeColors();
		const bool pressed = ImGui::InvisibleButton(id, size);
		const bool hovered = ImGui::IsItemHovered();
		const bool held = ImGui::IsItemActive();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();

		ImVec4 background = colors.Raised;
		ImVec4 border = colors.Border;
		if (selected)
		{
			background = WithAlpha(colors.AccentMuted, 0.6f);
			border = colors.Accent;
		}
		else if (held)
		{
			background = colors.Border;
		}
		else if (hovered)
		{
			background = colors.Control;
			border = colors.TextDisabled;
		}
		const float rounding = style.FrameRounding * 2.0f;
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(min, max, ToDrawColor(background), rounding);
		drawList->AddRect(min, max, ToDrawColor(border), rounding);

		const float fontScale = GetFontScale();
		const ImVec2 padding = style.WindowPadding;
		const float wrapWidth = std::max(max.x - min.x - padding.x * 2.0f, 1.0f);
		drawList->PushClipRect(min, max, true);
		float y = min.y + padding.y;
		if (icon && *icon)
		{
			const float iconSize = GetTextSize(TextSize::Display) * fontScale;
			drawList->AddText(EditorFonts::Get(EditorFont::Regular), iconSize, ImVec2(min.x + padding.x, y), ToDrawColor(colors.Accent), icon);
			y += iconSize + style.ItemSpacing.y;
		}
		const float titleSize = GetTextSize(TextSize::Body) * fontScale;
		drawList->AddText(EditorFonts::Get(EditorFont::SemiBold), titleSize, ImVec2(min.x + padding.x, y), ToDrawColor(colors.Text), title.data(),
			title.data() + title.size(), wrapWidth);
		y += titleSize + style.ItemInnerSpacing.y;
		drawList->AddText(EditorFonts::Get(EditorFont::Regular), GetTextSize(TextSize::Caption) * fontScale, ImVec2(min.x + padding.x, y),
			ToDrawColor(colors.TextSecondary), description.data(), description.data() + description.size(), wrapWidth);
		drawList->PopClipRect();
		ImGui::RenderNavCursor(ImRect(min, max), ImGui::GetItemID());

		ItemProbe::Record(id);
		return pressed;
	}

	void OpenModal(const char* name)
	{
		ImGui::OpenPopup(name);
	}

	bool BeginModal(const char* name, std::string_view title, bool* open)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x * 2.0f, style.WindowPadding.y * 2.0f));
		const bool visible = ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
		ImGui::PopStyleVar();
		if (!visible)
			return false;

		const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		Heading(title, TextSize::Title);
		if (open)
		{
			// The close button at the right end of the title line.
			ImGui::SameLine();
			ImGui::SetCursorPosX(std::max(right - ImGui::GetFrameHeight(), ImGui::GetCursorPosX()));
			if (IconButton("Modal.Close", Icons::X, "Close"))
			{
				*open = false;
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::Spacing();
		return true;
	}

	void EndModal()
	{
		ImGui::EndPopup();
	}

	bool DialogButton(const char* id, const char* label, bool primary)
	{
		const ThemeColors& colors = GetThemeColors();
		const float width = std::max(ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f,
			ImGui::GetFontSize() * c_DialogButtonWidthInFontSizes);
		if (primary)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, colors.Accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.AccentHover);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors.AccentActive);
			ImGui::PushStyleColor(ImGuiCol_Text, colors.TextOnAccent);
		}
		ImGui::PushID(id);
		const bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));
		ImGui::PopID();
		if (primary)
			ImGui::PopStyleColor(4);
		ItemProbe::Record(id);
		return pressed;
	}

}
