#include "stpch.h"
#include "Strata/Core/ErrorThrottle.h"

#include "Strata/Core/Log.h"

#include <utility>

namespace Strata
{

	ErrorThrottle::ErrorThrottle(std::string source)
		: m_Source(std::move(source))
	{
	}

	bool ErrorThrottle::Report(const std::string& message)
	{
		if (message == m_LastMessage)
			return false;
		m_LastMessage = message;
		ST_CORE_ERROR("{}: {}", m_Source, message);
		return true;
	}

}
