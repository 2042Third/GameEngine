#include "UI/Theme.h"

#include "UI/EditorFonts.h"

#include <array>
#include <cstdint>
#include <iterator>

namespace Strata::UI
{

	namespace
	{

		// The only place colors are spelled out (the source scan in EditorUITests.cpp enforces it).
		constexpr ImVec4 FromRGB(uint32_t rgb)
		{
			return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
				static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f);
		}

		constexpr ThemePalette c_Bedrock = {
			.Basalt = FromRGB(0x0E1013),
			.Shale = FromRGB(0x15181C),
			.Slate = FromRGB(0x1D2126),
			.Flint = FromRGB(0x262B31),
			.Seam = FromRGB(0x323840),
			.Limestone = FromRGB(0xE8E4DC),
			.Ash = FromRGB(0x9BA1A9),
			.Dust = FromRGB(0x5E656D),
			.Ochre = FromRGB(0xE08A2E),
			.Sandstone = FromRGB(0xF2B872),
			.Rust = FromRGB(0xB9562B),
			.Umber = FromRGB(0x6B3A22),
			.Malachite = FromRGB(0x3DBE8B),
			.Azurite = FromRGB(0x4C8FE0),
			.Sulfur = FromRGB(0xE9C34D),
			.Cinnabar = FromRGB(0xE5534B)
		};

		constexpr NamedThemeColor c_Tokens[] = {
			{ "Basalt", c_Bedrock.Basalt },
			{ "Shale", c_Bedrock.Shale },
			{ "Slate", c_Bedrock.Slate },
			{ "Flint", c_Bedrock.Flint },
			{ "Seam", c_Bedrock.Seam },
			{ "Limestone", c_Bedrock.Limestone },
			{ "Ash", c_Bedrock.Ash },
			{ "Dust", c_Bedrock.Dust },
			{ "Ochre", c_Bedrock.Ochre },
			{ "Sandstone", c_Bedrock.Sandstone },
			{ "Rust", c_Bedrock.Rust },
			{ "Umber", c_Bedrock.Umber },
			{ "Malachite", c_Bedrock.Malachite },
			{ "Azurite", c_Bedrock.Azurite },
			{ "Sulfur", c_Bedrock.Sulfur },
			{ "Cinnabar", c_Bedrock.Cinnabar }
		};
		static_assert(std::size(c_Tokens) * sizeof(ImVec4) == sizeof(ThemePalette), "Every palette token needs a name");

		constexpr ImVec4 WithAlphaConstant(const ImVec4& color, float alpha)
		{
			return ImVec4(color.x, color.y, color.z, alpha);
		}

		constexpr ThemeColors c_Colors = {
			.Accent = c_Bedrock.Ochre,
			.AccentHover = c_Bedrock.Sandstone,
			.AccentActive = c_Bedrock.Rust,
			.AccentMuted = c_Bedrock.Umber,
			.SelectionHovered = WithAlphaConstant(c_Bedrock.Rust, 0.7f),
			.Success = c_Bedrock.Malachite,
			.Info = c_Bedrock.Azurite,
			.Warning = c_Bedrock.Sulfur,
			.Error = c_Bedrock.Cinnabar,
			.AxisX = c_Bedrock.Cinnabar,
			.AxisY = c_Bedrock.Malachite,
			.AxisZ = c_Bedrock.Azurite,
			.PlayState = { .Edit = c_Bedrock.Ash, .Play = c_Bedrock.Malachite, .Simulate = c_Bedrock.Azurite, .Paused = c_Bedrock.Sulfur },
			.Text = c_Bedrock.Limestone,
			.TextSecondary = c_Bedrock.Ash,
			.TextDisabled = c_Bedrock.Dust,
			.TextOnAccent = c_Bedrock.Basalt,
			.Chrome = c_Bedrock.Basalt,
			.Panel = c_Bedrock.Shale,
			.Raised = c_Bedrock.Slate,
			.Control = c_Bedrock.Flint,
			.Border = c_Bedrock.Seam,
			.Overlay = WithAlphaConstant(c_Bedrock.Basalt, 0.78f)
		};

		// The palette token (and opacity) of every ImGui color.
		struct ColorRole
		{
			ImGuiCol Slot;
			ImVec4 ThemePalette::*Token;
			float Alpha;
		};

