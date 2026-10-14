#include "stpch.h"
#include "Platform/Vulkan/VulkanDebugMessages.h"

#include <cstring>

namespace Strata
{

	namespace
	{

		bool Equals(const char* text, const char* expected)
		{
			return text && std::strcmp(text, expected) == 0;
		}

		bool Contains(const char* text, const char* part)
		{
			return text && std::strstr(text, part) != nullptr;
		}

	}

	VulkanMessageDisposition ClassifyVulkanMessage(const VulkanMessage& message, const VulkanMessageContext& context)
	{
		if (message.Error)
		{
			// The validation layer's report of a descriptor set layout beyond the device's limits that the driver's support
			// query rejects. Where the query under-reports the bindless table, it says nothing about the table; the layer
			// still checks every set against the update-after-bind limits.
			if (context.SupportQueryUnderReportsBindlessTable && Equals(message.IdName, "VUID-vkCreateDescriptorSetLayout-support-09582"))
				return VulkanMessageDisposition::Trace;
			// The loader's reports of a validation layer it lists but cannot load (its library's search, "Requested layer ...
			// failed to load!"): the instance is created again without the layer, which is no error of the device.
			if (context.CreatingInstanceWithLayers && Equals(message.IdName, "Loader Message"))
				return VulkanMessageDisposition::Warning;
			return VulkanMessageDisposition::Error;
		}
		if (!message.Warning)
			return VulkanMessageDisposition::Trace;

		// The loader reports its search (e.g. missing registry keys for layer manifests) as warnings.
		if (Equals(message.IdName, "Loader Message"))
			return VulkanMessageDisposition::Trace;
		// MoltenVK reports every color attachment whose format Metal cannot blend (the prepass's R32_UINT entity IDs) as
		// "Blending is enabled ...", whether or not the pipeline blends it, and creates the pipeline without blending. The
		// engine never blends such a target; doing so would be invalid usage, which the validation layer reports as an error
		// (VUID-VkGraphicsPipelineCreateInfo-renderPass-06062, -06041 with render passes) where it is loaded.
		if (Equals(message.IdName, "mvk-warn") && Contains(message.Text, "Blending is enabled for attachment with format"))
			return VulkanMessageDisposition::Trace;
		return VulkanMessageDisposition::Warning;
	}

}
