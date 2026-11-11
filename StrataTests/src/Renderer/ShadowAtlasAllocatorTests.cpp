#include <doctest/doctest.h>

#include "Strata/Renderer/ShadowAtlasAllocator.h"

#include <algorithm>
#include <array>
#include <random>
#include <vector>

using namespace Strata;

namespace
{

	bool Overlaps(const ShadowAtlasTile& a, const ShadowAtlasTile& b)
	{
		return a.X < b.X + b.Size && b.X < a.X + a.Size && a.Y < b.Y + b.Size && b.Y < a.Y + a.Size;
	}

	// Every tile lies in the atlas, aligned to its size, and no two overlap.
	void CheckTiles(const std::vector<ShadowAtlasTile>& tiles, uint32_t atlasSize)
	{
		for (size_t first = 0; first < tiles.size(); first++)
		{
			const ShadowAtlasTile& tile = tiles[first];
			REQUIRE(tile.IsValid());
			CHECK(tile.X % tile.Size == 0);
			CHECK(tile.Y % tile.Size == 0);
			CHECK(tile.X + tile.Size <= atlasSize);
			CHECK(tile.Y + tile.Size <= atlasSize);
			for (size_t second = first + 1; second < tiles.size(); second++)
				CHECK_FALSE(Overlaps(tile, tiles[second]));
		}
	}

}

