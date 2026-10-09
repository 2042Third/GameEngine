#include "stpch.h"
#include "Platform/Vulkan/VulkanLoader.h"

#include "Strata/Core/DynamicLibrary.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"

namespace Strata::VulkanLoader
{

	namespace
	{

		struct LoaderState
		{
			// Never unloaded: Vulkan objects and GLFW may use the loader until process exit.
			DynamicLibrary Library;
			PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
			std::string Error;
		};

		LoaderState& Load()
		{
			static LoaderState s_State;
			static std::once_flag s_Once;
			std::call_once(s_Once, []()
			{
				std::vector<std::string> candidates;
#if defined(ST_PLATFORM_WINDOWS)
				candidates = { "vulkan-1.dll" };
#elif defined(ST_PLATFORM_MACOS)
				// A loader shipped next to the executable or inside the app bundle wins over system installs; MoltenVK
				// exports the Vulkan entry points itself and works without a loader (but without layers).
				const std::filesystem::path executableDirectory = Platform::GetExecutableDirectory();
				candidates = {
					FileSystem::ToUTF8(executableDirectory / "libvulkan.1.dylib"),
					FileSystem::ToUTF8(executableDirectory / ".." / "Frameworks" / "libvulkan.1.dylib"),
					"libvulkan.1.dylib",
					"libvulkan.dylib",
					"/usr/local/lib/libvulkan.1.dylib",
					"/opt/homebrew/lib/libvulkan.1.dylib",
					"libMoltenVK.dylib"
				};
#else
				candidates = { "libvulkan.so.1", "libvulkan.so" };
#endif
				for (const std::string& candidate : candidates)
				{
					if (s_State.Library.Load(FileSystem::FromUTF8(candidate)))
						break;
				}

				if (!s_State.Library.IsLoaded())
				{
					s_State.Error = fmt::format("the Vulkan loader library could not be loaded ({})", s_State.Library.GetLastError());
					return;
				}

				s_State.GetInstanceProcAddr = s_State.Library.GetFunction<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
				if (!s_State.GetInstanceProcAddr)
					s_State.Error = "the Vulkan loader does not export vkGetInstanceProcAddr";
			});
			return s_State;
		}

	}

	PFN_vkGetInstanceProcAddr GetInstanceProcAddr()
	{
		return Load().GetInstanceProcAddr;
	}

	std::string GetLoadError()
	{
		return Load().Error;
	}

}
