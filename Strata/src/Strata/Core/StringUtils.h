#pragma once

#include <cctype>
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

}
