#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The SPIR-V reading and patching of StrataShaderTool, the host tool that turns the compiler's module of every shader
// into the module and header the engine embeds (CMake/StrataShaders.cmake). Standard library only: it runs on the build
// machine and links nothing of the engine. Renderer.ShaderTool tests it, Renderer.ShaderModules reads the embedded
// modules with it.
namespace Strata::ShaderTool
{

	struct SpirvInstruction
	{
		uint16_t Opcode = 0;
		size_t Offset = 0;                   // Word index of the instruction in the module
		std::span<const uint32_t> Operands;  // The words after the first
	};

	// The module's instructions after its five-word header, or nullopt with `error` naming the first problem: no SPIR-V
	// magic number, fewer than five header words, an instruction whose word count is 0 or runs past the module's end.
	// The operands point into `words`.
	std::optional<std::vector<SpirvInstruction>> ParseSpirv(std::span<const uint32_t> words, std::string& error);

	// The file's words, or nullopt with `error` (unreadable, empty, size not a multiple of four).
	std::optional<std::vector<uint32_t>> ReadSpirvFile(const std::filesystem::path& path, std::string& error);

	enum class InvariantResult
	{
		Added,
		AlreadyInvariant,
		NotVertexShader,
		NoPositionOutput
	};

	// Decorates the vertex shader's BuiltIn Position output Invariant: after each `OpDecorate %v BuiltIn Position` an
	// `OpDecorate %v Invariant`, after each `OpMemberDecorate %s m BuiltIn Position` an `OpMemberDecorate %s m Invariant`,
	// unless that decoration exists. Slang has no way to say `invariant`. A module with an entry point that is not a
	// vertex shader (or with none) is NotVertexShader; `words` must parse (a module that does not is NoPositionOutput).
	InvariantResult DecoratePositionInvariant(std::vector<uint32_t>& words);

	// The header the registry includes: `const uint32_t <variableName>[] = { ... };`, eight words per line.
	std::string FormatShaderHeader(std::span<const uint32_t> words, std::string_view variableName, std::string_view shaderName);

	// SPIR-V's numbers the tool and its tests use.
	namespace Spirv
	{
		constexpr uint32_t c_MagicNumber = 0x07230203;
		constexpr size_t c_HeaderWords = 5;
		constexpr uint16_t c_OpCapability = 17;
		constexpr uint16_t c_OpExtension = 10;
		constexpr uint16_t c_OpEntryPoint = 15;
		constexpr uint16_t c_OpDecorate = 71;
		constexpr uint16_t c_OpMemberDecorate = 72;
		constexpr uint16_t c_OpVariable = 59;
		constexpr uint32_t c_DecorationBuiltIn = 11;
		constexpr uint32_t c_DecorationInvariant = 18;
		constexpr uint32_t c_DecorationNoContraction = 42;
		constexpr uint32_t c_DecorationLocation = 30;
		constexpr uint32_t c_DecorationBinding = 33;
		constexpr uint32_t c_DecorationDescriptorSet = 34;
		constexpr uint32_t c_BuiltInPosition = 0;
		constexpr uint32_t c_ExecutionModelVertex = 0;
		constexpr uint32_t c_ExecutionModelFragment = 4;
		constexpr uint32_t c_ExecutionModelGLCompute = 5;

		// The literal string that starts at `operands[first]` (nul-terminated, four characters per word, little-endian),
		// and in `wordCount` the words it takes; empty with 0 words when it does not end inside the operands.
		std::string ReadLiteralString(std::span<const uint32_t> operands, size_t first, size_t& wordCount);
	}

}
