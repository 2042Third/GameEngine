#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Strata::StringUtils
{

	// ASCII case conversion for identifiers and file extensions; bytes outside ASCII (UTF-8 sequences) are unchanged.
	inline std::string ToLower(std::string_view text)
	{
		std::string result(text);
		for (char& character : result)
			character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		return result;
	}

	inline bool EqualsIgnoreCase(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t index = 0; index < a.size(); index++)
		{
			if (std::tolower(static_cast<unsigned char>(a[index])) != std::tolower(static_cast<unsigned char>(b[index])))
				return false;
		}
		return true;
	}

	inline bool StartsWith(std::string_view text, std::string_view prefix)
	{
		return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
	}

	inline bool EndsWith(std::string_view text, std::string_view suffix)
	{
		return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
	}

	// The text with every byte sequence that is no valid UTF-8 (stray continuation bytes, truncated or overlong sequences,
	// surrogates, code points past U+10FFFF) replaced by U+FFFD, so that messages from third-party code (an image decoder
	// quoting the bytes of a file) can go into logs, JSON and the UI. Valid text comes back unchanged.
	inline std::string MakeValidUTF8(std::string_view text)
	{
		constexpr std::string_view c_Replacement = "\xEF\xBF\xBD";
		std::string result;
		result.reserve(text.size());
		size_t index = 0;
		while (index < text.size())
		{
			const unsigned char lead = static_cast<unsigned char>(text[index]);
			size_t length = 0;
			uint32_t codePoint = 0;
			if (lead < 0x80)
			{
				length = 1;
				codePoint = lead;
			}
			else if ((lead & 0xE0) == 0xC0)
			{
				length = 2;
				codePoint = lead & 0x1F;
			}
			else if ((lead & 0xF0) == 0xE0)
			{
				length = 3;
				codePoint = lead & 0x0F;
			}
			else if ((lead & 0xF8) == 0xF0)
			{
				length = 4;
				codePoint = lead & 0x07;
			}

			bool valid = length != 0 && index + length <= text.size();
			for (size_t offset = 1; valid && offset < length; offset++)
			{
				const unsigned char continuation = static_cast<unsigned char>(text[index + offset]);
				valid = (continuation & 0xC0) == 0x80;
				codePoint = (codePoint << 6) | (continuation & 0x3F);
			}
			// Overlong forms, UTF-16 surrogates and values past the last code point are no characters.
			constexpr uint32_t c_MinimumByLength[] = { 0, 0, 0x80, 0x800, 0x10000 };
			valid = valid && codePoint >= c_MinimumByLength[length] && !(codePoint >= 0xD800 && codePoint <= 0xDFFF) && codePoint <= 0x10FFFF;
			if (valid)
			{
				result.append(text.substr(index, length));
				index += length;
			}
			else
			{
				result.append(c_Replacement);
				index++;
			}
		}
		return result;
	}

}
