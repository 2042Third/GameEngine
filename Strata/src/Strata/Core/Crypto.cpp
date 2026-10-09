#include "stpch.h"
#include "Strata/Core/Crypto.h"

namespace Strata
{

	namespace
	{

		// FIPS 180-4, section 4.2.2: the first 32 bits of the fractional parts of the cube roots of the first 64 primes.
		constexpr std::array<uint32_t, 64> c_RoundConstants = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
		};

		// FIPS 180-4, section 5.3.3: the first 32 bits of the fractional parts of the square roots of the first 8 primes.
		constexpr std::array<uint32_t, 8> c_InitialState = {
			0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
		};

		constexpr size_t c_BlockSize = 64;

		constexpr uint32_t RotateRight(uint32_t value, int count)
		{
			return (value >> count) | (value << (32 - count));
		}

		std::span<const uint8_t> AsBytes(std::string_view text)
		{
			return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// Sha256
	////////////////////////////////////////////////////////////////////////////////

	Sha256::Sha256()
	{
		Reset();
	}

	void Sha256::Reset()
	{
		m_State = c_InitialState;
		m_Block.fill(0);
		m_BlockSize = 0;
		m_TotalBytes = 0;
	}

	void Sha256::Update(std::span<const uint8_t> data)
	{
		m_TotalBytes += data.size();
		size_t offset = 0;
		while (offset < data.size())
		{
			const size_t count = std::min(c_BlockSize - m_BlockSize, data.size() - offset);
			std::memcpy(m_Block.data() + m_BlockSize, data.data() + offset, count);
			m_BlockSize += count;
			offset += count;
			if (m_BlockSize == c_BlockSize)
			{
				ProcessBlock(m_Block.data());
				m_BlockSize = 0;
			}
		}
	}

	void Sha256::Update(std::string_view data)
	{
		Update(AsBytes(data));
	}

	Sha256Digest Sha256::Finalize()
	{
		// Padding (section 5.1.1): a 1 bit, zeros up to 56 bytes into a block, then the message length in bits.
		const uint64_t lengthInBits = m_TotalBytes * 8;
		m_Block[m_BlockSize++] = 0x80;
		if (m_BlockSize > c_BlockSize - 8)
		{
			std::fill(m_Block.begin() + static_cast<std::ptrdiff_t>(m_BlockSize), m_Block.end(), uint8_t(0));
			ProcessBlock(m_Block.data());
			m_BlockSize = 0;
		}
		std::fill(m_Block.begin() + static_cast<std::ptrdiff_t>(m_BlockSize), m_Block.end() - 8, uint8_t(0));
		for (int index = 0; index < 8; index++)
			m_Block[c_BlockSize - 1 - static_cast<size_t>(index)] = static_cast<uint8_t>(lengthInBits >> (8 * index));
		ProcessBlock(m_Block.data());

		Sha256Digest digest = {};
		for (size_t word = 0; word < m_State.size(); word++)
		{
			for (size_t byte = 0; byte < 4; byte++)
				digest[word * 4 + byte] = static_cast<uint8_t>(m_State[word] >> (24 - 8 * byte));
		}
		Reset();
		return digest;
	}

	Sha256Digest Sha256::Hash(std::span<const uint8_t> data)
	{
		Sha256 hash;
		hash.Update(data);
		return hash.Finalize();
	}

	Sha256Digest Sha256::Hash(std::string_view data)
	{
		return Hash(AsBytes(data));
	}

	void Sha256::ProcessBlock(const uint8_t* block)
	{
		// Message schedule (section 6.2.2, step 1).
		std::array<uint32_t, 64> schedule = {};
		for (size_t index = 0; index < 16; index++)
		{
			schedule[index] = (static_cast<uint32_t>(block[index * 4]) << 24) | (static_cast<uint32_t>(block[index * 4 + 1]) << 16)
				| (static_cast<uint32_t>(block[index * 4 + 2]) << 8) | static_cast<uint32_t>(block[index * 4 + 3]);
		}
		for (size_t index = 16; index < 64; index++)
		{
			const uint32_t sigma0 = RotateRight(schedule[index - 15], 7) ^ RotateRight(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3);
			const uint32_t sigma1 = RotateRight(schedule[index - 2], 17) ^ RotateRight(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10);
			schedule[index] = sigma1 + schedule[index - 7] + sigma0 + schedule[index - 16];
		}

		// Compression (steps 2 to 4).
		uint32_t a = m_State[0];
		uint32_t b = m_State[1];
		uint32_t c = m_State[2];
		uint32_t d = m_State[3];
		uint32_t e = m_State[4];
		uint32_t f = m_State[5];
		uint32_t g = m_State[6];
		uint32_t h = m_State[7];
		for (size_t index = 0; index < 64; index++)
		{
			const uint32_t upperSigma1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
			const uint32_t choose = (e & f) ^ (~e & g);
			const uint32_t temporary1 = h + upperSigma1 + choose + c_RoundConstants[index] + schedule[index];
			const uint32_t upperSigma0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
			const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t temporary2 = upperSigma0 + majority;
			h = g;
			g = f;
			f = e;
			e = d + temporary1;
			d = c;
			c = b;
			b = a;
			a = temporary1 + temporary2;
		}

		m_State[0] += a;
		m_State[1] += b;
		m_State[2] += c;
		m_State[3] += d;
		m_State[4] += e;
		m_State[5] += f;
		m_State[6] += g;
		m_State[7] += h;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Crypto
	////////////////////////////////////////////////////////////////////////////////

	Sha256Digest Crypto::HmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> message)
	{
		// Keys longer than a block are hashed first; shorter ones are zero-padded to the block size.
		std::array<uint8_t, c_BlockSize> blockKey = {};
		if (key.size() > c_BlockSize)
		{
			const Sha256Digest hashedKey = Sha256::Hash(key);
			std::copy(hashedKey.begin(), hashedKey.end(), blockKey.begin());
		}
		else if (!key.empty())
		{
			std::copy(key.begin(), key.end(), blockKey.begin());
		}

		std::array<uint8_t, c_BlockSize> innerPad = {};
		std::array<uint8_t, c_BlockSize> outerPad = {};
		for (size_t index = 0; index < c_BlockSize; index++)
		{
			innerPad[index] = static_cast<uint8_t>(blockKey[index] ^ 0x36);
			outerPad[index] = static_cast<uint8_t>(blockKey[index] ^ 0x5c);
		}

		Sha256 inner;
		inner.Update(innerPad);
		inner.Update(message);
		const Sha256Digest innerDigest = inner.Finalize();

		Sha256 outer;
		outer.Update(outerPad);
		outer.Update(innerDigest);
		return outer.Finalize();
	}

	Sha256Digest Crypto::HmacSha256(std::string_view key, std::string_view message)
	{
		return HmacSha256(AsBytes(key), AsBytes(message));
	}

	bool Crypto::ConstantTimeEquals(std::span<const uint8_t> left, std::span<const uint8_t> right)
	{
		if (left.size() != right.size())
			return false;

		uint8_t difference = 0;
		for (size_t index = 0; index < left.size(); index++)
			difference = static_cast<uint8_t>(difference | (left[index] ^ right[index]));
		return difference == 0;
	}

	bool Crypto::ConstantTimeEquals(std::string_view left, std::string_view right)
	{
		return ConstantTimeEquals(AsBytes(left), AsBytes(right));
	}

	std::string Crypto::ToHex(std::span<const uint8_t> bytes)
	{
		constexpr std::string_view c_Digits = "0123456789abcdef";
		std::string text;
		text.reserve(bytes.size() * 2);
		for (const uint8_t byte : bytes)
		{
			text.push_back(c_Digits[byte >> 4]);
			text.push_back(c_Digits[byte & 0x0f]);
		}
		return text;
	}

}
