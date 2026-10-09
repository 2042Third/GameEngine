#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace Strata
{

	// SPIR-V compiled at build time from Strata/shaders (see CMake/StrataShaders.cmake).
	struct EmbeddedShader
	{
		std::string_view Name; // Path relative to Strata/shaders, e.g. "ImGui.vert"
		const uint32_t* Code = nullptr;
		size_t Size = 0;       // Bytes
	};

	namespace EmbeddedShaders
	{
		std::span<const EmbeddedShader> GetAll(); // Defined by the generated registry source
		const EmbeddedShader* Find(std::string_view name);
	}

}
