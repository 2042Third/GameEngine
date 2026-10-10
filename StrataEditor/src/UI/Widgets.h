#pragma once

#include "UI/EditorFonts.h"

#include <imgui.h>

#include <optional>
#include <string_view>

namespace Strata::UI
{

	// The editor's widget kit: Bedrock-styled controls built on ImGui. Sizes follow the current font (they scale with the
	// UI), colors come from the theme (UI/Theme.h), and every widget records its rectangle in the ItemProbe under its id,
	// so UI tests can find and click it. Ids are ImGui ids (unique within the current ID scope) and probe keys (give the
	// widgets tests look for a unique, stable id such as "Toolbar.Play").

	struct ButtonStyle
	{
		bool Enabled = true;
		// Drawn as "on": a toggle that is set, the current mode, the running play state.
		bool Active = false;
		// The color of the active state (the accent when unset), e.g. the play-state color of Play while playing.
		std::optional<ImVec4> ActiveColor;
		// Filled with the accent: the main action of its group (at most one per group).
		bool Primary = false;
	};

	// A frameless, square icon button (frame-height sized) for inline actions such as removing a row's item. Shows the
	// tooltip on hover. Returns true when clicked.
	bool IconButton(const char* id, const char* icon, const char* tooltip, bool enabled = true);

	// A toolbar button: an icon, optionally followed by a label, frame-height tall (square without a label), in the given
	// style. Returns true when clicked.
	bool ToolbarButton(const char* id, const char* icon, const char* label, const char* tooltip, const ButtonStyle& style = {});

	// A rounded chip with an icon and a label on a translucent surface (for overlays such as the viewport's) that opens
	// something: a popup or a menu. Clicking a chip does not focus its window. Returns true when clicked.
	bool Chip(const char* id, const char* icon, const char* label, const char* tooltip);
	// A chip that toggles a setting: accent-tinted while on. Flips *value when clicked and returns true then.
	bool ToggleChip(const char* id, const char* icon, const char* label, bool* value, const char* tooltip);

	// A status pill: an icon and text in a color on a tint of that color, sized to its text in the current font. Returns
	// true when clicked (pills that lead somewhere, e.g. the errors pill opening the Console).
	bool Pill(const char* id, const char* icon, std::string_view text, const ImVec4& color, const char* tooltip = nullptr);

	// A collapsible section header in the SemiBold font on a raised surface (CollapsingHeader). Add
	// ImGuiTreeNodeFlags_AllowOverlap to place buttons on it. Returns whether the section is open.
	bool SectionHeader(const char* id, const char* label, ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen);

	// A heading in the SemiBold font at a size of the type scale (Title, or Display for large titles).
	void Heading(std::string_view text, TextSize size = TextSize::Title);

	// A clickable card: a raised, rounded surface with an icon, a title and a description (e.g. project templates or recent
	// projects). The size is in pixels: derive it from the font size. Returns true when clicked.
	bool Card(const char* id, const char* icon, std::string_view title, std::string_view description, const ImVec2& size, bool selected = false);

	// Modal dialogs: OpenModal(name) once (e.g. from a menu item), then every frame BeginModal(name, ...) and, while it
	// returns true, the contents and EndModal(). The dialog is centered on the main viewport, sized to its contents, and
	// shows the title as a heading. With `open`, a close button sets it to false.
	void OpenModal(const char* name);
	bool BeginModal(const char* name, std::string_view title, bool* open = nullptr);
	void EndModal();
	// A dialog button of a uniform width (from the font), the primary one filled with the accent. Returns true when clicked.
	bool DialogButton(const char* id, const char* label, bool primary = false);

}
