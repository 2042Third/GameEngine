#include "SpirvModule.h"

#include <fstream>
#include <set>
#include <utility>

namespace Strata::ShaderTool
{

	namespace
	{
		std::string Hex(uint32_t value)
		{
			static constexpr char c_Digits[] = "0123456789abcdef";
			std::string text = "0x00000000";
			for (int digit = 0; digit < 8; digit++)
				text[9 - digit] = c_Digits[(value >> (digit * 4)) & 0xFu];
			return text;
		}

		uint32_t MakeOpcodeWord(uint16_t opcode, uint32_t wordCount)
		{
			return (wordCount << 16) | opcode;
		}
	}

	std::optional<std::vector<SpirvInstruction>> ParseSpirv(std::span<const uint32_t> words, std::string& error)
	{
		if (words.empty())
		{
			error = "the module is empty, so it has no SPIR-V magic number";
			return std::nullopt;
		}
		if (words[0] != Spirv::c_MagicNumber)
		{
			error = "word 0 is " + Hex(words[0]) + ", not the SPIR-V magic number " + Hex(Spirv::c_MagicNumber);
			return std::nullopt;
		}
		if (words.size() < Spirv::c_HeaderWords)
		{
			error = "the module ends at word " + std::to_string(words.size()) + ", inside its five-word header";
			return std::nullopt;
		}

		std::vector<SpirvInstruction> instructions;
		size_t offset = Spirv::c_HeaderWords;
		while (offset < words.size())
		{
			const uint32_t wordCount = words[offset] >> 16;
			if (wordCount == 0)
			{
				error = "the instruction at word " + std::to_string(offset) + " has a word count of 0";
				return std::nullopt;
			}
			if (wordCount > words.size() - offset)
			{
				error = "the instruction at word " + std::to_string(offset) + " has " + std::to_string(wordCount) +
					" words and runs past the module's end at word " + std::to_string(words.size());
				return std::nullopt;
			}
			instructions.push_back({ static_cast<uint16_t>(words[offset] & 0xFFFFu), offset, words.subspan(offset + 1, wordCount - 1) });
			offset += wordCount;
		}
		return instructions;
	}

