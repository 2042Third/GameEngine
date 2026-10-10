#include "UI/EditorFonts.h"

#include <Strata/Core/Base.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>

#include <imgui.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

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

		// The system's fonts for scripts the embedded fonts lack, as read from their files.
		struct FallbackFonts
		{
			// The files' bytes. ImGui reads them for as long as an atlas uses them (it does not own them), so they stay
			// until the process ends and are never changed once read.
			std::vector<std::vector<uint8_t>> Data;
			std::vector<std::string> Names;
		};

		JobHandle s_FallbackJob;
		Ref<FallbackFonts> s_FallbackRead; // Shared with the read under way
		std::optional<FallbackFonts> s_Fallback;
		// The atlas that asked for the fallback fonts, and the one they are merged into. Load resets both, so an atlas
		// created where a destroyed one was is never taken for it.
		ImFontAtlas* s_FallbackRequestedAtlas = nullptr;
		ImFontAtlas* s_FallbackAtlas = nullptr;

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
		s_FallbackRequestedAtlas = nullptr;
		s_FallbackAtlas = nullptr;
		return true;
	}

	void EditorFonts::BeginLoadingFallback()
	{
		if (!ImGui::GetCurrentContext() || !Get(EditorFont::Regular))
			return;
		s_FallbackRequestedAtlas = ImGui::GetIO().Fonts;
		// Read once per process: later atlases (UI tests) merge the same bytes.
		if (s_Fallback || s_FallbackRead)
			return;
		Ref<FallbackFonts> read = CreateRef<FallbackFonts>();
		s_FallbackRead = read;
		s_FallbackJob = JobSystem::SubmitIO([read]()
		{
			for (const std::filesystem::path& file : Platform::FindFallbackFontFiles())
			{
				std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(file);
				if (!bytes || bytes->empty())
					continue;
				read->Data.push_back(std::move(*bytes));
				read->Names.push_back(FileSystem::ToUTF8(file.filename()));
			}
		}, JobPriority::Low);
	}

	bool EditorFonts::UpdateFallback()
	{
		if (s_FallbackRead && s_FallbackJob.IsComplete())
		{
			s_Fallback = std::move(*s_FallbackRead);
			s_FallbackRead = nullptr;
			s_FallbackJob = JobHandle();
			if (s_Fallback->Data.empty())
				ST_WARN("No system font for Chinese, Japanese and Korean text was found: such text shows as '?'");
		}
		if (!s_Fallback || s_Fallback->Data.empty() || !ImGui::GetCurrentContext())
			return false;
		ImFontAtlas& atlas = *ImGui::GetIO().Fonts;
		if (&atlas != s_FallbackRequestedAtlas || &atlas == s_FallbackAtlas)
			return false;

		// Behind the font's own glyphs and its icons: only what they lack comes from these, never the icons' range.
		bool merged = false;
		for (const EditorFont font : { EditorFont::Regular, EditorFont::SemiBold, EditorFont::Mono })
		{
			ImFont* destination = Get(font);
			if (!destination)
				continue;
			for (size_t index = 0; index < s_Fallback->Data.size(); index++)
			{
				ImFontConfig config;
				config.MergeMode = true;
				config.DstFont = destination;
				config.GlyphExcludeRanges = c_IconRanges;
				std::vector<uint8_t>& data = s_Fallback->Data[index];
				if (AddFont(atlas, data, s_Fallback->Names[index].c_str(), config, GetTextSize(TextSize::Body)))
					merged = true;
				else
					ST_WARN("The system font '{}' could not be used for text in other scripts", s_Fallback->Names[index]);
			}
		}
		// Asked once per atlas, whether or not a font could be used.
		s_FallbackRequestedAtlas = nullptr;
		if (!merged)
			return false;
		s_FallbackAtlas = &atlas;
		return true;
	}

	bool EditorFonts::IsLoadingFallback()
	{
		return s_FallbackRead != nullptr;
	}

	bool EditorFonts::HasFallback()
	{
		return ImGui::GetCurrentContext() && s_FallbackAtlas == ImGui::GetIO().Fonts;
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
