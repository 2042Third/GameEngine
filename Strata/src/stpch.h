#pragma once

// Strata engine precompiled header. Include first in every engine source file. Every layer library builds it, and the
// editor's and the tests' precompiled headers start with it (StrataEditor/src/EditorPch.h, StrataTests/src/TestsPch.h).
//
// It holds only the standard library, vendor headers and the Core basics below: never a header that changes often,
// because every engine object rebuilds when it changes. Sources and headers still include what they use (the lint
// compiles without precompiled headers, and STRATA_PRECOMPILED_HEADERS=OFF builds without them).

#include "Strata/Core/PlatformDetection.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include "Strata/Core/Base.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Assert.h"
#include "Strata/Core/Profiling.h"
