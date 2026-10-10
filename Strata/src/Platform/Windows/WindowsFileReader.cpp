#include "stpch.h"
#include "Strata/Core/FileReader.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>

namespace Strata
{

	namespace
	{

		std::atomic<uint64_t> s_OpenCount = 0;

		// The event a thread waits on for its reads to complete: reads of several threads on one overlapped handle must each
		// wait on their own event, as the handle itself is signaled by whichever read completes. Created once per thread.
		struct ReadEvent
		{
			HANDLE Handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);

			~ReadEvent()
			{
				if (Handle)
					CloseHandle(Handle);
			}
		};

		thread_local ReadEvent t_ReadEvent;

		// ReadFile takes 32-bit sizes: larger ranges are read in pieces.
		constexpr size_t c_MaxReadChunk = 1u << 30;

	}

	Scope<FileReader> FileReader::Open(const std::filesystem::path& path)
	{
		// Overlapped I/O, so that reads of several threads run concurrently instead of one after another (the system
		// serializes the synchronous I/O of a handle). Other processes may read the file and rename or delete it (e.g. move
		// the pack of a running game aside to export a new one), but not write into it.
		const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			return nullptr;

		LARGE_INTEGER size = {};
		if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileSizeEx(handle, &size) || size.QuadPart < 0)
		{
			CloseHandle(handle);
			return nullptr;
		}

		Scope<FileReader> reader(new FileReader());
		reader->m_Handle = handle;
		reader->m_Size = static_cast<uint64_t>(size.QuadPart);
		s_OpenCount.fetch_add(1, std::memory_order_relaxed);
		return reader;
	}

	FileReader::~FileReader()
	{
		if (m_Handle)
			CloseHandle(static_cast<HANDLE>(m_Handle));
	}

	bool FileReader::ReadAt(uint64_t offset, std::span<uint8_t> destination) const
	{
		if (offset > m_Size || destination.size() > m_Size - offset)
			return false;
		const HANDLE event = t_ReadEvent.Handle;
		if (!event)
			return false;

		const HANDLE handle = static_cast<HANDLE>(m_Handle);
		size_t done = 0;
		while (done < destination.size())
		{
			const uint64_t position = offset + done;
			OVERLAPPED overlapped = {};
			overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFull);
			overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
			overlapped.hEvent = event;
			const DWORD chunk = static_cast<DWORD>(std::min(destination.size() - done, c_MaxReadChunk));
			if (!ReadFile(handle, destination.data() + done, chunk, nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING)
				return false;
			DWORD read = 0;
			if (!GetOverlappedResult(handle, &overlapped, &read, TRUE) || read == 0)
				return false; // An error, or the file shrank below its size at opening
			done += read;
		}
		return true;
	}

	uint64_t FileReader::GetOpenCount()
	{
		return s_OpenCount.load(std::memory_order_relaxed);
	}

}
