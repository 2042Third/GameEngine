#pragma once

#include <cstdint>
#include <functional>

namespace Strata
{

	// The size of the renderer's bindless texture table on a Vulkan device (VulkanGraphicsDevice::CheckBindlessTableSupport).
	struct BindlessTableSize
	{
		uint32_t Slots = 0;
		// The driver's support query rejects the table although the driver samples every slot of it (MoltenVK): the
		// validation layer's report of the rejected query is not an error then (ClassifyVulkanMessage).
		bool SupportQueryUnderReports = false;
	};

	// The fewest slots a table shrinks to when the driver's support query rejects larger ones.
	constexpr uint32_t c_MinBindlessTableSlots = 1024;

	// A table of `wanted` slots (what the device's update-after-bind limits allow) where the driver's support query accepts
	// it (`isSupported(slots)`, vkGetDescriptorSetLayoutSupport) or the driver is known to under-report its support
	// (`driverUnderReportsSupport`); otherwise the table halves until the query accepts it, down to c_MinBindlessTableSlots.
	BindlessTableSize ChooseBindlessTableSize(uint32_t wanted, bool driverUnderReportsSupport, const std::function<bool(uint32_t)>& isSupported);

}
