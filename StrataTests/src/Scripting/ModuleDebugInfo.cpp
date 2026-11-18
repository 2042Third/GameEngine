#include "Scripting/ModuleDebugInfo.h"

#include <algorithm>
#include <string_view>

namespace Strata::Tests
{

	namespace
	{

		// Little- or big-endian unsigned integers of `size` bytes at `offset`; false outside the bytes.
		bool ReadUnsigned(std::span<const uint8_t> bytes, uint64_t offset, uint32_t size, bool bigEndian, uint64_t& outValue)
		{
			if (offset > bytes.size() || size > bytes.size() - offset)
				return false;
			outValue = 0;
			for (uint32_t index = 0; index < size; index++)
			{
				const uint64_t byte = bytes[static_cast<size_t>(offset) + (bigEndian ? index : size - 1 - index)];
				outValue = (outValue << 8) | byte;
			}
			return true;
		}

		uint64_t Read(std::span<const uint8_t> bytes, uint64_t offset, uint32_t size, bool& ok, bool bigEndian = false)
		{
			uint64_t value = 0;
			if (!ReadUnsigned(bytes, offset, size, bigEndian, value))
				ok = false;
			return value;
		}

		// A zero-terminated string at `offset`, at most `limit` bytes; empty outside the bytes.
		std::string_view ReadString(std::span<const uint8_t> bytes, uint64_t offset, uint64_t limit)
		{
			if (offset >= bytes.size())
				return {};
			const uint64_t available = std::min<uint64_t>(limit, bytes.size() - offset);
			const char* start = reinterpret_cast<const char*>(bytes.data() + offset);
			size_t length = 0;
			while (length < available && start[length] != '\0')
				length++;
			return std::string_view(start, length);
		}

		std::string Fail(std::string* outReason, std::string reason)
		{
			if (outReason)
				*outReason = std::move(reason);
			return {};
		}

		std::string FindInElf(std::span<const uint8_t> bytes, std::string* outReason)
		{
			if (bytes.size() < 64 || bytes[4] != 2 || bytes[5] != 1)
				return Fail(outReason, "an ELF file that is not 64-bit little-endian, or truncated");
			bool ok = true;
			const uint64_t sectionOffset = Read(bytes, 0x28, 8, ok);
			const uint64_t entrySize = Read(bytes, 0x3A, 2, ok);
			const uint64_t sectionCount = Read(bytes, 0x3C, 2, ok);
			const uint64_t namesIndex = Read(bytes, 0x3E, 2, ok);
			if (!ok || entrySize < 64 || namesIndex >= sectionCount)
				return Fail(outReason, "an ELF file without a readable section table");
			const uint64_t namesHeader = sectionOffset + namesIndex * entrySize;
			const uint64_t namesOffset = Read(bytes, namesHeader + 0x18, 8, ok);
			const uint64_t namesSize = Read(bytes, namesHeader + 0x20, 8, ok);
			for (uint64_t section = 0; ok && section < sectionCount; section++)
			{
				const uint64_t name = Read(bytes, sectionOffset + section * entrySize, 4, ok);
				if (!ok || name >= namesSize)
					continue;
				const std::string_view sectionName = ReadString(bytes, namesOffset + name, namesSize - name);
				if (sectionName == ".debug_info" || sectionName == ".zdebug_info")
					return std::string(sectionName);
			}
			return Fail(outReason, ok ? "an ELF file without .debug_info" : "an ELF file whose section table is truncated");
		}

		std::string FindInMachO(std::span<const uint8_t> bytes, std::string* outReason)
		{
			bool ok = true;
			const uint64_t commandCount = Read(bytes, 16, 4, ok);
			uint64_t command = 32;
			for (uint64_t index = 0; ok && index < commandCount; index++)
			{
				const uint64_t type = Read(bytes, command, 4, ok);
				const uint64_t size = Read(bytes, command + 4, 4, ok);
				if (!ok || size < 8)
					break;
				// LC_SEGMENT_64 named __DWARF: the DWARF linked into the file.
				if (type == 0x19 && ReadString(bytes, command + 8, 16) == "__DWARF")
					return "__DWARF";
				// LC_SYMTAB: stabs of type N_OSO name the objects whose DWARF lldb reads (the debug map).
				if (type == 0x2)
				{
					const uint64_t symbols = Read(bytes, command + 8, 4, ok);
					const uint64_t symbolCount = Read(bytes, command + 12, 4, ok);
					for (uint64_t symbol = 0; ok && symbol < symbolCount; symbol++)
					{
						if (Read(bytes, symbols + symbol * 16 + 4, 1, ok) == 0x66)
							return "debug map";
					}
				}
				command += size;
			}
			return Fail(outReason, ok ? "a Mach-O file without DWARF or a debug map" : "a truncated Mach-O file");
		}

