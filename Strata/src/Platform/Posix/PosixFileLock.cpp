#include "stpch.h"
#include "Strata/Core/FileLock.h"

#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace Strata
{

	// flock() locks belong to an open file description: another open() of the file - in any process - cannot lock it
	// while this one holds it, and the lock ends when the descriptor is closed, also when the process ends. O_CLOEXEC
	// keeps programs this process starts from inheriting it.

	Scope<FileLock> FileLock::Create(const std::filesystem::path& path)
	{
		// The file is created and locked under a temporary name, then renamed into place, so whoever opens `path` finds
		// it locked already.
		std::filesystem::path temporary = path;
		temporary += ".creating";
		const int descriptor = open(temporary.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (descriptor < 0)
			return nullptr;
		if (flock(descriptor, LOCK_EX | LOCK_NB) != 0 || std::rename(temporary.c_str(), path.c_str()) != 0)
		{
			close(descriptor);
			unlink(temporary.c_str());
			return nullptr;
		}

		Scope<FileLock> lock(new FileLock());
		lock->m_Descriptor = descriptor;
		return lock;
	}

	Scope<FileLock> FileLock::TryAcquire(const std::filesystem::path& path)
	{
		const int descriptor = open(path.c_str(), O_RDWR | O_CLOEXEC);
		if (descriptor < 0)
			return nullptr;
		if (flock(descriptor, LOCK_EX | LOCK_NB) != 0)
		{
			close(descriptor);
			return nullptr;
		}

		Scope<FileLock> lock(new FileLock());
		lock->m_Descriptor = descriptor;
		return lock;
	}

	FileLock::~FileLock()
	{
		if (m_Descriptor >= 0)
			close(m_Descriptor);
	}

}
