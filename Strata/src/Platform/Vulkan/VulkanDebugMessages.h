#pragma once

namespace Strata
{

	// What the engine makes of a message of the Vulkan debug messenger (VulkanGraphicsDevice's callback): errors count
	// against the device (GraphicsDevice::GetErrorCount, which tests require to stay zero), warnings are logged as such,
	// and the rest only at trace level.
	enum class VulkanMessageDisposition
	{
		Trace,
		Warning,
		Error
	};

	struct VulkanMessage
	{
		bool Error = false;           // VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
		bool Warning = false;         // VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
		const char* IdName = nullptr; // VkDebugUtilsMessengerCallbackDataEXT::pMessageIdName (may be null)
		const char* Text = nullptr;   // pMessage (may be null)
	};

	// What the device knows when the message arrives.
	struct VulkanMessageContext
	{
		// vkCreateInstance is trying the validation layer; when the loader cannot load it, the instance is created again
		// without it (VulkanGraphicsDevice::CreateInstance).
		bool CreatingInstanceWithLayers = false;
	};

	VulkanMessageDisposition ClassifyVulkanMessage(const VulkanMessage& message, const VulkanMessageContext& context);

}
