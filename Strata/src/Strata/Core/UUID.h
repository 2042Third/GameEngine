#pragma once

#include "Strata/Core/Base.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	// 64-bit random identifier used for entities and assets. Zero is reserved as the invalid/null value
	// and is never generated. The canonical text form is 16 upper-case hexadecimal digits.
	class UUID
	{
	public:
		UUID(); // Generates a new random, non-zero UUID
		constexpr UUID(uint64_t uuid)
			: m_UUID(uuid)
		{
		}

		constexpr operator uint64_t() const { return m_UUID; }
		constexpr bool IsValid() const { return m_UUID != 0; }

		std::string ToString() const;
		static std::optional<UUID> FromString(std::string_view text);

		static constexpr UUID Null() { return UUID(uint64_t(0)); }
	private:
		uint64_t m_UUID;
	};

}

template<>
struct std::hash<Strata::UUID>
{
	size_t operator()(const Strata::UUID& uuid) const noexcept
	{
		return std::hash<uint64_t>()(static_cast<uint64_t>(uuid));
	}
};
