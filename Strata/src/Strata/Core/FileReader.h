#pragma once

#include "Strata/Core/Base.h"

#include <cstdint>
#include <filesystem>
#include <span>

namespace Strata
{

	// Reads byte ranges of one open file from any number of threads at once. Every read names its offset, so readers never
	// share a file position: ReadFile with an offset in an OVERLAPPED structure on a handle opened for overlapped I/O on
	// Windows, pread elsewhere. Replaces opening a stream per read (asset packs keep one reader for their whole life).
	// Other processes may read the file, and rename or delete it, while it is open; the reader keeps reading the file it
	// opened. Implemented per platform in Platform/<OS>/<OS>FileReader.cpp.
	class FileReader
	{
	public:
		// Null when the file does not exist, is not a regular file or cannot be opened for reading.
		static Scope<FileReader> Open(const std::filesystem::path& path);
		~FileReader();

		FileReader(const FileReader&) = delete;
		FileReader& operator=(const FileReader&) = delete;

		// The file's size when it was opened.
		uint64_t GetSize() const { return m_Size; }

		// Fills `destination` with the bytes at `offset`. False, with `destination` undefined, when the range does not lie
		// entirely within the file's size at opening or the system reports an error. Thread-safe.
		bool ReadAt(uint64_t offset, std::span<uint8_t> destination) const;

		// Files opened through Open since the process started (successfully), for tests and statistics. Thread-safe.
		static uint64_t GetOpenCount();
	private:
		FileReader() = default;
	private:
		uint64_t m_Size = 0;
#if defined(ST_PLATFORM_WINDOWS)
		void* m_Handle = nullptr;
#else
		int m_Descriptor = -1;
#endif
	};

}
