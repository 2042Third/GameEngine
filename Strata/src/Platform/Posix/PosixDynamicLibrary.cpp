#include "stpch.h"
#include "Strata/Core/DynamicLibrary.h"

#include <dlfcn.h>

#if defined(ST_PLATFORM_LINUX)
	#include <cerrno>
	#include <cstdint>
	#include <cstring>
	#include <elf.h>
	#include <fcntl.h>
	#include <optional>
	#include <sys/stat.h>
	#include <unistd.h>
	#include <vector>
#endif

namespace Strata
{

#if defined(ST_PLATFORM_LINUX)
	namespace
	{

	#if UINTPTR_MAX == UINT64_MAX
		using ElfHeader = Elf64_Ehdr;
		using ElfProgramHeader = Elf64_Phdr;
		constexpr unsigned char c_ElfClass = ELFCLASS64;
	#else
		using ElfHeader = Elf32_Ehdr;
		using ElfProgramHeader = Elf32_Phdr;
		constexpr unsigned char c_ElfClass = ELFCLASS32;
	#endif

		// Reads exactly `size` bytes at `offset`; false at the end of the file or on an error.
		bool ReadAt(int file, uint64_t offset, void* destination, size_t size)
		{
			uint8_t* bytes = static_cast<uint8_t*>(destination);
			size_t done = 0;
			while (done < size)
			{
				const ssize_t count = pread(file, bytes + done, size - done, static_cast<off_t>(offset + done));
				if (count < 0 && errno == EINTR)
					continue;
				if (count <= 0)
					return false;
				done += static_cast<size_t>(count);
			}
			return true;
		}

		// glibc maps a library's loadable segments as its program headers describe them and then reads them: where a
		// segment lies past the end of the file (a build cut short while it was written or copied), that read raises
		// SIGBUS inside the loader. A crash guard around the load contains the fault, but jumping out of the loader leaves
		// its lock held by this thread, and every other thread that needs the lock afterwards (for its first thread_local
		// with a destructor, or its own dlopen) blocks for good. So a file the loader would read past is refused before
		// dlopen sees it: this returns why the loader would, if it would. Files that are no ELF object of this process's
		// class are left to dlopen, which reads their header without mapping anything and reports them itself.
		std::optional<std::string> FindTruncation(const std::filesystem::path& path)
		{
			const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC);
			if (file < 0)
				return std::nullopt;

			std::optional<std::string> problem;
			struct stat status = {};
			ElfHeader header = {};
			if (fstat(file, &status) == 0 && S_ISREG(status.st_mode) && ReadAt(file, 0, &header, sizeof(header)) &&
				std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0 && header.e_ident[EI_CLASS] == c_ElfClass &&
				static_cast<size_t>(header.e_phentsize) == sizeof(ElfProgramHeader))
			{
				const uint64_t fileSize = static_cast<uint64_t>(status.st_size);
				const uint64_t tableSize = static_cast<uint64_t>(header.e_phnum) * sizeof(ElfProgramHeader);
				std::vector<ElfProgramHeader> programHeaders(header.e_phnum);
				if (header.e_phoff > fileSize || tableSize > fileSize - header.e_phoff ||
					!ReadAt(file, header.e_phoff, programHeaders.data(), static_cast<size_t>(tableSize)))
				{
					problem = fmt::format("the file is cut short: it has {} bytes, but its program headers end at byte {}", fileSize,
						static_cast<uint64_t>(header.e_phoff) + tableSize);
				}
				else
				{
					for (const ElfProgramHeader& segment : programHeaders)
					{
						if (segment.p_type != PT_LOAD)
							continue;
						const uint64_t offset = segment.p_offset;
						const uint64_t size = segment.p_filesz;
						if (offset > fileSize || size > fileSize - offset)
						{
							problem = fmt::format("the file is cut short: it has {} bytes, but a loadable segment ends at byte {}", fileSize, offset + size);
							break;
						}
					}
				}
			}
			close(file);
			return problem;
		}

	}
#endif

	bool DynamicLibrary::Load(const std::filesystem::path& path, [[maybe_unused]] const std::filesystem::path& dependencyDirectory)
	{
		Unload();
		m_LastError.clear();

#if defined(ST_PLATFORM_LINUX)
		// A name without a directory is found by the loader's search, not opened here.
		if (path.has_parent_path())
		{
			if (std::optional<std::string> truncation = FindTruncation(path))
			{
				m_LastError = std::move(*truncation);
				return false;
			}
		}
#endif

		// Dependencies resolve through the library's RUNPATH; `dependencyDirectory` is a Windows search directory (see the
		// header).
		void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!handle)
		{
			const char* error = dlerror();
			m_LastError = error ? error : "dlopen failed";
			return false;
		}
		m_Handle = handle;
		m_Path = path;
		return true;
	}

	void DynamicLibrary::Unload()
	{
		if (!m_Handle)
			return;

		dlclose(m_Handle);
		m_Handle = nullptr;
		m_Path.clear();
	}

	void* DynamicLibrary::GetSymbol(const char* name) const
	{
		if (!m_Handle)
			return nullptr;
		return dlsym(m_Handle, name);
	}

	std::string_view DynamicLibrary::GetFileExtension()
	{
#if defined(ST_PLATFORM_MACOS)
		return ".dylib";
#else
		return ".so";
#endif
	}

	std::string DynamicLibrary::GetPlatformFileName(std::string_view baseName)
	{
		return fmt::format("lib{}{}", baseName, GetFileExtension());
	}

}
