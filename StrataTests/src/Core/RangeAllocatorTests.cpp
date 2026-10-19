#include <doctest/doctest.h>

#include "Strata/Core/RangeAllocator.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <vector>

using namespace Strata;

namespace
{

	// The units of a buffer the allocator manages, each holding the id of the allocation it belongs to (0: free), and the
	// moves applied to it as a GPU copy would apply them (through a scratch copy, so overlapping slides are exact).
	struct Contents
	{
		std::vector<uint32_t> Units;

		explicit Contents(uint32_t capacity) : Units(capacity, 0) {}

		void Fill(uint32_t offset, uint32_t size, uint32_t id)
		{
			std::fill(Units.begin() + offset, Units.begin() + offset + size, id);
		}

		void Apply(const RangeAllocator::Move& move)
		{
			const std::vector<uint32_t> scratch(Units.begin() + move.From, Units.begin() + move.From + move.Size);
			std::copy(scratch.begin(), scratch.end(), Units.begin() + move.To);
		}
	};

}

TEST_SUITE("Core.RangeAllocator")
{
	TEST_CASE("Allocations are first-fit, aligned and within the capacity")
	{
		RangeAllocator allocator(1000);
		CHECK(allocator.GetCapacity() == 1000);
		CHECK(allocator.GetFreeUnits() == 1000);
		CHECK(allocator.GetLargestFree() == 1000);
		CHECK(allocator.GetFragmentation() == 0.0f);

		CHECK(allocator.Allocate(10) == 0);
		CHECK(allocator.Allocate(5) == 10);
		// Aligned to 16: the 15 units after the first two allocations stay free.
		CHECK(allocator.Allocate(20, 16) == 16);
		CHECK(allocator.GetSize(16) == 20);
		CHECK(allocator.GetUsedUnits() == 35);
		CHECK(allocator.GetHighWaterMark() == 36);
		// The padding before an aligned allocation is reused.
		CHECK(allocator.Allocate(1) == 15);
		CHECK(allocator.Validate());

		// Invalid requests.
		CHECK(allocator.Allocate(0) == RangeAllocator::c_InvalidOffset);
		CHECK(allocator.Allocate(4, 3) == RangeAllocator::c_InvalidOffset);
		CHECK(allocator.Allocate(4, 0) == RangeAllocator::c_InvalidOffset);
		CHECK(allocator.GetSize(1) == 0);
		CHECK_FALSE(allocator.Free(1));
		CHECK(allocator.Validate());
	}

	TEST_CASE("Freed ranges coalesce with their free neighbours")
	{
		RangeAllocator allocator(100);
		const uint32_t a = allocator.Allocate(10);
		const uint32_t b = allocator.Allocate(10);
		const uint32_t c = allocator.Allocate(10);
		const uint32_t d = allocator.Allocate(10);
		REQUIRE(d == 30);
		CHECK(allocator.Free(b));
		CHECK(allocator.Free(d)); // Merges with the free tail
		CHECK(allocator.GetFreeRanges() == std::map<uint32_t, uint32_t> { { 10, 10 }, { 30, 70 } });
		CHECK(allocator.Free(c)); // Bridges both
		CHECK(allocator.GetFreeRanges() == std::map<uint32_t, uint32_t> { { 10, 90 } });
		CHECK(allocator.Free(a));
		CHECK(allocator.GetFreeRanges() == std::map<uint32_t, uint32_t> { { 0, 100 } });
		CHECK_FALSE(allocator.Free(a)); // Twice
		CHECK(allocator.GetUsedUnits() == 0);
		CHECK(allocator.Validate());
	}

	TEST_CASE("A full allocator refuses, and growing it adds free units at the end")
	{
		RangeAllocator allocator(32);
		CHECK(allocator.Allocate(32) == 0);
		CHECK(allocator.Allocate(1) == RangeAllocator::c_InvalidOffset);
		CHECK(allocator.GetLargestFree() == 0);
		CHECK(allocator.GetFragmentation() == 0.0f);
		allocator.Grow(16); // Never shrinks
		CHECK(allocator.GetCapacity() == 32);
		allocator.Grow(64);
		CHECK(allocator.GetCapacity() == 64);
		CHECK(allocator.Allocate(32) == 32);
		CHECK(allocator.Allocate(1) == RangeAllocator::c_InvalidOffset);
		CHECK(allocator.Validate());

		// Growth extends a free range that ends at the old capacity.
		CHECK(allocator.Free(32));
		allocator.Grow(80);
		CHECK(allocator.GetFreeRanges() == std::map<uint32_t, uint32_t> { { 32, 48 } });
		CHECK(allocator.Validate());

		// An empty allocator holds nothing until it grows.
		RangeAllocator empty;
		CHECK(empty.Allocate(1) == RangeAllocator::c_InvalidOffset);
		CHECK(empty.GetFragmentation() == 0.0f);
		CHECK(empty.Validate());
	}

	TEST_CASE("Fragmentation counts the free units outside the largest free range")
	{
		RangeAllocator allocator(100);
		std::vector<uint32_t> offsets;
		for (int index = 0; index < 10; index++)
			offsets.push_back(allocator.Allocate(10));
		for (int index = 0; index < 10; index += 2)
			allocator.Free(offsets[index]);
		// Five free ranges of 10: the largest holds a fifth of the free units.
		CHECK(allocator.GetFreeUnits() == 50);
		CHECK(allocator.GetLargestFree() == 10);
		CHECK(allocator.GetFragmentation() == doctest::Approx(0.8f));
		CHECK(allocator.GetScatteredFreeShare() == doctest::Approx(0.4f));
	}

	TEST_CASE("A compaction plan leaves no gap and moves contents to their new offsets")
	{
		RangeAllocator allocator(1024);
		Contents contents(1024);
		std::map<uint32_t, uint32_t> ids; // Offset to id
		uint32_t nextId = 1;
		std::mt19937 random(7);
		std::vector<uint32_t> live;
		// Allocate and free at random until the allocator is fragmented.
		for (int step = 0; step < 400; step++)
		{
			if (live.size() > 4 && random() % 3 == 0)
			{
				const size_t victim = random() % live.size();
				const uint32_t offset = live[victim];
				REQUIRE(allocator.Free(offset));
				contents.Fill(offset, 1, 0); // Only for clarity: freed units are never read
				ids.erase(offset);
				live.erase(live.begin() + static_cast<std::ptrdiff_t>(victim));
				continue;
			}
			const uint32_t size = 1 + random() % 24;
			const uint32_t offset = allocator.Allocate(size);
			if (offset == RangeAllocator::c_InvalidOffset)
				continue;
			contents.Fill(offset, size, nextId);
			ids[offset] = nextId++;
			live.push_back(offset);
		}
		REQUIRE(allocator.Validate());
		REQUIRE(allocator.GetFragmentation() > 0.0f);
		const uint32_t used = allocator.GetUsedUnits();

		// Bounded calls until nothing moves; every move goes to a lower offset.
		int calls = 0;
		while (true)
		{
			const std::vector<RangeAllocator::Move> moves = allocator.PlanCompaction(64);
			if (moves.empty())
				break;
			REQUIRE(++calls < 10000);
			uint32_t movedUnits = 0;
			for (const RangeAllocator::Move& move : moves)
			{
				CHECK(move.To < move.From);
				contents.Apply(move);
				const uint32_t id = ids.at(move.From);
				ids.erase(move.From);
				ids[move.To] = id;
				movedUnits += move.Size;
			}
			// The budget holds, except for a call's first move.
			CHECK((moves.size() == 1 || movedUnits <= 64));
			REQUIRE(allocator.Validate());
		}
		// No gap: the free units are one range at the end.
		CHECK(allocator.GetUsedUnits() == used);
		CHECK(allocator.GetFragmentation() == 0.0f);
		CHECK(allocator.GetFreeRanges().size() == 1);
		CHECK(allocator.GetFreeRanges().begin()->first == allocator.GetHighWaterMark());
		// Every allocation holds its own contents at its new offset.
		for (const auto& [offset, id] : ids)
		{
			const uint32_t size = allocator.GetSize(offset);
			REQUIRE(size > 0);
			for (uint32_t unit = offset; unit < offset + size; unit++)
				REQUIRE(contents.Units[unit] == id);
		}
	}

	TEST_CASE("Compaction keeps alignments: only gaps smaller than the alignment above them remain")
	{
		RangeAllocator allocator(64);
		const uint32_t a = allocator.Allocate(3);     // [0, 3)
		const uint32_t b = allocator.Allocate(8, 8);  // [8, 16): [3, 8) stays free
		const uint32_t c = allocator.Allocate(6);     // [16, 22): too large for [3, 8)
		REQUIRE(a == 0);
		REQUIRE(b == 8);
		REQUIRE(c == 16);
		allocator.Free(a);
		while (!allocator.PlanCompaction(100).empty())
			REQUIRE(allocator.Validate());
		// The highest allocation moved whole into the gap at 0; the aligned one cannot move below 8, so the two units between
		// them stay free.
		CHECK(allocator.GetSize(0) == 6);
		CHECK(allocator.GetSize(8) == 8);
		CHECK(allocator.GetFreeRanges() == std::map<uint32_t, uint32_t> { { 6, 2 }, { 16, 48 } });

		// A gap smaller than the alignment of the allocation above it cannot close.
		RangeAllocator padded(64);
		const uint32_t unaligned = padded.Allocate(2); // [0, 2)
		const uint32_t aligned = padded.Allocate(4, 4); // [4, 8)
		REQUIRE(unaligned == 0);
		REQUIRE(aligned == 4);
		padded.Free(unaligned);
		while (!padded.PlanCompaction(100).empty())
			REQUIRE(padded.Validate());
		CHECK(padded.GetSize(0) == 4); // Moved to 0, the gap closed
		CHECK(padded.Validate());
	}

	TEST_CASE("Compaction is deterministic and leaves allocations that may not move where they are")
	{
		std::vector<uint32_t> offsets;
		const auto build = [&offsets]()
		{
			RangeAllocator allocator(256);
			offsets.clear();
			for (int index = 0; index < 16; index++)
				offsets.push_back(allocator.Allocate(8 + static_cast<uint32_t>(index % 5)));
			for (int index = 0; index < 16; index += 3)
				allocator.Free(offsets[index]);
			return allocator;
		};
		RangeAllocator first = build();
		RangeAllocator second = build();
		CHECK(first.PlanCompaction(1000) == second.PlanCompaction(1000));

		// A pinned allocation (an upload in progress) stays; the others compact around it.
		RangeAllocator pinned = build();
		const uint32_t keep = offsets[13]; // Live: every third one was freed
		const uint32_t keepSize = pinned.GetSize(keep);
		REQUIRE(keepSize > 0);
		while (!pinned.PlanCompaction(1000, [keep](uint32_t offset) { return offset != keep; }).empty())
			REQUIRE(pinned.Validate());
		CHECK(pinned.GetSize(keep) == keepSize);
		CHECK(pinned.Validate());
	}
}