TEST_SUITE("Renderer.ShadowAtlas")
{
	TEST_CASE("Tiles of every size from the minimum to the atlas are handed out without overlapping, and only of those sizes")
	{
		ShadowAtlasAllocator atlas(4096, 128);
		CHECK(atlas.GetAtlasSize() == 4096);
		CHECK(atlas.GetMinTileSize() == 128);
		CHECK(atlas.GetFreeArea() == 4096ull * 4096ull);
		CHECK_FALSE(atlas.Allocate(64).IsValid());
		CHECK_FALSE(atlas.Allocate(8192).IsValid());
		CHECK_FALSE(atlas.Allocate(300).IsValid());
		CHECK_FALSE(atlas.Allocate(0).IsValid());

		std::vector<ShadowAtlasTile> tiles;
		for (const uint32_t size : { 1024u, 128u, 512u, 256u, 1024u, 128u, 2048u })
		{
			tiles.push_back(atlas.Allocate(size));
			REQUIRE(tiles.back().IsValid());
			CHECK(tiles.back().Size == size);
		}
		CheckTiles(tiles, 4096);
		uint64_t used = 0;
		for (const ShadowAtlasTile& tile : tiles)
			used += static_cast<uint64_t>(tile.Size) * tile.Size;
		CHECK(atlas.GetFreeArea() == 4096ull * 4096ull - used);

		// The first tile of a size takes the first free block in order: an empty atlas gives the top-left corner.
		ShadowAtlasAllocator empty(2048, 128);
		CHECK(empty.Allocate(512) == ShadowAtlasTile { 0, 0, 512 });
		CHECK(empty.Allocate(512) == ShadowAtlasTile { 512, 0, 512 });
		CHECK(empty.Allocate(512) == ShadowAtlasTile { 0, 512, 512 });
	}

	TEST_CASE("An atlas fills up exactly, and freed tiles merge back into larger blocks")
	{
		ShadowAtlasAllocator atlas(1024, 128);
		std::vector<ShadowAtlasTile> tiles;
		for (uint32_t index = 0; index < 64; index++)
			tiles.push_back(atlas.Allocate(128));
		CheckTiles(tiles, 1024);
		CHECK(atlas.GetFreeArea() == 0);
		CHECK_FALSE(atlas.Allocate(128).IsValid());

		// Freeing one tile of each quarter leaves no free 256 block; freeing a whole quarter does.
		for (const uint32_t index : { 0u, 16u, 32u, 48u })
			atlas.Free(tiles[index]);
		CHECK_FALSE(atlas.Allocate(256).IsValid());
		const ShadowAtlasTile reused = atlas.Allocate(128);
		CHECK(reused == tiles[0]);
		atlas.Free(reused);
		for (const uint32_t index : { 1u, 2u, 3u })
			atlas.Free(tiles[index]);
		const ShadowAtlasTile merged = atlas.Allocate(256);
		CHECK(merged == ShadowAtlasTile { 0, 0, 256 });

		// Freeing what is not allocated changes nothing; freeing everything gives the whole atlas back.
		const uint64_t free = atlas.GetFreeArea();
		atlas.Free(ShadowAtlasTile { 0, 0, 512 });
		atlas.Free(ShadowAtlasTile { 64, 0, 128 });
		atlas.Free(tiles[16]);
		CHECK(atlas.GetFreeArea() == free);
		atlas.Free(merged);
		for (uint32_t index = 4; index < 64; index++)
			atlas.Free(tiles[index]);
		CHECK(atlas.GetFreeArea() == 1024ull * 1024ull);
		CHECK(atlas.Allocate(1024) == ShadowAtlasTile { 0, 0, 1024 });
		atlas.Clear();
		CHECK(atlas.GetFreeArea() == 1024ull * 1024ull);
	}

	TEST_CASE("Sizes allocated in descending order into an empty atlas always fit while their areas do")
	{
		std::mt19937 random(7);
		std::uniform_int_distribution<uint32_t> level(0, 3);
		for (uint32_t round = 0; round < 200; round++)
		{
			ShadowAtlasAllocator atlas(2048, 128);
			std::vector<uint32_t> sizes;
			uint64_t area = 0;
			while (true)
			{
				const uint32_t size = 1024u >> level(random);
				if (area + static_cast<uint64_t>(size) * size > 2048ull * 2048ull)
					break;
				sizes.push_back(size);
				area += static_cast<uint64_t>(size) * size;
			}
			std::sort(sizes.begin(), sizes.end(), std::greater<uint32_t>());
			std::vector<ShadowAtlasTile> tiles;
			for (const uint32_t size : sizes)
			{
				tiles.push_back(atlas.Allocate(size));
				REQUIRE(tiles.back().IsValid());
			}
			CheckTiles(tiles, 2048);
		}
	}

	TEST_CASE("Wanted sizes that do not fit step down from the least relevant entry toward the most relevant, in rounds")
	{
		// Sixteen point lights (six tiles each) wanting 1024 texels do not fit a 4096 atlas: the least relevant step down first.
		std::vector<uint32_t> sizes(16, 1024);
		const std::vector<uint32_t> counts(16, 6);
		CHECK(ShadowAtlasAllocator::FitTileSizes(sizes, counts, 4096, 128) == 16);
		uint64_t area = 0;
		for (size_t entry = 0; entry < sizes.size(); entry++)
		{
			area += static_cast<uint64_t>(counts[entry]) * sizes[entry] * sizes[entry];
			if (entry > 0)
				CHECK(sizes[entry] <= sizes[entry - 1]); // The more relevant keep at least as much
		}
		CHECK(area <= 4096ull * 4096ull);
		CHECK(sizes.front() >= 512);
		CHECK(sizes.back() < 1024);

		// Into a 2048 atlas they fit at the smallest sizes: every light keeps its tiles.
		std::vector<uint32_t> small(16, 1024);
		CHECK(ShadowAtlasAllocator::FitTileSizes(small, counts, 2048, 128) == 16);
		area = 0;
		for (size_t entry = 0; entry < small.size(); entry++)
			area += static_cast<uint64_t>(counts[entry]) * small[entry] * small[entry];
		CHECK(area <= 2048ull * 2048ull);
		ShadowAtlasAllocator atlas(2048, 128);
		std::vector<uint32_t> order;
		for (size_t entry = 0; entry < small.size(); entry++)
		{
			for (uint32_t tile = 0; tile < counts[entry]; tile++)
				order.push_back(small[entry]);
		}
		std::sort(order.begin(), order.end(), std::greater<uint32_t>());
		for (const uint32_t size : order)
			CHECK(atlas.Allocate(size).IsValid());

		// Sizes that fit are kept, rounded down to powers of two; sizes outside the range are clamped to it.
		std::array<uint32_t, 3> fitting = { 1024, 64, 3000 };
		const std::array<uint32_t, 3> one = { 1, 1, 1 };
		CHECK(ShadowAtlasAllocator::FitTileSizes(fitting, one, 4096, 128) == 3);
		CHECK(fitting == std::array<uint32_t, 3> { 1024, 128, 2048 });
		std::array<uint32_t, 1> whole = { 9000 };
		CHECK(ShadowAtlasAllocator::FitTileSizes(whole, std::span<const uint32_t>(one.data(), 1), 4096, 128) == 1);
		CHECK(whole[0] == 4096);

		// One round: each entry steps down once before any steps down twice, starting with the least relevant (already the
		// smallest, it ends smaller still).
		std::array<uint32_t, 3> round = { 1024, 1024, 512 };
		CHECK(ShadowAtlasAllocator::FitTileSizes(round, one, 1024, 128) == 3);
		CHECK(round == std::array<uint32_t, 3> { 512, 512, 256 });

		// Even at the smallest size not every entry fits: the most relevant ones that do.
		std::vector<uint32_t> crowded(20, 512);
		const std::vector<uint32_t> six(20, 6);
		CHECK(ShadowAtlasAllocator::FitTileSizes(crowded, six, 512, 128) == 2);
		CHECK(std::all_of(crowded.begin(), crowded.end(), [](uint32_t size) { return size == 128; }));

		// Within a budget below the atlas (SceneRenderer fits seven eighths): the sizes add up to at most the budget.
		std::vector<uint32_t> budgeted(64, 512);
		const std::vector<uint32_t> points(64, 6);
		const uint64_t budget = 4096ull * 4096ull / 8 * 7;
		CHECK(ShadowAtlasAllocator::FitTileSizes(budgeted, points, 4096, 128, budget) == 64);
		area = 0;
		for (size_t entry = 0; entry < budgeted.size(); entry++)
			area += static_cast<uint64_t>(points[entry]) * budgeted[entry] * budgeted[entry];
		CHECK(area <= budget);
		CHECK(area > budget / 2);
		// At the smallest size 149 point lights fit seven eighths of a 4096 atlas, 150 do not.
		std::vector<uint32_t> many(150, 128);
		const std::vector<uint32_t> manyPoints(150, 6);
		CHECK(ShadowAtlasAllocator::FitTileSizes(many, manyPoints, 4096, 128, budget) == 149);
	}

	TEST_CASE("Allocation takes the first free block in a fixed order, the same for the same requests, in every level of a large atlas")
	{
		// An 8192 atlas of 128-texel tiles: 4096 of them, each found through the free bits of its level.
		ShadowAtlasAllocator first(8192, 128);
		ShadowAtlasAllocator second(8192, 128);
		std::mt19937 random(7);
		std::vector<ShadowAtlasTile> held;
		for (uint32_t step = 0; step < 3000; step++)
		{
			if (!held.empty() && random() % 3 == 0)
			{
				const size_t index = random() % held.size();
				first.Free(held[index]);
				second.Free(held[index]);
				held.erase(held.begin() + static_cast<std::ptrdiff_t>(index));
				continue;
			}
			const uint32_t size = 128u << (random() % 4);
			const ShadowAtlasTile tile = first.Allocate(size);
			CHECK(second.Allocate(size) == tile);
			if (tile.IsValid())
				held.push_back(tile);
		}
		CheckTiles(held, 8192);
		uint64_t used = 0;
		for (const ShadowAtlasTile& tile : held)
			used += static_cast<uint64_t>(tile.Size) * tile.Size;
		CHECK(first.GetFreeArea() == 8192ull * 8192ull - used);

		// Filled with the smallest tiles, the atlas has none left; a freed one is the next handed out, and freeing all of them
		// merges the atlas back into one block.
		ShadowAtlasAllocator full(8192, 128);
		std::vector<ShadowAtlasTile> tiles;
		for (uint32_t index = 0; index < 4096; index++)
		{
			tiles.push_back(full.Allocate(128));
			REQUIRE(tiles.back().IsValid());
		}
		CHECK_FALSE(full.Allocate(128).IsValid());
		CHECK(full.GetFreeArea() == 0);
		full.Free(tiles[2345]);
		CHECK(full.Allocate(128) == tiles[2345]);
		for (const ShadowAtlasTile& tile : tiles)
			full.Free(tile);
		CHECK(full.GetFreeArea() == 8192ull * 8192ull);
		CHECK(full.Allocate(8192).IsValid());
	}
}
