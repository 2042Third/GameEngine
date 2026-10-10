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
}
