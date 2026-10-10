#include "UI/EditorFonts.h"

#include <Strata/Core/Log.h>

#include <imgui.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

namespace Strata::EmbeddedFiles
{
	// Generated from StrataEditor/Resources/Fonts by strata_embed_file (StrataEditor/CMakeLists.txt).
	std::span<const uint8_t> GetInterRegularFont();
	std::span<const uint8_t> GetInterSemiBoldFont();
	std::span<const uint8_t> GetJetBrainsMonoFont();
	std::span<const uint8_t> GetLucideFont();
}

namespace Strata::UI
{

	namespace
	{

		constexpr float c_TextSizes[] = { 12.0f, 14.0f, 17.0f, 24.0f };
		static_assert(std::size(c_TextSizes) == static_cast<size_t>(TextSize::Display) + 1, "A size for every TextSize");

		// The Private Use Area of the Basic Multilingual Plane, where the icon font has its glyphs. Only these come from the
		// icon font, and never from a text font (a text font with glyphs there would otherwise shadow the icons).
		constexpr ImWchar c_IconRanges[] = { 0xE000, 0xF8FF, 0 };
		// Lucide's glyphs fill its em square, which sits higher than Inter's text: at 0.9 of the text size and moved down by
		// 2 px at the base size (the offset scales with the size) icons are as tall as Inter's capitals with ascenders and
		// centered on the text line, also next to labels in buttons and pills (checked at 100% and 150%).
		constexpr float c_IconScale = 0.9f;
		constexpr float c_IconOffsetY = 2.0f;

		std::array<ImFont*, 3> s_Fonts = {};

		ImFont* AddFont(ImFontAtlas& atlas, std::span<const uint8_t> data, const char* name, ImFontConfig config, float size)
		{
			// ImGui only reads the data: it is not owned by the atlas, so it is neither written nor freed.
			config.FontDataOwnedByAtlas = false;
			std::snprintf(config.Name, sizeof(config.Name), "%s", name);
			return atlas.AddFontFromMemoryTTF(const_cast<uint8_t*>(data.data()), static_cast<int>(data.size()), size, &config);
		}

		// A text font at the body size, optionally with the icons merged in.
		ImFont* AddTextFont(ImFontAtlas& atlas, std::span<const uint8_t> data, const char* name, bool icons)
		{
			const float size = GetTextSize(TextSize::Body);
			ImFontConfig textConfig;
			textConfig.GlyphExcludeRanges = c_IconRanges;
			ImFont* font = AddFont(atlas, data, name, textConfig, size);
			if (!font || !icons)
				return font;

			ImFontConfig iconConfig;
			iconConfig.MergeMode = true;
			iconConfig.GlyphRanges = c_IconRanges;
			// Icons advance by a whole text size, so rows of icons line up like a grid.
			iconConfig.GlyphMinAdvanceX = size;
			iconConfig.GlyphOffset = ImVec2(0.0f, c_IconOffsetY);
			iconConfig.PixelSnapH = true;
			if (!AddFont(atlas, EmbeddedFiles::GetLucideFont(), "Lucide", iconConfig, size * c_IconScale))
				return nullptr;
			return font;
		}

	}

	float GetTextSize(TextSize size)
	{
		return c_TextSizes[static_cast<size_t>(size)];
	}

	bool EditorFonts::Load()
	{
		if (!ImGui::GetCurrentContext())
		{
			ST_ERROR("The editor's fonts need an ImGui context");
			return false;
		}
		ImGuiIO& io = ImGui::GetIO();
		ImFontAtlas& atlas = *io.Fonts;
		ImFont* regular = AddTextFont(atlas, EmbeddedFiles::GetInterRegularFont(), "Inter Regular", true);
		ImFont* semiBold = AddTextFont(atlas, EmbeddedFiles::GetInterSemiBoldFont(), "Inter SemiBold", true);
		ImFont* mono = AddTextFont(atlas, EmbeddedFiles::GetJetBrainsMonoFont(), "JetBrains Mono", false);
		if (!regular || !semiBold || !mono)
		{
			ST_ERROR("The editor's fonts could not be loaded");
			return false;
		}
		io.FontDefault = regular;
		s_Fonts = { regular, semiBold, mono };
		return true;
	}

	ImFont* EditorFonts::Get(EditorFont font)
	{
		ImFont* candidate = s_Fonts[static_cast<size_t>(font)];
		if (!candidate || !ImGui::GetCurrentContext())
			return nullptr;
		// Fonts belong to an atlas, which goes away with its context: only fonts of the current atlas are handed out.
		for (ImFont* loaded : ImGui::GetIO().Fonts->Fonts)
		{
			if (loaded == candidate)
				return candidate;
		}
		return nullptr;
	}

	void PushFont(EditorFont font, TextSize size)
	{
		ImGui::PushFont(EditorFonts::Get(font), GetTextSize(size));
	}

}
