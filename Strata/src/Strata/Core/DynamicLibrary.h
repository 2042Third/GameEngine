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

		// Loads the library at `path`. For an absolute path on Windows, its dependencies are searched in the library's own
		// directory, `dependencyDirectory` (e.g. where the original of a copied library lives), the executable's directory
		// and the system directories - never in the current directory or PATH. Other platforms resolve dependencies through
		// the library's RUNPATH (where $ORIGIN / @loader_path is the directory of `path`), the loader's environment
		// variables and the system paths; `dependencyDirectory` is not searched there. Names without a directory use the
		// platform's standard search.
		bool Load(const std::filesystem::path& path, const std::filesystem::path& dependencyDirectory = {});
		void Unload();
		// Forgets the library without unloading it (it stays loaded until the process exits). For libraries that must
		// not be touched again, e.g. after their unload code crashed.
		void Release();

		bool IsLoaded() const { return m_Handle != nullptr; }
		// The platform's handle (HMODULE, dlopen handle). Loading a library file that is already loaded in the process
		// returns the same handle: the platform counts references instead of loading it again, so both users share all of
		// the library's state.
		void* GetNativeHandle() const { return m_Handle; }
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
