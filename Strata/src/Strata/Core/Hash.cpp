#include "stpch.h"
#include "Strata/Core/Hash.h"

#include <array>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#define ST_HASH_CRC32C_X86 1
#include <nmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#define ST_HASH_CRC32C_ARM 1
#include <arm_acle.h>
#endif

namespace Strata
{

	namespace
	{

		// The reflected Castagnoli polynomial.
		constexpr uint32_t c_CRC32CPolynomial = 0x82F63B78u;

		// Slicing-by-8 tables: Table[0] is the classic byte table; Table[k][b] advances the checksum of byte b by k more zero
		// bytes, so eight bytes are folded in with eight lookups.
		using CRCTables = std::array<std::array<uint32_t, 256>, 8>;

		constexpr CRCTables MakeCRCTables()
		{
			CRCTables tables {};
			for (uint32_t byte = 0; byte < 256; byte++)
			{
				uint32_t crc = byte;
				for (int bit = 0; bit < 8; bit++)
					crc = (crc & 1u) ? (crc >> 1) ^ c_CRC32CPolynomial : crc >> 1;
				tables[0][byte] = crc;
			}
			for (uint32_t byte = 0; byte < 256; byte++)
			{
				for (size_t slice = 1; slice < 8; slice++)
				{
					const uint32_t previous = tables[slice - 1][byte];
					tables[slice][byte] = (previous >> 8) ^ tables[0][previous & 0xFFu];
				}
			}
			return tables;
		}

		constexpr CRCTables c_CRCTables = MakeCRCTables();

		// Works on the inverted checksum (the register of the CRC computation).
		uint32_t UpdatePortable(uint32_t crc, const uint8_t* data, size_t size)
		{
			while (size >= 8)
			{
				uint32_t low = 0;
				uint32_t high = 0;
				std::memcpy(&low, data, 4);
				std::memcpy(&high, data + 4, 4);
				low ^= crc; // Little endian: the first byte is the lowest
				crc = c_CRCTables[7][low & 0xFFu] ^ c_CRCTables[6][(low >> 8) & 0xFFu] ^ c_CRCTables[5][(low >> 16) & 0xFFu]
					^ c_CRCTables[4][low >> 24] ^ c_CRCTables[3][high & 0xFFu] ^ c_CRCTables[2][(high >> 8) & 0xFFu]
					^ c_CRCTables[1][(high >> 16) & 0xFFu] ^ c_CRCTables[0][high >> 24];
				data += 8;
				size -= 8;
			}
			while (size > 0)
			{
				crc = (crc >> 8) ^ c_CRCTables[0][(crc ^ *data) & 0xFFu];
				data++;
				size--;
			}
			return crc;
		}

#if defined(ST_HASH_CRC32C_X86)
#if defined(__GNUC__) || defined(__clang__)
		// GCC and Clang compile the SSE 4.2 intrinsics only in functions that target it; the caller checks the processor.
		__attribute__((target("sse4.2")))
#endif
		uint32_t UpdateHardware(uint32_t crc, const uint8_t* data, size_t size)
		{
			uint64_t state = crc;
			while (size >= 8)
			{
				uint64_t word = 0;
				std::memcpy(&word, data, 8);
				state = _mm_crc32_u64(state, word);
				data += 8;
				size -= 8;
			}
			uint32_t result = static_cast<uint32_t>(state);
			while (size > 0)
			{
				result = _mm_crc32_u8(result, *data);
				data++;
				size--;
			}
			return result;
		}

		bool HasHardwareCRC()
		{
			// SSE 4.2 is CPUID leaf 1, ECX bit 20.
#if defined(_MSC_VER)
			int registers[4] = {};
			__cpuid(registers, 1);
			return (registers[2] & (1 << 20)) != 0;
#else
			unsigned int eax = 0;
			unsigned int ebx = 0;
			unsigned int ecx = 0;
			unsigned int edx = 0;
			return __get_cpuid(1, &eax, &ebx, &ecx, &edx) != 0 && (ecx & (1u << 20)) != 0;
#endif
		}
#elif defined(ST_HASH_CRC32C_ARM)
		uint32_t UpdateHardware(uint32_t crc, const uint8_t* data, size_t size)
		{
			while (size >= 8)
			{
				uint64_t word = 0;
				std::memcpy(&word, data, 8);
				crc = __crc32cd(crc, word);
				data += 8;
				size -= 8;
			}
			while (size > 0)
			{
				crc = __crc32cb(crc, *data);
				data++;
				size--;
			}
			return crc;
		}

		bool HasHardwareCRC()
		{
			return true; // The compiler targets processors with the CRC extension (__ARM_FEATURE_CRC32)
		}
#endif

	}

	uint32_t Hash::CRC32CPortable(std::span<const uint8_t> bytes, uint32_t previous)
	{
		return ~UpdatePortable(~previous, bytes.data(), bytes.size());
	}

	uint32_t Hash::CRC32C(std::span<const uint8_t> bytes, uint32_t previous)
	{
#if defined(ST_HASH_CRC32C_X86) || defined(ST_HASH_CRC32C_ARM)
		static const bool s_Hardware = HasHardwareCRC();
		if (s_Hardware)
			return ~UpdateHardware(~previous, bytes.data(), bytes.size());
#endif
		return CRC32CPortable(bytes, previous);
	}

}
