#include "stpch.h"
#include "Strata/Core/DynamicLibrary.h"

#include <dlfcn.h>

namespace Strata
{

	bool DynamicLibrary::Load(const std::filesystem::path& path, [[maybe_unused]] const std::filesystem::path& dependencyDirectory)
	{
		Unload();
		m_LastError.clear();

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
