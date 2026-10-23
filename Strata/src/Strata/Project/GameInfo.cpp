#include "stpch.h"
#include "Strata/Project/GameInfo.h"

#include "Strata/Core/StringUtils.h"

#include <algorithm>

namespace Strata
{

	std::optional<std::string> ProjectGameInfo::Validate() const
	{
		if (std::optional<std::string> problem = ValidateText(Title))
			return fmt::format("Title: {}", *problem);
		if (std::optional<std::string> problem = ValidateVersion(Version))
			return fmt::format("Version: {}", *problem);
		if (std::optional<std::string> problem = ValidateText(Company))
			return fmt::format("Company: {}", *problem);
		if (std::optional<std::string> problem = ValidateText(Copyright))
			return fmt::format("Copyright: {}", *problem);
		return std::nullopt;
	}

	std::optional<std::string> ProjectGameInfo::ValidateText(std::string_view text)
	{
		if (StringUtils::MakeValidUTF8(text) != text)
			return std::string("it is not valid UTF-8 text");
		size_t characters = 0;
		for (size_t index = 0; index < text.size(); index++)
		{
			const auto byte = static_cast<unsigned char>(text[index]);
			// Continuation bytes belong to the character their sequence started.
			if ((byte & 0xC0) == 0x80)
				continue;
			characters++;
			// C0 controls and DEL, and the C1 controls (U+0080 to U+009F, encoded as 0xC2 0x80 to 0xC2 0x9F).
			const bool c1 = byte == 0xC2 && index + 1 < text.size() && static_cast<unsigned char>(text[index + 1]) <= 0x9F;
			if (byte < 0x20 || byte == 0x7F || c1)
				return std::string("it contains control characters (line breaks, tabs)");
		}
		if (characters > c_MaxTextLength)
			return fmt::format("it has {} characters, more than the {} allowed", characters, c_MaxTextLength);
		return std::nullopt;
	}

	std::optional<std::string> ProjectGameInfo::ValidateVersion(std::string_view version)
	{
		if (ParseVersion(version))
			return std::nullopt;
		return fmt::format("'{}' is not a version: give one to {} whole numbers from 0 to {} separated by dots, such as \"1.0.0\"",
			StringUtils::MakeValidUTF8(version.substr(0, 32)), c_MaxVersionParts, c_MaxVersionPart);
	}

	std::optional<std::array<uint16_t, ProjectGameInfo::c_MaxVersionParts>> ProjectGameInfo::ParseVersion(std::string_view version)
	{
		std::array<uint16_t, c_MaxVersionParts> parts = {};
		size_t count = 0;
		size_t start = 0;
		while (true)
		{
			const size_t end = std::min(version.find('.', start), version.size());
			const std::string_view part = version.substr(start, end - start);
			// At most five digits (65535), nothing else: no signs, spaces or empty parts.
			if (part.empty() || part.size() > 5 || count == c_MaxVersionParts)
				return std::nullopt;
			uint32_t value = 0;
			for (const char character : part)
			{
				if (character < '0' || character > '9')
					return std::nullopt;
				value = value * 10 + static_cast<uint32_t>(character - '0');
			}
			if (value > c_MaxVersionPart)
				return std::nullopt;
			parts[count++] = static_cast<uint16_t>(value);
			if (end == version.size())
				return parts;
			start = end + 1;
		}
	}

}
