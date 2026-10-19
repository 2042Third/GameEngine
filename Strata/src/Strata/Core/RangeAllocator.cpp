#include "stpch.h"
#include "Strata/Core/RangeAllocator.h"

#include <algorithm>
#include <bit>

namespace Strata
{

	namespace
	{

		uint64_t AlignUp(uint64_t value, uint32_t alignment)
		{
			return (value + alignment - 1) / alignment * alignment;
		}

		// Compaction looks at this many of the highest allocations for one that fits a free range below it whole, per move.
		constexpr uint32_t c_MaxWholeMoveCandidates = 64;

	}

	RangeAllocator::RangeAllocator(uint32_t capacity)
	{
		Grow(capacity);
	}

	uint32_t RangeAllocator::GetSizeClass(uint32_t size)
	{
		return size == 0 ? 0 : static_cast<uint32_t>(std::bit_width(size) - 1);
	}

	void RangeAllocator::EraseFree(std::map<uint32_t, uint32_t>::iterator range)
	{
		m_FreeBySize[GetSizeClass(range->second)].erase(range->first);
		m_Free.erase(range);
	}

	void RangeAllocator::InsertFree(uint32_t offset, uint32_t size)
	{
		if (size == 0)
			return;
		uint32_t start = offset;
		uint64_t end = static_cast<uint64_t>(offset) + size;
		// Merge with the free range ending where this one starts, and the one starting where it ends.
		auto next = m_Free.lower_bound(offset);
		if (next != m_Free.begin())
		{
			auto previous = std::prev(next);
			if (static_cast<uint64_t>(previous->first) + previous->second == offset)
			{
				start = previous->first;
				EraseFree(previous);
			}
		}
		next = m_Free.lower_bound(offset);
		if (next != m_Free.end() && next->first == end)
		{
			end += next->second;
			EraseFree(next);
		}
		const uint32_t merged = static_cast<uint32_t>(end - start);
		m_Free.emplace(start, merged);
		m_FreeBySize[GetSizeClass(merged)].insert(start);
	}

	void RangeAllocator::CarveFree(uint32_t offset, uint32_t size)
	{
		auto range = m_Free.upper_bound(offset);
		ST_CORE_ASSERT(range != m_Free.begin(), "RangeAllocator: carving units that are not free");
		range = std::prev(range);
		const uint32_t rangeStart = range->first;
		const uint64_t rangeEnd = static_cast<uint64_t>(range->first) + range->second;
		ST_CORE_ASSERT(offset >= rangeStart && static_cast<uint64_t>(offset) + size <= rangeEnd, "RangeAllocator: carving units that are not free");
		EraseFree(range);
		if (offset > rangeStart)
		{
			m_Free.emplace(rangeStart, offset - rangeStart);
			m_FreeBySize[GetSizeClass(offset - rangeStart)].insert(rangeStart);
		}
		const uint64_t carvedEnd = static_cast<uint64_t>(offset) + size;
		if (carvedEnd < rangeEnd)
		{
			const uint32_t rest = static_cast<uint32_t>(rangeEnd - carvedEnd);
			m_Free.emplace(static_cast<uint32_t>(carvedEnd), rest);
			m_FreeBySize[GetSizeClass(rest)].insert(static_cast<uint32_t>(carvedEnd));
		}
	}

	uint32_t RangeAllocator::Allocate(uint32_t size, uint32_t alignment)
	{
		if (size == 0 || alignment == 0 || !std::has_single_bit(alignment))
			return c_InvalidOffset;
		// The smallest size class that can hold the allocation first; in it, ranges may still be too small, in the larger
		// ones only alignment can make them so. The lowest fitting offset of a class wins: deterministic, and allocations
		// gather at the start.
		for (uint32_t sizeClass = GetSizeClass(size); sizeClass < m_FreeBySize.size(); sizeClass++)
		{
			for (const uint32_t offset : m_FreeBySize[sizeClass])
			{
				const uint32_t rangeSize = m_Free.at(offset);
				const uint64_t aligned = AlignUp(offset, alignment);
				if (aligned + size > static_cast<uint64_t>(offset) + rangeSize)
					continue;
				const uint32_t result = static_cast<uint32_t>(aligned);
				CarveFree(result, size);
				m_Allocations.emplace(result, Allocation { size, alignment });
				m_Used += size;
				return result;
			}
		}
		return c_InvalidOffset;
	}

	bool RangeAllocator::Free(uint32_t offset)
	{
		const auto allocation = m_Allocations.find(offset);
		if (allocation == m_Allocations.end())
			return false;
		const uint32_t size = allocation->second.Size;
		m_Allocations.erase(allocation);
		m_Used -= size;
		InsertFree(offset, size);
		return true;
	}

	void RangeAllocator::Grow(uint32_t capacity)
	{
		if (capacity <= m_Capacity)
			return;
		const uint32_t added = capacity - m_Capacity;
		const uint32_t start = m_Capacity;
		m_Capacity = capacity;
		InsertFree(start, added);
	}

	uint32_t RangeAllocator::GetSize(uint32_t offset) const
	{
		const auto allocation = m_Allocations.find(offset);
		return allocation == m_Allocations.end() ? 0 : allocation->second.Size;
	}

	uint32_t RangeAllocator::GetLargestFree() const
	{
		for (size_t sizeClass = m_FreeBySize.size(); sizeClass > 0; sizeClass--)
		{
			const std::set<uint32_t>& ranges = m_FreeBySize[sizeClass - 1];
			if (ranges.empty())
				continue;
			uint32_t largest = 0;
			for (const uint32_t offset : ranges)
				largest = std::max(largest, m_Free.at(offset));
			return largest;
		}
		return 0;
	}

