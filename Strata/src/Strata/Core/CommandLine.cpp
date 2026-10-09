#include "stpch.h"
#include "Strata/Core/CommandLine.h"

#include <charconv>

namespace Strata
{

	CommandLine::CommandLine(int argc, char** argv)
	{
		m_Arguments.reserve(static_cast<size_t>(argc));
		for (int index = 0; index < argc; index++)
			m_Arguments.emplace_back(argv[index] ? argv[index] : "");
	}

	CommandLine::CommandLine(std::vector<std::string> arguments)
		: m_Arguments(std::move(arguments))
	{
	}

	bool CommandLine::HasFlag(std::string_view name) const
	{
		for (size_t index = 1; index < m_Arguments.size(); index++)
		{
			const std::string& argument = m_Arguments[index];
			if (argument == name)
				return true;
			if (argument.size() > name.size() && argument.compare(0, name.size(), name) == 0 && argument[name.size()] == '=')
				return true;
		}
		return false;
	}

	std::optional<std::string> CommandLine::GetOption(std::string_view name) const
	{
		for (size_t index = 1; index < m_Arguments.size(); index++)
		{
			const std::string& argument = m_Arguments[index];
			if (argument == name)
			{
				if (index + 1 < m_Arguments.size())
					return m_Arguments[index + 1];
				return std::nullopt;
			}

			if (argument.size() > name.size() && argument.compare(0, name.size(), name) == 0 && argument[name.size()] == '=')
				return argument.substr(name.size() + 1);
		}
		return std::nullopt;
	}

	std::optional<int64_t> CommandLine::GetIntOption(std::string_view name) const
	{
		const std::optional<std::string> text = GetOption(name);
		if (!text)
			return std::nullopt;

		int64_t value = 0;
		const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
		if (error != std::errc() || end != text->data() + text->size())
			return std::nullopt;
		return value;
	}

}
