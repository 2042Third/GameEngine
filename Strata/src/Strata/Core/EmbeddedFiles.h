#pragma once

#include <cstdint>
#include <span>

// Built-in data compiled into the engine (see CMake/StrataEmbeddedFiles.cmake).
namespace Strata::EmbeddedFiles
{

	// Roboto Medium (TrueType, Apache License 2.0): the default font of text rendering.
	std::span<const uint8_t> GetDefaultFont();

}
