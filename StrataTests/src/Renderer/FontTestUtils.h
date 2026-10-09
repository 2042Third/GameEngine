#pragma once

#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace Strata::Tests
{

	// A file distributed with a vendored dependency (a path below the repository root), read from the source tree.
	inline std::vector<uint8_t> ReadSourceFile(std::string_view relativePath)
	{
		const std::filesystem::path path = FileSystem::FromUTF8(STRATA_SOURCE_DIR) / FileSystem::FromUTF8(relativePath);
		std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(path);
		REQUIRE_MESSAGE(bytes.has_value(), "Missing source file " << std::string(relativePath));
		return std::move(*bytes);
	}

}
