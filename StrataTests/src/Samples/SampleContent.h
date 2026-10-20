#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Strata::Tests
{

	// The rules every sample project under Samples/ follows (Docs/DeveloperGuide.md, "Samples"): the files a sample must
	// have, its credits for every source asset, the licenses allowed, the notices of third-party packs, and its size budgets.

	// The limits a sample's files keep (MB = 1,000,000 bytes).
	struct SampleBudgets
	{
		uint64_t SourceAssetBytes = 25'000'000;  // Files under Assets/ an importer reads (models, buffers, images, sounds, fonts)
		uint64_t DocumentBytes = 20'000'000;     // Text documents of one sample (scenes, cells, prefabs, materials, .meta...)
		uint64_t ScreenshotBytes = 3'000'000;    // Screenshots/, together
		uint32_t MaxScreenshots = 3;
		uint64_t TotalBinaryBytes = 55'000'000;  // Binary files of all samples (source assets, pictures, screenshots)
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
	// `notices` (ThirdPartyNotices.md's text: its "Sample assets" section) and budgets exceeded. Hidden entries (.strata,
	// .gitignore) are left out. Empty when the sample follows every rule.
	std::vector<std::string> CheckSampleContent(const std::filesystem::path& directory, const std::string& notices, const SampleBudgets& budgets = {});

	// What is wrong with the samples' sizes together (SampleBudgets::TotalBinaryBytes and TotalDocumentBytes).
	std::vector<std::string> CheckSamplesTotals(const std::vector<std::filesystem::path>& directories, const SampleBudgets& budgets = {});

}
