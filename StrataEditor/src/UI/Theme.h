#pragma once

#include <imgui.h>

#include <span>

namespace Strata::UI
{

	// The 'Bedrock' palette: mineral surfaces, limestone text and an ochre accent. Colors are sRGB values, the way ImGui
	// draws them. Code never spells colors itself: it uses these tokens or, better, their meanings (ThemeColors).
	struct ThemePalette
	{
		// Surfaces, darkest first.
		ImVec4 Basalt;    // #0E1013: chrome bands (menu bar, toolbar, status bar), recessed fields, empty dock space
		ImVec4 Shale;     // #15181C: panels
		ImVec4 Slate;     // #1D2126: raised surfaces (popups, headers, cards)
		ImVec4 Flint;     // #262B31: controls (buttons), hovered rows
		ImVec4 Seam;      // #323840: borders and separators
		// Text.
		ImVec4 Limestone; // #E8E4DC: text
		ImVec4 Ash;       // #9BA1A9: secondary text, hints
		ImVec4 Dust;      // #5E656D: disabled and decorative elements
		// The accent.
		ImVec4 Ochre;     // #E08A2E: selection, primary actions, focus
		ImVec4 Sandstone; // #F2B872: the accent hovered
		ImVec4 Rust;      // #B9562B: the accent pressed
		ImVec4 Umber;     // #6B3A22: accent backgrounds (selected rows)
		// Meanings.
		ImVec4 Malachite; // #3DBE8B: success, playing, the Y axis
		ImVec4 Azurite;   // #4C8FE0: information, simulating, the Z axis
		ImVec4 Sulfur;    // #E9C34D: warnings, paused
		ImVec4 Cinnabar;  // #E5534B: errors, the X axis
	};

	// What the colors mean, for widgets and panels.
	struct ThemeColors
	{
		struct PlayStateColors
		{
			ImVec4 Edit;     // Ash
			ImVec4 Play;     // Malachite
			ImVec4 Simulate; // Azurite
			ImVec4 Paused;   // Sulfur
		};

		ImVec4 Accent;       // Ochre
		ImVec4 AccentHover;  // Sandstone
		ImVec4 AccentActive; // Rust
		ImVec4 AccentMuted;  // Umber: backgrounds of selected or active items
		// Rust, translucent: a selected row under the mouse (UI::PushSelectionColors), brighter than a selected row, while
		// rows that are not selected hover in a neutral color.
		ImVec4 SelectionHovered;
		ImVec4 Success;      // Malachite
		ImVec4 Info;         // Azurite
		ImVec4 Warning;      // Sulfur
		ImVec4 Error;        // Cinnabar
		ImVec4 AxisX;        // Cinnabar
		ImVec4 AxisY;        // Malachite
		ImVec4 AxisZ;        // Azurite
		PlayStateColors PlayState;

		ImVec4 Text;          // Limestone
		ImVec4 TextSecondary; // Ash
		ImVec4 TextDisabled;  // Dust
		ImVec4 TextOnAccent;  // Basalt: text and icons on accent-filled surfaces
		ImVec4 Chrome;        // Basalt: the bands around the panels
		ImVec4 Panel;         // Shale
		ImVec4 Raised;        // Slate
		ImVec4 Control;       // Flint
		ImVec4 Border;        // Seam
		ImVec4 Overlay;       // Basalt, translucent: boxes over the viewport image
	};

	const ThemePalette& GetThemePalette();
	const ThemeColors& GetThemeColors();

	// Every token of the palette, for code that checks colors against it (tests).
	struct NamedThemeColor
	{
		const char* Name;
		ImVec4 Color;
	};
	std::span<const NamedThemeColor> GetThemePaletteTokens();

	// Styles ImGui as Bedrock for a UI scale (an ImGuiStyleCallback, see ImGuiLayer::SetStyleCallback): every ImGuiCol_
	// from the palette, 4 px rounding for frames and tabs, square windows, hairline borders in Seam, an Ochre overline on
	// the selected tab and neither a window menu nor a close button on dock nodes (tabs have their own). Sizes are the
	// theme's base sizes times the scale
	// (ImGuiStyle::ScaleAllSizes); the base sizes are even, so 1.5x and 2x give whole pixels. Fonts are scaled separately
	// (ImGuiStyle::FontScaleDpi).
	void ApplyTheme(ImGuiStyle& style, float scale);

	// A color as ImGui's packed form, its alpha multiplied (e.g. for translucent fills).
	ImU32 ToColorU32(const ImVec4& color, float alpha = 1.0f);
	// The color with another alpha.
	ImVec4 WithAlpha(const ImVec4& color, float alpha);

}
