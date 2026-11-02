#include <doctest/doctest.h>

#include "SpirvModule.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace Strata::ShaderTool;

namespace
{
	using Words = std::vector<uint32_t>;

	constexpr uint32_t c_Bound = 10;

	uint32_t Op(uint16_t opcode, uint32_t wordCount)
	{
		return (wordCount << 16) | opcode;
	}

	enum class PositionForm
	{
		Missing,
		Variable,
		BlockMember
	};

	// A small module (not a complete shader, but framed as SPIR-V): capability, memory model, an entry point of
	// `executionModel` named "main" with the output %2, the position decoration in `form`, and the output's types.
	Words MakeModule(uint32_t executionModel, PositionForm form)
	{
		Words words = { Spirv::c_MagicNumber, 0x00010500u, 0u, c_Bound, 0u };
		words.insert(words.end(), { Op(Spirv::c_OpCapability, 2), 1u });                      // Shader
		words.insert(words.end(), { Op(14, 3), 0u, 1u });                                    // Logical GLSL450
		words.insert(words.end(), { Op(Spirv::c_OpEntryPoint, 6), executionModel, 1u, 0x6E69616Du, 0u, 2u }); // "main", %2
		if (form == PositionForm::Variable)
			words.insert(words.end(), { Op(Spirv::c_OpDecorate, 4), 2u, Spirv::c_DecorationBuiltIn, Spirv::c_BuiltInPosition });
		if (form == PositionForm::BlockMember)
		{
			words.insert(words.end(), { Op(Spirv::c_OpMemberDecorate, 5), 3u, 0u, Spirv::c_DecorationBuiltIn, Spirv::c_BuiltInPosition });
			words.insert(words.end(), { Op(Spirv::c_OpDecorate, 3), 3u, 2u });                // Block
		}
		words.insert(words.end(), { Op(19, 2), 4u });                                        // %4 void
		words.insert(words.end(), { Op(22, 3), 5u, 32u });                                   // %5 float
		words.insert(words.end(), { Op(23, 4), 6u, 5u, 4u });                                // %6 float4
		words.insert(words.end(), { Op(32, 4), 7u, 3u, 6u });                               // %7 pointer to Output float4
		words.insert(words.end(), { Op(Spirv::c_OpVariable, 4), 7u, 2u, 3u });              // %2 Output
		return words;
	}

	std::optional<std::vector<SpirvInstruction>> Parse(const Words& words, std::string& error)
	{
		return ParseSpirv(words, error);
	}
}

