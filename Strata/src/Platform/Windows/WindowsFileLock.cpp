#include "stpch.h"
#include "Strata/Core/FileLock.h"

#include <Windows.h>

namespace Strata
{

	namespace
	{

		// Opened without sharing, the file cannot be opened again (by anyone) while this handle exists: that is the lock.
		// The handle is not inherited by child processes, and the system closes it when the process ends.
		HANDLE OpenExclusive(const std::filesystem::path& path, DWORD disposition)
		{
			const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
			return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
		}

	}

	Scope<FileLock> FileLock::Create(const std::filesystem::path& path)
	{
		// CREATE_NEW creates and opens in one step, so the file never exists unlocked.
		const HANDLE handle = OpenExclusive(path, CREATE_NEW);
		if (!handle)
			return nullptr;
		Scope<FileLock> lock(new FileLock());
		lock->m_Handle = handle;
		return lock;
	}

	Scope<FileLock> FileLock::TryAcquire(const std::filesystem::path& path)
	{
		const HANDLE handle = OpenExclusive(path, OPEN_EXISTING);
		if (!handle)
			return nullptr;
		Scope<FileLock> lock(new FileLock());
		lock->m_Handle = handle;
		return lock;
	}

	FileLock::~FileLock()
	{
		if (m_Handle)
			CloseHandle(static_cast<HANDLE>(m_Handle));
	}

}
