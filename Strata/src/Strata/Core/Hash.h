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

		// CRC-32C (Castagnoli polynomial, the checksum of iSCSI, ext4 and SSE 4.2's CRC32 instruction), for detecting
		// corrupted data such as the blocks of asset packs. Chains: CRC32C(b, CRC32C(a)) equals the checksum of a followed by
		// b. Uses the processor's CRC instructions where it has them (x86-64 with SSE 4.2, ARMv8 with the CRC extension);
		// every implementation gives the same results. CRC32C("123456789") is 0xE3069283. Thread-safe.
		static uint32_t CRC32C(std::span<const uint8_t> bytes, uint32_t previous = 0);
		// The portable implementation alone (tests compare it with the accelerated one).
		static uint32_t CRC32CPortable(std::span<const uint8_t> bytes, uint32_t previous = 0);
	};

}
