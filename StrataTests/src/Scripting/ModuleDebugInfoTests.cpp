#include <doctest/doctest.h>

#include "Scripting/ModuleDebugInfo.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/FileSystem.h"
#include "TestHelpers.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	void WriteLE(std::vector<uint8_t>& bytes, size_t offset, uint64_t value, uint32_t size)
	{
		if (bytes.size() < offset + size)
			bytes.resize(offset + size, 0);
		for (uint32_t index = 0; index < size; index++)
			bytes[offset + index] = static_cast<uint8_t>(value >> (8 * index));
	}

	// A 64-bit little-endian ELF file of three sections: none, `debugName` and the section names.
	std::vector<uint8_t> MakeElf(const std::string& debugName)
	{
		std::vector<uint8_t> bytes(64, 0);
		bytes[0] = 0x7F;
		bytes[1] = 'E';
		bytes[2] = 'L';
		bytes[3] = 'F';
		bytes[4] = 2; // 64-bit
		bytes[5] = 1; // Little-endian
		WriteLE(bytes, 0x28, 64, 8); // Section headers right after the file header
		WriteLE(bytes, 0x3A, 64, 2);
		WriteLE(bytes, 0x3C, 3, 2);
		WriteLE(bytes, 0x3E, 2, 2);
		const std::string names = std::string(1, '\0') + debugName + '\0' + ".shstrtab" + '\0';
		const uint64_t namesOffset = 64 + 3 * 64;
		WriteLE(bytes, 64 + 64, 1, 4);                       // Section 1's name
		WriteLE(bytes, 64 + 128, 2 + debugName.size(), 4);    // Section 2's name: .shstrtab
		WriteLE(bytes, 64 + 128 + 0x18, namesOffset, 8);
		WriteLE(bytes, 64 + 128 + 0x20, names.size(), 8);
		bytes.resize(namesOffset, 0);
		bytes.insert(bytes.end(), names.begin(), names.end());
		return bytes;
	}

	// A 64-bit Mach-O file with one load command: a symbol table whose second symbol has type `type`, or (dwarf) a segment
	// named __DWARF.
	std::vector<uint8_t> MakeMachO(uint8_t type, bool dwarf)
	{
		std::vector<uint8_t> bytes(32, 0);
		WriteLE(bytes, 0, 0xFEEDFACF, 4);
		WriteLE(bytes, 16, 1, 4);
		if (dwarf)
		{
			WriteLE(bytes, 32, 0x19, 4);
			WriteLE(bytes, 36, 72, 4);
			const char name[] = "__DWARF";
			bytes.resize(32 + 72, 0);
			for (size_t index = 0; index + 1 < sizeof(name); index++)
				bytes[40 + index] = static_cast<uint8_t>(name[index]);
			return bytes;
		}
		WriteLE(bytes, 32, 0x2, 4);
		WriteLE(bytes, 36, 24, 4);
		WriteLE(bytes, 40, 56, 4); // The symbols, after the command
		WriteLE(bytes, 44, 2, 4);
		bytes.resize(56 + 2 * 16, 0);
		bytes[56 + 4] = 0x0F; // A defined external symbol
		bytes[56 + 16 + 4] = type;
		return bytes;
	}

}

TEST_SUITE("Scripting.Module")
{
	TEST_CASE("Script modules carry debug information in every configuration, as a debugger needs to bind breakpoints")
	{
		// MSVC writes a program database in every configuration; GCC and Clang compile with -g in every configuration too
		// (StrataScriptModule.cmake), so a released editor's script builds keep it.
		const std::filesystem::path module = GetTestScriptModule(STRATA_TEST_SCRIPTS_API);
		const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(module);
		REQUIRE(bytes);
		std::string reason;
		const std::string found = FindModuleDebugInformation(*bytes, &reason);
		INFO(FileSystem::ToUTF8(module), ": ", reason);
		CHECK_FALSE(found.empty());
#if defined(_MSC_VER)
		std::filesystem::path symbols = module;
		symbols.replace_extension(".pdb");
		CHECK(found.rfind("CodeView ", 0) == 0);
		CHECK(FileSystem::IsRegularFile(symbols));
#endif
	}

	TEST_CASE("The debug information check reads ELF and Mach-O files and refuses what it cannot read")
	{
		std::string reason;
		CHECK(FindModuleDebugInformation(MakeElf(".debug_info"), &reason) == ".debug_info");
		CHECK(FindModuleDebugInformation(MakeElf(".zdebug_info"), &reason) == ".zdebug_info");
		CHECK(FindModuleDebugInformation(MakeElf(".debug_abbr"), &reason).empty());
		CHECK(reason == "an ELF file without .debug_info");

		CHECK(FindModuleDebugInformation(MakeMachO(0x66, false), &reason) == "debug map");
		CHECK(FindModuleDebugInformation(MakeMachO(0, true), &reason) == "__DWARF");
		CHECK(FindModuleDebugInformation(MakeMachO(0x0F, false), &reason).empty());
		CHECK(reason == "a Mach-O file without DWARF or a debug map");

		// Truncated files at every length, and other files, are refused without reading outside their bytes.
		for (const std::vector<uint8_t>& whole : { MakeElf(".debug_info"), MakeMachO(0x66, false) })
		{
			for (size_t length = 0; length < whole.size(); length++)
			{
				const std::vector<uint8_t> truncated(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(length));
				FindModuleDebugInformation(truncated, &reason);
			}
		}
		const std::optional<std::vector<uint8_t>> module = FileSystem::ReadBytes(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		REQUIRE(module);
		const std::vector<uint8_t> head(module->begin(), module->begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(module->size(), 512)));
		CHECK(FindModuleDebugInformation(head, &reason).empty());
		CHECK(FindModuleDebugInformation(std::vector<uint8_t> { 'n', 'o', 't' }, &reason).empty());
		CHECK(reason == "neither an ELF, a Mach-O nor a PE file");
	}
}
