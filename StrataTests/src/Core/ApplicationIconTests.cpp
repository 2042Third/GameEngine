#include <doctest/doctest.h>

#include "Platform/GLFW/GLFWApplicationIcon.h"

#include <cstdint>
#include <span>
#include <vector>

using namespace Strata;

// Only refusals, which change nothing: an image the call takes would become this process's Dock icon on macOS
// (Platform.DockIcon checks that, in a test of its own).
TEST_SUITE("Core.ApplicationIcon")
{
	TEST_CASE("SetApplicationIcon refuses empty or inconsistent images")
	{
		const std::vector<uint8_t> pixels(16 * 16 * 4, 0x80);
		const std::span<const uint8_t> all(pixels);
		CHECK_FALSE(SetApplicationIcon(0, 16, all));
		CHECK_FALSE(SetApplicationIcon(16, 0, all));
		CHECK_FALSE(SetApplicationIcon(0, 0, {}));
		CHECK_FALSE(SetApplicationIcon(16, 16, {}));
		CHECK_FALSE(SetApplicationIcon(16, 16, all.first(all.size() - 1)));
		CHECK_FALSE(SetApplicationIcon(16, 8, all)); // More pixels than the size holds
		CHECK_FALSE(SetApplicationIcon(8, 16, all.first(8 * 16 * 3)));

		// Larger than the Dock draws, however consistent.
		const std::vector<uint8_t> wide(static_cast<size_t>(c_MaxApplicationIconSize + 1) * 4, 0xFF);
		CHECK_FALSE(SetApplicationIcon(c_MaxApplicationIconSize + 1, 1, wide));
		CHECK_FALSE(SetApplicationIcon(1, c_MaxApplicationIconSize + 1, wide));
		// A size whose byte count overflows 32 bits is not taken for a smaller one.
		CHECK_FALSE(SetApplicationIcon(UINT32_MAX, UINT32_MAX, all));
	}
}
