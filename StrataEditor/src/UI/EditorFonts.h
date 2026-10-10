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

		// Text in scripts the embedded fonts lack (Chinese, Japanese, Korean: project names, paths, logs) comes from the
		// system's fonts (Platform::FindFallbackFontFiles), merged into the editor's fonts behind their own glyphs.
		// BeginLoadingFallback asks for them for the current atlas (after Load) and reads the files on an I/O thread once
		// per process (they are large: tens of megabytes); UpdateFallback, called between frames, merges them into that
		// atlas once they were read. Glyphs are rasterized only when text needs them. Until then, and on a system without
		// such fonts, that text shows ImGui's fallback character.
		static void BeginLoadingFallback();
		// True when it merged the fallback fonts into the current atlas now.
		static bool UpdateFallback();
		// Whether a read of the fallback fonts is under way.
		static bool IsLoadingFallback();
		// Whether fallback fonts are merged into the current atlas.
		static bool HasFallback();
	};

	// Pushes one of the editor's fonts at a size of the type scale; pair it with ImGui::PopFont(). Before the fonts are
	// loaded only the size changes.
	void PushFont(EditorFont font, TextSize size = TextSize::Body);

}
