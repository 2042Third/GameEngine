#include "stpch.h"
#include "Strata/Core/UUID.h"

#include <random>

namespace Strata
{

	static uint64_t GenerateRandomUUID()
	{
		thread_local std::mt19937_64 s_Engine = []
		{
			std::random_device device;
			std::seed_seq seed { device(), device(), device(), device(), device(), device(), device(), device() };
			return std::mt19937_64(seed);
		}();
		thread_local std::uniform_int_distribution<uint64_t> s_Distribution(1, UINT64_MAX);
		return s_Distribution(s_Engine);
	}

	UUID::UUID()
		: m_UUID(GenerateRandomUUID())
	{
	}

	std::string UUID::ToString() const
	{
		return fmt::format("{:016X}", m_UUID);
	}

	std::optional<UUID> UUID::FromString(std::string_view text)
	{
		if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
			text.remove_prefix(2);

		if (text.empty() || text.size() > 16)
			return std::nullopt;

		uint64_t value = 0;
		for (char character : text)
		{
			uint64_t digit = 0;
			if (character >= '0' && character <= '9')
				digit = static_cast<uint64_t>(character - '0');
			else if (character >= 'a' && character <= 'f')
				digit = static_cast<uint64_t>(character - 'a' + 10);
			else if (character >= 'A' && character <= 'F')
				digit = static_cast<uint64_t>(character - 'A' + 10);
			else
				return std::nullopt;

			value = (value << 4) | digit;
		}
		return UUID(value);
	}

}
