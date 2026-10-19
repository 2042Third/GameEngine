#pragma once

// The precompiled header of StrataTests: the engine's (stpch.h: the standard library, EnTT, nlohmann::json, glm, spdlog
// and the Core basics), doctest and glm. TestMain.cpp compiles without it: it defines DOCTEST_CONFIG_IMPLEMENT before
// including doctest.
//
// Only the standard library, vendor headers and the Core basics belong here, never a header of the engine, the editor or
// the tests that changes often: every test object rebuilds when the precompiled header changes. Sources still include
// what they use.

#include "stpch.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
