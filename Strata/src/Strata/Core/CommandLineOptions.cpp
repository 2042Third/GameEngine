#include "stpch.h"
#include "Strata/Core/CommandLineOptions.h"

#include <spdlog/fmt/fmt.h>

namespace Strata
{

	namespace CommandLineOptions
	{

		std::optional<std::string> FindUnknownArgument(const CommandLine& commandLine, std::span<const CommandLineOption> options)
		{
			for (size_t index = 1; index < commandLine.GetCount(); index++)
			{
				const std::string& text = commandLine[index];
				const size_t equals = text.find('=');
				const std::string_view name = std::string_view(text).substr(0, equals);
				const CommandLineOption* option = nullptr;
				for (const CommandLineOption& candidate : options)
				{
					if (name == candidate.Name)
						option = &candidate;
				}
				if (!option)
					return fmt::format("Unknown argument '{}'", text);
				if (!option->Value && equals != std::string::npos)
					return fmt::format("{} takes no value", option->Name);
				if (option->Value && equals == std::string::npos)
				{
					if (index + 1 >= commandLine.GetCount())
						return fmt::format("{} needs a value: {} {}", option->Name, option->Name, option->Value);
					index++; // Its value
				}
			}
			return std::nullopt;
		}

		std::string FormatUsage(std::string_view program, std::span<const CommandLineOption> options)
		{
			std::string usage = fmt::format("Usage: {} [options]\n\nOptions:\n", program);
			for (const CommandLineOption& option : options)
			{
				const std::string name = option.Value ? fmt::format("{} {}", option.Name, option.Value) : std::string(option.Name);
				usage += fmt::format("  {:<36} {}\n", name, option.Description);
			}
			return usage;
		}

	}

}
