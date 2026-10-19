#include <doctest/doctest.h>

#include "Strata/Core/RadixSort.h"

#include <algorithm>
#include <random>
#include <vector>

using namespace Strata;

namespace
{

	std::vector<SortItem> StableSorted(std::vector<SortItem> items)
	{
		std::stable_sort(items.begin(), items.end(), [](const SortItem& a, const SortItem& b) { return a.Key < b.Key; });
		return items;
	}

	bool SameItems(const std::vector<SortItem>& a, const std::vector<SortItem>& b)
	{
		return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const SortItem& x, const SortItem& y) { return x.Key == y.Key && x.Value == y.Value; });
	}

}

TEST_SUITE("Core.RadixSort")
{
	TEST_CASE("Sorts by key and keeps the order of equal keys, as a stable sort does")
	{
		std::mt19937_64 random(5);
		std::vector<SortItem> scratch;
		for (const uint64_t mask : { ~0ull, 0xFF00ull, 0xF000000000000000ull, 0x00FF00FF00FF0000ull, 0ull })
		{
			CAPTURE(mask);
			std::vector<SortItem> items(20'000);
			for (uint32_t index = 0; index < items.size(); index++)
				items[index] = SortItem { random() & mask, index, 0 };
			const std::vector<SortItem> expected = StableSorted(items);
			RadixSort(items, scratch);
			CHECK(SameItems(items, expected));
		}
	}

	TEST_CASE("Items whose keys are all equal are left in place without a pass")
	{
		// Draws of one mesh and material share their key: their order is already the sorted one, and the sort neither moves
		// nor copies them (the scratch list is not even sized).
		std::vector<SortItem> items(10'000);
		for (uint32_t index = 0; index < items.size(); index++)
			items[index] = SortItem { 0x0123456789ABCDEFull, index, 0 };
		const SortItem* data = items.data();
		std::vector<SortItem> scratch;
		RadixSort(items, scratch);
		CHECK(items.data() == data);
		CHECK(scratch.empty());
		for (uint32_t index = 0; index < items.size(); index++)
			REQUIRE(items[index].Value == index);
	}

	TEST_CASE("Empty and single-item lists are left alone")
	{
		std::vector<SortItem> scratch;
		std::vector<SortItem> items;
		RadixSort(items, scratch);
		CHECK(items.empty());
		items.push_back(SortItem { 42, 7, 0 });
		RadixSort(items, scratch);
		REQUIRE(items.size() == 1);
		CHECK(items[0].Key == 42);
		CHECK(items[0].Value == 7);
	}

	TEST_CASE("Indices sort like std::sort, sorted ones stay untouched")
	{
		std::vector<SortItem> items;
		std::vector<SortItem> scratch;
		// Long lists in no order, with repeats (as a GPU's atomics list slots), and short ones.
		for (const size_t count : { size_t(0), size_t(1), size_t(100), size_t(255), size_t(256), size_t(10'000) })
		{
			CAPTURE(count);
			std::vector<uint32_t> values(count);
			uint32_t state = 12345;
			for (uint32_t& value : values)
			{
				state = state * 1664525u + 1013904223u;
				value = (state >> 8) % 20'000u;
			}
			std::vector<uint32_t> expected = values;
			std::sort(expected.begin(), expected.end());
			SortIndices(values, items, scratch);
			CHECK(values == expected);
		}
		// Every value range a 32-bit index can have.
		std::vector<uint32_t> wide = { 0xFFFFFFFFu, 0u, 0x80000000u, 0x00010000u, 0x7FFFFFFFu, 1u };
		wide.resize(1'000, 0xABCDEF01u);
		std::vector<uint32_t> expected = wide;
		std::sort(expected.begin(), expected.end());
		SortIndices(wide, items, scratch);
		CHECK(wide == expected);

		// A sorted list is not copied into the items.
		std::vector<uint32_t> sorted(5'000);
		for (uint32_t index = 0; index < sorted.size(); index++)
			sorted[index] = index * 3u;
		std::vector<SortItem> unused;
		std::vector<SortItem> unusedScratch;
		const std::vector<uint32_t> copy = sorted;
		SortIndices(sorted, unused, unusedScratch);
		CHECK(sorted == copy);
		CHECK(unused.empty());
	}
}