	std::optional<std::vector<uint32_t>> ReadSpirvFile(const std::filesystem::path& path, std::string& error)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file)
		{
			error = "the file cannot be opened";
			return std::nullopt;
		}
		const std::streamoff size = file.tellg();
		if (size < 0)
		{
			error = "the file's size cannot be read";
			return std::nullopt;
		}
		if (size == 0)
		{
			error = "the file is empty";
			return std::nullopt;
		}
		if (size % 4 != 0)
		{
			error = "the file has " + std::to_string(size) + " bytes, not a multiple of 4";
			return std::nullopt;
		}
		std::vector<uint32_t> words(static_cast<size_t>(size / 4));
		file.seekg(0);
		if (!file.read(reinterpret_cast<char*>(words.data()), size))
		{
			error = "the file cannot be read";
			return std::nullopt;
		}
		return words;
	}

	InvariantResult DecoratePositionInvariant(std::vector<uint32_t>& words)
	{
		std::string error;
		const std::optional<std::vector<SpirvInstruction>> instructions = ParseSpirv(words, error);
		if (!instructions)
			return InvariantResult::NoPositionOutput;

		bool hasEntryPoint = false;
		std::set<uint32_t> invariantVariables;
		std::set<std::pair<uint32_t, uint32_t>> invariantMembers;
		for (const SpirvInstruction& instruction : *instructions)
		{
			if (instruction.Opcode == Spirv::c_OpEntryPoint && !instruction.Operands.empty())
			{
				hasEntryPoint = true;
				if (instruction.Operands[0] != Spirv::c_ExecutionModelVertex)
					return InvariantResult::NotVertexShader;
			}
			if (instruction.Opcode == Spirv::c_OpDecorate && instruction.Operands.size() >= 2 &&
				instruction.Operands[1] == Spirv::c_DecorationInvariant)
				invariantVariables.insert(instruction.Operands[0]);
			if (instruction.Opcode == Spirv::c_OpMemberDecorate && instruction.Operands.size() >= 3 &&
				instruction.Operands[2] == Spirv::c_DecorationInvariant)
				invariantMembers.insert({ instruction.Operands[0], instruction.Operands[1] });
		}
		if (!hasEntryPoint)
			return InvariantResult::NotVertexShader;

		// Where each decoration goes (the word after the BuiltIn Position one) and what it is, collected before any
		// insertion moves the words the operands point into.
		std::vector<std::pair<size_t, std::vector<uint32_t>>> insertions;
		bool hasPosition = false;
		for (const SpirvInstruction& instruction : *instructions)
		{
			const size_t end = instruction.Offset + 1 + instruction.Operands.size();
			if (instruction.Opcode == Spirv::c_OpDecorate && instruction.Operands.size() >= 3 &&
				instruction.Operands[1] == Spirv::c_DecorationBuiltIn && instruction.Operands[2] == Spirv::c_BuiltInPosition)
			{
				hasPosition = true;
				const uint32_t variable = instruction.Operands[0];
				if (!invariantVariables.contains(variable))
					insertions.push_back({ end, { MakeOpcodeWord(Spirv::c_OpDecorate, 3), variable, Spirv::c_DecorationInvariant } });
			}
			if (instruction.Opcode == Spirv::c_OpMemberDecorate && instruction.Operands.size() >= 4 &&
				instruction.Operands[2] == Spirv::c_DecorationBuiltIn && instruction.Operands[3] == Spirv::c_BuiltInPosition)
			{
				hasPosition = true;
				const uint32_t structure = instruction.Operands[0];
				const uint32_t member = instruction.Operands[1];
				if (!invariantMembers.contains({ structure, member }))
				{
					insertions.push_back({ end, { MakeOpcodeWord(Spirv::c_OpMemberDecorate, 4), structure, member,
						Spirv::c_DecorationInvariant } });
				}
			}
		}
		if (!hasPosition)
			return InvariantResult::NoPositionOutput;
		if (insertions.empty())
			return InvariantResult::AlreadyInvariant;

		// From the back, so the earlier positions stay where they were.
		for (auto it = insertions.rbegin(); it != insertions.rend(); ++it)
			words.insert(words.begin() + static_cast<std::ptrdiff_t>(it->first), it->second.begin(), it->second.end());
		return InvariantResult::Added;
	}

	std::string FormatShaderHeader(std::span<const uint32_t> words, std::string_view variableName, std::string_view shaderName)
	{
		std::string text;
		text.reserve(words.size() * 11 + 256);
		text += "// Generated by StrataShaderTool (CMake/StrataShaders.cmake) from the shader ";
		text += shaderName;
		text += " - do not edit.\n#pragma once\n\n#include <cstdint>\n\nconst uint32_t ";
		text += variableName;
		text += "[] = {\n";
		for (size_t index = 0; index < words.size(); index++)
		{
			if (index % 8 == 0)
				text += '\t';
			text += Hex(words[index]);
			if (index + 1 < words.size())
				text += ',';
			if (index % 8 == 7 || index + 1 == words.size())
				text += '\n';
		}
		text += "};\n";
		return text;
	}

	namespace Spirv
	{
		std::string ReadLiteralString(std::span<const uint32_t> operands, size_t first, size_t& wordCount)
		{
			std::string text;
			for (size_t index = first; index < operands.size(); index++)
			{
				for (int byte = 0; byte < 4; byte++)
				{
					const char character = static_cast<char>((operands[index] >> (byte * 8)) & 0xFFu);
					if (character == '\0')
					{
						wordCount = index - first + 1;
						return text;
					}
					text += character;
				}
			}
			wordCount = 0;
			return {};
		}
	}

}
