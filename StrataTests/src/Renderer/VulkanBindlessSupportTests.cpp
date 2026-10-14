#include <doctest/doctest.h>

#include "Platform/Vulkan/VulkanBindlessSupport.h"

#include <cstdint>
#include <vector>

using namespace Strata;

TEST_SUITE("Renderer.BindlessSupport")
{
	TEST_CASE("A table the driver's query accepts keeps the slots the limits allow")
	{
		std::vector<uint32_t> asked;
		const BindlessTableSize size = ChooseBindlessTableSize(32768, false, [&asked](uint32_t slots) { asked.push_back(slots); return true; });
		CHECK(size.Slots == 32768);
		CHECK_FALSE(size.SupportQueryUnderReports);
		CHECK(asked == std::vector<uint32_t> { 32768 });
	}

	TEST_CASE("A driver that under-reports keeps the table and has its report traced")
	{
		// As MoltenVK's query: sets beyond 1,212 descriptors rejected.
		const BindlessTableSize size = ChooseBindlessTableSize(32768, true, [](uint32_t slots) { return slots < 1212; });
		CHECK(size.Slots == 32768);
		CHECK(size.SupportQueryUnderReports);
	}

	TEST_CASE("Elsewhere the table halves until the driver's query accepts it, down to the smallest table")
	{
		std::vector<uint32_t> asked;
		const BindlessTableSize halved = ChooseBindlessTableSize(32768, false, [&asked](uint32_t slots) { asked.push_back(slots); return slots <= 5000; });
		CHECK(halved.Slots == 4096);
		CHECK_FALSE(halved.SupportQueryUnderReports);
		CHECK(asked == std::vector<uint32_t> { 32768, 16384, 8192, 4096 });

		const BindlessTableSize smallest = ChooseBindlessTableSize(32768, false, [](uint32_t) { return false; });
		CHECK(smallest.Slots == c_MinBindlessTableSlots);
		CHECK_FALSE(smallest.SupportQueryUnderReports);

		// Limits below the smallest table are kept as they are.
		CHECK(ChooseBindlessTableSize(500, false, [](uint32_t) { return false; }).Slots == 500);
	}
}
