#include <doctest/doctest.h>

#include "Renderer/FontTestUtils.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/FontAtlasCache.h"
#include "Strata/Renderer/TextRenderer.h"

#include <string>

using namespace Strata;

TEST_SUITE("Renderer.FontAtlasCache")
{
	TEST_CASE("A font's atlas is shared, and dropped at the start of a frame after one in which nobody used it")
	{
		std::string error;
		Ref<Font> font = Font::Create(Tests::ReadSourceFile("Strata/vendor/tracy/profiler/src/font/FiraCode-Retina.ttf"), &error);
		REQUIRE_MESSAGE(font, error);
		FontAtlasCache cache;
		cache.BeginFrame(TextRenderer::c_FrameRasterBudget);
		FontAtlas* atlas = cache.Get(font);
		REQUIRE(atlas);
		CHECK(cache.Get(font) == atlas);
		CHECK(cache.Get(Font::GetDefault()) != atlas);
		CHECK(cache.GetAtlasCount() == 2);

		// Only the cache holds the font now. It was used in the frame that ends, so a pass later in that frame (the game UI
		// after the text) still finds it, and so does the next frame.
		font.reset();
		CHECK(cache.GetAtlasCount() == 2);
		cache.BeginFrame(TextRenderer::c_FrameRasterBudget);
		CHECK(cache.GetAtlasCount() == 2);
		// A frame without it: dropped at the start of the next one. The default font's atlas stays.
		cache.BeginFrame(TextRenderer::c_FrameRasterBudget);
		CHECK(cache.GetAtlasCount() == 1);
	}

	TEST_CASE("The passes of a frame share one rasterization budget")
	{
		FontAtlasCache cache;
		cache.BeginFrame({ 3, 1u << 30 });
		FontAtlas* atlas = cache.Get(Font::GetDefault());
		REQUIRE(atlas);

		// The first pass rasterizes two glyphs and gives the rest back; the second gets one more.
		atlas->SetRasterBudget(cache.GetBudget());
		CHECK(atlas->GetGlyph('A').Visible);
		CHECK(atlas->GetGlyph('B').Visible);
		cache.SetBudget(atlas->GetRasterBudget());
		CHECK(cache.GetBudget().Glyphs == 1);
		atlas->SetRasterBudget(cache.GetBudget());
		CHECK(atlas->GetGlyph('C').Visible);
		CHECK(atlas->GetGlyph('D').Pending);
		cache.SetBudget(atlas->GetRasterBudget());
		CHECK(cache.GetBudget().Glyphs == 0);

		// The next frame starts with a new budget, and glyphs of the previous frame may be evicted again.
		cache.BeginFrame({ 1, 1u << 30 });
		CHECK(cache.GetBudget().Glyphs == 1);
	}
}
