#pragma once

#include "Strata/Core/Base.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
		// A size written "<width>x<height>" (e.g. 1920x1080), each a whole number from 1 to c_MaxSizeOption; nullopt when
		// the option is missing or malformed.
		std::optional<std::pair<uint32_t, uint32_t>> GetSizeOption(std::string_view name) const;

		static constexpr uint32_t c_MaxSizeOption = 65535;
	private:
		std::vector<std::string> m_Arguments; // Includes the program name at index 0
	};

}
