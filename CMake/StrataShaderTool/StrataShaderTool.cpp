// StrataShaderTool: turns the shader compiler's module of one shader into what the engine embeds (CMake/StrataShaders.cmake,
// Docs/DeveloperGuide.md "Shaders"). It reads and checks the module, decorates the vertex position Invariant when asked
// (the compiler has no way to say it, and the depth prepass's EQUAL test needs it), and writes the final module and the
// header the shader registry includes.
//
// StrataShaderTool --input <raw.spv> --spirv <out.spv> --header <out.h> --variable <identifier> --name <registered name>
//                  [--invariant-position]
// Exit 0 on success, 1 when the module is refused or a file cannot be written, 2 on bad arguments.

#include "SpirvModule.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
	using namespace Strata::ShaderTool;

	struct Arguments
	{
		std::filesystem::path Input;
		std::filesystem::path Spirv;
		std::filesystem::path Header;
		std::string Variable;
		std::string Name;
		bool InvariantPosition = false;
	};

	void PrintUsage()
	{
		std::fputs("Usage: StrataShaderTool --input <raw.spv> --spirv <out.spv> --header <out.h> --variable <identifier> "
			"--name <registered name> [--invariant-position]\n", stderr);
	}

	std::optional<Arguments> ParseArguments(int argc, char** argv)
	{
		Arguments arguments;
		for (int index = 1; index < argc; index++)
		{
			const std::string_view argument = argv[index];
			if (argument == "--invariant-position")
			{
				arguments.InvariantPosition = true;
				continue;
			}
			if (index + 1 >= argc)
			{
				std::fprintf(stderr, "StrataShaderTool: %s needs a value\n", argv[index]);
				return std::nullopt;
			}
			const char* value = argv[++index];
			if (argument == "--input")
				arguments.Input = value;
			else if (argument == "--spirv")
				arguments.Spirv = value;
			else if (argument == "--header")
				arguments.Header = value;
			else if (argument == "--variable")
				arguments.Variable = value;
			else if (argument == "--name")
				arguments.Name = value;
			else
			{
				std::fprintf(stderr, "StrataShaderTool: unknown argument %s\n", argv[index - 1]);
				return std::nullopt;
			}
		}
		if (arguments.Input.empty() || arguments.Spirv.empty() || arguments.Header.empty() || arguments.Variable.empty() ||
			arguments.Name.empty())
		{
			std::fputs("StrataShaderTool: --input, --spirv, --header, --variable and --name are required\n", stderr);
			return std::nullopt;
		}
		return arguments;
	}

	// Writes `size` bytes to `path` through a temporary file renamed into place, so a failed write leaves no output a
	// later build would take as current.
	bool WriteFileAtomically(const std::filesystem::path& path, const char* data, size_t size, std::string& error)
	{
		std::filesystem::path temporary = path;
		temporary += ".tmp";
		{
			std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
			if (!file)
			{
				error = "cannot be created";
				return false;
			}
			file.write(data, static_cast<std::streamsize>(size));
			file.close();
			if (!file)
			{
				error = "cannot be written";
				std::error_code ignored;
				std::filesystem::remove(temporary, ignored);
				return false;
			}
		}
		std::error_code renameError;
		std::filesystem::rename(temporary, path, renameError);
		if (renameError)
		{
			error = "cannot be replaced: " + renameError.message();
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			return false;
		}
		return true;
	}

	int Fail(const std::string& name, const std::string& reason)
	{
		std::fprintf(stderr, "StrataShaderTool: %s: %s\n", name.c_str(), reason.c_str());
		return 1;
	}
}

int main(int argc, char** argv)
{
	const std::optional<Arguments> arguments = ParseArguments(argc, argv);
	if (!arguments)
	{
		PrintUsage();
		return 2;
	}

	std::string error;
	std::optional<std::vector<uint32_t>> words = ReadSpirvFile(arguments->Input, error);
	if (!words)
		return Fail(arguments->Name, "the compiler's module cannot be read: " + error);
	if (!ParseSpirv(*words, error))
		return Fail(arguments->Name, "the compiler's module is not SPIR-V: " + error);

	if (arguments->InvariantPosition)
	{
		switch (DecoratePositionInvariant(*words))
		{
			case InvariantResult::Added:
			case InvariantResult::AlreadyInvariant:
				break;
			case InvariantResult::NotVertexShader:
				return Fail(arguments->Name, "INVARIANT_POSITION names it, but it is not a vertex shader");
			case InvariantResult::NoPositionOutput:
				return Fail(arguments->Name, "it has no BuiltIn Position output to decorate Invariant (did the compiler change how it "
					"emits the position?)");
		}
	}

	if (!WriteFileAtomically(arguments->Spirv, reinterpret_cast<const char*>(words->data()), words->size() * sizeof(uint32_t), error))
		return Fail(arguments->Name, "the module " + error);
	const std::string header = FormatShaderHeader(*words, arguments->Variable, arguments->Name);
	if (!WriteFileAtomically(arguments->Header, header.data(), header.size(), error))
		return Fail(arguments->Name, "the header " + error);
	return 0;
}
