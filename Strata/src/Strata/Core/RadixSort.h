#pragma once

#include <cstdint>
#include <vector>

namespace Strata
{

	// A 64-bit sort key with a 32-bit payload (e.g. a draw: its batch key and its instance).
	struct SortItem
	{
		uint64_t Key = 0;
		uint32_t Value = 0;
		uint32_t Padding = 0;
	};
	static_assert(sizeof(SortItem) == 16);

	// Sorts items by key, keeping the order of items with equal keys (stable): least significant digit first, a byte per
	// pass. Bytes that are equal in every key get neither a pass nor a histogram, so keys that differ in a few bytes cost a
	// few passes, and keys that are all equal one read of the items (they stay in place). `scratch` keeps its capacity for
	// later calls.
	void RadixSort(std::vector<SortItem>& items, std::vector<SortItem>& scratch);

}
