#include "stpch.h"
#include "Strata/Core/RadixSort.h"

#include "Strata/Core/Assert.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace Strata
{

	void RadixSort(std::vector<SortItem>& items, std::vector<SortItem>& scratch)
	{
		const size_t count = items.size();
		if (count < 2)
			return;
		ST_CORE_ASSERT(count <= std::numeric_limits<uint32_t>::max(), "RadixSort: {} items are more than its 32-bit counters hold", count);

		// The bits in which some key differs from the first: a byte without any needs no pass, and no histogram either.
		// Draw keys differ in a byte or two (the cascade, the mesh), or not at all.
		const uint64_t first = items[0].Key;
		uint64_t differing = 0;
		for (const SortItem& item : items)
			differing |= item.Key ^ first;
		if (differing == 0)
			return;
		std::array<uint32_t, 8> shifts = {};
		uint32_t passes = 0;
		for (uint32_t byte = 0; byte < 8; byte++)
		{
			if (((differing >> (byte * 8)) & 0xFF) != 0)
				shifts[passes++] = byte * 8;
		}

		// The histograms of the bytes that differ, in one pass over the keys.
		std::array<std::array<uint32_t, 256>, 8> histograms;
		for (uint32_t pass = 0; pass < passes; pass++)
			histograms[pass].fill(0);
		for (const SortItem& item : items)
		{
			for (uint32_t pass = 0; pass < passes; pass++)
				histograms[pass][(item.Key >> shifts[pass]) & 0xFF]++;
		}

		scratch.resize(count);
		std::vector<SortItem>* source = &items;
		std::vector<SortItem>* destination = &scratch;
		for (uint32_t pass = 0; pass < passes; pass++)
		{
			std::array<uint32_t, 256>& histogram = histograms[pass];
			uint32_t offset = 0;
			for (uint32_t& bucket : histogram)
			{
				const uint32_t size = bucket;
				bucket = offset;
				offset += size;
			}
			const uint32_t shift = shifts[pass];
			for (const SortItem& item : *source)
				(*destination)[histogram[(item.Key >> shift) & 0xFF]++] = item;
			std::swap(source, destination);
		}
		if (source != &items)
			items.swap(scratch);
	}

	void SortIndices(std::vector<uint32_t>& values, std::vector<SortItem>& items, std::vector<SortItem>& scratch)
	{
		// Below this many values a comparison sort is as fast as the radix sort's passes.
		constexpr size_t c_RadixSortValues = 256;
		if (values.size() < c_RadixSortValues)
		{
			std::sort(values.begin(), values.end());
			return;
		}
		if (std::is_sorted(values.begin(), values.end()))
			return;
		items.resize(values.size());
		for (size_t index = 0; index < values.size(); index++)
			items[index] = SortItem { values[index], values[index], 0 };
		RadixSort(items, scratch);
		for (size_t index = 0; index < values.size(); index++)
			values[index] = items[index].Value;
	}

}
