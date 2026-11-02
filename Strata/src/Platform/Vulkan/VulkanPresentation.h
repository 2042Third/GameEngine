#pragma once

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Strata
{

	// What a window system gives the Vulkan graphics device to present into one of its windows (Window::
	// GetVulkanPresentation): the instance extensions its surfaces need, whether a queue family can present to it, and a
	// surface for the window. The window backend implements it (Platform/GLFW), so the device names no window system.
	// Called on the main thread while the window lives.
	class VulkanPresentation
	{
	public:
		virtual ~VulkanPresentation() = default;

		// Appends the instance extensions the window system's surfaces need (VK_KHR_surface and the platform's) to
		// `outExtensions`; false, appending nothing, when it cannot present through Vulkan on this system (no loader, no
		// surface extension).
		virtual bool GetRequiredInstanceExtensions(std::vector<std::string>& outExtensions) const = 0;
		// Whether `queueFamily` of `physicalDevice` can present to the window system's windows.
		virtual bool SupportsPresentation(VkInstance instance, VkPhysicalDevice physicalDevice, uint32_t queueFamily) const = 0;
		// A surface for the window, which the caller destroys (vkDestroySurfaceKHR), or the error that prevented it.
		virtual VkResult CreateSurface(VkInstance instance, VkSurfaceKHR& outSurface) const = 0;
	};

}
