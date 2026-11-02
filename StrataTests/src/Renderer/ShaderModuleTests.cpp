#include <doctest/doctest.h>

#include "SpirvModule.h"
#include "Strata/Renderer/EmbeddedShaders.h"

#include <spdlog/fmt/fmt.h>

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strata;
using namespace Strata::ShaderTool;

// Checks every shader module the engine embeds, as StrataShaderTool wrote it (Docs/DeveloperGuide.md, "Shaders"): what
// the compiler emits must suit every device the engine accepts and the passes that rely on decorations.
namespace
{
	struct ParsedShader
	{
		std::string Name;
		std::span<const uint32_t> Words;
		std::vector<SpirvInstruction> Instructions;
	};

	std::vector<ParsedShader> ParseEmbeddedShaders()
	{
		std::vector<ParsedShader> shaders;
		for (const EmbeddedShader& shader : EmbeddedShaders::GetAll())
		{
			ParsedShader parsed;
			parsed.Name = std::string(shader.Name);
			parsed.Words = std::span<const uint32_t>(shader.Code, shader.Size / sizeof(uint32_t));
			std::string error;
			std::optional<std::vector<SpirvInstruction>> instructions = ParseSpirv(parsed.Words, error);
			INFO(parsed.Name, ": ", error);
			CHECK(shader.Size % sizeof(uint32_t) == 0);
			REQUIRE(instructions.has_value());
			parsed.Instructions = std::move(*instructions);
			shaders.push_back(std::move(parsed));
		}
		return shaders;
	}

	bool EndsWith(std::string_view text, std::string_view suffix)
	{
		return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
	}

	bool StartsWith(std::string_view text, std::string_view prefix)
	{
		return text.substr(0, prefix.size()) == prefix;
	}

	// The capabilities any embedded shader may declare: what core Vulkan 1.2 always supports and what device selection
	// requires (Platform/Vulkan/VulkanGraphicsDevice.cpp rejects a device without them).
	struct AllowedCapability
	{
		uint32_t Capability;
		const char* Name;
	};
	constexpr AllowedCapability c_AllowedCapabilities[] = {
		{ 1, "Shader" },                              // Every Vulkan shader
		{ 0, "Matrix" },                              // Implied by Shader
		{ 50, "ImageQuery" },                         // Core Vulkan, no feature
		{ 51, "DerivativeControl" },                  // Core Vulkan, no feature
		{ 5302, "RuntimeDescriptorArray" },           // Descriptor indexing, required (the bindless table)
		{ 5307, "SampledImageArrayNonUniformIndexing" }, // Descriptor indexing, required
		{ 5301, "ShaderNonUniform" },                 // Descriptor indexing, required
		{ 45, "SampledCubeArray" },                   // imageCubeArray, required (reflection probes)
		{ 34, "ImageCubeArray" },                     // imageCubeArray, required
	};

	constexpr uint32_t c_StorageImageExtendedFormats = 49;

	// The entries that may declare StorageImageExtendedFormats (shaderStorageImageExtendedFormats is enabled only where the
	// device has it), each with its reason.
	constexpr std::string_view c_ExtendedFormatEntries[] = {
		"Effects/GTAOHalf.comp",      // Runs only when SupportsExtendedStorageFormats (SceneRendererAmbientOcclusion.cpp)
		"Effects/AODenoiseHalf.comp", // Likewise
		"Effects/SceneColorCopy.comp", // Likewise (SceneRendererGlass.cpp); SceneColorCopyFallback.comp is the other path
		"IBL/BRDFLut.comp",           // Writes its RG16F table always: device selection does not check the feature, a
		                              // pre-existing gap recorded in Docs/Roadmap.md's open items
	};

	// The extensions any embedded shader may declare.
	constexpr std::string_view c_AllowedExtensions[] = {
		"SPV_KHR_storage_buffer_storage_class", // Core since SPIR-V 1.3
	};

	std::string CapabilityHint(uint32_t capability)
	{
		switch (capability)
		{
			case 9:    // Float16
			case 22:   // Int16
				return "f32tof16 lowers to 16-bit types: use PackHalf2x16 of Include/Common.slang";
			case 5288: // ComputeDerivativeGroupQuadsKHR
			case 5350: // ComputeDerivativeGroupLinearKHR
				return "Sample in a compute shader: use SampleLevel";
			case 55:   // StorageImageReadWithoutFormat
			case 56:   // StorageImageWriteWithoutFormat
				return "give the storage image [vk::image_format]";
			case 4427: // DrawParameters
				return "SV_InstanceID excludes the base instance: use SV_VulkanInstanceID";
			case c_StorageImageExtendedFormats:
				return "an extended storage format needs a fallback for devices without shaderStorageImageExtendedFormats "
					"(SupportsExtendedStorageFormats), then a line here";
			default:
				return "only a capability every accepted device enables may be added to the allow-list";
		}
	}

