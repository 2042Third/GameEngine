#include "UI/Widgets.h"

#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace Strata::UI
{

	namespace
	{

		// Dialog buttons are at least this many text heights wide, so a row of them looks even.
		constexpr float c_DialogButtonWidthInFontSizes = 7.0f;
		// Action buttons are this many frame heights tall.
		constexpr float c_ActionButtonHeightInFrames = 1.6f;
		// Below this size (in pixels) the brand mark keeps three bands, like its 16 pixel icon.
		constexpr float c_BrandMarkDetailSize = 24.0f;

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

		bool DrawChip(const char* id, const char* icon, const char* label, const char* tooltip, bool on, bool inEffect = true)
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
			if (on && inEffect)
			{
				background = WithAlpha(colors.AccentMuted, held ? 1.0f : (hovered ? 0.95f : 0.85f));
				border = WithAlpha(colors.Accent, 0.6f);
				iconColor = colors.Accent;
				labelColor = colors.Text;
			}
			else if (on)
			{
				// On, but waiting for something to do: the surface of an idle chip, the outline and a quiet icon of an on one.
				background = held ? colors.Control : (hovered ? WithAlpha(colors.Raised, 0.92f) : colors.Overlay);
				border = WithAlpha(colors.Accent, 0.6f);
				iconColor = WithAlpha(colors.Accent, 0.55f);
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
			ItemProbe::Record(id, true, ToColorU32(iconColor));
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

	bool ToggleChip(const char* id, const char* icon, const char* label, bool* value, const char* tooltip, bool inEffect)
	{
		if (!DrawChip(id, icon, label, tooltip, *value, inEffect))
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

	float GetCardHeight(float width, bool hasIcon, std::string_view title, std::string_view description)
	{
		// Card's layout: padding, the icon and a gap, the title, a small gap, the description, padding.
		const ImGuiStyle& style = ImGui::GetStyle();
		const float fontScale = GetFontScale();
		const float wrapWidth = std::max(width - style.WindowPadding.x * 2.0f, 1.0f);
		ImFont* semiBold = EditorFonts::Get(EditorFont::SemiBold);
		ImFont* regular = EditorFonts::Get(EditorFont::Regular);
		const float titleSize = GetTextSize(TextSize::Body) * fontScale;
		const float captionSize = GetTextSize(TextSize::Caption) * fontScale;
		const float titleHeight = (semiBold ? semiBold : ImGui::GetFont())->CalcTextSizeA(titleSize, FLT_MAX, wrapWidth, title.data(), title.data() + title.size()).y;
		const float descriptionHeight = (regular ? regular : ImGui::GetFont())->CalcTextSizeA(captionSize, FLT_MAX, wrapWidth, description.data(),
			description.data() + description.size()).y;
		float height = style.WindowPadding.y * 2.0f + std::max(titleHeight, titleSize) + style.ItemInnerSpacing.y + descriptionHeight;
		if (hasIcon)
			height += GetTextSize(TextSize::Display) * fontScale + style.ItemSpacing.y;
		return std::ceil(height);
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

	bool ActionButton(const char* id, const char* icon, std::string_view label, const char* tooltip, const ButtonStyle& style, float width)
	{
		const ImGuiStyle& imguiStyle = ImGui::GetStyle();
		const ThemeColors& colors = GetThemeColors();
		const float height = std::round(ImGui::GetFrameHeight() * c_ActionButtonHeightInFrames);
		const float padding = std::round(imguiStyle.FramePadding.x * 1.5f);
		const float gap = imguiStyle.ItemInnerSpacing.x * 2.0f;
		const ImVec2 iconSize = ImGui::CalcTextSize(icon);
		const ImVec2 labelSize = ImGui::CalcTextSize(label.data(), label.data() + label.size());
		float buttonWidth = width;
		if (width == 0.0f)
			buttonWidth = padding * 2.0f + iconSize.x + gap + labelSize.x;
		else if (width < 0.0f)
			buttonWidth = std::max(ImGui::GetContentRegionAvail().x, height);

		ImGui::BeginDisabled(!style.Enabled);
		const bool pressed = ImGui::InvisibleButton(id, ImVec2(buttonWidth, height));
		const bool hovered = ImGui::IsItemHovered();
		const bool held = ImGui::IsItemActive();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();

		ImVec4 background = held ? colors.Raised : (hovered ? colors.Border : colors.Control);
		ImVec4 border = hovered ? colors.TextDisabled : colors.Border;
		ImVec4 iconColor = colors.Accent;
		ImVec4 labelColor = colors.Text;
		if (style.Primary)
		{
			background = held ? colors.AccentActive : (hovered ? colors.AccentHover : colors.Accent);
			border = background;
			iconColor = colors.TextOnAccent;
			labelColor = colors.TextOnAccent;
		}
		else if (style.Active)
		{
			const ImVec4 tint = style.ActiveColor.value_or(colors.Accent);
			background = WithAlpha(tint, held ? 0.34f : (hovered ? 0.26f : 0.18f));
			border = WithAlpha(tint, 0.6f);
			iconColor = tint;
		}

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const float rounding = imguiStyle.FrameRounding * 1.5f;
		drawList->AddRectFilled(min, max, ToDrawColor(background), rounding);
		drawList->AddRect(min, max, ToDrawColor(border), rounding);
		const float textY = min.y + (height - ImGui::GetFontSize()) * 0.5f;
		AddText(drawList, ImVec2(min.x + padding, textY), iconColor, icon);
		drawList->PushClipRect(min, max, true);
		drawList->AddText(EditorFonts::Get(EditorFont::SemiBold), ImGui::GetFontSize(), ImVec2(min.x + padding + iconSize.x + gap, textY), ToDrawColor(labelColor),
			label.data(), label.data() + label.size());
		drawList->PopClipRect();
		ImGui::RenderNavCursor(ImRect(min, max), ImGui::GetItemID());
		ImGui::EndDisabled();

		ShowTooltip(tooltip);
		ItemProbe::Record(id, style.Enabled);
		return pressed;
	}

	bool LinkButton(const char* id, std::string_view label, const char* icon, const char* tooltip)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const ThemeColors& colors = GetThemeColors();
		const ImVec2 labelSize = ImGui::CalcTextSize(label.data(), label.data() + label.size());
		const float iconWidth = icon ? style.ItemInnerSpacing.x + ImGui::CalcTextSize(icon).x : 0.0f;
		const bool pressed = ImGui::InvisibleButton(id, ImVec2(labelSize.x + iconWidth, ImGui::GetTextLineHeight()));
		const bool hovered = ImGui::IsItemHovered();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();

		const ImVec4& color = hovered ? colors.Text : colors.TextSecondary;
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		AddText(drawList, min, color, label);
		if (icon)
			AddText(drawList, ImVec2(min.x + labelSize.x + style.ItemInnerSpacing.x, min.y), color, icon);
		if (hovered)
		{
			const float underline = std::floor(max.y) - 0.5f;
			drawList->AddLine(ImVec2(min.x, underline), ImVec2(min.x + labelSize.x, underline), ToDrawColor(color));
		}
		ImGui::RenderNavCursor(ImRect(min, max), ImGui::GetItemID());

		ShowTooltip(tooltip);
		ItemProbe::Record(id);
		return pressed;
	}

	bool TextField(const char* id, std::string_view label, std::string& value, const char* hint, float width)
	{
		const ThemeColors& colors = GetThemeColors();
		ImGui::PushID(id);
		PushFont(EditorFont::Regular, TextSize::Caption);
		ImGui::PushStyleColor(ImGuiCol_Text, colors.TextSecondary);
		ImGui::TextUnformatted(label.data(), label.data() + label.size());
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::SetNextItemWidth(width < 0.0f ? -FLT_MIN : width);
		const bool changed = ImGui::InputTextWithHint("##Field", hint ? hint : "", &value);
		ImGui::PopID();
		ItemProbe::Record(id);
		return changed;
	}

	bool CopyableCode(const char* id, std::string_view code)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const ThemeColors& colors = GetThemeColors();
		const std::string buttonId = std::string(id) + ".Copy";
		ImGui::PushID(id);

		const float buttonSize = ImGui::GetFrameHeight();
		const ImVec2 padding(style.FramePadding.x * 1.5f, style.FramePadding.y * 2.0f);
		const float width = std::max(ImGui::GetContentRegionAvail().x, buttonSize * 4.0f);
		const float wrapWidth = std::max(width - padding.x * 2.0f - buttonSize - style.ItemSpacing.x, 1.0f);
		PushFont(EditorFont::Mono, TextSize::Body);
		const ImVec2 textSize = ImGui::CalcTextSize(code.data(), code.data() + code.size(), false, wrapWidth);
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const ImVec2 size(width, std::max(textSize.y, buttonSize) + padding.y * 2.0f);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const float rounding = style.FrameRounding * 1.5f;
		drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), ToDrawColor(colors.Chrome), rounding);
		drawList->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), ToDrawColor(colors.Border), rounding);
		drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(origin.x + padding.x, origin.y + (size.y - textSize.y) * 0.5f), ToDrawColor(colors.Text),
			code.data(), code.data() + code.size(), wrapWidth);
		ImGui::PopFont();

		// The moment of the last copy, kept with the widget's state: the button confirms it for a moment.
		ImGuiStorage* storage = ImGui::GetStateStorage();
		const ImGuiID copiedKey = ImGui::GetID("CopiedAt");
		const double copiedAt = static_cast<double>(storage->GetFloat(copiedKey, -1.0e6f));
		const bool confirming = ImGui::GetTime() - copiedAt < c_CopiedFeedbackSeconds;

		ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - padding.y - buttonSize, origin.y + (size.y - buttonSize) * 0.5f));
		const bool copied = IconButton(buttonId.c_str(), confirming ? Icons::Check : Icons::Copy, confirming ? "Copied" : "Copy to the clipboard");
		if (copied)
		{
			ImGui::SetClipboardText(std::string(code).c_str());
			storage->SetFloat(copiedKey, static_cast<float>(ImGui::GetTime()));
		}
		ImGui::SetCursorScreenPos(origin);
		ImGui::Dummy(size);
		ImGui::PopID();
		return copied;
	}

	bool EntryCard(const char* id, const EntryCardContent& content, const ImVec2& size)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const ThemeColors& colors = GetThemeColors();
		const bool pressed = ImGui::InvisibleButton(id, size);
		const bool hovered = ImGui::IsItemHovered();
		const bool held = ImGui::IsItemActive();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();

		const ImVec4 background = held ? colors.Border : (hovered ? colors.Control : colors.Raised);
		const ImVec4 border = hovered ? colors.TextDisabled : colors.Border;
		const float rounding = style.FrameRounding * 2.0f;
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(min, max, ToDrawColor(background), rounding);
		drawList->AddRect(min, max, ToDrawColor(border), rounding);

		const float fontScale = GetFontScale();
		const ImVec2 padding = style.WindowPadding;
		drawList->PushClipRect(min, max, true);

		// The badge: a square as tall as the card's content.
		const float badgeSize = std::max(size.y - padding.y * 2.0f, 1.0f);
		const ImVec2 badgeMin(min.x + padding.x, min.y + (size.y - badgeSize) * 0.5f);
		const ImVec2 badgeMax(badgeMin.x + badgeSize, badgeMin.y + badgeSize);
		drawList->AddRectFilled(badgeMin, badgeMax, ToDrawColor(WithAlpha(content.BadgeColor, 0.16f)), rounding);
		drawList->AddRect(badgeMin, badgeMax, ToDrawColor(WithAlpha(content.BadgeColor, 0.4f)), rounding);
		ImFont* semiBold = EditorFonts::Get(EditorFont::SemiBold);
		PushFont(EditorFont::SemiBold, TextSize::Title);
		const ImVec2 badgeTextSize = ImGui::CalcTextSize(content.Badge.data(), content.Badge.data() + content.Badge.size());
		ImGui::PopFont();
		drawList->AddText(semiBold, GetTextSize(TextSize::Title) * fontScale,
			ImVec2(badgeMin.x + (badgeSize - badgeTextSize.x) * 0.5f, badgeMin.y + (badgeSize - badgeTextSize.y) * 0.5f), ToDrawColor(content.BadgeColor),
			content.Badge.data(), content.Badge.data() + content.Badge.size());

		// Title, subtitle and detail, centered vertically next to it.
		const float textX = badgeMax.x + padding.x;
		const float textWidth = std::max(max.x - padding.x - textX, 1.0f);
		const float titleSize = GetTextSize(TextSize::Body) * fontScale;
		const float captionSize = GetTextSize(TextSize::Caption) * fontScale;
		const float lineGap = style.ItemInnerSpacing.y;
		const float textHeight = titleSize + (captionSize + lineGap) * 2.0f;
		float y = min.y + (size.y - textHeight) * 0.5f;

		PushFont(EditorFont::SemiBold, TextSize::Body);
		ImGui::PushStyleColor(ImGuiCol_Text, colors.Text);
		ImGui::RenderTextEllipsis(drawList, ImVec2(textX, y), ImVec2(textX + textWidth, y + titleSize), textX + textWidth, content.Title.data(),
			content.Title.data() + content.Title.size(), nullptr);
		ImGui::PopStyleColor();
		ImGui::PopFont();
		y += titleSize + lineGap;

		PushFont(EditorFont::Regular, TextSize::Caption);
		const std::string subtitle = ElideStart(content.Subtitle, textWidth);
		AddText(drawList, ImVec2(textX, y), colors.TextSecondary, subtitle);
		y += captionSize + lineGap;
		ImGui::PushStyleColor(ImGuiCol_Text, colors.TextDisabled);
		ImGui::RenderTextEllipsis(drawList, ImVec2(textX, y), ImVec2(textX + textWidth, y + captionSize), textX + textWidth, content.Detail.data(),
			content.Detail.data() + content.Detail.size(), nullptr);
		ImGui::PopStyleColor();
		ImGui::PopFont();
		drawList->PopClipRect();
		ImGui::RenderNavCursor(ImRect(min, max), ImGui::GetItemID());

		ItemProbe::Record(id);
		return pressed;
	}

	void BrandMark(float size, bool tile)
	{
		const ThemePalette& palette = GetThemePalette();
		const ImVec2 min = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(size, size));
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		if (tile)
		{
			const float corner = size * (size < c_BrandMarkDetailSize ? 0.18f : 0.22f);
			drawList->AddRectFilled(min, ImVec2(min.x + size, min.y + size), ToDrawColor(palette.Basalt, true), corner);
			drawList->AddRect(min, ImVec2(min.x + size, min.y + size), ToDrawColor(palette.Seam, true), corner);
		}

		// The proportions of Tools/GenerateBrandAssets.py: small marks keep three thicker bands.
		const bool compact = size < c_BrandMarkDetailSize;
		const ImVec4 colors[] = { palette.Sandstone, palette.Ochre, palette.Rust, palette.Umber };
		const int count = compact ? 3 : 4;
		const float thickness = compact ? 0.16f : 0.115f;
		const float gap = compact ? 0.10f : 0.06f;
		const float width = compact ? 0.58f : 0.56f;
		const float step = compact ? 0.08f : 0.06f;
		const float top = (1.0f - (static_cast<float>(count) * thickness + static_cast<float>(count - 1) * gap)) * 0.5f;
		for (int index = 0; index < count; index++)
		{
			// Staggered like layers along a fault: upper bands sit further right.
			const float center = 0.5f + (static_cast<float>(count - 1) * 0.5f - static_cast<float>(index)) * step;
			const float bandTop = top + static_cast<float>(index) * (thickness + gap);
			drawList->AddRectFilled(ImVec2(min.x + (center - width * 0.5f) * size, min.y + bandTop * size),
				ImVec2(min.x + (center + width * 0.5f) * size, min.y + (bandTop + thickness) * size), ToDrawColor(colors[index], true), thickness * 0.5f * size);
		}
	}

	std::string ElideStart(std::string_view text, float maxWidth)
	{
		if (ImGui::CalcTextSize(text.data(), text.data() + text.size()).x <= maxWidth)
			return std::string(text);
		constexpr std::string_view ellipsis = "\xE2\x80\xA6";
		const float ellipsisWidth = ImGui::CalcTextSize(ellipsis.data(), ellipsis.data() + ellipsis.size()).x;
		// The longest end of the text that fits after the ellipsis, cut at a character boundary. The width only shrinks as the
		// start moves right, so the start is searched for by bisection.
		auto toBoundary = [text](size_t position)
		{
			while (position < text.size() && (static_cast<unsigned char>(text[position]) & 0xC0) == 0x80)
				position++;
			return position;
		};
		auto fits = [&](size_t start)
		{
			return ellipsisWidth + ImGui::CalcTextSize(text.data() + start, text.data() + text.size()).x <= maxWidth;
		};
		size_t low = 0;           // Does not fit
		size_t high = text.size(); // Fits (the ellipsis alone)
		while (high - low > 1)
		{
			const size_t middle = toBoundary(low + (high - low) / 2);
			if (middle >= high)
				break;
			if (fits(middle))
				high = middle;
			else
				low = middle;
		}
		return std::string(ellipsis) + std::string(text.substr(high));
	}

	float DialogButtonWidth(const char* label)
	{
		return std::max(ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f, ImGui::GetFontSize() * c_DialogButtonWidthInFontSizes);
	}

	bool DialogButton(const char* id, const char* label, bool primary, bool enabled)
	{
		const ThemeColors& colors = GetThemeColors();
		const float width = DialogButtonWidth(label);
		ImGui::BeginDisabled(!enabled);
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
		ImGui::EndDisabled();
		ItemProbe::Record(id, enabled);
		return pressed;
	}

}
