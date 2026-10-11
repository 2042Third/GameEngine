#pragma once

#include "UI/EditorFonts.h"

#include <imgui.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Strata
{
	struct TextureSpecification;
}

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
	// A chip that toggles a setting: accent-tinted while on. Flips *value when clicked and returns true then. A setting that
	// is on but has nothing to do now (inEffect false, e.g. preview lighting in a scene with lights of its own) is drawn
	// muted, only outlined in the accent. The probe records the icon's color.
	bool ToggleChip(const char* id, const char* icon, const char* label, bool* value, const char* tooltip, bool inEffect = true);
	// The width a chip with this icon and label takes, e.g. to align one to the right.
	float ChipWidth(const char* icon, const char* label);

	// A status pill: an icon and text in a color on a tint of that color, sized to its text in the current font. Returns
	// true when clicked (pills that lead somewhere, e.g. the errors pill opening the Console).
	bool Pill(const char* id, const char* icon, std::string_view text, const ImVec4& color, const char* tooltip = nullptr);

	// Whether the last item's tooltip shows this frame (hovered for the tooltip delay, also while disabled). Widgets take
	// finished tooltips; one that takes work to write (formatted statistics) is better written only then:
	// `if (UI::IsTooltipDue()) ImGui::SetTooltip("%s", Describe().c_str());` after the widget.
	bool IsTooltipDue();

	// A banner across the available width: an icon and text (wrapped) in a color on a tint of that color, e.g. the play-mode
	// notice over the Hierarchy and the Inspector. The probe records its color and text.
	void Banner(const char* id, const char* icon, std::string_view text, const ImVec4& color);

	// Text wrapped at the available width with every line centered in it, in a color (empty states: a hint under a centered
	// title). The probe records the block and its text.
	void CenteredText(const char* id, std::string_view text, const ImVec4& color);

	// A search field: a text field with a search icon as its hint and a clear button while it holds text, at the given
	// width (negative: the available width), with an optional tooltip (e.g. the search syntax). Returns true when the text
	// changed this frame (also when cleared). The probe records the field under the id.
	bool SearchField(const char* id, std::string& text, const char* hint = "Search", float width = -1.0f, const char* tooltip = nullptr);

	// Rows of lists with a selection (tree nodes and selectables, e.g. the Hierarchy's). ImGui draws every hovered row in
	// ImGuiCol_HeaderHovered, selected or not, and the theme keeps that neutral so that hovering is not mistaken for
	// selecting. Between PushSelectionColors(selected) and PopSelectionColors() a selected row keeps the accent under the
	// mouse instead (ThemeColors::SelectionHovered).
	void PushSelectionColors(bool selected);
	void PopSelectionColors();

	// A collapsible section header in the SemiBold font on a raised surface (CollapsingHeader). Add
	// ImGuiTreeNodeFlags_AllowOverlap to place buttons on it. A summary of what the section holds (for sections that start
	// closed) shows at its right end in the secondary text color, cut with an ellipsis where it would reach the label; the
	// probe records it as the header's text. Returns whether the section is open.
	bool SectionHeader(const char* id, const char* label, ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen, std::string_view summary = {});

	// A heading in the SemiBold font at a size of the type scale (Title, or Display for large titles).
	void Heading(std::string_view text, TextSize size = TextSize::Title);

	// A clickable card: a raised, rounded surface with an icon, a title and a description (e.g. project templates or recent
	// projects). The size is in pixels: derive it from the font size. Returns true when clicked.
	bool Card(const char* id, const char* icon, std::string_view title, std::string_view description, const ImVec2& size, bool selected = false);
	// The height a Card of this width needs to show its icon, title and description without clipping.
	float GetCardHeight(float width, bool hasIcon, std::string_view title, std::string_view description);

	// Modal dialogs: OpenModal(name) once (e.g. from a menu item), then every frame BeginModal(name, ...) and, while it
	// returns true, the contents and EndModal(). The dialog is centered on the main viewport, sized to its contents, and
	// shows the title as a heading. With `open`, a close button sets it to false.
	void OpenModal(const char* name);
	bool BeginModal(const char* name, std::string_view title, bool* open = nullptr);
	void EndModal();
	// A dialog button of a uniform width (from the font, DialogButtonWidth), the primary one filled with the accent. Returns
	// true when clicked (never while disabled).
	bool DialogButton(const char* id, const char* label, bool primary = false, bool enabled = true);
	float DialogButtonWidth(const char* label);

	// A large action button with an icon and a label, left-aligned on a framed surface (filled with the accent when primary),
	// for the main actions of a page such as the launcher's. A width of 0 fits the label; a negative one fills the
	// available width. Its height is 1.6 frame heights. Returns true when clicked.
	bool ActionButton(const char* id, const char* icon, std::string_view label, const char* tooltip, const ButtonStyle& style = {}, float width = 0.0f);

	// Text that acts as a link (secondary text, underlined while hovered), with an optional icon after it, e.g. "About" in
	// a footer. Returns true when clicked.
	bool LinkButton(const char* id, std::string_view label, const char* icon = nullptr, const char* tooltip = nullptr);

	// A labeled single-line text field: the label above in the caption size, the field below at the given width (negative:
	// the available width), with a hint while it is empty. Returns true when the text changed this frame; Enter in the
	// field is ImGui::IsItemDeactivatedAfterEdit or IsKeyPressed after it. The probe records the field.
	bool TextField(const char* id, std::string_view label, std::string& value, const char* hint = nullptr, float width = -1.0f);

	// Code to copy, e.g. a command line: monospaced text on a recessed surface with a copy button at its right end, which
	// copies the code to the clipboard and shows a check mark for a moment. The text wraps at the available width.
	// Returns true when it was copied. The probe records the copy button under "<id>.Copy".
	bool CopyableCode(const char* id, std::string_view code);
	// How long the copy button shows that it copied.
	inline constexpr double c_CopiedFeedbackSeconds = 1.5;

	// A card for an entry of a list or grid, such as a recent project: a badge (initials or an icon on a tinted square)
	// at the left, then a title, a subtitle (e.g. a path, shortened at its start when it does not fit) and a dimmer detail
	// line. The size is in pixels: derive it from the font size. Returns true when clicked.
	struct EntryCardContent
	{
		std::string_view Badge;
		ImVec4 BadgeColor;
		std::string_view Title;
		std::string_view Subtitle;
		std::string_view Detail;
	};
	bool EntryCard(const char* id, const EntryCardContent& content, const ImVec2& size);

	// The strata mark: four staggered bands graded Sandstone, Ochre, Rust and Umber (three below 24 pixels, so they stay
	// apart), on a Basalt rounded square when `tile` is set: the shapes of StrataEditor/Resources/Brand (drawn by
	// Tools/GenerateBrandAssets.py), drawn as vectors at any size. Takes a square item of `size` pixels.
	void BrandMark(float size, bool tile = true);

	// `text` shortened at its start with an ellipsis ("...rojects/Game/Game.stproj") to fit `maxWidth` in the current font.
	std::string ElideStart(std::string_view text, float maxWidth);

	// A progress bar in the strata motif: the done share fills a recessed track with the mark's four bands (Sandstone,
	// Ochre, Rust and Umber, top to bottom), with the label centered over it. `fraction` is clamped to [0, 1]; a negative
	// width fills the available width. Frame-height tall. The probe records the label as its text.
	void StrataProgressBar(const char* id, float fraction, std::string_view label, float width = -1.0f);

	// A legend drawn over an image (no item, so it takes no input): per line a color swatch, a label and a count, the counts
	// right-aligned in a column of their own so they line up whatever the labels' lengths, then an optional note, on the
	// overlay surface with its bottom-left corner at `bottomLeft`, in the current font. The probe records each line's label
	// and count as "<id>.Label<n>" and "<id>.Count<n>" (n from 0; keep legends to a few lines).
	struct LegendLine
	{
		ImVec4 Color;
		std::string Label;
		std::string Count;
	};
	void Legend(const char* id, const ImVec2& bottomLeft, std::span<const LegendLine> lines, std::string_view note = {});

	// A texture drawn into a rectangle of the draw list (no item, so it takes no input) the way its content looks: sampled
	// as the texture is set to be (Nearest keeps pixel art crisp) and shown in the display its stored layout needs
	// (GetImGuiTextureDisplay, Strata/ImGui/ImGuiRenderer.h: sRGB colors encoded for the display, one-channel grayscale
	// as gray, two-channel normal maps with their Z), through the renderer's draw callbacks. The list's later draws are
	// back to the defaults.
	void TextureImage(ImDrawList* drawList, ImTextureID texture, const TextureSpecification& specification, const ImVec2& min, const ImVec2& max);

}
