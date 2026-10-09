#pragma once

#include "Strata/Core/Base.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace Strata
{

	// RAII wrapper around a shared library (.dll / .so / .dylib).
	class DynamicLibrary
	{
	public:
		DynamicLibrary() = default;
		~DynamicLibrary();

		DynamicLibrary(const DynamicLibrary&) = delete;
		DynamicLibrary& operator=(const DynamicLibrary&) = delete;
		DynamicLibrary(DynamicLibrary&& other) noexcept;
		DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

		bool Load(const std::filesystem::path& path);
		void Unload();
		// Forgets the library without unloading it (it stays loaded until the process exits). For libraries that must
		// not be touched again, e.g. after their unload code crashed.
		void Release();

		bool IsLoaded() const { return m_Handle != nullptr; }
		const std::filesystem::path& GetPath() const { return m_Path; }
		const std::string& GetLastError() const { return m_LastError; }

		void* GetSymbol(const char* name) const;

		template<typename T>
		T GetFunction(const char* name) const
		{
			return reinterpret_cast<T>(GetSymbol(name));
		}

		// ".dll", ".so" or ".dylib"
		static std::string_view GetFileExtension();
		// File name CMake produces for a shared library target: "Name.dll", "libName.so", "libName.dylib".
		static std::string GetPlatformFileName(std::string_view baseName);
	private:
		void* m_Handle = nullptr;
		std::filesystem::path m_Path;
		std::string m_LastError;
	};

}
