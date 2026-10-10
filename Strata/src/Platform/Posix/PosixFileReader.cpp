#include "stpch.h"
#include "Strata/Core/FileReader.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace Strata
{

	namespace
	{

		std::atomic<uint64_t> s_OpenCount = 0;

		// pread returns a signed size: larger ranges are read in pieces.
		constexpr size_t c_MaxReadChunk = 1u << 30;

	}

	Scope<FileReader> FileReader::Open(const std::filesystem::path& path)
	{
		// O_CLOEXEC keeps programs this process starts from inheriting the descriptor.
		const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (descriptor < 0)
			return nullptr;

		struct stat status = {};
		if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0)
		{
			close(descriptor);
			return nullptr;
		}

		Scope<FileReader> reader(new FileReader());
		reader->m_Descriptor = descriptor;
		reader->m_Size = static_cast<uint64_t>(status.st_size);
		s_OpenCount.fetch_add(1, std::memory_order_relaxed);
		return reader;
	}

	FileReader::~FileReader()
	{
		if (m_Descriptor >= 0)
			close(m_Descriptor);
	}

	bool FileReader::ReadAt(uint64_t offset, std::span<uint8_t> destination) const
	{
		if (offset > m_Size || destination.size() > m_Size - offset)
			return false;

		// pread reads at an offset without moving the descriptor's position, so threads share the descriptor freely.
		size_t done = 0;
		while (done < destination.size())
		{
			const size_t chunk = std::min(destination.size() - done, c_MaxReadChunk);
			const ssize_t read = pread(m_Descriptor, destination.data() + done, chunk, static_cast<off_t>(offset + done));
			if (read < 0)
			{
				if (errno == EINTR)
					continue;
				return false;
			}
			if (read == 0)
				return false; // The file shrank below its size at opening
			done += static_cast<size_t>(read);
		}
		return true;
	}

	uint64_t FileReader::GetOpenCount()
	{
		return s_OpenCount.load(std::memory_order_relaxed);
	}

}
