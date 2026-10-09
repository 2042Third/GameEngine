#include "stpch.h"
#include "Strata/Core/Base64.h"

#include <array>

namespace Strata
{

	namespace
	{

		constexpr std::string_view c_Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		constexpr uint8_t c_Invalid = 0xFF;

		constexpr std::array<uint8_t, 256> BuildDecodeTable()
		{
			std::array<uint8_t, 256> table = {};
			for (uint8_t& entry : table)
				entry = c_Invalid;
			for (size_t index = 0; index < c_Alphabet.size(); index++)
				table[static_cast<uint8_t>(c_Alphabet[index])] = static_cast<uint8_t>(index);
			return table;
		}

		constexpr std::array<uint8_t, 256> c_DecodeTable = BuildDecodeTable();

	}

	std::string Base64::Encode(std::span<const uint8_t> data)
	{
		std::string text;
		text.reserve(GetEncodedSize(data.size()));
		size_t index = 0;
		for (; index + 3 <= data.size(); index += 3)
		{
			const uint32_t group = (static_cast<uint32_t>(data[index]) << 16) | (static_cast<uint32_t>(data[index + 1]) << 8) | data[index + 2];
			text.push_back(c_Alphabet[(group >> 18) & 0x3F]);
			text.push_back(c_Alphabet[(group >> 12) & 0x3F]);
			text.push_back(c_Alphabet[(group >> 6) & 0x3F]);
			text.push_back(c_Alphabet[group & 0x3F]);
		}

		const size_t remaining = data.size() - index;
		if (remaining == 1)
		{
			const uint32_t group = static_cast<uint32_t>(data[index]) << 16;
			text.push_back(c_Alphabet[(group >> 18) & 0x3F]);
			text.push_back(c_Alphabet[(group >> 12) & 0x3F]);
			text.append("==");
		}
		else if (remaining == 2)
		{
			const uint32_t group = (static_cast<uint32_t>(data[index]) << 16) | (static_cast<uint32_t>(data[index + 1]) << 8);
			text.push_back(c_Alphabet[(group >> 18) & 0x3F]);
			text.push_back(c_Alphabet[(group >> 12) & 0x3F]);
			text.push_back(c_Alphabet[(group >> 6) & 0x3F]);
			text.push_back('=');
		}
		return text;
	}

	std::optional<std::vector<uint8_t>> Base64::Decode(std::string_view text)
	{
		if (text.size() % 4 != 0)
			return std::nullopt;

		std::vector<uint8_t> data;
		data.reserve(text.size() / 4 * 3);
		for (size_t index = 0; index < text.size(); index += 4)
		{
			const bool last = index + 4 == text.size();
			// Padding may only end the final group: "xx==" or "xxx=".
			size_t padding = 0;
			if (last && text[index + 3] == '=')
				padding = text[index + 2] == '=' ? 2 : 1;

			uint32_t group = 0;
			for (size_t offset = 0; offset < 4; offset++)
			{
				uint32_t value = 0;
				if (offset < 4 - padding)
				{
					value = c_DecodeTable[static_cast<uint8_t>(text[index + offset])];
					if (value == c_Invalid)
						return std::nullopt;
				}
				group = (group << 6) | value;
			}

			data.push_back(static_cast<uint8_t>(group >> 16));
			if (padding == 2)
			{
				// The 4 bits after the last byte must be zero.
				if ((group & 0xFFFF) != 0)
					return std::nullopt;
				break;
			}
			data.push_back(static_cast<uint8_t>(group >> 8));
			if (padding == 1)
			{
				if ((group & 0xFF) != 0)
					return std::nullopt;
				break;
			}
			data.push_back(static_cast<uint8_t>(group));
		}
		return data;
	}

}
