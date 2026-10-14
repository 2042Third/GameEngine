#pragma once

// The precompiled header of the editor's libraries: the engine's (stpch.h: the standard library, EnTT, nlohmann::json,
// glm, spdlog and the Core basics). StrataEditorUI adds Dear ImGui's <imgui.h> to it (StrataEditor/CMakeLists.txt): the
// core does not link ImGui.
//
// Only the standard library, vendor headers and the Core basics belong here, never an editor or engine header that
// changes often: every editor object rebuilds when the precompiled header changes. Sources still include what they use.

#include "stpch.h"