		std::string FindInPE(std::span<const uint8_t> bytes, std::string* outReason)
		{
			bool ok = true;
			const uint64_t header = Read(bytes, 0x3C, 4, ok);
			if (!ok || Read(bytes, header, 4, ok) != 0x00004550)
				return Fail(outReason, "a file starting with MZ without a PE header");
			const uint64_t sectionCount = Read(bytes, header + 6, 2, ok);
			const uint64_t optionalSize = Read(bytes, header + 20, 2, ok);
			const uint64_t optional = header + 24;
			const uint64_t magic = Read(bytes, optional, 2, ok);
			if (!ok || (magic != 0x20B && magic != 0x10B))
				return Fail(outReason, "a PE file with an unknown optional header");
			const uint64_t directories = optional + (magic == 0x20B ? 112 : 96);
			const uint64_t directoryCount = Read(bytes, directories - 4, 4, ok);
			if (!ok || directoryCount <= 6)
				return Fail(outReason, "a PE file without a debug directory");
			const uint64_t debugAddress = Read(bytes, directories + 6 * 8, 4, ok);
			const uint64_t debugSize = Read(bytes, directories + 6 * 8 + 4, 4, ok);
			// The section holding the debug directory maps its address to the file.
			uint64_t debugOffset = 0;
			bool mapped = false;
			const uint64_t sections = optional + optionalSize;
			for (uint64_t section = 0; ok && section < sectionCount && !mapped; section++)
			{
				const uint64_t entry = sections + section * 40;
				const uint64_t virtualSize = Read(bytes, entry + 8, 4, ok);
				const uint64_t virtualAddress = Read(bytes, entry + 12, 4, ok);
				const uint64_t rawOffset = Read(bytes, entry + 20, 4, ok);
				if (ok && debugAddress >= virtualAddress && debugAddress < virtualAddress + virtualSize)
				{
					debugOffset = rawOffset + (debugAddress - virtualAddress);
					mapped = true;
				}
			}
			if (!ok || !mapped || debugSize == 0)
				return Fail(outReason, "a PE file without a debug directory");
			for (uint64_t entry = debugOffset; ok && entry + 28 <= debugOffset + debugSize; entry += 28)
			{
				// IMAGE_DEBUG_TYPE_CODEVIEW: an RSDS record (signature, GUID, age) and the program database's path.
				if (Read(bytes, entry + 12, 4, ok) != 2)
					continue;
				const uint64_t record = Read(bytes, entry + 24, 4, ok);
				if (ok && Read(bytes, record, 4, ok) == 0x53445352)
					return "CodeView " + std::string(ReadString(bytes, record + 24, 4096));
			}
			return Fail(outReason, ok ? "a PE file without a CodeView record" : "a truncated PE file");
		}

	}

	std::string FindModuleDebugInformation(std::span<const uint8_t> bytes, std::string* outReason)
	{
		if (bytes.size() >= 4 && bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F')
			return FindInElf(bytes, outReason);
		bool ok = true;
		const uint64_t magic = Read(bytes, 0, 4, ok);
		if (ok && magic == 0xFEEDFACF)
			return FindInMachO(bytes, outReason);
		// A universal file: its first slice (fat_header and fat_arch are big-endian).
		if (ok && Read(bytes, 0, 4, ok, true) == 0xCAFEBABE)
		{
			const uint64_t offset = Read(bytes, 8 + 8, 4, ok, true);
			const uint64_t size = Read(bytes, 8 + 12, 4, ok, true);
			if (!ok || offset > bytes.size() || size > bytes.size() - offset)
				return Fail(outReason, "a truncated universal Mach-O file");
			return FindModuleDebugInformation(bytes.subspan(static_cast<size_t>(offset), static_cast<size_t>(size)), outReason);
		}
		if (bytes.size() >= 2 && bytes[0] == 'M' && bytes[1] == 'Z')
			return FindInPE(bytes, outReason);
		return Fail(outReason, "neither an ELF, a Mach-O nor a PE file");
	}

}
