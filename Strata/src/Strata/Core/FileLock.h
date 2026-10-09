#pragma once

#include "Strata/Core/Base.h"

#include <filesystem>

namespace Strata
{

	// An exclusive lock on a file that other processes can test, e.g. to tell whether the process that owns a directory
	// still runs. It is held until the object is destroyed or the process ends, however it ends (the system releases it
	// then). A lock is exclusive within the process too: a second FileLock on the same file fails while one exists.
	// Implemented per platform in Platform/<OS>/.
	class FileLock
	{
	public:
		// Creates the file `path`, which must not exist, already locked: nobody ever finds it unlocked while this lock
		// exists. Null on failure.
		static Scope<FileLock> Create(const std::filesystem::path& path);
		// Locks an existing file. Null if it does not exist, cannot be opened, or someone holds its lock.
		static Scope<FileLock> TryAcquire(const std::filesystem::path& path);
		~FileLock();

		FileLock(const FileLock&) = delete;
		FileLock& operator=(const FileLock&) = delete;
	private:
		FileLock() = default;
	private:
#if defined(ST_PLATFORM_WINDOWS)
		void* m_Handle = nullptr;
#else
		int m_Descriptor = -1;
#endif
	};

}
