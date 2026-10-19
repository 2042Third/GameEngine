#include <doctest/doctest.h>

#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/SceneRenderData.h"
#include "Strata/Renderer/TextureTable.h"

using namespace Strata;

TEST_SUITE("Renderer.TextureTable")
{
	TEST_CASE("Indices stay with their textures while their entries follow the bindless slots")
	{
		TextureTable table(nullptr, 2);
		// The reserved indices name the reserved bindless slots.
		CHECK(table.GetEntry(TextureTable::c_White) == TextureTable::PackEntry(BindlessTextureTable::c_WhiteSlot, 0, false));
		CHECK(table.GetEntry(TextureTable::c_Black) == TextureTable::PackEntry(BindlessTextureTable::c_BlackSlot, 0, false));
		CHECK(table.GetEntry(TextureTable::c_FlatNormal) == TextureTable::PackEntry(BindlessTextureTable::c_FlatNormalSlot, 0, false));

		const uint32_t entry = TextureTable::PackEntry(17, 4, true);
		CHECK((entry & 0xFFFFFFu) == 17);
		CHECK(((entry >> 24) & 0x7Fu) == 4);
		CHECK((entry & RenderData::c_TextureSlotGrayscale) != 0);
		const uint32_t index = table.Allocate(entry);
		CHECK(index >= TextureTable::c_ReservedIndices);
		CHECK(table.GetEntry(index) == entry);
		CHECK(table.GetAllocatedCount() == 1);

		// A level change moves the texture to another slot: the index stays, its entry follows.
		const uint32_t moved = TextureTable::PackEntry(40, 4, true);
		table.Set(index, moved);
		CHECK(table.GetEntry(index) == moved);
		// Reserved indices never change.
		table.Set(TextureTable::c_White, moved);
		CHECK(table.GetEntry(TextureTable::c_White) == TextureTable::PackEntry(BindlessTextureTable::c_WhiteSlot, 0, false));
	}

	TEST_CASE("A freed index draws white and is reused only once frames in flight completed")
	{
		TextureTable table(nullptr, 2);
		table.BeginFrame(10);
		const uint32_t index = table.Allocate(TextureTable::PackEntry(33, 1, false));
		table.Free(index);
		CHECK(table.GetEntry(index) == TextureTable::PackEntry(BindlessTextureTable::c_WhiteSlot, 0, false));
		CHECK(table.GetAllocatedCount() == 0);
		table.Free(index); // Twice: ignored
		table.Set(index, TextureTable::PackEntry(34, 1, false)); // Freed: ignored
		CHECK(table.GetEntry(index) == TextureTable::PackEntry(BindlessTextureTable::c_WhiteSlot, 0, false));

		// Frames 11 and 12 may still read it (two frames in flight): a new index meanwhile, the freed one after.
		table.BeginFrame(12);
		CHECK(table.Allocate(TextureTable::PackEntry(35, 1, false)) != index);
		table.BeginFrame(13);
		CHECK(table.Allocate(TextureTable::PackEntry(36, 1, false)) == index);
		CHECK(table.GetEntry(index) == TextureTable::PackEntry(36, 1, false));
	}

	TEST_CASE("A texture that gained levels fades them in over the fade-in time")
	{
		TextureTable table(nullptr, 2);
		CHECK(table.GetFadeInSeconds() == TextureTable::c_FadeInSeconds);
		const uint32_t index = table.Allocate(TextureTable::PackEntry(20, 1, false));
		CHECK(table.GetMinLod(index) == 0.0f);

		// Raised by 3 levels: sampled no finer than the level the old texture's finest matches, then linearly finer.
		const double start = TextureTable::GetClockSeconds();
		table.SetFadeIn(index, TextureTable::PackEntry(21, 1, false), 3);
		CHECK(table.GetEntry(index) == TextureTable::PackEntry(21, 1, false));
		CHECK(table.GetMinLod(index) == 3.0f);
		CHECK(table.GetFadeCount() == 1);
		table.UpdateFades(start + 0.5 * TextureTable::c_FadeInSeconds);
		CHECK(table.GetMinLod(index) >= 1.5f);
		CHECK(table.GetMinLod(index) < 1.75f);
		table.UpdateFades(start + 1.5 * TextureTable::c_FadeInSeconds);
		CHECK(table.GetMinLod(index) == 0.0f);
		CHECK(table.GetFadeCount() == 0);

		// A drop (nothing gained) and a plain Set end a fade at once, as does freeing the index.
		table.SetFadeIn(index, TextureTable::PackEntry(22, 1, false), 2);
		CHECK(table.GetMinLod(index) == 2.0f);
		table.SetFadeIn(index, TextureTable::PackEntry(23, 1, false), 0);
		CHECK(table.GetMinLod(index) == 0.0f);
		CHECK(table.GetFadeCount() == 0);
		table.SetFadeIn(index, TextureTable::PackEntry(24, 1, false), 2);
		table.Set(index, TextureTable::PackEntry(25, 1, false));
		CHECK(table.GetMinLod(index) == 0.0f);
		CHECK(table.GetFadeCount() == 0);
		table.SetFadeIn(index, TextureTable::PackEntry(26, 1, false), 2);
		table.Free(index);
		CHECK(table.GetMinLod(index) == 0.0f);
		CHECK(table.GetFadeCount() == 0);

		// Fades turned off: the new levels show at once. Reserved indices never fade.
		const uint32_t other = table.Allocate(TextureTable::PackEntry(30, 1, false));
		table.SetFadeInSeconds(0.0f);
		table.SetFadeIn(other, TextureTable::PackEntry(31, 1, false), 4);
		CHECK(table.GetEntry(other) == TextureTable::PackEntry(31, 1, false));
		CHECK(table.GetMinLod(other) == 0.0f);
		table.SetFadeInSeconds(TextureTable::c_FadeInSeconds);
		table.SetFadeIn(TextureTable::c_White, TextureTable::PackEntry(32, 1, false), 4);
		CHECK(table.GetMinLod(TextureTable::c_White) == 0.0f);
		CHECK(table.GetFadeCount() == 0);
	}
}
