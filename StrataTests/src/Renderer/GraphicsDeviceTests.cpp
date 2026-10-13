#include <doctest/doctest.h>

#include "Strata/Core/PlatformDetection.h"
#include "Strata/Renderer/GraphicsDevice.h"

#include <string>

using namespace Strata;

TEST_SUITE("Renderer.GraphicsDevice")
{
	TEST_CASE("Without Vulkan or a GPU, errors say what to install on this platform")
	{
		// The device's errors end with it (VulkanGraphicsDevice): a person who downloaded Strata must learn what is missing.
		const std::string advice = GraphicsDevice::GetDriverInstallAdvice();
		CAPTURE(advice);
#if defined(ST_PLATFORM_WINDOWS)
		CHECK(advice.find("update the GPU driver") != std::string::npos);
		CHECK(advice.find("Vulkan runtime") != std::string::npos);
#elif defined(ST_PLATFORM_MACOS)
		CHECK(advice.find("MoltenVK") != std::string::npos);
		CHECK(advice.find("https://vulkan.lunarg.com/sdk/home#mac") != std::string::npos);
		CHECK(advice.find("brew install molten-vk vulkan-loader") != std::string::npos);
#else
		CHECK(advice.find("sudo apt install libvulkan1 mesa-vulkan-drivers") != std::string::npos);
#endif
	}
}
