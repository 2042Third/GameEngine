#include <doctest/doctest.h>

#include "Strata/Core/Crypto.h"

#include <string>
#include <vector>

using namespace Strata;

namespace
{
	std::string HexOf(const Sha256Digest& digest)
	{
		return Crypto::ToHex(digest);
	}

	std::vector<uint8_t> Repeat(uint8_t value, size_t count)
	{
		return std::vector<uint8_t>(count, value);
	}

	std::string Hmac(std::span<const uint8_t> key, std::span<const uint8_t> message)
	{
		return HexOf(Crypto::HmacSha256(key, message));
	}

	std::span<const uint8_t> Bytes(std::string_view text)
	{
		return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
	}
}

TEST_SUITE("Core.Crypto")
{
	TEST_CASE("SHA-256 matches the FIPS 180-4 examples")
	{
		CHECK(HexOf(Sha256::Hash(std::string_view("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
		CHECK(HexOf(Sha256::Hash(std::string_view(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
		CHECK(HexOf(Sha256::Hash(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))
			== "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
		CHECK(HexOf(Sha256::Hash(std::string_view("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")))
			== "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
		CHECK(HexOf(Sha256::Hash(std::string(1000000, 'a'))) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
	}

	TEST_CASE("Incremental hashing equals one-shot hashing at every split")
	{
		std::string message;
		for (int index = 0; index < 200; index++)
			message.push_back(static_cast<char>('a' + index % 26));

		// Splits around the 55/56/64-byte padding boundaries exercise every Finalize path.
		for (size_t length : { size_t(0), size_t(1), size_t(55), size_t(56), size_t(63), size_t(64), size_t(65), size_t(119), size_t(120), size_t(200) })
		{
			const std::string_view prefix(message.data(), length);
			const Sha256Digest expected = Sha256::Hash(prefix);
			for (size_t split = 0; split <= length; split += 7)
			{
				Sha256 hash;
				hash.Update(prefix.substr(0, split));
				hash.Update(prefix.substr(split));
				CHECK(hash.Finalize() == expected);
			}
		}

		// Finalize resets the object for the next hash.
		Sha256 reused;
		reused.Update(std::string_view("garbage"));
		reused.Finalize();
		reused.Update(std::string_view("abc"));
		CHECK(HexOf(reused.Finalize()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	}

	TEST_CASE("HMAC-SHA256 matches the RFC 4231 test cases")
	{
		CHECK(Hmac(Repeat(0x0b, 20), Bytes("Hi There")) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
		CHECK(Hmac(Bytes("Jefe"), Bytes("what do ya want for nothing?")) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
		CHECK(Hmac(Repeat(0xaa, 20), Repeat(0xdd, 50)) == "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

		std::vector<uint8_t> incrementingKey;
		for (uint8_t value = 0x01; value <= 0x19; value++)
			incrementingKey.push_back(value);
		CHECK(Hmac(incrementingKey, Repeat(0xcd, 50)) == "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");

		// Test case 5 specifies the output truncated to 128 bits.
		CHECK(Hmac(Repeat(0x0c, 20), Bytes("Test With Truncation")).substr(0, 32) == "a3b6167473100ee06e0c796c2955552b");

		// Keys longer than the block size are hashed first.
		CHECK(Hmac(Repeat(0xaa, 131), Bytes("Test Using Larger Than Block-Size Key - Hash Key First"))
			== "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
		CHECK(Hmac(Repeat(0xaa, 131), Bytes("This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm."))
			== "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");

		CHECK(Crypto::HmacSha256(std::string_view("Jefe"), std::string_view("what do ya want for nothing?")) == Crypto::HmacSha256(Bytes("Jefe"), Bytes("what do ya want for nothing?")));
	}

	TEST_CASE("Constant-time comparison and hexadecimal output")
	{
		CHECK(Crypto::ConstantTimeEquals(std::string_view("secret"), std::string_view("secret")));
		CHECK_FALSE(Crypto::ConstantTimeEquals(std::string_view("secret"), std::string_view("secreT")));
		CHECK_FALSE(Crypto::ConstantTimeEquals(std::string_view("secret"), std::string_view("secrets")));
		CHECK(Crypto::ConstantTimeEquals(std::string_view(""), std::string_view("")));

		const std::vector<uint8_t> bytes = { 0x00, 0x0f, 0xa5, 0xff };
		CHECK(Crypto::ToHex(bytes) == "000fa5ff");
		CHECK(Crypto::ToHex(std::span<const uint8_t>()).empty());
	}
}
