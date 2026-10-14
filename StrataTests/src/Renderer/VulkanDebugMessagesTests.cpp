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

	VulkanMessageContext UnderReportedBindlessTable()
	{
		VulkanMessageContext context;
		context.SupportQueryUnderReportsBindlessTable = true;
		return context;
	}

	VulkanMessageContext CreatingInstanceWithLayers()
	{
		VulkanMessageContext context;
		context.CreatingInstanceWithLayers = true;
		return context;
	}

	constexpr const char* c_LayoutSupport = "VUID-vkCreateDescriptorSetLayout-support-09582";
	constexpr const char* c_MoltenVKBlending = "VK_ERROR_FEATURE_NOT_PRESENT: Blending is enabled for attachment with format VK_FORMAT_R32_UINT, "
		"which does not support it.";

}

TEST_SUITE("Renderer.VulkanMessages")
{
	TEST_CASE("Validation errors count, also without a message id")
	{
		CHECK(ClassifyVulkanMessage(Error("VUID-VkGraphicsPipelineCreateInfo-renderPass-06062"), {}) == VulkanMessageDisposition::Error);
		CHECK(ClassifyVulkanMessage(Error(nullptr, nullptr), {}) == VulkanMessageDisposition::Error);
		CHECK(ClassifyVulkanMessage(Error("Loader Message"), {}) == VulkanMessageDisposition::Error);
	}

	TEST_CASE("The layout support report counts unless the driver's query under-reports the bindless table")
	{
		CHECK(ClassifyVulkanMessage(Error(c_LayoutSupport), {}) == VulkanMessageDisposition::Error);
		CHECK(ClassifyVulkanMessage(Error(c_LayoutSupport), UnderReportedBindlessTable()) == VulkanMessageDisposition::Trace);
		// Other errors count whatever the driver's query says.
		CHECK(ClassifyVulkanMessage(Error("VUID-VkPipelineLayoutCreateInfo-descriptorType-03022"), UnderReportedBindlessTable())
			== VulkanMessageDisposition::Error);
	}

	TEST_CASE("The loader's errors about a validation layer it cannot load are warnings while the instance tries the layer")
	{
		CHECK(ClassifyVulkanMessage(Error("Loader Message", "Requested layer \"VK_LAYER_KHRONOS_validation\" failed to load!"), CreatingInstanceWithLayers())
			== VulkanMessageDisposition::Warning);
		CHECK(ClassifyVulkanMessage(Error("Loader Message"), {}) == VulkanMessageDisposition::Error);
		// The layer's own errors still count.
		CHECK(ClassifyVulkanMessage(Error("VUID-VkInstanceCreateInfo-pNext-04925"), CreatingInstanceWithLayers()) == VulkanMessageDisposition::Error);
	}

	TEST_CASE("MoltenVK's blending report of attachments that do not blend is traced, other warnings are warnings")
	{
		CHECK(ClassifyVulkanMessage(Warning("mvk-warn", c_MoltenVKBlending), {}) == VulkanMessageDisposition::Trace);
		CHECK(ClassifyVulkanMessage(Warning("mvk-warn", "VK_ERROR_FEATURE_NOT_PRESENT: Another feature is missing."), {})
			== VulkanMessageDisposition::Warning);
		CHECK(ClassifyVulkanMessage(Warning("BestPractices-Something", c_MoltenVKBlending), {}) == VulkanMessageDisposition::Warning);
		CHECK(ClassifyVulkanMessage(Warning(nullptr, nullptr), {}) == VulkanMessageDisposition::Warning);
		// Blending an attachment that cannot blend is invalid usage: the validation layer's error still counts.
		CHECK(ClassifyVulkanMessage(Error("mvk-warn", c_MoltenVKBlending), {}) == VulkanMessageDisposition::Error);
	}

	TEST_CASE("The loader's warnings about its search are traced")
	{
		CHECK(ClassifyVulkanMessage(Warning("Loader Message", "Searching for layer manifests"), {}) == VulkanMessageDisposition::Trace);
		VulkanMessage information;
		information.IdName = "Loader Message";
		CHECK(ClassifyVulkanMessage(information, {}) == VulkanMessageDisposition::Trace);
	}
}
