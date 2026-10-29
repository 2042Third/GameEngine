#pragma once

#include "Strata/Asset/AssetImporter.h"

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	// A 3D table as a .cube file holds it (the Cube LUT format grading tools export).
	struct CubeTable
	{
		std::string Title;
		uint32_t Size = 0;                     // LUT_3D_SIZE: points each way
		glm::vec3 DomainMin = glm::vec3(0.0f); // DOMAIN_MIN, or LUT_3D_INPUT_RANGE's first number: the input of the first point
		glm::vec3 DomainMax = glm::vec3(1.0f); // DOMAIN_MAX, or its second: the input of the last
		std::vector<glm::vec3> Colors;         // Size^3 output colors, red changing fastest, then green, then blue
		std::vector<std::string> Warnings;     // Keywords it ignored, with their lines
	};

	// Reads a .cube file: lines of keywords (TITLE "...", LUT_3D_SIZE n with n from ColorLookup::c_MinSize to c_MaxSize,
	// DOMAIN_MIN and DOMAIN_MAX r g b, LUT_3D_INPUT_RANGE min max), comments starting with '#' and blank lines, then
	// LUT_3D_SIZE^3 lines of three numbers. Numbers are read without the C locale. Fails, with the line and the reason (quoting
	// the file as valid UTF-8), for a 1D table (LUT_1D_SIZE), a missing or repeated size, a line that is not three finite
	// numbers, a keyword among the values, too few or too many values and an empty domain; unknown keywords are ignored with a
	// warning. Stops, failing, once `cancel` is set (checked between slices of the table).
	std::optional<CubeTable> ParseCubeFile(std::span<const uint8_t> text, std::string* outError, const std::atomic<bool>* cancel = nullptr);

	// The table at `size` points each way over the inputs [0, 1]: each point's input is mapped into the file's domain (inputs
	// outside it clamp to its edge) and looked up trilinearly (ColorLookup::Sample). A table of that size over [0, 1] comes
	// back unchanged, and a size of (n - 1) * k + 1 refines a table of n points exactly. Empty once `cancel` is set (checked per
	// slice).
	std::vector<glm::vec3> ResampleCubeTable(const CubeTable& table, uint32_t size, const std::atomic<bool>* cancel = nullptr);

	// Imports .cube files into ColorLookup's cooked form. Setting "Size": the points each way of the stored table, "17", "33"
	// (the default) or "65", or "File" for the file's own; other sizes are resampled.
	class ColorLookupImporter final : public AssetImporter
	{
	public:
		AssetType GetType() const override { return AssetType::ColorLookup; }
		std::vector<std::string> GetExtensions() const override { return { ".cube" }; }
		uint32_t GetVersion() const override { return 1; }
		nlohmann::json GetDefaultSettings(const std::filesystem::path& sourcePath) const override;
		std::vector<PropertyInfo> GetSettingsProperties() const override;
		bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const override;
	};

}
