#include <doctest/doctest.h>

#include "Renderer/FontTestUtils.h"
#include "Renderer/GPUTestUtils.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/FontAtlas.h"
#include "Strata/Renderer/Renderer.h"

#include <algorithm>
#include <limits>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Uploads the atlas's changes like a frame would and waits for the GPU.
	void Upload(nvrhi::IDevice* device, FontAtlas& atlas)
	{
		nvrhi::CommandListHandle commandList = device->createCommandList();
		commandList->open();
		REQUIRE(atlas.Upload(device, commandList));
		commandList->close();
		device->executeCommandList(commandList);
		device->waitForIdle();
	}

	// Every page of the GPU texture holds exactly the CPU page.
	void CheckGPUPages(const FontAtlas& atlas)
	{
		REQUIRE(atlas.GetTexture());
		const nvrhi::TextureDesc& desc = atlas.GetTexture()->getDesc();
		CHECK(desc.dimension == nvrhi::TextureDimension::Texture2DArray);
		REQUIRE(desc.arraySize >= atlas.GetPageCount());
		for (uint32_t page = 0; page < atlas.GetPageCount(); page++)
		{
			CAPTURE(page);
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(atlas.GetTexture(), image, 0, page));
			REQUIRE(image.Pixels.size() == atlas.GetPagePixels(page).size());
			CHECK(image.Pixels == atlas.GetPagePixels(page));
		}
	}

}

TEST_SUITE("GPU.Renderer.FontAtlas")
{
	TEST_CASE("Atlas uploads send only changed rows and keep the GPU pages equal to the CPU pages")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		std::string error;
		const Ref<Font> font = Font::Create(ReadSourceFile("Strata/vendor/tracy/profiler/src/font/FiraCode-Retina.ttf"), &error);
		REQUIRE_MESSAGE(font, error);
		FontAtlasSpecification specification;
		specification.MaxPages = 2;
		Scope<FontAtlas> atlas = FontAtlas::Create(font, &error, specification);
		REQUIRE_MESSAGE(atlas, error);

		// The first glyph creates the texture with the whole page.
		atlas->BeginFrame();
		REQUIRE(atlas->GetGlyph('A').Visible);
		Upload(device, *atlas);
		CHECK(atlas->GetStats().UploadedBytes == static_cast<uint64_t>(FontAtlas::c_PageSize) * FontAtlas::c_PageSize);
		CheckGPUPages(*atlas);

		// Another glyph uploads one band of rows; nothing new uploads nothing.
		atlas->BeginFrame();
		REQUIRE(atlas->GetGlyph('B').Visible);
		uint64_t uploaded = atlas->GetStats().UploadedBytes;
		Upload(device, *atlas);
		CHECK(atlas->GetStats().UploadedBytes - uploaded == static_cast<uint64_t>(FontAtlas::c_PageSize) * FontAtlas::c_UploadBandRows);
		uploaded = atlas->GetStats().UploadedBytes;
		Upload(device, *atlas);
		CHECK(atlas->GetStats().UploadedBytes == uploaded);
		CheckGPUPages(*atlas);

		// Frames of new glyphs grow the texture to two pages, then evict pages: band uploads, new pages and cleared pages
		// all leave the GPU copy identical.
		uint32_t glyph = 1;
		for (uint32_t frame = 0; frame < 120 && atlas->GetStats().EvictedGlyphs < 200; frame++)
		{
			atlas->BeginFrame();
			atlas->SetRasterBudget({ 24, std::numeric_limits<uint64_t>::max() });
			for (uint32_t count = 0; count < 24 && glyph < font->GetGlyphCount(); count++)
				atlas->GetGlyphByIndex(glyph++);
			Upload(device, *atlas);
			if (frame % 5 == 0)
				CheckGPUPages(*atlas);
		}
		CHECK(atlas->GetPageCount() == 2);
		CHECK(atlas->GetStats().EvictedGlyphs >= 200);
		CheckGPUPages(*atlas);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
