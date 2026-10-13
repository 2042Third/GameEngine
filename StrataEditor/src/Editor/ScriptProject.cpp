#include "Editor/ScriptProject.h"

#include "Editor/ScriptBuild.h"
#include "Editor/ScriptSources.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Project/Project.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

namespace Strata
{

	namespace
	{

		// Project names and paths only go into comments of the generated CMake code: without line breaks (or other
		// control characters), which would end the comment. Projects reject them on load and save; this also covers
		// settings changed in memory.
		std::string ToCMakeComment(std::string_view text)
		{
			std::string comment(text);
			for (char& character : comment)
			{
				const auto value = static_cast<unsigned char>(character);
				if (value < 0x20 || value == 0x7F)
					character = ' ';
			}
			return comment;
		}

		constexpr size_t c_MaxIdentifierLength = 128;
		// Code files read when looking for declared names, up to this size each (generated or vendored code is larger).
		constexpr uint64_t c_MaxScannedFileSize = 4ull * 1024 * 1024;

		constexpr std::string_view c_Keywords[] = {
			"alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char", "char8_t", "char16_t",
			"char32_t", "class", "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return",
			"co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "final",
			"float", "for", "friend", "goto", "if", "import", "inline", "int", "long", "module", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
			"nullptr", "operator", "or", "or_eq", "override", "private", "protected", "public", "register", "reinterpret_cast", "requires", "return",
			"short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
			"true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor",
			"xor_eq", "std", "glm", "Strata" };

		// A field type of new scripts: its name in script.create, the C++ type and how a field of it starts.
		struct ScriptFieldType
		{
			std::string_view Name;
			std::string_view CppType;
			std::string_view Initializer;
		};

		constexpr ScriptFieldType c_ScriptFieldTypes[] = {
			{ "bool", "bool", " = false" },
			{ "int", "int32_t", " = 0" },
			{ "float", "float", " = 1.0f" },
			{ "vec2", "glm::vec2", " = glm::vec2(0.0f)" },
			{ "vec3", "glm::vec3", " = glm::vec3(0.0f)" },
			{ "vec4", "glm::vec4", " = glm::vec4(1.0f)" },
			{ "quat", "glm::quat", " = glm::quat(1.0f, 0.0f, 0.0f, 0.0f)" },
			{ "string", "std::string", "" },
			{ "entity", "Entity", "" },
			{ "asset", "AssetHandle", "" } };

		const ScriptFieldType* FindFieldType(std::string_view name)
		{
			for (const ScriptFieldType& type : c_ScriptFieldTypes)
			{
				if (StringUtils::EqualsIgnoreCase(type.Name, name))
					return &type;
			}
			return nullptr;
		}

		bool IsIdentifierCharacter(char character)
		{
			return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
		}

