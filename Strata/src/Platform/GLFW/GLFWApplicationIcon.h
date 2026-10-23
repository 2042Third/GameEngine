#pragma once

#include <cstdint>
#include <span>

namespace Strata
{

	// The largest side SetApplicationIcon accepts: the Dock draws icons of at most 1024 pixels.
	constexpr uint32_t c_MaxApplicationIconSize = 1024;

	// Sets the icon the system shows for the running program, apart from its windows: the Dock's on macOS, for as long as the
	// program runs (a bare executable has no bundle to give it one). The image is RGBA8 with straight alpha, rows top to
	// bottom, width * height * 4 bytes; the pixels are copied. Returns false, changing nothing, for an empty image, one
	// larger than c_MaxApplicationIconSize or one whose pixel count does not match its size, and on platforms whose
	// programs have no icon of their own (everywhere but macOS: their windows carry it, Window::SetIcon). Main thread only.
	bool SetApplicationIcon(uint32_t width, uint32_t height, std::span<const uint8_t> pixels);

}
