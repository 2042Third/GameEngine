#include "Editor/ScriptProject.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Project/Project.h>

#include <utility>
#include <vector>

namespace Strata
{

	std::string MakeScriptCMakeLists(const Project& project)
	{
		const std::string module = project.GetScriptModuleName();
		return fmt::format(R"CMAKE(# Game scripts of "{0}": every .cpp and .h file in this directory (and below) belongs to the script module {1}.
# The editor builds it (script.build) with the engine's compiler and configuration and loads it from .strata/Scripts/Bin.
# Keep the module's name and output directory; to build by hand:
#   cmake -S {2} -B <build directory> -DSTRATA_ENGINE_DIR=<engine checkout> && cmake --build <build directory>
cmake_minimum_required(VERSION 3.25)
project({1} CXX)

set(STRATA_ENGINE_DIR "" CACHE PATH "The Strata engine checkout (the editor sets it)")
find_package(StrataScriptCore CONFIG REQUIRED PATHS "${{STRATA_ENGINE_DIR}}/StrataScriptCore/CMake" NO_DEFAULT_PATH)
strata_add_script_module({1} SOURCE_DIR "${{CMAKE_CURRENT_SOURCE_DIR}}")
)CMAKE", project.GetConfig().Name, module, project.GetConfig().Scripts.SourceDirectory);
	}

	std::string MakeExampleScript()
	{
		return R"SCRIPT(// An example script. Attach it to an entity to make it spin, e.g. with the editor command
//   script.add {"entity": "<entity id>", "class": "Spinner", "fields": {"DegreesPerSecond": 45}}
// Scripts see only the StrataScript SDK (see .claude/skills/strata-scripting/SKILL.md in the engine).

#include "StrataScript/StrataScript.h"

using namespace Strata;

// Turns its entity around the world's Y axis.
class Spinner : public Script
{
public:
	float DegreesPerSecond = 90.0f; // A field: editable per entity, kept across hot reloads

	void OnUpdate(float deltaTime) override
	{
		TransformComponent transform = GetTransform();
		const glm::quat turn = glm::angleAxis(glm::radians(DegreesPerSecond * deltaTime), glm::vec3(0.0f, 1.0f, 0.0f));
		transform.SetRotation(turn * transform.GetRotation());
	}
};

ST_SCRIPT_CLASS(Spinner)
{
	ST_SCRIPT_FIELD(DegreesPerSecond);
}
)SCRIPT";
	}

	bool CreateScriptProjectFiles(const Project& project, bool includeExample, std::vector<std::filesystem::path>* outCreated, std::string* outError)
	{
		const std::filesystem::path directory = project.GetScriptSourceDirectory();
		if (!FileSystem::CreateDirectories(directory))
		{
			if (outError)
				*outError = fmt::format("Cannot create the script directory '{}'", FileSystem::ToUTF8(directory));
			return false;
		}

		std::vector<std::pair<std::filesystem::path, std::string>> files = { { directory / "CMakeLists.txt", MakeScriptCMakeLists(project) } };
		if (includeExample)
			files.emplace_back(directory / "Spinner.cpp", MakeExampleScript());

		for (const auto& [path, content] : files)
		{
			if (FileSystem::Exists(path))
				continue;
			if (!FileSystem::WriteText(path, content))
			{
				if (outError)
					*outError = fmt::format("Cannot write '{}'", FileSystem::ToUTF8(path));
				return false;
			}
			if (outCreated)
				outCreated->push_back(path);
		}
		return true;
	}

}