		constexpr ColorRole c_ColorRoles[] = {
			{ ImGuiCol_Text, &ThemePalette::Limestone, 1.0f },
			// ImGui draws hints, disabled menu items and TextDisabled() with it: secondary text must stay readable.
			{ ImGuiCol_TextDisabled, &ThemePalette::Ash, 1.0f },
			{ ImGuiCol_WindowBg, &ThemePalette::Shale, 1.0f },
			{ ImGuiCol_ChildBg, &ThemePalette::Shale, 0.0f },
			{ ImGuiCol_PopupBg, &ThemePalette::Slate, 0.98f },
			{ ImGuiCol_Border, &ThemePalette::Seam, 1.0f },
			{ ImGuiCol_BorderShadow, &ThemePalette::Basalt, 0.0f },
			{ ImGuiCol_FrameBg, &ThemePalette::Basalt, 1.0f },
			{ ImGuiCol_FrameBgHovered, &ThemePalette::Slate, 1.0f },
			{ ImGuiCol_FrameBgActive, &ThemePalette::Flint, 1.0f },
			{ ImGuiCol_TitleBg, &ThemePalette::Basalt, 1.0f },
			{ ImGuiCol_TitleBgActive, &ThemePalette::Slate, 1.0f },
			{ ImGuiCol_TitleBgCollapsed, &ThemePalette::Basalt, 0.75f },
			{ ImGuiCol_MenuBarBg, &ThemePalette::Basalt, 1.0f },
			{ ImGuiCol_ScrollbarBg, &ThemePalette::Shale, 0.0f },
			{ ImGuiCol_ScrollbarGrab, &ThemePalette::Seam, 1.0f },
			{ ImGuiCol_ScrollbarGrabHovered, &ThemePalette::Dust, 1.0f },
			{ ImGuiCol_ScrollbarGrabActive, &ThemePalette::Ash, 1.0f },
			{ ImGuiCol_CheckMark, &ThemePalette::Ochre, 1.0f },
			{ ImGuiCol_CheckboxSelectedBg, &ThemePalette::Umber, 1.0f },
			{ ImGuiCol_SliderGrab, &ThemePalette::Ochre, 1.0f },
			{ ImGuiCol_SliderGrabActive, &ThemePalette::Sandstone, 1.0f },
			{ ImGuiCol_Button, &ThemePalette::Flint, 1.0f },
			{ ImGuiCol_ButtonHovered, &ThemePalette::Seam, 1.0f },
			{ ImGuiCol_ButtonActive, &ThemePalette::Umber, 1.0f },
			// Selected rows (hierarchy, combos, selectables) carry the accent, like the viewport's selection outline. Hovering
			// is neutral, so it is not mistaken for selecting; selected rows keep the accent under the mouse through
			// UI::PushSelectionColors (ImGui draws every hovered row in HeaderHovered).
			{ ImGuiCol_Header, &ThemePalette::Umber, 0.9f },
			{ ImGuiCol_HeaderHovered, &ThemePalette::Flint, 1.0f },
			{ ImGuiCol_HeaderActive, &ThemePalette::Rust, 1.0f },
			{ ImGuiCol_Separator, &ThemePalette::Seam, 1.0f },
			{ ImGuiCol_SeparatorHovered, &ThemePalette::Rust, 1.0f },
			{ ImGuiCol_SeparatorActive, &ThemePalette::Ochre, 1.0f },
			{ ImGuiCol_ResizeGrip, &ThemePalette::Dust, 0.25f },
			{ ImGuiCol_ResizeGripHovered, &ThemePalette::Rust, 0.7f },
			{ ImGuiCol_ResizeGripActive, &ThemePalette::Ochre, 1.0f },
			{ ImGuiCol_InputTextCursor, &ThemePalette::Sandstone, 1.0f },
			{ ImGuiCol_TabHovered, &ThemePalette::Flint, 1.0f },
			{ ImGuiCol_Tab, &ThemePalette::Basalt, 1.0f },
			// The selected tab has its panel's color, so tab and panel read as one surface.
			{ ImGuiCol_TabSelected, &ThemePalette::Shale, 1.0f },
			{ ImGuiCol_TabSelectedOverline, &ThemePalette::Ochre, 1.0f },
			{ ImGuiCol_TabDimmed, &ThemePalette::Basalt, 1.0f },
			{ ImGuiCol_TabDimmedSelected, &ThemePalette::Shale, 1.0f },
			{ ImGuiCol_TabDimmedSelectedOverline, &ThemePalette::Seam, 1.0f },
			{ ImGuiCol_DockingPreview, &ThemePalette::Ochre, 0.4f },
			{ ImGuiCol_DockingEmptyBg, &ThemePalette::Basalt, 1.0f },
			{ ImGuiCol_PlotLines, &ThemePalette::Ash, 1.0f },
			{ ImGuiCol_PlotLinesHovered, &ThemePalette::Sandstone, 1.0f },
			{ ImGuiCol_PlotHistogram, &ThemePalette::Ochre, 1.0f },
			{ ImGuiCol_PlotHistogramHovered, &ThemePalette::Sandstone, 1.0f },
			{ ImGuiCol_TableHeaderBg, &ThemePalette::Slate, 1.0f },
			{ ImGuiCol_TableBorderStrong, &ThemePalette::Seam, 1.0f },
			{ ImGuiCol_TableBorderLight, &ThemePalette::Flint, 1.0f },
			{ ImGuiCol_TableRowBg, &ThemePalette::Shale, 0.0f },
			{ ImGuiCol_TableRowBgAlt, &ThemePalette::Slate, 0.5f },
			{ ImGuiCol_TextLink, &ThemePalette::Azurite, 1.0f },
			{ ImGuiCol_TextSelectedBg, &ThemePalette::Ochre, 0.35f },
			{ ImGuiCol_TreeLines, &ThemePalette::Seam, 1.0f },
			{ ImGuiCol_DragDropTarget, &ThemePalette::Ochre, 1.0f },
			{ ImGuiCol_DragDropTargetBg, &ThemePalette::Ochre, 0.12f },
			{ ImGuiCol_UnsavedMarker, &ThemePalette::Sandstone, 1.0f },
			{ ImGuiCol_NavCursor, &ThemePalette::Ochre, 1.0f },
			{ ImGuiCol_NavWindowingHighlight, &ThemePalette::Limestone, 0.7f },
			{ ImGuiCol_NavWindowingDimBg, &ThemePalette::Basalt, 0.6f },
			{ ImGuiCol_ModalWindowDimBg, &ThemePalette::Basalt, 0.7f }
		};
		// An ImGui upgrade that adds colors fails here until they are given a token.
		static_assert(std::size(c_ColorRoles) == ImGuiCol_COUNT, "Give every ImGuiCol_ a palette token");