		// The identifier starting at `position` after blanks, or empty.
		std::string_view ReadIdentifier(std::string_view text, size_t position)
		{
			while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])))
				position++;
			const size_t start = position;
			while (position < text.size() && IsIdentifierCharacter(text[position]))
				position++;
			return text.substr(start, position - start);
		}

		// Whether the identifier characters right before `quote` are a character literal's encoding prefix (u8'a', u'a', U'a',
		// L'a') rather than the digits a digit separator follows.
		bool IsCharacterLiteralPrefix(std::string_view code, size_t quote)
		{
			size_t start = quote;
			while (start > 0 && IsIdentifierCharacter(code[start - 1]))
				start--;
			const std::string_view prefix = code.substr(start, quote - start);
			return prefix == "u8" || prefix == "u" || prefix == "U" || prefix == "L";
		}

		// The code of a C++ file with its comments and the contents of its string and character literals (raw ones too) blanked
		// out, so that only declarations are scanned: the SDK's usage comments mention ST_SCRIPT_CLASS(Player), which declares
		// nothing. Unterminated comments and literals run to the end of the text.
		std::string BlankCommentsAndLiterals(std::string_view text)
		{
			std::string code(text);
			size_t position = 0;
			auto blank = [&code](size_t from, size_t to)
			{
				for (size_t index = from; index < to && index < code.size(); index++)
				{
					if (code[index] != '\n')
						code[index] = ' ';
				}
			};
			while (position < code.size())
			{
				const char character = code[position];
				const char next = position + 1 < code.size() ? code[position + 1] : '\0';
				if (character == '/' && next == '/')
				{
					const size_t end = code.find('\n', position);
					blank(position, end == std::string::npos ? code.size() : end);
					position = end == std::string::npos ? code.size() : end;
				}
				else if (character == '/' && next == '*')
				{
					const size_t end = code.find("*/", position + 2);
					const size_t stop = end == std::string::npos ? code.size() : end + 2;
					blank(position, stop);
					position = stop;
				}
				else if (character == 'R' && next == '"' && (position == 0 || !IsIdentifierCharacter(code[position - 1])
					|| code[position - 1] == '8' || code[position - 1] == 'u' || code[position - 1] == 'U' || code[position - 1] == 'L'))
				{
					// R"delimiter( ... )delimiter": the delimiter is at most 16 characters.
					const size_t open = code.find('(', position + 2);
					if (open == std::string::npos || open - (position + 2) > 16)
					{
						position += 2;
						continue;
					}
					const std::string terminator = ")" + code.substr(position + 2, open - (position + 2)) + "\"";
					const size_t end = code.find(terminator, open + 1);
					const size_t stop = end == std::string::npos ? code.size() : end + terminator.size();
					blank(position + 1, stop);
					position = stop;
				}
				else if (character == '\'' && position > 0 && IsIdentifierCharacter(code[position - 1]) && !IsCharacterLiteralPrefix(code, position))
				{
					position++; // A digit separator (1'000'000)
				}
				else if (character == '"' || character == '\'')
				{
					size_t index = position + 1;
					while (index < code.size() && code[index] != character && code[index] != '\n')
						index += code[index] == '\\' ? 2 : 1;
					const size_t stop = std::min(index + 1, code.size());
					blank(position, stop);
					position = stop;
				}
				else
				{
					position++;
				}
			}
			return code;
		}

		// Names after the words class, struct and enum (also "enum class"), and in ST_SCRIPT_CLASS(...), in code: not in
		// comments or literals (BlankCommentsAndLiterals).
		void CollectDeclaredTypes(std::string_view source, std::vector<std::string>& outNames)
		{
			const std::string code = BlankCommentsAndLiterals(source);
			const std::string_view text = code;
			constexpr std::string_view c_Words[] = { "class", "struct", "enum", "ST_SCRIPT_CLASS(" };
			for (std::string_view word : c_Words)
			{
				for (size_t found = text.find(word); found != std::string_view::npos; found = text.find(word, found + 1))
				{
					// A whole word: not the end of a longer identifier, and (but for the macro) followed by a blank.
					if (found > 0 && IsIdentifierCharacter(text[found - 1]))
						continue;
					const size_t after = found + word.size();
					if (word.back() != '(' && (after >= text.size() || !std::isspace(static_cast<unsigned char>(text[after]))))
						continue;
					std::string_view name = ReadIdentifier(text, after);
					if (name == "class" || name == "struct")
						name = ReadIdentifier(text, text.find(name, after) + name.size());
					if (!name.empty() && !std::isdigit(static_cast<unsigned char>(name.front())))
						outNames.emplace_back(name);
				}
			}
		}

	}

	std::string MakeScriptCMakeLists(const Project& project)
	{
		// Code must only ever see a C identifier: settings changed in memory are not validated until the project is saved.
		std::string module = project.GetScriptModuleName();
		if (!Project::IsValidScriptModuleName(module))
			module = Project::MakeScriptModuleName(project.GetConfig().Name);
		return fmt::format(R"CMAKE(# Game scripts of "{0}": every .cpp and .h file in this directory (and below) belongs to the script module {1}.
# The editor builds it (script.build) with the engine's compiler and configuration and loads it from .strata/Scripts/Bin.
# Keep the module's name and output directory; to build by hand:
#   cmake -S {2} -B <build directory> -DSTRATA_ENGINE_DIR=<Strata folder> && cmake --build <build directory>
cmake_minimum_required(VERSION 3.25)
project({1} CXX)

set(STRATA_ENGINE_DIR "" CACHE PATH "The Strata folder, a source checkout or an installation (the editor sets it)")
find_package(StrataScriptCore CONFIG REQUIRED PATHS "${{STRATA_ENGINE_DIR}}/StrataScriptCore/CMake" NO_DEFAULT_PATH)
strata_add_script_module({1} SOURCE_DIR "${{CMAKE_CURRENT_SOURCE_DIR}}")
)CMAKE", ToCMakeComment(project.GetConfig().Name), module, ToCMakeComment(project.GetConfig().Scripts.SourceDirectory));
	}

	std::string MakeExampleScript()
	{
		return R"SCRIPT(// An example script. Attach it to an entity to make it spin: select the entity, Add Component > Script in the
// Inspector, and pick Spinner (or run the editor command
//   script.add {"entity": "<entity id>", "class": "Spinner", "fields": {"DegreesPerSecond": 45}}).
// Scripts see only the StrataScript SDK. How to create, build, debug and hot reload scripts, and to open them in an IDE:
// Docs/Manual/Scripting.md in the Strata engine's folder.

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

	std::vector<std::string> GetScriptFieldTypeNames()
	{
		std::vector<std::string> names;
		for (const ScriptFieldType& type : c_ScriptFieldTypes)
			names.emplace_back(type.Name);
		return names;
	}

	bool IsValidScriptIdentifier(std::string_view name)
	{
		if (name.empty() || name.size() > c_MaxIdentifierLength || std::isdigit(static_cast<unsigned char>(name.front())))
			return false;
		for (char character : name)
		{
			if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_')
				return false;
		}
		// Reserved for the implementation: "__" anywhere, or '_' and a capital letter first.
		if (name.find("__") != std::string_view::npos || (name.size() > 1 && name[0] == '_' && std::isupper(static_cast<unsigned char>(name[1]))))
			return false;
		return std::find(std::begin(c_Keywords), std::end(c_Keywords), name) == std::end(c_Keywords);
	}

	std::vector<std::string> FindDeclaredScriptTypes(const std::filesystem::path& directory)
	{
		std::vector<std::string> names;
		std::error_code error;
		std::filesystem::recursive_directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, error);
		for (; !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
		{
			std::error_code typeError;
			if (!it->is_regular_file(typeError))
				continue;
			if (!IsScriptCodeFileName(FileSystem::ToUTF8(it->path().filename())))
				continue;
			const std::optional<uint64_t> size = FileSystem::GetFileSize(it->path());
			if (!size || *size > c_MaxScannedFileSize)
				continue;
			if (const std::optional<std::string> text = FileSystem::ReadText(it->path()))
				CollectDeclaredTypes(*text, names);
		}
		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
		return names;
	}

	std::optional<std::string> ValidateNewScriptClass(const Project& project, const std::filesystem::path& engineDirectory, std::string_view className)
	{
		if (!IsValidScriptIdentifier(className))
		{
			return fmt::format("'{}' is not a C++ class name: use ASCII letters, digits and '_' (not first a digit, at most {} characters), and no "
				"keyword or reserved name", className, c_MaxIdentifierLength);
		}
		const std::vector<std::string> declared = FindDeclaredScriptTypes(project.GetScriptSourceDirectory());
		if (std::binary_search(declared.begin(), declared.end(), className))
			return fmt::format("The project's scripts declare '{}' already", className);
		// Names of the SDK would be ambiguous in scripts, which use namespace Strata.
		const std::vector<std::string> sdk = FindDeclaredScriptTypes(engineDirectory / "StrataScriptCore" / "Include" / "StrataScript");
		if (std::binary_search(sdk.begin(), sdk.end(), className))
			return fmt::format("'{}' is a name of the script SDK (Strata::{}); choose another", className, className);
		return std::nullopt;
	}

	std::string MakeScriptSource(std::string_view className, ScriptTemplate scriptTemplate, const std::vector<ScriptFieldDeclaration>& fields)
	{
		std::vector<ScriptFieldDeclaration> declared = fields;
		if (declared.empty() && scriptTemplate == ScriptTemplate::Behaviour)
			declared.push_back({ "Speed", "float" });

		std::string members;
		std::string registrations;
		for (const ScriptFieldDeclaration& field : declared)
		{
			const ScriptFieldType* type = FindFieldType(field.Type);
			if (!type || !IsValidScriptIdentifier(field.Name))
				continue; // Callers validate; code never receives what does not validate
			members += fmt::format("\t{} {}{};\n", type->CppType, field.Name, type->Initializer);
			registrations += fmt::format("\tST_SCRIPT_FIELD({});\n", field.Name);
		}

		std::string body = members;
		if (scriptTemplate == ScriptTemplate::Behaviour)
		{
			body += std::string(members.empty() ? "" : "\n") + R"CPP(	// When the entity starts playing (its fields hold the scene's values by now).
	void OnCreate() override
	{
	}

	// Every frame; deltaTime is the frame's game time in seconds.
	void OnUpdate([[maybe_unused]] float deltaTime) override
	{
	}
)CPP";
		}

		const std::string header = fmt::format(R"CPP(// {0}: a script class. Once the scripts are built with it (Scripts > Build Scripts; New Script starts a build), attach
// it to an entity (Inspector > Add Component > Script, or the command script.add); Play rebuilds the scripts when they
// changed. Fields (ST_SCRIPT_FIELD) are set per entity in the Inspector and kept across hot reloads.
// Docs/Manual/Scripting.md in the Strata engine's folder describes the SDK, building, debugging and hot reload.

#include "StrataScript/StrataScript.h"

using namespace Strata;

class {0} : public Script
{{
public:
)CPP", className);
		return header + body + fmt::format("}};\n\nST_SCRIPT_CLASS({})\n{{\n{}}}\n", className, registrations);
	}

	bool CreateScriptProjectFiles(const Project& project, bool includeExample, std::vector<std::filesystem::path>* outCreated, std::string* outError,
		const ScriptBuildSettings* presetsSettings)
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

		if (presetsSettings)
		{
			bool written = false;
			if (!UpdateScriptCMakePresets(project, *presetsSettings, &written, outError))
				return false;
			if (written && outCreated)
				outCreated->push_back(directory / "CMakePresets.json");
		}
		return true;
	}

}
