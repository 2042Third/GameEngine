#include "stpch.h"
#include "Platform/Vulkan/VulkanBindlessSupport.h"

#include <algorithm>

namespace Strata
{

	BindlessTableSize ChooseBindlessTableSize(uint32_t wanted, bool driverUnderReportsSupport, const std::function<bool(uint32_t)>& isSupported)
	{
		BindlessTableSize size;
		size.Slots = wanted;
		if (isSupported(wanted))
			return size;
		if (driverUnderReportsSupport)
		{
			size.SupportQueryUnderReports = true;
			return size;
		}
		while (size.Slots > c_MinBindlessTableSlots)
		{
			size.Slots = std::max(size.Slots / 2, c_MinBindlessTableSlots);
			if (isSupported(size.Slots))
				break;
		}
		return size;
	}

}
