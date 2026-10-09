#include <doctest/doctest.h>

#include "Strata/Core/Base64.h"

#include <ostream> // doctest prints captured string_views with operator<<
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strata;

namespace
{

	std::span<const uint8_t> Bytes(std::string_view text)
	{
		return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
	}

	std::string DecodeText(std::string_view text)
	{
		const std::optional<std::vector<uint8_t>> data = Base64::Decode(text);
		REQUIRE(data);
		return std::string(data->begin(), data->end());
	}

}

TEST_SUITE("Core.Base64")
{
	TEST_CASE("Encoding and decoding match the RFC 4648 test vectors")
	{
		const std::pair<std::string_view, std::string_view> vectors[] = {
			{ "", "" },
			{ "f", "Zg==" },
			{ "fo", "Zm8=" },
			{ "foo", "Zm9v" },
			{ "foob", "Zm9vYg==" },
			{ "fooba", "Zm9vYmE=" },
			{ "foobar", "Zm9vYmFy" }
		};
		for (const auto& [plain, encoded] : vectors)
		{
			CAPTURE(plain);
			CHECK(Base64::Encode(Bytes(plain)) == encoded);
			CHECK(Base64::GetEncodedSize(plain.size()) == encoded.size());
			CHECK(DecodeText(encoded) == plain);
		}
	}

	TEST_CASE("Every byte value round trips at every length")
	{
		std::vector<uint8_t> data;
		for (int value = 0; value < 256; value++)
			data.push_back(static_cast<uint8_t>(255 - value));
		for (size_t length = 0; length <= data.size(); length++)
		{
			const std::span<const uint8_t> prefix(data.data(), length);
			const std::string encoded = Base64::Encode(prefix);
			CHECK(encoded.size() == Base64::GetEncodedSize(length));
			const std::optional<std::vector<uint8_t>> decoded = Base64::Decode(encoded);
			REQUIRE(decoded);
			CHECK(std::vector<uint8_t>(prefix.begin(), prefix.end()) == *decoded);
		}
		// Both characters beyond the letters and digits appear.
		CHECK(Base64::Encode(std::vector<uint8_t> { 0xFB, 0xFF }) == "+/8=");
	}

	TEST_CASE("Malformed text is rejected")
	{
		const std::string_view invalid[] = {
			"Zg=",       // Length not a multiple of four
			"Zg",
			"Z===",      // Too much padding
			"====",
			"Zg==Zg==",  // Padding before the end
			"Zm9v Yg==", // Whitespace
			"Zm9vYg=a",
			"Zm9vYm*y",  // Outside the alphabet
			"Zh==",      // Non-zero bits in the padding
			"Zm9=",
			"Zm-v",      // URL-safe alphabet
			"Zm_v"
		};
		for (std::string_view text : invalid)
		{
			CAPTURE(text);
			CHECK_FALSE(Base64::Decode(text));
		}
		CHECK(Base64::Decode("")->empty());
	}
}
