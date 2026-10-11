#include "stpch.h"
#include "Strata/Core/Symbolizer.h"

#include "Strata/Core/FileSystem.h"

#include <cxxabi.h>
#include <dlfcn.h>

#if defined(__linux__)
	#include <elf.h>
#endif

#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace Strata::Symbolizer
{

	namespace
	{

		std::string Demangle(const char* name)
		{
			int status = 0;
			const std::unique_ptr<char, decltype(&std::free)> demangled(abi::__cxa_demangle(name, nullptr, nullptr, &status), &std::free);
			return status == 0 && demangled ? std::string(demangled.get()) : std::string(name);
		}

#if defined(__linux__)
		// Script modules hide their symbols (dladdr only sees exported ones), but their files keep a full symbol table unless
		// stripped: the function containing an offset of the module, from the ELF file's .symtab. Every read is bounds-checked
		// against the file.
		constexpr uint64_t c_MaxModuleFileSize = 512ull * 1024 * 1024;

		template<typename T>
		std::optional<T> ReadStruct(const std::vector<uint8_t>& bytes, uint64_t offset)
		{
			if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
				return std::nullopt;
			T value;
			std::memcpy(&value, bytes.data() + offset, sizeof(T));
			return value;
		}

		std::string FindElfFunction(const std::filesystem::path& file, uint64_t offset)
		{
			const std::optional<uint64_t> size = FileSystem::GetFileSize(file);
			if (!size || *size > c_MaxModuleFileSize)
				return {};
			const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(file);
			if (!bytes)
				return {};
			const std::optional<Elf64_Ehdr> header = ReadStruct<Elf64_Ehdr>(*bytes, 0);
			if (!header || std::memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 || header->e_ident[EI_CLASS] != ELFCLASS64
				|| header->e_shentsize != sizeof(Elf64_Shdr))
			{
				return {};
			}

			for (uint16_t index = 0; index < header->e_shnum; index++)
			{
				const std::optional<Elf64_Shdr> symbols = ReadStruct<Elf64_Shdr>(*bytes, header->e_shoff + static_cast<uint64_t>(index) * sizeof(Elf64_Shdr));
				if (!symbols || symbols->sh_type != SHT_SYMTAB || symbols->sh_entsize != sizeof(Elf64_Sym) || symbols->sh_link >= header->e_shnum)
					continue;
				const std::optional<Elf64_Shdr> strings = ReadStruct<Elf64_Shdr>(*bytes, header->e_shoff + static_cast<uint64_t>(symbols->sh_link) * sizeof(Elf64_Shdr));
				if (!strings || strings->sh_offset > bytes->size() || bytes->size() - strings->sh_offset < strings->sh_size)
					continue;
				const uint64_t count = symbols->sh_size / sizeof(Elf64_Sym);
				for (uint64_t symbolIndex = 0; symbolIndex < count; symbolIndex++)
				{
					const std::optional<Elf64_Sym> symbol = ReadStruct<Elf64_Sym>(*bytes, symbols->sh_offset + symbolIndex * sizeof(Elf64_Sym));
					if (!symbol)
						break;
					if (ELF64_ST_TYPE(symbol->st_info) != STT_FUNC || symbol->st_size == 0 || offset < symbol->st_value
						|| offset - symbol->st_value >= symbol->st_size || symbol->st_name >= strings->sh_size)
					{
						continue;
					}
					// The name ends with a null inside the string table.
					const char* name = reinterpret_cast<const char*>(bytes->data() + strings->sh_offset + symbol->st_name);
					const size_t available = static_cast<size_t>(strings->sh_size - symbol->st_name);
					if (std::memchr(name, '\0', available) == nullptr)
						continue;
					return Demangle(name);
				}
			}
			return {};
		}
#endif

	}

	Frame Describe(uint64_t address)
	{
		Frame frame;
		frame.Address = address;
		Dl_info info = {};
		if (dladdr(reinterpret_cast<const void*>(static_cast<uintptr_t>(address)), &info) == 0)
			return frame;
		if (info.dli_fname)
			frame.ModulePath = std::filesystem::path(info.dli_fname);
		frame.ModuleBase = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info.dli_fbase));
		// On Linux dladdr names the nearest exported symbol, which for hidden code is another function: Resolve reads the
		// symbol table instead. macOS's dyld searches the image's symbol table, local symbols included.
#if !defined(__linux__)
		if (info.dli_sname && info.dli_saddr)
			frame.Function = Demangle(info.dli_sname);
#endif
		return frame;
	}

	void Resolve([[maybe_unused]] std::span<Frame> frames, std::span<const std::filesystem::path>)
	{
		// No DWARF reader: no files or lines. Functions of hidden code come from the ELF symbol table on Linux.
#if defined(__linux__)
		for (Frame& frame : frames)
		{
			if (frame.Function.empty() && !frame.ModulePath.empty() && frame.Address >= frame.ModuleBase)
				frame.Function = FindElfFunction(frame.ModulePath, frame.Address - frame.ModuleBase);
		}
#endif
	}

}
