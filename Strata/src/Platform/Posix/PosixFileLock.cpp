#include "stpch.h"
#include "Strata/Core/FileLock.h"

#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <unistd.h>

namespace Strata
{

	// flock() locks belong to an open file description: another open() of the file - in any process - cannot lock it
	// while this one holds it, and the lock ends when the descriptor is closed, also when the process ends. O_CLOEXEC
	// keeps programs this process starts from inheriting it.

	Scope<FileLock> FileLock::Create(const std::filesystem::path& path)
	{
		// The file is created and locked under a unique temporary name next to `path`, then given the name `path` with
		// link(), which fails if `path` exists: whoever opens `path` finds it locked already, and an existing file is
		// never replaced. The temporary name is removed either way.
		std::string temporary = path.string() + ".XXXXXX";
		const int descriptor = mkstemp(temporary.data());
		if (descriptor < 0)
			return nullptr;
		const bool created = fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0 && flock(descriptor, LOCK_EX | LOCK_NB) == 0
			&& link(temporary.c_str(), path.c_str()) == 0;
		unlink(temporary.c_str());
		if (!created)
		{
			close(descriptor);
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
