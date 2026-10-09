#include "stpch.h"
#include "Strata/Core/DynamicLibrary.h"

#include "Strata/Core/FileSystem.h"

#if defined(ST_PLATFORM_WINDOWS)
	#include "Platform/Windows/WindowsUtils.h"
#else
	#include <dlfcn.h>
#endif

namespace Strata
{

	DynamicLibrary::~DynamicLibrary()
	{
		Unload();
	}

	DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
		: m_Handle(std::exchange(other.m_Handle, nullptr)), m_Path(std::move(other.m_Path)), m_LastError(std::move(other.m_LastError))
	{
	}

	DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
	{
		if (this != &other)
		{
			Unload();
			m_Handle = std::exchange(other.m_Handle, nullptr);
			m_Path = std::move(other.m_Path);
			m_LastError = std::move(other.m_LastError);
		}
		return *this;
	}

	bool DynamicLibrary::Load(const std::filesystem::path& path)
	{
		Unload();
		m_LastError.clear();

#if defined(ST_PLATFORM_WINDOWS)
		// Search the library's own directory for its dependencies.
		HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!module)
		{
			const DWORD errorCode = ::GetLastError();
			m_LastError = fmt::format("LoadLibrary failed (error {}): {}", errorCode, WindowsUtils::GetErrorMessage(errorCode));
			return false;
		}
		m_Handle = module;
#else
		void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!handle)
		{
			const char* error = dlerror();
			m_LastError = error ? error : "dlopen failed";
			return false;
		}
		m_Handle = handle;
#endif
		m_Path = path;
		return true;
	}

	void DynamicLibrary::Unload()
	{
		if (!m_Handle)
			return;

#if defined(ST_PLATFORM_WINDOWS)
		FreeLibrary(static_cast<HMODULE>(m_Handle));
#else
		dlclose(m_Handle);
#endif
		m_Handle = nullptr;
		m_Path.clear();
	}

	void DynamicLibrary::Release()
	{
		m_Handle = nullptr;
		m_Path.clear();
	}

	void* DynamicLibrary::GetSymbol(const char* name) const
	{
		if (!m_Handle)
			return nullptr;

#if defined(ST_PLATFORM_WINDOWS)
		return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_Handle), name));
#else
		return dlsym(m_Handle, name);
#endif
	}

	std::string_view DynamicLibrary::GetFileExtension()
	{
#if defined(ST_PLATFORM_WINDOWS)
		return ".dll";
#elif defined(ST_PLATFORM_MACOS)
		return ".dylib";
#else
		return ".so";
#endif
	}

	std::string DynamicLibrary::GetPlatformFileName(std::string_view baseName)
	{
#if defined(ST_PLATFORM_WINDOWS)
		return fmt::format("{}{}", baseName, GetFileExtension());
#else
		return fmt::format("lib{}{}", baseName, GetFileExtension());
#endif
	}

}
