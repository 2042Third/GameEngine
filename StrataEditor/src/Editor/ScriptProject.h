#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class Project;
	struct ScriptBuildSettings;

	// The CMakeLists.txt of a project's script directory: builds every .cpp/.h file below it into the project's script
	// module through the StrataScriptCore package (STRATA_ENGINE_DIR names the Strata folder, a source checkout or an
	// installation; script.build sets it).
	std::string MakeScriptCMakeLists(const Project& project);
	// A small example script class ("Spinner") for new projects.
	std::string MakeExampleScript();

	// What a new script's source starts with (script.create).
	enum class ScriptTemplate : uint8_t
	{
		Behaviour = 0, // OnCreate and OnUpdate, and an example field
		Empty          // The class and its registration only
	};

	// A field of a new script: a C++ identifier and one of the SDK's field types, named "bool", "int", "float", "vec2",
	// "vec3", "vec4", "quat", "string", "entity" or "asset" (ignoring case).
	struct ScriptFieldDeclaration
	{
		std::string Name;
		std::string Type;
	};

	// The field type names ScriptFieldDeclaration::Type takes.
	std::vector<std::string> GetScriptFieldTypeNames();
	// Whether `name` can name a C++ class or member: an identifier (ASCII letters, digits, '_', not starting with a digit)
	// of at most 128 characters that is no keyword and no reserved name ("__" anywhere, '_' and a capital letter first).
	bool IsValidScriptIdentifier(std::string_view name);
	// The classes, structs and enums the code below `directory` declares, and the ST_SCRIPT_CLASS registrations (code
	// files only, each read up to a bounded size; what comments and string literals mention is not declared).
	std::vector<std::string> FindDeclaredScriptTypes(const std::filesystem::path& directory);
	// Why `className` cannot name a new script class of the project, or nothing when it can: an invalid identifier, a name
	// the project's scripts declare already, or a name of the script SDK (which the scripts' `using namespace Strata`
	// would make ambiguous).
	std::optional<std::string> ValidateNewScriptClass(const Project& project, const std::filesystem::path& engineDirectory, std::string_view className);
	// The source of a new script class. Only the validated class and field names go into the code.
	std::string MakeScriptSource(std::string_view className, ScriptTemplate scriptTemplate, const std::vector<ScriptFieldDeclaration>& fields);

	// Writes the script build of a project into its script directory: the CMakeLists.txt and, with `includeExample`, the
	// example script. Files that exist are kept as they are. With `presetsSettings`, also the CMakePresets.json for IDEs
	// (UpdateScriptCMakePresets: written when missing or out of date and Strata's). Returns the files that were written.
	bool CreateScriptProjectFiles(const Project& project, bool includeExample, std::vector<std::filesystem::path>* outCreated = nullptr,
		std::string* outError = nullptr, const ScriptBuildSettings* presetsSettings = nullptr);

}
