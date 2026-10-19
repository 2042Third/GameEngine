#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace Strata
{

	// An offset allocator over a range of units [0, capacity): allocates first-fit from size-class free lists (the lowest
	// fitting offset of the smallest size class that has one), with alignment, and coalesces freed ranges with their free
	// neighbours. It holds no memory itself: the units are whatever the caller's buffer holds (vertices, indices, entries).
	// PlanCompaction moves allocations to lower offsets, deterministically and bounded per call, until no free unit lies
	// below an allocation. Not thread-safe.
	class RangeAllocator
	{
	public:
		static constexpr uint32_t c_InvalidOffset = ~0u;

		// An allocation moved by PlanCompaction: its units were at From and are now at To (the ranges may overlap).
		struct Move
		{
			uint32_t From = 0;
			uint32_t To = 0;
			uint32_t Size = 0;

			bool operator==(const Move&) const = default;
		};

		explicit RangeAllocator(uint32_t capacity = 0);

		// The offset of `size` units aligned to `alignment` (a power of two), or c_InvalidOffset when no free range holds them
		// (or size is 0, or the alignment is not a power of two).
		uint32_t Allocate(uint32_t size, uint32_t alignment = 1);
		// Frees the allocation at `offset`; false (nothing changes) when no allocation starts there.
		bool Free(uint32_t offset);
		// Adds units at the end: the new ones are free. Never shrinks (a smaller capacity is ignored).
		void Grow(uint32_t capacity);

		// The size of the allocation at `offset` (0 when none starts there).
		uint32_t GetSize(uint32_t offset) const;
		uint32_t GetCapacity() const { return m_Capacity; }
		uint32_t GetUsedUnits() const { return m_Used; }
		uint32_t GetFreeUnits() const { return m_Capacity - m_Used; }
		uint32_t GetAllocationCount() const { return static_cast<uint32_t>(m_Allocations.size()); }
		// The largest free range (0 when full).
		uint32_t GetLargestFree() const;
		// The share of the free units outside the largest free range: 0 when all free units are one range (or none are free),
		// toward 1 when they are scattered in small ranges.
		float GetFragmentation() const;
		// The free units outside the largest free range, as a share of the capacity: what compaction would gather.
		float GetScatteredFreeShare() const;
		// The end of the highest allocation (0 without allocations): the capacity a buffer needs to keep its contents.
		uint32_t GetHighWaterMark() const;

		// Moves allocations to lower offsets and returns the moves (in the order they must be made: a later move may read what
		// an earlier one wrote), already applied to the allocator. It first moves the highest allocations whole into the
		// lowest free range below them that holds them, then, when none fits, slides the allocation right above the lowest free
		// range down onto it (overlapping its old place). Calls repeated until one returns nothing leave all free units in one
		// range at the end. Each call moves at most `maxUnits` units, but its first move whatever its size (so a large
		// allocation is not stuck). `movable` (null: every allocation) says whether the allocation at an offset may move now.
		std::vector<Move> PlanCompaction(uint32_t maxUnits, const std::function<bool(uint32_t offset)>& movable = nullptr);

		// Whether the bookkeeping is consistent: free ranges coalesced and disjoint, allocations disjoint, every unit either
		// free or allocated exactly once (tests).
		bool Validate() const;
		// The free ranges, offset to size, in order (tests and statistics).
		const std::map<uint32_t, uint32_t>& GetFreeRanges() const { return m_Free; }
	private:
		struct Allocation
		{
			uint32_t Size = 0;
			uint32_t Alignment = 1;
		};

		static uint32_t GetSizeClass(uint32_t size);
		void InsertFree(uint32_t offset, uint32_t size); // Coalesces with its neighbours
		void EraseFree(std::map<uint32_t, uint32_t>::iterator range);
		// Takes [offset, offset + size) out of the free range that holds it; the rest of that range stays free.
		void CarveFree(uint32_t offset, uint32_t size);
		// Moves the allocation at `from` to `to` (the bookkeeping only).
		void MoveAllocation(uint32_t from, uint32_t to);
	private:
		uint32_t m_Capacity = 0;
		uint32_t m_Used = 0;
		std::map<uint32_t, uint32_t> m_Free;                 // Offset to size, coalesced
		std::array<std::set<uint32_t>, 33> m_FreeBySize;      // Offsets of the free ranges per size class (floor(log2(size)))
		std::map<uint32_t, Allocation> m_Allocations;        // Offset to allocation
	};

}
