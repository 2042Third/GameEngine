#include <doctest/doctest.h>

#include "UI/EditorFonts.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace Strata;

namespace
{

	bool SameColor(const ImVec4& a, const ImVec4& b)
	{
		return std::abs(a.x - b.x) < 1e-6f && std::abs(a.y - b.y) < 1e-6f && std::abs(a.z - b.z) < 1e-6f && std::abs(a.w - b.w) < 1e-6f;
	}

	bool SameRGB(const ImVec4& a, const ImVec4& b)
	{
		return SameColor(ImVec4(a.x, a.y, a.z, 1.0f), ImVec4(b.x, b.y, b.z, 1.0f));
	}

	using NamedSizes = std::vector<std::pair<std::string, float>>;

	// Every size ImGuiStyle::ScaleAllSizes scales (imgui.cpp), but the hairlines and the tab close button settings
	// below: they all scale with the UI.
	NamedSizes CollectScaledSizes(const ImGuiStyle& style)
	{
		return {
			{ "WindowPadding.x", style.WindowPadding.x }, { "WindowPadding.y", style.WindowPadding.y },
			{ "WindowRounding", style.WindowRounding },
			{ "WindowMinSize.x", style.WindowMinSize.x }, { "WindowMinSize.y", style.WindowMinSize.y },
			{ "WindowBorderHoverPadding", style.WindowBorderHoverPadding },
			{ "ChildRounding", style.ChildRounding }, { "PopupRounding", style.PopupRounding },
			{ "FramePadding.x", style.FramePadding.x }, { "FramePadding.y", style.FramePadding.y },
			{ "FrameRounding", style.FrameRounding },
			{ "ItemSpacing.x", style.ItemSpacing.x }, { "ItemSpacing.y", style.ItemSpacing.y },
			{ "ItemInnerSpacing.x", style.ItemInnerSpacing.x }, { "ItemInnerSpacing.y", style.ItemInnerSpacing.y },
			{ "CellPadding.x", style.CellPadding.x }, { "CellPadding.y", style.CellPadding.y },
			{ "TouchExtraPadding.x", style.TouchExtraPadding.x }, { "TouchExtraPadding.y", style.TouchExtraPadding.y },
			{ "IndentSpacing", style.IndentSpacing }, { "ColumnsMinSpacing", style.ColumnsMinSpacing },
			{ "ScrollbarSize", style.ScrollbarSize }, { "ScrollbarRounding", style.ScrollbarRounding }, { "ScrollbarPadding", style.ScrollbarPadding },
			{ "GrabMinSize", style.GrabMinSize }, { "GrabRounding", style.GrabRounding }, { "LogSliderDeadzone", style.LogSliderDeadzone },
			{ "ImageRounding", style.ImageRounding }, { "ImageBorderSize", style.ImageBorderSize },
			{ "TabRounding", style.TabRounding }, { "TabBorderSize", style.TabBorderSize }, { "TabMinWidthShrink", style.TabMinWidthShrink },
			{ "TabBarOverlineSize", style.TabBarOverlineSize },
			{ "TreeLinesRounding", style.TreeLinesRounding }, { "MenuItemRounding", style.MenuItemRounding },
			{ "SelectableRounding", style.SelectableRounding },
			{ "DragDropTargetRounding", style.DragDropTargetRounding }, { "DragDropTargetBorderSize", style.DragDropTargetBorderSize },
			{ "DragDropTargetPadding", style.DragDropTargetPadding }, { "ColorMarkerSize", style.ColorMarkerSize },
			{ "SeparatorTextBorderSize", style.SeparatorTextBorderSize },
			{ "SeparatorTextPadding.x", style.SeparatorTextPadding.x }, { "SeparatorTextPadding.y", style.SeparatorTextPadding.y },
			{ "DockingSeparatorSize", style.DockingSeparatorSize },
			{ "DisplayWindowPadding.x", style.DisplayWindowPadding.x }, { "DisplayWindowPadding.y", style.DisplayWindowPadding.y },
			{ "DisplaySafeAreaPadding.x", style.DisplaySafeAreaPadding.x }, { "DisplaySafeAreaPadding.y", style.DisplaySafeAreaPadding.y },
			{ "MouseCursorScale", style.MouseCursorScale }
		};
	}

