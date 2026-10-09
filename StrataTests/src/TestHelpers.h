#pragma once

#include "Strata/Core/Platform.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace Strata::Tests
{

	// Creates a fresh, empty directory under the system temp directory. All directories created this way
	// are removed when the test run finishes.
	std::filesystem::path CreateTemporaryDirectory(const std::string& name);
	void CleanupTemporaryDirectories();

	// Polls condition until it returns true or the timeout elapses; returns the final condition value.
	inline bool WaitUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (condition())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return condition();
	}

	// Builds a 16-bit PCM WAV file containing a sine wave (the same signal on every channel), optionally preceded by
	// silence.
	std::vector<uint8_t> CreateSineWav(float durationSeconds, uint32_t sampleRate = 48000, uint16_t channels = 1, float frequency = 440.0f,
		float silentSeconds = 0.0f, float amplitude = 0.5f);

	// Encodes 8-bit RGBA pixels (tightly packed rows) as a PNG file.
	std::vector<uint8_t> EncodePNG(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba);
	// A PNG filled with one color.
	std::vector<uint8_t> CreateSolidPNG(uint32_t width, uint32_t height, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255);

#if defined(ST_PLATFORM_WINDOWS)
	// Creates a junction (a mount-point reparse point, which needs no special privilege) at link that redirects to target.
	// False when it cannot be created (e.g. on file systems without reparse points, such as exFAT).
	bool CreateJunction(const std::filesystem::path& link, const std::filesystem::path& target);
#endif

	inline std::filesystem::path GetTestExecutablePath()
	{
		return Platform::GetExecutablePath();
	}

}