	float RangeAllocator::GetFragmentation() const
	{
		const uint32_t free = GetFreeUnits();
		if (free == 0)
			return 0.0f;
		return 1.0f - static_cast<float>(GetLargestFree()) / static_cast<float>(free);
	}

	float RangeAllocator::GetScatteredFreeShare() const
	{
		if (m_Capacity == 0)
			return 0.0f;
		return static_cast<float>(GetFreeUnits() - GetLargestFree()) / static_cast<float>(m_Capacity);
	}

	uint32_t RangeAllocator::GetHighWaterMark() const
	{
		if (m_Allocations.empty())
			return 0;
		const auto& last = *m_Allocations.rbegin();
		return last.first + last.second.Size;
	}

	void RangeAllocator::MoveAllocation(uint32_t from, uint32_t to)
	{
		const auto allocation = m_Allocations.find(from);
		ST_CORE_ASSERT(allocation != m_Allocations.end(), "RangeAllocator: moving an allocation that does not exist");
		const Allocation moved = allocation->second;
		m_Allocations.erase(allocation);
		// The old place becomes free first: a slide's new place overlaps it.
		InsertFree(from, moved.Size);
		CarveFree(to, moved.Size);
		m_Allocations.emplace(to, moved);
	}

	std::vector<RangeAllocator::Move> RangeAllocator::PlanCompaction(uint32_t maxUnits, const std::function<bool(uint32_t offset)>& movable)
	{
		std::vector<Move> moves;
		uint64_t movedUnits = 0;
		const auto canMove = [&movable](uint32_t offset) { return !movable || movable(offset); };
		const auto withinBudget = [&](uint32_t size) { return moves.empty() || movedUnits + size <= maxUnits; };

		while (!m_Free.empty())
		{
			// Whole moves: the highest allocations into the lowest free range below them that holds them.
			bool moved = false;
			uint32_t candidates = 0;
			for (auto allocation = m_Allocations.rbegin(); allocation != m_Allocations.rend() && candidates < c_MaxWholeMoveCandidates; ++allocation)
			{
				const uint32_t offset = allocation->first;
				const Allocation current = allocation->second;
				if (m_Free.begin()->first > offset)
					break; // No free unit below this or any lower allocation
				candidates++;
				if (!canMove(offset))
					continue;
				for (const auto& [freeOffset, freeSize] : m_Free)
				{
					if (freeOffset > offset)
						break;
					const uint64_t aligned = AlignUp(freeOffset, current.Alignment);
					if (aligned + current.Size > static_cast<uint64_t>(freeOffset) + freeSize || aligned >= offset)
						continue;
					if (!withinBudget(current.Size))
						return moves;
					const uint32_t target = static_cast<uint32_t>(aligned);
					MoveAllocation(offset, target);
					moves.push_back(Move { offset, target, current.Size });
					movedUnits += current.Size;
					moved = true;
					break;
				}
				if (moved)
					break;
			}
			if (moved)
				continue;

			// A slide: the movable allocation right above the lowest free range that has one, down onto that range.
			for (const auto& [freeOffset, freeSize] : m_Free)
			{
				const uint64_t end = static_cast<uint64_t>(freeOffset) + freeSize;
				if (end >= m_Capacity)
					break; // The free range at the end has nothing above it
				const auto above = m_Allocations.find(static_cast<uint32_t>(end));
				if (above == m_Allocations.end() || !canMove(above->first))
					continue;
				const uint64_t aligned = AlignUp(freeOffset, above->second.Alignment);
				if (aligned >= above->first)
					continue;
				const uint32_t size = above->second.Size;
				if (!withinBudget(size))
					return moves;
				const uint32_t from = above->first;
				const uint32_t target = static_cast<uint32_t>(aligned);
				MoveAllocation(from, target);
				moves.push_back(Move { from, target, size });
				movedUnits += size;
				moved = true;
				break;
			}
			if (!moved)
				break;
		}
		return moves;
	}

	bool RangeAllocator::Validate() const
	{
		uint64_t freeUnits = 0;
		uint64_t previousEnd = 0;
		bool first = true;
		for (const auto& [offset, size] : m_Free)
		{
			if (size == 0 || static_cast<uint64_t>(offset) + size > m_Capacity)
				return false;
			// Disjoint and coalesced: a free range never touches the previous one.
			if (!first && offset <= previousEnd)
				return false;
			if (m_FreeBySize[GetSizeClass(size)].count(offset) != 1)
				return false;
			first = false;
			previousEnd = static_cast<uint64_t>(offset) + size;
			freeUnits += size;
		}
		size_t indexed = 0;
		for (const std::set<uint32_t>& ranges : m_FreeBySize)
			indexed += ranges.size();
		if (indexed != m_Free.size())
			return false;

		uint64_t usedUnits = 0;
		previousEnd = 0;
		for (const auto& [offset, allocation] : m_Allocations)
		{
			if (allocation.Size == 0 || offset < previousEnd || static_cast<uint64_t>(offset) + allocation.Size > m_Capacity || offset % allocation.Alignment != 0)
				return false;
			// No allocation overlaps a free range.
			auto range = m_Free.upper_bound(offset);
			if (range != m_Free.begin())
			{
				const auto previous = std::prev(range);
				if (static_cast<uint64_t>(previous->first) + previous->second > offset)
					return false;
			}
			if (range != m_Free.end() && range->first < static_cast<uint64_t>(offset) + allocation.Size)
				return false;
			previousEnd = static_cast<uint64_t>(offset) + allocation.Size;
			usedUnits += allocation.Size;
		}
		return usedUnits == m_Used && freeUnits + usedUnits == m_Capacity;
	}

}
