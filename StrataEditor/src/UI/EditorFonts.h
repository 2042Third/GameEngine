#pragma once

#include <cstdint>

struct ImFont;

namespace Strata::UI
{

	// The editor's typefaces, embedded in the executable (StrataEditor/Resources/Fonts, see ThirdPartyNotices.md).
	enum class EditorFont : uint8_t
	{
		Regular = 0, // Inter Regular with the Lucide icons merged in (UI/Icons.h): the default font
		SemiBold,    // Inter SemiBold with the icons: headers, titles, emphasis
		Mono         // JetBrains Mono: the console, IDs, numbers, code
	};

	// The type scale. Sizes are unscaled pixels: the UI scale (ImGuiStyle::FontScaleDpi) multiplies them.
	enum class TextSize : uint8_t
	{
		Caption = 0, // 12 px: status bar, captions, dense tables
		Body,        // 14 px: the default size
		Title,       // 17 px: section and dialog titles
		Display      // 24 px: large headings (launcher, empty states)
	};

	float GetTextSize(TextSize size);

	class EditorFonts
	{
	public:
		// Adds the editor's fonts to the current ImGui context's font atlas and makes Inter Regular the default font, so
		// ImGui's built-in font is never used. The font data stays owned by the executable (it is embedded). Returns false,
		// logging why, when ImGui cannot read a font.
		static bool Load();
		// The font in the current context's atlas; null until Load succeeded for that atlas.
		static ImFont* Get(EditorFont font);
	};

	// Pushes one of the editor's fonts at a size of the type scale; pair it with ImGui::PopFont(). Before the fonts are
	// loaded only the size changes.
	void PushFont(EditorFont font, TextSize size = TextSize::Body);

}
