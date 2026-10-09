#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Strata
{

	// Stable, platform-independent hashing (FNV-1a, 64-bit). Suitable for identifiers persisted to
	// disk, such as deterministic sub-asset handles. Not cryptographic.
	class Hash
	{
	public:
		static constexpr uint64_t FNVOffsetBasis = 14695981039346656037ull;
		static constexpr uint64_t FNVPrime = 1099511628211ull;

		static constexpr uint64_t FNV1a(std::string_view text, uint64_t seed = FNVOffsetBasis)
		{
			uint64_t hash = seed;
			for (char character : text)
			{
				hash ^= static_cast<uint8_t>(character);
				hash *= FNVPrime;
			}
			return hash;
		}

		static constexpr uint64_t FNV1a(std::span<const uint8_t> bytes, uint64_t seed = FNVOffsetBasis)
		{
			uint64_t hash = seed;
			for (uint8_t byte : bytes)
			{
				hash ^= byte;
				hash *= FNVPrime;
			}
			return hash;
		}

		static constexpr uint64_t Combine(uint64_t seed, uint64_t value)
		{
			// 64-bit variant of boost::hash_combine with a stronger mix.
			value *= 0xff51afd7ed558ccdull;
			value ^= value >> 33;
			return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
		}
	};

}
