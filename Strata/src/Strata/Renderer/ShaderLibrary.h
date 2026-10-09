#pragma once

#include "Strata/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Strata
{

	// Creates and caches NVRHI shader objects for the engine's embedded SPIR-V. Shaders are looked up by their
	// file name relative to Strata/shaders ("ImGui.vert"); the stage comes from the extension
	// (.vert, .frag, .comp, .geom). Thread-safe.
	class ShaderLibrary
	{
	public:
		explicit ShaderLibrary(nvrhi::IDevice* device);

		// Returns null (and logs an error) if the shader does not exist or fails to load.
		nvrhi::ShaderHandle Get(std::string_view name);
		void Clear();

		static nvrhi::ShaderType GetStageFromName(std::string_view name);
	private:
		nvrhi::IDevice* m_Device;
		std::mutex m_Mutex;
		std::unordered_map<std::string, nvrhi::ShaderHandle> m_Shaders;
	};

}