	// Whether the module decorates a BuiltIn Position Invariant (as a variable or as a block member), and whether it
	// decorates anything Invariant at all.
	std::pair<bool, bool> FindInvariantPosition(const ParsedShader& shader)
	{
		std::set<uint32_t> positionVariables;
		std::set<std::pair<uint32_t, uint32_t>> positionMembers;
		std::set<uint32_t> invariantVariables;
		std::set<std::pair<uint32_t, uint32_t>> invariantMembers;
		for (const SpirvInstruction& instruction : shader.Instructions)
		{
			const std::span<const uint32_t> operands = instruction.Operands;
			if (instruction.Opcode == Spirv::c_OpDecorate && operands.size() >= 2)
			{
				if (operands[1] == Spirv::c_DecorationInvariant)
					invariantVariables.insert(operands[0]);
				if (operands.size() >= 3 && operands[1] == Spirv::c_DecorationBuiltIn && operands[2] == Spirv::c_BuiltInPosition)
					positionVariables.insert(operands[0]);
			}
			if (instruction.Opcode == Spirv::c_OpMemberDecorate && operands.size() >= 3)
			{
				if (operands[2] == Spirv::c_DecorationInvariant)
					invariantMembers.insert({ operands[0], operands[1] });
				if (operands.size() >= 4 && operands[2] == Spirv::c_DecorationBuiltIn && operands[3] == Spirv::c_BuiltInPosition)
					positionMembers.insert({ operands[0], operands[1] });
			}
		}
		bool invariantPosition = false;
		for (uint32_t variable : positionVariables)
			invariantPosition = invariantPosition || invariantVariables.contains(variable);
		for (const std::pair<uint32_t, uint32_t>& member : positionMembers)
			invariantPosition = invariantPosition || invariantMembers.contains(member);
		return { invariantPosition, !invariantVariables.empty() || !invariantMembers.empty() };
	}
}

TEST_SUITE("Renderer.ShaderModules")
{
	TEST_CASE("Every embedded shader is SPIR-V 1.5 with entry point main of its name's stage")
	{
		const std::vector<ParsedShader> shaders = ParseEmbeddedShaders();
		REQUIRE_FALSE(shaders.empty());
		for (const ParsedShader& shader : shaders)
		{
			INFO(shader.Name);
			CHECK(shader.Words[1] == 0x00010500u);
			uint32_t expectedModel = 0;
			if (EndsWith(shader.Name, ".vert"))
				expectedModel = Spirv::c_ExecutionModelVertex;
			else if (EndsWith(shader.Name, ".frag"))
				expectedModel = Spirv::c_ExecutionModelFragment;
			else if (EndsWith(shader.Name, ".comp"))
				expectedModel = Spirv::c_ExecutionModelGLCompute;
			else
				FAIL("the name has no stage suffix the engine uses");

			int entryPoints = 0;
			for (const SpirvInstruction& instruction : shader.Instructions)
			{
				if (instruction.Opcode != Spirv::c_OpEntryPoint)
					continue;
				entryPoints++;
				REQUIRE(instruction.Operands.size() >= 3);
				CHECK(instruction.Operands[0] == expectedModel);
				size_t nameWords = 0;
				CHECK(Spirv::ReadLiteralString(instruction.Operands, 2, nameWords) == "main");
			}
			CHECK(entryPoints == 1);
		}
	}

	TEST_CASE("Every embedded shader declares only capabilities and extensions every accepted device enables")
	{
		for (const ParsedShader& shader : ParseEmbeddedShaders())
		{
			INFO(shader.Name);
			bool extendedFormatsAllowed = false;
			for (std::string_view entry : c_ExtendedFormatEntries)
				extendedFormatsAllowed = extendedFormatsAllowed || entry == shader.Name;
			for (const SpirvInstruction& instruction : shader.Instructions)
			{
				if (instruction.Opcode == Spirv::c_OpCapability && !instruction.Operands.empty())
				{
					const uint32_t capability = instruction.Operands[0];
					bool allowed = capability == c_StorageImageExtendedFormats && extendedFormatsAllowed;
					for (const AllowedCapability& entry : c_AllowedCapabilities)
						allowed = allowed || entry.Capability == capability;
					if (!allowed)
						FAIL_CHECK(fmt::format("{} declares capability {}: {}", shader.Name, capability, CapabilityHint(capability)));
				}
				if (instruction.Opcode == Spirv::c_OpExtension)
				{
					size_t words = 0;
					const std::string extension = Spirv::ReadLiteralString(instruction.Operands, 0, words);
					bool allowed = false;
					for (std::string_view entry : c_AllowedExtensions)
						allowed = allowed || entry == extension;
					if (!allowed)
						FAIL_CHECK(fmt::format("{} declares extension '{}', which no accepted device is known to support", shader.Name, extension));
				}
			}
		}
	}

	TEST_CASE("The depth prepass's vertex shaders keep their position invariant")
	{
		int meshEntries = 0;
		for (const ParsedShader& shader : ParseEmbeddedShaders())
		{
			INFO(shader.Name);
			const std::pair<bool, bool> invariant = FindInvariantPosition(shader);
			if (StartsWith(shader.Name, "Scene/Mesh") && EndsWith(shader.Name, ".vert"))
			{
				meshEntries++;
				CHECK(invariant.first);
			}
			else
			{
				CHECK_FALSE(invariant.second);
			}
		}
		CHECK(meshEntries == 4);
	}
}
