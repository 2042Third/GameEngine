#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace Strata
{

	// A square tile of a shadow atlas, in texels from the atlas's top-left corner.
	struct ShadowAtlasTile
	{
		uint32_t X = 0;
		uint32_t Y = 0;
		uint32_t Size = 0; // 0: no tile

		bool IsValid() const { return Size > 0; }
		bool operator==(const ShadowAtlasTile&) const = default;
	};

	// Hands out square tiles of a square shadow atlas (the local light shadows' atlas, SceneRenderer): power-of-two sizes from a
	// smallest tile up to the whole atlas, from a quadtree whose free quarters merge again when all four are free. Tiles stay
	// where they are until freed, so the shadows they hold can be kept from frame to frame. Allocation takes the first free
	// block of the size in a fixed order, splitting a larger one only when no block of the size is free: the same requests
	// give the same tiles. Allocating sizes in descending order into an empty atlas always succeeds while their areas add up to
	// at most the atlas's (FitTileSizes makes them do).
	class ShadowAtlasAllocator
	{
	public:
		// An empty atlas of `atlasSize` texels each way with tiles from `minTileSize` texels up (both powers of two, the
		// minimum at most the atlas; other values are clamped down to one).
		ShadowAtlasAllocator(uint32_t atlasSize = 0, uint32_t minTileSize = 128);

		// Starts again with another atlas: every tile is freed.
		void Reset(uint32_t atlasSize, uint32_t minTileSize);
		// Frees every tile.
		void Clear();

		// A free tile of `size` texels (a power of two from the minimum tile size to the atlas size), or an invalid tile when
		// none is free or the size is not one of those.
		ShadowAtlasTile Allocate(uint32_t size);
		// Gives a tile Allocate returned back. Tiles that are not allocated are ignored.
		void Free(const ShadowAtlasTile& tile);

		uint32_t GetAtlasSize() const { return m_AtlasSize; }
		uint32_t GetMinTileSize() const { return m_MinTileSize; }
		// Texels not in an allocated tile.
		uint64_t GetFreeArea() const { return m_FreeArea; }

		// The largest power of two at most `value` (0 for 0).
		static uint32_t FloorPowerOfTwo(uint32_t value);

		// Fits wanted tile sizes into an atlas: `sizes[i]` (a power of two within [minTileSize, atlasSize]) is the size of each
		// of `tileCounts[i]` tiles of entry i, entries in order of relevance (the most relevant first). While their areas add up
		// to more than the atlas's, sizes are halved one at a time in rounds from the least relevant entry toward the most
		// relevant (entries at the minimum size are passed over), so the most relevant keep the most detail. Returns how many
		// entries fit, from the first: all of them, or fewer when even at the minimum size they do not (the others' sizes are
		// left as they are).
		static size_t FitTileSizes(std::span<uint32_t> sizes, std::span<const uint32_t> tileCounts, uint32_t atlasSize, uint32_t minTileSize);
	private:
		enum class NodeState : uint8_t
		{
			Absent = 0, // Inside a free or allocated ancestor
			Free,
			Split,      // Its four children hold the state
			Used
		};

		// The quadtree's nodes level by level (level 0: the whole atlas), each level's nodes in Morton order of their
		// position, so that node i's children at the next level are 4i to 4i + 3.
		size_t GetNodeIndex(uint32_t level, uint32_t index) const { return m_LevelStart[level] + index; }
		uint32_t GetLevelOfSize(uint32_t size) const;
		ShadowAtlasTile GetTile(uint32_t level, uint32_t index) const;
	private:
		uint32_t m_AtlasSize = 0;
		uint32_t m_MinTileSize = 0;
		uint32_t m_Levels = 0;
		std::vector<size_t> m_LevelStart;
		std::vector<NodeState> m_Nodes;
		uint64_t m_FreeArea = 0;
	};

}
