#include "stpch.h"
#include "Strata/Core/DynamicLibrary.h"

#include "Strata/Core/FileSystem.h"

#include "Platform/Windows/WindowsUtils.h"

namespace Strata
{

	bool DynamicLibrary::Load(const std::filesystem::path& path, const std::filesystem::path& dependencyDirectory)
	{
		Unload();
		m_LastError.clear();

		HMODULE module = nullptr;
		DWORD errorCode = ERROR_SUCCESS;
		if (path.is_absolute())
		{
			// The dependency directory is only needed while the loader resolves the library's imports.
			DLL_DIRECTORY_COOKIE dependencyCookie = nullptr;
			if (!dependencyDirectory.empty())
			{
				dependencyCookie = AddDllDirectory(dependencyDirectory.c_str());
				if (!dependencyCookie)
				{
					errorCode = ::GetLastError();
					m_LastError = fmt::format("Cannot search '{}' for the library's dependencies (error {}): {}", FileSystem::ToUTF8(dependencyDirectory),
						errorCode, WindowsUtils::GetErrorMessage(errorCode));
					return false;
				}
			}
			module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
			errorCode = ::GetLastError();
			if (dependencyCookie)
				RemoveDllDirectory(dependencyCookie);
		}
		else
		{
			module = LoadLibraryExW(path.c_str(), nullptr, 0);
			errorCode = ::GetLastError();
		}
		if (!module)
		{
			m_LastError = fmt::format("LoadLibrary failed (error {}): {}", errorCode, WindowsUtils::GetErrorMessage(errorCode));
			return false;
		}
		m_Handle = module;
		m_Path = path;
		return true;
	}

	void DynamicLibrary::Unload()
	{
		if (!m_Handle)
			return;

		FreeLibrary(static_cast<HMODULE>(m_Handle));
		m_Handle = nullptr;
		m_Path.clear();
	}

	void* DynamicLibrary::GetSymbol(const char* name) const
	{
		if (!m_Handle)
			return nullptr;
		return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_Handle), name));
	}

	std::string_view DynamicLibrary::GetFileExtension()
	{
		return ".dll";
	}

	std::string DynamicLibrary::GetPlatformFileName(std::string_view baseName)
	{
		return fmt::format("{}{}", baseName, GetFileExtension());
	}

}