		// Base sizes in unscaled pixels. All even, so scaled by 1.5 or 2 they stay whole pixels (ScaleAllSizes truncates).
		void SetBaseSizes(ImGuiStyle& style)
		{
			style.WindowPadding = ImVec2(8.0f, 8.0f);
			style.FramePadding = ImVec2(8.0f, 4.0f);
			style.ItemSpacing = ImVec2(8.0f, 6.0f);
			style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
			style.CellPadding = ImVec2(6.0f, 4.0f);
			style.IndentSpacing = 20.0f;
			style.ScrollbarSize = 12.0f;
			style.GrabMinSize = 10.0f;
			style.WindowMinSize = ImVec2(32.0f, 32.0f);

			style.WindowRounding = 0.0f;
			style.ChildRounding = 0.0f;
			style.PopupRounding = 6.0f;
			style.FrameRounding = 4.0f;
			style.GrabRounding = 4.0f;
			style.TabRounding = 4.0f;
			style.ScrollbarRounding = 6.0f;

			// Hairlines: one pixel up to a UI scale of 2.
			style.WindowBorderSize = 1.0f;
			style.ChildBorderSize = 1.0f;
			style.PopupBorderSize = 1.0f;
			style.FrameBorderSize = 1.0f;
			style.TabBorderSize = 0.0f;
			style.TabBarBorderSize = 1.0f;
			style.TabBarOverlineSize = 2.0f;
			style.SeparatorTextBorderSize = 2.0f;
		}

	}

	const ThemePalette& GetThemePalette()
	{
		return c_Bedrock;
	}

	const ThemeColors& GetThemeColors()
	{
		return c_Colors;
	}

	std::span<const NamedThemeColor> GetThemePaletteTokens()
	{
		return c_Tokens;
	}

	void ApplyTheme(ImGuiStyle& style, float scale)
	{
		for (const ColorRole& role : c_ColorRoles)
			style.Colors[role.Slot] = WithAlpha(c_Bedrock.*role.Token, role.Alpha);

		SetBaseSizes(style);
		style.ScaleAllSizes(scale);

		style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
		// No triangle menu and no close button on dock nodes: every tab has its own close button, and closed panels come
		// back through the View menu.
		style.WindowMenuButtonPosition = ImGuiDir_None;
		style.DockingNodeHasCloseButton = false;
		style.DisabledAlpha = 0.5f;
		style.FontSizeBase = GetTextSize(TextSize::Body);
	}

	ImU32 ToColorU32(const ImVec4& color, float alpha)
	{
		return ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, color.w * alpha));
	}

	ImVec4 WithAlpha(const ImVec4& color, float alpha)
	{
		return WithAlphaConstant(color, alpha);
	}

}
