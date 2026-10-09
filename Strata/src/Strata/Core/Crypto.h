#pragma once

#include "Strata/Core/Base.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Strata
{

	using Sha256Digest = std::array<uint8_t, 32>;

	// SHA-256 (FIPS 180-4). Feed data with Update; Finalize returns the digest and resets for a new hash.
	class Sha256
	{
	public:
		Sha256();

		void Reset();
		void Update(std::span<const uint8_t> data);
		void Update(std::string_view data);
		Sha256Digest Finalize();

		static Sha256Digest Hash(std::span<const uint8_t> data);
		static Sha256Digest Hash(std::string_view data);
	private:
		void ProcessBlock(const uint8_t* block);
	private:
		std::array<uint32_t, 8> m_State = {};
		std::array<uint8_t, 64> m_Block = {};
		size_t m_BlockSize = 0;
		uint64_t m_TotalBytes = 0;
	};

	// Cryptographic helpers for authenticating peers with a shared secret.
	class Crypto
	{
	public:
		// HMAC-SHA256 (RFC 2104 / RFC 4231).
		static Sha256Digest HmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> message);
		static Sha256Digest HmacSha256(std::string_view key, std::string_view message);

		// Compares without an early exit on the first difference, so the time taken does not reveal how much of a
		// secret matched. Inputs of different lengths are unequal (lengths are not considered secret).
		static bool ConstantTimeEquals(std::span<const uint8_t> left, std::span<const uint8_t> right);
		static bool ConstantTimeEquals(std::string_view left, std::string_view right);

		// Lower-case hexadecimal.
		static std::string ToHex(std::span<const uint8_t> bytes);
	};

}
