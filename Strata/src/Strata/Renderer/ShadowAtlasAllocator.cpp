#include "stpch.h"
#include "Strata/Renderer/ShadowAtlasAllocator.h"

#include <algorithm>
#include <bit>

namespace Strata
{

	namespace
	{

		// Morton order: the x bits of a node's position in the even bits of its index, the y bits in the odd ones.
		uint32_t SpreadBits(uint32_t value)
		{
			value &= 0xFFFFu;
			value = (value | (value << 8)) & 0x00FF00FFu;
			value = (value | (value << 4)) & 0x0F0F0F0Fu;
			value = (value | (value << 2)) & 0x33333333u;
			value = (value | (value << 1)) & 0x55555555u;
			return value;
		}

		uint32_t CompactBits(uint32_t value)
		{
			value &= 0x55555555u;
			value = (value | (value >> 1)) & 0x33333333u;
			value = (value | (value >> 2)) & 0x0F0F0F0Fu;
			value = (value | (value >> 4)) & 0x00FF00FFu;
			value = (value | (value >> 8)) & 0x0000FFFFu;
			return value;
		}

	}

	ShadowAtlasAllocator::ShadowAtlasAllocator(uint32_t atlasSize, uint32_t minTileSize)
	{
		Reset(atlasSize, minTileSize);
	}

	uint32_t ShadowAtlasAllocator::FloorPowerOfTwo(uint32_t value)
	{
		uint32_t result = 0;
		for (uint32_t bit = 1; bit != 0 && bit <= value; bit <<= 1)
			result = bit;
		return result;
	}

	void ShadowAtlasAllocator::Reset(uint32_t atlasSize, uint32_t minTileSize)
	{
		// Positions are 16-bit per axis in a level's index: at most 2^16 tiles each way.
		m_AtlasSize = FloorPowerOfTwo(atlasSize);
		m_MinTileSize = std::min(FloorPowerOfTwo(std::max(minTileSize, 1u)), m_AtlasSize);
		while (m_MinTileSize > 0 && m_AtlasSize / m_MinTileSize > (1u << 16))
			m_MinTileSize *= 2;
		m_Levels = 0;
		m_LevelStart.clear();
		m_Nodes.clear();
		m_FreeWordStart.clear();
		m_FreeWords.clear();
		if (m_AtlasSize == 0)
		{
			m_FreeArea = 0;
			return;
		}
		size_t nodes = 0;
		size_t words = 0;
		for (uint32_t size = m_AtlasSize; size >= m_MinTileSize; size /= 2)
		{
			const size_t levelNodes = static_cast<size_t>(1) << (2 * m_Levels);
			m_LevelStart.push_back(nodes);
			m_FreeWordStart.push_back(words);
			nodes += levelNodes;
			words += (levelNodes + 63) / 64;
			m_Levels++;
			if (size == m_MinTileSize)
				break;
		}
		m_Nodes.assign(nodes, NodeState::Absent);
		m_FreeWordStart.push_back(words); // The end of the last level's words
		m_FreeWords.assign(words, 0);
		Clear();
	}

	void ShadowAtlasAllocator::Clear()
	{
		if (m_Levels == 0)
			return;
		std::fill(m_Nodes.begin(), m_Nodes.end(), NodeState::Absent);
		std::fill(m_FreeWords.begin(), m_FreeWords.end(), 0);
		SetNode(0, 0, NodeState::Free);
		m_FreeArea = static_cast<uint64_t>(m_AtlasSize) * m_AtlasSize;
	}

	void ShadowAtlasAllocator::SetNode(uint32_t level, uint32_t index, NodeState state)
	{
		m_Nodes[GetNodeIndex(level, index)] = state;
		uint64_t& word = m_FreeWords[m_FreeWordStart[level] + index / 64];
		const uint64_t bit = static_cast<uint64_t>(1) << (index % 64);
		word = state == NodeState::Free ? (word | bit) : (word & ~bit);
	}

	uint32_t ShadowAtlasAllocator::FindFreeNode(uint32_t level) const
	{
		for (size_t word = m_FreeWordStart[level]; word < m_FreeWordStart[level + 1]; word++)
		{
			if (m_FreeWords[word] != 0)
				return static_cast<uint32_t>((word - m_FreeWordStart[level]) * 64 + static_cast<size_t>(std::countr_zero(m_FreeWords[word])));
		}
		return ~0u;
	}

	uint32_t ShadowAtlasAllocator::GetLevelOfSize(uint32_t size) const
	{
		uint32_t level = 0;
		for (uint32_t levelSize = m_AtlasSize; level < m_Levels; levelSize /= 2, level++)
		{
			if (levelSize == size)
				return level;
		}
		return ~0u;
	}

