#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// Base64 with the standard alphabet and '=' padding (RFC 4648, section 4), e.g. for binary data inside JSON.
	class Base64
	{
	public:
		static std::string Encode(std::span<const uint8_t> data);
		// Strict: rejects characters outside the alphabet (including whitespace), missing or misplaced padding and
		// non-zero bits in the padding (so every byte sequence has exactly one accepted encoding).
		static std::optional<std::vector<uint8_t>> Decode(std::string_view text);
		// Length of the encoding of `size` bytes.
		static size_t GetEncodedSize(size_t size) { return (size + 2) / 3 * 4; }
	};

}
