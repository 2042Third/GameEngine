#pragma once

#include <vulkan/vulkan_core.h>

#include <string>

namespace Strata::VulkanLoader
{

	// Loads the platform's Vulkan loader library once per process and returns its vkGetInstanceProcAddr, or null when
	// Vulkan is unavailable (see GetLoadError). The window system is initialized with the same function
	// (glfwInitVulkanLoader), so the instance and window surfaces always come from one loader. Thread-safe.
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr();
	std::string GetLoadError();

}
