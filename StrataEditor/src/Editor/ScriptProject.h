#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Strata
{

	class Project;

	// The CMakeLists.txt of a project's script directory: builds every .cpp/.h file below it into the project's script
	// module through the StrataScriptCore package (STRATA_ENGINE_DIR names the engine checkout; script.build sets it).
	std::string MakeScriptCMakeLists(const Project& project);
	// A small example script class ("Spinner") for new projects.
	std::string MakeExampleScript();

	// Writes the script build of a project into its script directory: the CMakeLists.txt and, with `includeExample`, the
	// example script. Files that exist are kept as they are. Returns the files that were written.
	bool CreateScriptProjectFiles(const Project& project, bool includeExample, std::vector<std::filesystem::path>* outCreated = nullptr,
		std::string* outError = nullptr);

}