	ShadowAtlasTile ShadowAtlasAllocator::GetTile(uint32_t level, uint32_t index) const
	{
		const uint32_t size = m_AtlasSize >> level;
		return ShadowAtlasTile { CompactBits(index) * size, CompactBits(index >> 1) * size, size };
	}

	ShadowAtlasTile ShadowAtlasAllocator::Allocate(uint32_t size)
	{
		const uint32_t level = GetLevelOfSize(size);
		if (level == ~0u)
			return {};
		// The first free block of the size, else of the next larger size, and so on (split down to the size).
		for (uint32_t searchLevel = level + 1; searchLevel-- > 0;)
		{
			const uint32_t index = FindFreeNode(searchLevel);
			if (index == ~0u)
				continue;
			uint32_t node = index;
			for (uint32_t splitLevel = searchLevel; splitLevel < level; splitLevel++)
			{
				SetNode(splitLevel, node, NodeState::Split);
				for (uint32_t child = 0; child < 4; child++)
					SetNode(splitLevel + 1, node * 4 + child, NodeState::Free);
				node *= 4;
			}
			SetNode(level, node, NodeState::Used);
			m_FreeArea -= static_cast<uint64_t>(size) * size;
			return GetTile(level, node);
		}
		return {};
	}

	void ShadowAtlasAllocator::Free(const ShadowAtlasTile& tile)
	{
		const uint32_t level = GetLevelOfSize(tile.Size);
		if (level == ~0u || tile.X % tile.Size != 0 || tile.Y % tile.Size != 0 || tile.X >= m_AtlasSize || tile.Y >= m_AtlasSize)
			return;
		uint32_t index = SpreadBits(tile.X / tile.Size) | (SpreadBits(tile.Y / tile.Size) << 1);
		if (m_Nodes[GetNodeIndex(level, index)] != NodeState::Used)
			return;
		SetNode(level, index, NodeState::Free);
		m_FreeArea += static_cast<uint64_t>(tile.Size) * tile.Size;
		// Four free quarters make a free block again.
		for (uint32_t mergeLevel = level; mergeLevel > 0; mergeLevel--)
		{
			const uint32_t parent = index / 4;
			bool allFree = true;
			for (uint32_t child = 0; child < 4 && allFree; child++)
				allFree = m_Nodes[GetNodeIndex(mergeLevel, parent * 4 + child)] == NodeState::Free;
			if (!allFree)
				break;
			for (uint32_t child = 0; child < 4; child++)
				SetNode(mergeLevel, parent * 4 + child, NodeState::Absent);
			SetNode(mergeLevel - 1, parent, NodeState::Free);
			index = parent;
		}
	}

	size_t ShadowAtlasAllocator::FitTileSizes(std::span<uint32_t> sizes, std::span<const uint32_t> tileCounts, uint32_t atlasSize, uint32_t minTileSize,
		uint64_t areaBudget)
	{
		const size_t count = std::min(sizes.size(), tileCounts.size());
		const uint32_t atlas = FloorPowerOfTwo(atlasSize);
		const uint32_t minimum = std::min(FloorPowerOfTwo(std::max(minTileSize, 1u)), atlas);
		const uint64_t whole = static_cast<uint64_t>(atlas) * atlas;
		const uint64_t area = areaBudget == 0 ? whole : std::min(areaBudget, whole);
		uint64_t total = 0;
		for (size_t entry = 0; entry < count; entry++)
		{
			sizes[entry] = std::clamp(FloorPowerOfTwo(sizes[entry]), minimum, atlas);
			total += static_cast<uint64_t>(tileCounts[entry]) * sizes[entry] * sizes[entry];
		}
		// Rounds from the least relevant entry toward the most relevant one, halving one size per step.
		size_t cursor = count;
		while (total > area)
		{
			bool stepped = false;
			for (size_t tried = 0; tried < count && !stepped; tried++)
			{
				cursor = cursor == 0 ? count - 1 : cursor - 1;
				if (sizes[cursor] <= minimum)
					continue;
				const uint64_t size = sizes[cursor];
				total -= static_cast<uint64_t>(tileCounts[cursor]) * (size * size - (size / 2) * (size / 2));
				sizes[cursor] /= 2;
				stepped = true;
			}
			if (!stepped)
				break;
		}
		if (total <= area)
			return count;
		// Even at the minimum size they do not all fit: the most relevant ones that do.
		size_t fit = 0;
		uint64_t used = 0;
		while (fit < count && used + static_cast<uint64_t>(tileCounts[fit]) * sizes[fit] * sizes[fit] <= area)
		{
			used += static_cast<uint64_t>(tileCounts[fit]) * sizes[fit] * sizes[fit];
			fit++;
		}
		return fit;
	}

}