TEST_SUITE("Renderer.ShaderTool")
{
	TEST_CASE("A module's framing is checked")
	{
		std::string error;
		const Words module = MakeModule(Spirv::c_ExecutionModelVertex, PositionForm::Variable);
		const std::optional<std::vector<SpirvInstruction>> instructions = Parse(module, error);
		REQUIRE(instructions.has_value());
		CHECK(instructions->size() == 9);
		CHECK(instructions->front().Opcode == Spirv::c_OpCapability);
		CHECK(instructions->front().Offset == 5);
		CHECK(instructions->front().Operands.size() == 1);

		Words wrongMagic = module;
		wrongMagic[0] = 0x03022307u;
		CHECK_FALSE(Parse(wrongMagic, error).has_value());
		CHECK(error.find("word 0") != std::string::npos);

		const Words shortHeader(module.begin(), module.begin() + 4);
		CHECK_FALSE(Parse(shortHeader, error).has_value());
		CHECK(error.find("word 4") != std::string::npos);

		Words zeroCount = module;
		zeroCount[7] = Op(14, 0);
		CHECK_FALSE(Parse(zeroCount, error).has_value());
		CHECK(error.find("word 7") != std::string::npos);

		Words pastTheEnd = module;
		pastTheEnd[pastTheEnd.size() - 4] = Op(Spirv::c_OpVariable, 5);
		CHECK_FALSE(Parse(pastTheEnd, error).has_value());
		CHECK(error.find("word " + std::to_string(pastTheEnd.size() - 4)) != std::string::npos);
	}

	TEST_CASE("Every truncation of a module is refused or parsed without reading past it")
	{
		Words module = MakeModule(Spirv::c_ExecutionModelVertex, PositionForm::Variable);
		while (module.size() < 40)
			module.push_back(Op(0, 1)); // OpNop
		REQUIRE(module.size() == 40);
		for (size_t length = 0; length < module.size(); length++)
		{
			// A copy of exactly `length` words, so that reading past it reads past an allocation.
			const Words truncated(module.begin(), module.begin() + static_cast<std::ptrdiff_t>(length));
			std::string error;
			const std::optional<std::vector<SpirvInstruction>> instructions = Parse(truncated, error);
			if (!instructions)
			{
				CHECK_FALSE(error.empty());
				continue;
			}
			for (const SpirvInstruction& instruction : *instructions)
				CHECK(instruction.Offset + 1 + instruction.Operands.size() <= length);
		}
	}

	TEST_CASE("A vertex shader's position is decorated invariant once")
	{
		Words module = MakeModule(Spirv::c_ExecutionModelVertex, PositionForm::Variable);
		const size_t builtInOffset = 16; // After the header (5), the capability (2), the memory model (3), the entry point (6)
		REQUIRE((module[builtInOffset] & 0xFFFFu) == Spirv::c_OpDecorate);
		const size_t originalSize = module.size();
		CHECK(DecoratePositionInvariant(module) == InvariantResult::Added);
		REQUIRE(module.size() == originalSize + 3);
		CHECK(module[builtInOffset + 4] == Op(Spirv::c_OpDecorate, 3));
		CHECK(module[builtInOffset + 5] == 2u);
		CHECK(module[builtInOffset + 6] == Spirv::c_DecorationInvariant);
		CHECK(module[3] == c_Bound);
		std::string error;
		CHECK(Parse(module, error).has_value());

		const Words decorated = module;
		CHECK(DecoratePositionInvariant(module) == InvariantResult::AlreadyInvariant);
		CHECK(module == decorated);
	}

	TEST_CASE("A position in a block is decorated through its member")
	{
		Words module = MakeModule(Spirv::c_ExecutionModelVertex, PositionForm::BlockMember);
		const size_t memberOffset = 16;
		REQUIRE((module[memberOffset] & 0xFFFFu) == Spirv::c_OpMemberDecorate);
		CHECK(DecoratePositionInvariant(module) == InvariantResult::Added);
		CHECK(module[memberOffset + 5] == Op(Spirv::c_OpMemberDecorate, 4));
		CHECK(module[memberOffset + 6] == 3u);
		CHECK(module[memberOffset + 7] == 0u);
		CHECK(module[memberOffset + 8] == Spirv::c_DecorationInvariant);
		CHECK(DecoratePositionInvariant(module) == InvariantResult::AlreadyInvariant);
	}

	TEST_CASE("Only a vertex shader with a position output is decorated")
	{
		Words fragment = MakeModule(Spirv::c_ExecutionModelFragment, PositionForm::Variable);
		const Words fragmentBefore = fragment;
		CHECK(DecoratePositionInvariant(fragment) == InvariantResult::NotVertexShader);
		CHECK(fragment == fragmentBefore);

		Words withoutPosition = MakeModule(Spirv::c_ExecutionModelVertex, PositionForm::Missing);
		const Words withoutPositionBefore = withoutPosition;
		CHECK(DecoratePositionInvariant(withoutPosition) == InvariantResult::NoPositionOutput);
		CHECK(withoutPosition == withoutPositionBefore);
	}

	TEST_CASE("The header holds the words, eight per line")
	{
		const Words words = { 0x07230203u, 0x00010500u, 0x0008000Bu, 0x0000002Au, 0u, 0x00020011u, 1u, 0xFFFFFFFFu, 0x10u, 0xABCDEF01u };
		const std::string header = FormatShaderHeader(words, "g_StrataShader_Test_vert", "Test.vert");
		CHECK(header ==
			"// Generated by StrataShaderTool (CMake/StrataShaders.cmake) from the shader Test.vert - do not edit.\n"
			"#pragma once\n"
			"\n"
			"#include <cstdint>\n"
			"\n"
			"const uint32_t g_StrataShader_Test_vert[] = {\n"
			"\t0x07230203,0x00010500,0x0008000b,0x0000002a,0x00000000,0x00020011,0x00000001,0xffffffff,\n"
			"\t0x00000010,0xabcdef01\n"
			"};\n");
	}
}