	// The exceptions: one-pixel lines (and the smallest tab width, one pixel: no minimum) stay one pixel up to a UI scale of
	// 2, rather than turning into blurry 1.5 pixel lines at 150%.
	NamedSizes CollectHairlines(const ImGuiStyle& style)
	{
		return {
			{ "WindowBorderSize", style.WindowBorderSize }, { "ChildBorderSize", style.ChildBorderSize },
			{ "PopupBorderSize", style.PopupBorderSize }, { "FrameBorderSize", style.FrameBorderSize },
			{ "TabBarBorderSize", style.TabBarBorderSize }, { "TreeLinesSize", style.TreeLinesSize },
			{ "InputTextCursorSize", style.InputTextCursorSize }, { "SeparatorSize", style.SeparatorSize },
			{ "TabMinWidthBase", style.TabMinWidthBase }
		};
	}

}

TEST_SUITE("Editor.Theme")
{
	TEST_CASE("ApplyTheme sets every ImGui color from the Bedrock palette")
	{
		ImGuiStyle style;
		UI::ApplyTheme(style, 1.0f);
		const std::span<const UI::NamedThemeColor> tokens = UI::GetThemePaletteTokens();
		REQUIRE(tokens.size() == 16);
		for (int slot = 0; slot < ImGuiCol_COUNT; slot++)
		{
			CAPTURE(ImGui::GetStyleColorName(slot));
			const ImVec4& color = style.Colors[slot];
			bool fromPalette = false;
			for (const UI::NamedThemeColor& token : tokens)
				fromPalette |= SameRGB(color, token.Color);
			CHECK(fromPalette);
			CHECK(color.w >= 0.0f);
			CHECK(color.w <= 1.0f);
		}

		// The palette's values (sRGB, as the owner's brief gives them).
		const UI::ThemePalette& palette = UI::GetThemePalette();
		CHECK(UI::ToColorU32(palette.Basalt) == IM_COL32(0x0E, 0x10, 0x13, 0xFF));
		CHECK(UI::ToColorU32(palette.Limestone) == IM_COL32(0xE8, 0xE4, 0xDC, 0xFF));
		CHECK(UI::ToColorU32(palette.Ochre) == IM_COL32(0xE0, 0x8A, 0x2E, 0xFF));
		CHECK(UI::ToColorU32(palette.Cinnabar) == IM_COL32(0xE5, 0x53, 0x4B, 0xFF));
		CHECK(SameColor(style.Colors[ImGuiCol_Text], palette.Limestone));
		CHECK(SameColor(style.Colors[ImGuiCol_WindowBg], palette.Shale));
		CHECK(SameColor(style.Colors[ImGuiCol_Border], palette.Seam));
		CHECK(SameColor(style.Colors[ImGuiCol_TabSelectedOverline], palette.Ochre));
	}

	TEST_CASE("The accent slots differ from ImGui's dark style")
	{
		ImGuiStyle dark;
		ImGui::StyleColorsDark(&dark);
		ImGuiStyle themed;
		UI::ApplyTheme(themed, 1.0f);
		for (const ImGuiCol slot : { ImGuiCol_CheckMark, ImGuiCol_SliderGrab, ImGuiCol_ButtonActive, ImGuiCol_TabSelectedOverline, ImGuiCol_NavCursor,
			ImGuiCol_HeaderActive })
		{
			CAPTURE(ImGui::GetStyleColorName(slot));
			CHECK_FALSE(SameColor(themed.Colors[slot], dark.Colors[slot]));
		}
		const UI::ThemeColors& colors = UI::GetThemeColors();
		CHECK(SameColor(themed.Colors[ImGuiCol_CheckMark], colors.Accent));
		CHECK(SameColor(themed.Colors[ImGuiCol_NavCursor], colors.Accent));
		// The meanings of the semantic slots.
		const UI::ThemePalette& palette = UI::GetThemePalette();
		CHECK(SameColor(colors.AxisX, palette.Cinnabar));
		CHECK(SameColor(colors.AxisY, palette.Malachite));
		CHECK(SameColor(colors.AxisZ, palette.Azurite));
		CHECK(SameColor(colors.PlayState.Play, palette.Malachite));
		CHECK(SameColor(colors.PlayState.Simulate, palette.Azurite));
		CHECK(SameColor(colors.PlayState.Paused, palette.Sulfur));
	}

	TEST_CASE("The theme's sizes are exact multiples of the content scale")
	{
		ImGuiStyle base;
		UI::ApplyTheme(base, 1.0f);
		CHECK(base.FramePadding.x == 8.0f);
		CHECK(base.FramePadding.y == 4.0f);
		CHECK(base.ItemSpacing.x == 8.0f);
		CHECK(base.ItemSpacing.y == 6.0f);
		CHECK(base.FrameRounding == 4.0f);
		CHECK(base.TabRounding == 4.0f);
		CHECK(base.WindowRounding == 0.0f);
		CHECK(base.WindowBorderSize == 1.0f);
		CHECK(base.WindowMenuButtonPosition == ImGuiDir_None);
		CHECK_FALSE(base.DockingNodeHasCloseButton);
		CHECK(base.FontSizeBase == UI::GetTextSize(UI::TextSize::Body));

		const NamedSizes baseSizes = CollectScaledSizes(base);
		for (const std::pair<std::string, float>& hairline : CollectHairlines(base))
		{
			CAPTURE(hairline.first);
			CHECK(hairline.second == 1.0f);
		}
		for (const float scale : { 1.0f, 1.5f, 2.0f })
		{
			CAPTURE(scale);
			ImGuiStyle scaled;
			UI::ApplyTheme(scaled, scale);
			const NamedSizes sizes = CollectScaledSizes(scaled);
			REQUIRE(sizes.size() == baseSizes.size());
			for (size_t index = 0; index < sizes.size(); index++)
			{
				CAPTURE(sizes[index].first);
				CHECK(std::abs(sizes[index].second - baseSizes[index].second * scale) <= 0.01f);
			}
			for (const std::pair<std::string, float>& hairline : CollectHairlines(scaled))
			{
				CAPTURE(hairline.first);
				CHECK(hairline.second == std::floor(scale));
			}
			// Not sizes: always shown on the selected tab, on hover on the others.
			CHECK(scaled.TabCloseButtonMinWidthSelected == -1.0f);
			CHECK(scaled.TabCloseButtonMinWidthUnselected == 0.0f);
		}

		// The theme sets every size: a style that was scaled before gets the same sizes as a fresh one.
		ImGuiStyle reused;
		reused.ScaleAllSizes(3.0f);
		UI::ApplyTheme(reused, 1.5f);
		ImGuiStyle fresh;
		UI::ApplyTheme(fresh, 1.5f);
		const NamedSizes reusedSizes = CollectScaledSizes(reused);
		const NamedSizes freshSizes = CollectScaledSizes(fresh);
		for (size_t index = 0; index < freshSizes.size(); index++)
		{
			CAPTURE(freshSizes[index].first);
			CHECK(reusedSizes[index].second == freshSizes[index].second);
		}
		CHECK(CollectHairlines(reused) == CollectHairlines(fresh));
	}

	TEST_CASE("Hovered rows stay neutral, and selected rows keep the accent under the mouse")
	{
		ImGuiStyle style;
		UI::ApplyTheme(style, 1.0f);
		const UI::ThemePalette& palette = UI::GetThemePalette();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		// Selected rows carry the accent family, hovered ones a neutral surface (hovering is not selecting).
		CHECK(SameRGB(style.Colors[ImGuiCol_Header], palette.Umber));
		CHECK(SameRGB(style.Colors[ImGuiCol_HeaderHovered], palette.Flint));
		// A selected row under the mouse: the accent's pressed hue, on the panel brighter than a selected row and warm.
		CHECK(SameRGB(colors.SelectionHovered, palette.Rust));
		const auto overPanel = [&](const ImVec4& color)
		{
			const ImVec4& panel = colors.Panel;
			return ImVec4(color.x * color.w + panel.x * (1.0f - color.w), color.y * color.w + panel.y * (1.0f - color.w),
				color.z * color.w + panel.z * (1.0f - color.w), 1.0f);
		};
		const ImVec4 selected = overPanel(style.Colors[ImGuiCol_Header]);
		const ImVec4 selectedHovered = overPanel(colors.SelectionHovered);
		CHECK(selectedHovered.x > selected.x + 0.08f);
		CHECK(selectedHovered.x > selectedHovered.y);
		CHECK(selectedHovered.y > selectedHovered.z);
	}

	TEST_CASE("The type scale")
	{
		CHECK(UI::GetTextSize(UI::TextSize::Caption) == 12.0f);
		CHECK(UI::GetTextSize(UI::TextSize::Body) == 14.0f);
		CHECK(UI::GetTextSize(UI::TextSize::Title) == 17.0f);
		CHECK(UI::GetTextSize(UI::TextSize::Display) == 24.0f);
	}
}
