#pragma once

#include "Strata/Core/Base.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// Command line arguments (UTF-8). Options use the forms "--name value", "--name=value" and "--flag".
	class CommandLine
	{
	public:
		CommandLine() = default;
		CommandLine(int argc, char** argv);
		explicit CommandLine(std::vector<std::string> arguments);

		const std::vector<std::string>& GetArguments() const { return m_Arguments; }
		size_t GetCount() const { return m_Arguments.size(); }
		const std::string& operator[](size_t index) const { return m_Arguments[index]; }

		bool HasFlag(std::string_view name) const;
		std::optional<std::string> GetOption(std::string_view name) const;
		std::optional<int64_t> GetIntOption(std::string_view name) const;
	private:
		std::vector<std::string> m_Arguments; // Includes the program name at index 0
	};

}
