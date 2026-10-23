#include "Editor/DesktopEntry.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/StringUtils.h>

#include <spdlog/fmt/fmt.h>

#include <utility>

namespace Strata
{

	namespace
	{

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

		bool CheckValue(std::string_view key, std::string_view value, std::string* outError)
		{
			if (value.empty())
				return Fail(outError, fmt::format("its {} is empty", key));
			if (StringUtils::MakeValidUTF8(value) != value)
				return Fail(outError, fmt::format("its {} is not valid UTF-8", key));
			return true;
		}

	}

	std::string DesktopEntry::EscapeString(std::string_view value)
	{
		// Readers trim the spaces around '=' and at the end of a line: spaces at either end of the value survive as \s.
		size_t first = 0;
		while (first < value.size() && value[first] == ' ')
			first++;
		size_t last = value.size();
		while (last > first && value[last - 1] == ' ')
			last--;

		std::string escaped;
		escaped.reserve(value.size() + 8);
		for (size_t index = 0; index < value.size(); index++)
		{
			const char character = value[index];
			if (character == ' ' && (index < first || index >= last))
				escaped += "\\s";
			else if (character == '\\')
				escaped += "\\\\";
			else if (character == '\n')
				escaped += "\\n";
			else if (character == '\t')
				escaped += "\\t";
			else if (character == '\r')
				escaped += "\\r";
			else
				escaped += character;
		}
		return escaped;
	}

	std::string DesktopEntry::QuoteArgument(std::string_view argument)
	{
		std::string quoted = "\"";
		for (const char character : argument)
		{
			if (character == '"' || character == '`' || character == '$' || character == '\\')
				quoted += '\\';
			// Field codes (%f, %u...) are expanded after the quotes are undone: a literal % is %% even in quotes.
			if (character == '%')
				quoted += '%';
			quoted += character;
		}
		quoted += '"';
		return quoted;
	}

	std::optional<std::string> DesktopEntry::ToText(std::string* outError) const
	{
		const std::string executable = FileSystem::ToUTF8(Executable);
		const std::string icon = FileSystem::ToUTF8(Icon);
		if (!CheckValue("name", Name, outError) || !CheckValue("executable", executable, outError) || !CheckValue("window class", WindowClass, outError))
			return std::nullopt;
		if (!Executable.is_absolute())
		{
			Fail(outError, fmt::format("its executable '{}' is not an absolute path", executable));
			return std::nullopt;
		}
		if (!Icon.empty())
		{
			if (!CheckValue("icon", icon, outError))
				return std::nullopt;
			if (!Icon.is_absolute())
			{
				Fail(outError, fmt::format("its icon '{}' is not an absolute path", icon));
				return std::nullopt;
			}
		}

		std::string text = "[Desktop Entry]\n";
		text += "Type=Application\n";
		text += "Name=" + EscapeString(Name) + "\n";
		text += "Exec=" + EscapeString(QuoteArgument(executable)) + "\n";
		if (!Icon.empty())
			text += "Icon=" + EscapeString(icon) + "\n";
		text += "StartupWMClass=" + EscapeString(WindowClass) + "\n";
		text += "Terminal=false\n";
		text += "Categories=Game;\n";
		return text;
	}

}
