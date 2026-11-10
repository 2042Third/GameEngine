#include <doctest/doctest.h>

#include "SpirvModule.h"
#include "Strata/Renderer/EmbeddedShaders.h"
#include "Strata/Renderer/SceneVertexLayouts.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstdint>
#include <map>
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

	// A resource an entry point uses: a variable of its interface (SPIR-V 1.4 and later list every global the entry point
	// reaches) that carries a binding, with its descriptor set (0 without the decoration).
	struct UsedBinding
	{
		uint32_t Variable = 0;
		uint32_t Set = 0;
		uint32_t Binding = 0;
	};

	std::vector<UsedBinding> FindUsedBindings(const ParsedShader& shader)
	{
		std::set<uint32_t> interfaceVariables;
		std::map<uint32_t, uint32_t> descriptorSets;
		std::map<uint32_t, uint32_t> bindings;
		for (const SpirvInstruction& instruction : shader.Instructions)
		{
			const std::span<const uint32_t> operands = instruction.Operands;
			if (instruction.Opcode == Spirv::c_OpEntryPoint && operands.size() >= 3)
			{
				size_t nameWords = 0;
				Spirv::ReadLiteralString(operands, 2, nameWords);
				for (size_t index = 2 + nameWords; nameWords > 0 && index < operands.size(); index++)
					interfaceVariables.insert(operands[index]);
			}
			if (instruction.Opcode == Spirv::c_OpDecorate && operands.size() >= 3)
			{
				if (operands[1] == Spirv::c_DecorationDescriptorSet)
					descriptorSets[operands[0]] = operands[2];
				else if (operands[1] == Spirv::c_DecorationBinding)
					bindings[operands[0]] = operands[2];
			}
		}
		std::vector<UsedBinding> used;
		for (const std::pair<const uint32_t, uint32_t>& binding : bindings)
		{
			if (!interfaceVariables.contains(binding.first))
				continue;
			const auto set = descriptorSets.find(binding.first);
			used.push_back({ binding.first, set != descriptorSets.end() ? set->second : 0u, binding.second });
		}
		return used;
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

	// SPIR-V numbers the input check reads besides SpirvModule.h's.
	constexpr uint16_t c_OpTypeInt = 21;
	constexpr uint16_t c_OpTypeFloat = 22;
	constexpr uint16_t c_OpTypeVector = 23;
	constexpr uint16_t c_OpTypePointer = 32;
	constexpr uint32_t c_StorageClassInput = 1;

	// The shape a vertex input has in the shader: its components and whether they are integers.
	struct ShaderInputType
	{
		uint32_t Components = 0;
		bool Integer = false;
	};

	// What each scene vertex attribute's format (SceneRenderer.cpp's GetVertexAttributeDesc) gives the shader.
	ShaderInputType GetExpectedInputType(SceneVertexAttribute attribute)
	{
		switch (attribute)
		{
			case SceneVertexAttribute::Position: return { 3, false }; // RGB32_FLOAT
			case SceneVertexAttribute::Normal:   return { 3, false }; // RGB32_FLOAT
			case SceneVertexAttribute::Tangent:  return { 4, false }; // RGBA32_FLOAT
			case SceneVertexAttribute::TexCoord: return { 2, false }; // RG32_FLOAT
			case SceneVertexAttribute::Joints:   return { 4, true };  // RGBA16_UINT
			case SceneVertexAttribute::Weights:  return { 4, false }; // RGBA16_UNORM
		}
		return {};
	}

	// The entry point's Input variables that carry a Location (built-ins carry none), by location, with their shapes. A
	// type the module declares as neither a scalar nor a vector reads as 0 components.
	std::map<uint32_t, ShaderInputType> FindInputLocations(const ParsedShader& shader)
	{
		std::set<uint32_t> interfaceVariables;
		std::map<uint32_t, uint32_t> locations;      // Variable -> its Location
		std::map<uint32_t, uint32_t> inputPointers;  // Input variable -> its pointer type
		std::map<uint32_t, uint32_t> pointees;       // Pointer type -> the type it points to
		std::map<uint32_t, ShaderInputType> shapes;  // Scalar or vector type -> its shape
		for (const SpirvInstruction& instruction : shader.Instructions)
		{
			const std::span<const uint32_t> operands = instruction.Operands;
			if (instruction.Opcode == Spirv::c_OpEntryPoint && operands.size() >= 3)
			{
				size_t nameWords = 0;
				Spirv::ReadLiteralString(operands, 2, nameWords);
				for (size_t index = 2 + nameWords; nameWords > 0 && index < operands.size(); index++)
					interfaceVariables.insert(operands[index]);
			}
			else if (instruction.Opcode == Spirv::c_OpDecorate && operands.size() >= 3 && operands[1] == Spirv::c_DecorationLocation)
				locations[operands[0]] = operands[2];
			else if (instruction.Opcode == Spirv::c_OpVariable && operands.size() >= 3 && operands[2] == c_StorageClassInput)
				inputPointers[operands[1]] = operands[0];
			else if (instruction.Opcode == c_OpTypePointer && operands.size() >= 3)
				pointees[operands[0]] = operands[2];
			else if (instruction.Opcode == c_OpTypeInt && !operands.empty())
				shapes[operands[0]] = { 1, true };
			else if (instruction.Opcode == c_OpTypeFloat && !operands.empty())
				shapes[operands[0]] = { 1, false };
			else if (instruction.Opcode == c_OpTypeVector && operands.size() >= 3)
			{
				// SPIR-V declares a vector's component type before the vector.
				const auto component = shapes.find(operands[1]);
				if (component != shapes.end())
					shapes[operands[0]] = { operands[2], component->second.Integer };
			}
		}
		std::map<uint32_t, ShaderInputType> inputs;
		for (const std::pair<const uint32_t, uint32_t>& variable : inputPointers)
		{
			const auto location = locations.find(variable.first);
			if (!interfaceVariables.contains(variable.first) || location == locations.end())
				continue;
			ShaderInputType shape;
			const auto pointee = pointees.find(variable.second);
			if (pointee != pointees.end())
			{
				const auto found = shapes.find(pointee->second);
				if (found != shapes.end())
					shape = found->second;
			}
			inputs[location->second] = shape;
		}
		return inputs;
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

	TEST_CASE("Scene vertex shaders read exactly the attributes of their input layout")
	{
		// The validation layer only warns about an attribute no vertex shader consumes, and the GPU suites count errors:
		// this keeps each scene pipeline's layout (SceneVertexLayouts.h, chosen by the pipeline's vertex shader) equal to
		// what the shader reads, location by location (NVRHI numbers a layout's attributes in order).
		const std::vector<ParsedShader> shaders = ParseEmbeddedShaders();
		for (const SceneVertexEntry& entry : c_SceneVertexEntries)
		{
			INFO(std::string(entry.Shader));
			const auto shader = std::find_if(shaders.begin(), shaders.end(), [&](const ParsedShader& parsed) { return parsed.Name == entry.Shader; });
			REQUIRE(shader != shaders.end());
			const std::map<uint32_t, ShaderInputType> inputs = FindInputLocations(*shader);
			const std::span<const SceneVertexAttribute> attributes = GetSceneVertexAttributes(entry.Layout);
			for (const std::pair<const uint32_t, ShaderInputType>& input : inputs)
			{
				if (input.first >= attributes.size())
					FAIL_CHECK(fmt::format("{} reads location {}, which its layout does not provide", entry.Shader, input.first));
			}
			for (size_t location = 0; location < attributes.size(); location++)
			{
				INFO("location ", location);
				const auto input = inputs.find(static_cast<uint32_t>(location));
				if (input == inputs.end())
				{
					FAIL_CHECK(fmt::format("{} does not read location {} of its layout: the validation layer warns that the attribute is not "
						"consumed by the vertex shader", entry.Shader, location));
					continue;
				}
				const ShaderInputType expected = GetExpectedInputType(attributes[location]);
				CHECK(input->second.Components == expected.Components);
				CHECK(input->second.Integer == expected.Integer);
			}
		}
		// Every scene vertex shader is listed, so that GetScenePipeline finds its layout.
		for (const ParsedShader& shader : shaders)
		{
			if (StartsWith(shader.Name, "Scene/") && EndsWith(shader.Name, ".vert"))
				CHECK_MESSAGE(FindSceneVertexLayout(shader.Name).has_value(), shader.Name, " is not in c_SceneVertexEntries");
		}
	}

	TEST_CASE("No embedded shader binds two used variables to one slot")
	{
		// What makes disabling Slang's warning 39001 (explicit binding overlap) safe: passes that include a header for its
		// helpers declare resources at slots the header gives to others, but no entry point uses two of them.
		for (const ParsedShader& shader : ParseEmbeddedShaders())
		{
			INFO(shader.Name);
			std::map<std::pair<uint32_t, uint32_t>, uint32_t> slots;
			for (const UsedBinding& binding : FindUsedBindings(shader))
			{
				const auto inserted = slots.emplace(std::pair<uint32_t, uint32_t> { binding.Set, binding.Binding }, binding.Variable);
				if (!inserted.second)
				{
					FAIL_CHECK(fmt::format("{} binds %{} and %{} to set {} binding {}", shader.Name, inserted.first->second, binding.Variable,
						binding.Set, binding.Binding));
				}
			}
		}
	}

	TEST_CASE("Only the forward shaders' field variants read global illumination's field")
	{
		// A frame without global illumination draws with forward shaders that hold nothing of the field's gather (Scene/
		// Forward.frag.slang's ST_FORWARD_FIELD, SceneRenderer's PipelineKey::Field): compiled into the one shader behind a
		// branch such a frame never took, the gather still cost its opaque pass 6-11% of its time at 3840 x 2160. So the six
		// variants without the field use none of its slots of the scene layout (Include/GIField.slang: SRV 35, 37-40, 54 and
		// 55, CBV 3), and their six twins read it. The slots as NVRHI's Vulkan binding offsets place them
		// (CMake/StrataShaders.cmake): register(tN) is binding N, register(bN) binding 256 + N.
		constexpr uint32_t c_FieldConstants = 256 + 3;
		const std::set<uint32_t> fieldBindings = { 35, 37, 38, 39, 40, 54, 55, c_FieldConstants };
		size_t plainCount = 0;
		size_t fieldCount = 0;
		for (const ParsedShader& shader : ParseEmbeddedShaders())
		{
			if (!StartsWith(shader.Name, "Scene/Forward") || !EndsWith(shader.Name, ".frag"))
				continue;
			INFO(shader.Name);
			std::set<uint32_t> used;
			for (const UsedBinding& binding : FindUsedBindings(shader))
			{
				if (binding.Set == 0 && fieldBindings.contains(binding.Binding))
					used.insert(binding.Binding);
			}
			if (EndsWith(shader.Name, "Field.frag"))
			{
				fieldCount++;
				// What every read of the field takes: its constants, the probes' harmonics and their ambient cubes.
				CHECK(used.contains(c_FieldConstants));
				CHECK(used.contains(35));
				CHECK(used.contains(55));
			}
			else
			{
				plainCount++;
				CHECK_MESSAGE(used.empty(), shader.Name, " uses ", used.size(), " of the field's slots, the first at binding ",
					used.empty() ? 0u : *used.begin());
			}
		}
		// Opaque, alpha-blended and the glass's four (with and without entity ids and motion), each with its twin.
		CHECK(plainCount == 6);
		CHECK(fieldCount == 6);
	}
}
