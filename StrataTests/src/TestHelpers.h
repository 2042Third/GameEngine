#pragma once

#include "Strata/Core/Platform.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

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

	inline std::filesystem::path GetTestExecutablePath()
	{
		return Platform::GetExecutablePath();
	}

}
