#pragma once

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Strata::Tests
{

	// The rules every sample project under Samples/ follows (Docs/DeveloperGuide.md, "Samples"): the files a sample must
	// have, its credits for every source asset, the licenses allowed, the notices of third-party packs, and its size budgets.

	// Which limits a sample keeps: Samples.json's "Budget" of its entry, absent for the standard ones, "Showcase" for a
	// graphical showcase (larger source assets, counted apart from the others' binaries).
	enum class SampleBudgetClass : uint8_t
	{
		Standard,
		Showcase
	};

	// The budget class of a Samples.json entry; nullopt, with the reason, for a "Budget" that names no class.
	std::optional<SampleBudgetClass> ReadSampleBudgetClass(const nlohmann::json& entry, std::string* outError = nullptr);

	// The limits a sample's files keep (MB = 1,000,000 bytes).
	struct SampleBudgets
	{
		uint64_t SourceAssetBytes = 25'000'000;  // Files under Assets/ an importer reads (models, buffers, images, sounds, fonts)
		uint64_t ShowcaseSourceAssetBytes = 80'000'000; // The same, of a showcase sample
		uint64_t DocumentBytes = 20'000'000;     // Text documents of one sample (scenes, cells, prefabs, materials, .meta...)
		uint64_t ScreenshotBytes = 3'000'000;    // Screenshots/, together
		uint32_t MaxScreenshots = 3;
		uint64_t ShowcaseScreenshotBytes = 8'000'000; // The same, of a showcase sample: one picture per shot of its tour
		uint32_t ShowcaseMaxScreenshots = 6;
		uint64_t TotalBinaryBytes = 55'000'000;  // Binary files of all standard samples (source assets, pictures, screenshots)
		uint64_t ShowcaseTotalBinaryBytes = 85'000'000; // Binary files of the showcase samples, counted apart
		uint64_t TotalDocumentBytes = 50'000'000; // Text documents of all samples
	};

	// The size of the launcher's picture of a sample (ProjectThumbnail::c_Width x c_Height).
	constexpr uint32_t c_SamplePictureWidth = 480;
	constexpr uint32_t c_SamplePictureHeight = 270;

	// Whether a file is binary content (an image, model, buffer, sound or font) rather than a text document, by extension.
	bool IsSampleBinaryFile(const std::filesystem::path& path);

	// What is wrong with the sample in `directory`: missing README.md, CREDITS.md or Picture.png (a 480 x 270 PNG), a source
	// asset under Assets/ that no row or more than one row of CREDITS.md credits, a row that credits no file, a license other
	// than CC0-1.0, CC-BY-4.0 and Strata, a CC-BY-4.0 row without attribution, a CC0 or CC-BY source whose URL is not in
	// `notices` (ThirdPartyNotices.md's text: its "Sample assets" section), a link of README.md or CREDITS.md that leaves the
	// sample or names a file it does not hold (copies open anywhere; link to https:// pages) and budgets exceeded. Hidden
	// entries (.strata, .gitignore) are left out. Empty when the sample follows every rule.
	std::vector<std::string> CheckSampleContent(const std::filesystem::path& directory, const std::string& notices, const SampleBudgets& budgets = {},
		SampleBudgetClass budgetClass = SampleBudgetClass::Standard);

	// What is wrong with the samples' sizes together (SampleBudgets::TotalBinaryBytes, ShowcaseTotalBinaryBytes and
	// TotalDocumentBytes). `classes` holds each directory's class (missing ones: Standard).
	std::vector<std::string> CheckSamplesTotals(const std::vector<std::filesystem::path>& directories, const SampleBudgets& budgets = {},
		const std::vector<SampleBudgetClass>& classes = {});

}
