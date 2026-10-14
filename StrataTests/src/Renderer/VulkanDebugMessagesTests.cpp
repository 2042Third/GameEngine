#include <doctest/doctest.h>

#include "Platform/Vulkan/VulkanDebugMessages.h"

using namespace Strata;

namespace
{

	VulkanMessage Error(const char* idName, const char* text = "A message")
	{
		VulkanMessage message;
		message.Error = true;
		message.IdName = idName;
		message.Text = text;
		return message;
	}

	VulkanMessage Warning(const char* idName, const char* text = "A message")
	{
		VulkanMessage message;
		message.Warning = true;
		message.IdName = idName;
		message.Text = text;
		return message;
	}

	VulkanMessageContext CreatingInstanceWithLayers()
	{
		VulkanMessageContext context;
		context.CreatingInstanceWithLayers = true;
		return context;
	}


}

TEST_SUITE("Renderer.VulkanMessages")
{
	TEST_CASE("Validation errors count, also without a message id")
	{
		CHECK(ClassifyVulkanMessage(Error("VUID-VkGraphicsPipelineCreateInfo-renderPass-06062"), {}) == VulkanMessageDisposition::Error);
		CHECK(ClassifyVulkanMessage(Error(nullptr, nullptr), {}) == VulkanMessageDisposition::Error);
		CHECK(ClassifyVulkanMessage(Error("Loader Message"), {}) == VulkanMessageDisposition::Error);
	}

	TEST_CASE("The loader's errors about a validation layer it cannot load are warnings while the instance tries the layer")
	{
		CHECK(ClassifyVulkanMessage(Error("Loader Message", "Requested layer \"VK_LAYER_KHRONOS_validation\" failed to load!"), CreatingInstanceWithLayers())
			== VulkanMessageDisposition::Warning);
		CHECK(ClassifyVulkanMessage(Error("Loader Message"), {}) == VulkanMessageDisposition::Error);
		// The layer's own errors still count.
		CHECK(ClassifyVulkanMessage(Error("VUID-VkInstanceCreateInfo-pNext-04925"), CreatingInstanceWithLayers()) == VulkanMessageDisposition::Error);
	}

	TEST_CASE("The loader's warnings about its search are traced")
	{
		CHECK(ClassifyVulkanMessage(Warning("Loader Message", "Searching for layer manifests"), {}) == VulkanMessageDisposition::Trace);
		VulkanMessage information;
		information.IdName = "Loader Message";
		CHECK(ClassifyVulkanMessage(information, {}) == VulkanMessageDisposition::Trace);
	}
}
