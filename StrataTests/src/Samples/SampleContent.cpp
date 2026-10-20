#include "Samples/SampleContent.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/StringUtils.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <system_error>

namespace Strata::Tests
{

	namespace
	{

		// The extensions of the files importers read: models and their buffers, images, sounds and fonts.
		constexpr std::array<std::string_view, 17> c_BinaryExtensions = { ".gltf", ".glb", ".bin", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd",
			".gif", ".hdr", ".wav", ".mp3", ".flac", ".ogg", ".ttf", ".otf" };

		constexpr std::array<std::string_view, 3> c_Licenses = { "CC0-1.0", "CC-BY-4.0", "Strata" };

		constexpr std::string_view c_CreditsHeader = "| Files | Source | Author | License | Attribution | Changes |";

		struct CreditsRow
		{
			std::string Files; // A path relative to the sample, or a directory ending in '/'
			std::string Source;
			std::string License;
			std::string Attribution;
			size_t Covered = 0; // Files it credits
		};

		struct SampleFile
		{
			std::string Path; // Relative to the sample, '/' separated
			uint64_t Size = 0;
		};

		std::string FormatMB(uint64_t bytes)
		{
			return fmt::format("{:.1f} MB", static_cast<double>(bytes) / 1'000'000.0);
		}

		std::string Trim(std::string_view text)
		{
			const size_t first = text.find_first_not_of(" \t");
			if (first == std::string_view::npos)
				return {};
			const size_t last = text.find_last_not_of(" \t");
			return std::string(text.substr(first, last - first + 1));
		}

		// The cells of a Markdown table row "| a | b |", trimmed; a `code` cell loses its backticks.
		std::vector<std::string> SplitRow(std::string_view line)
		{
			std::vector<std::string> cells;
			const std::string trimmed = Trim(line);
			if (trimmed.size() < 2 || trimmed.front() != '|' || trimmed.back() != '|')
				return cells;
			size_t start = 1;
			while (start < trimmed.size())
			{
				const size_t end = trimmed.find('|', start);
				if (end == std::string::npos)
					break;
				std::string cell = Trim(std::string_view(trimmed).substr(start, end - start));
				if (cell.size() >= 2 && cell.front() == '`' && cell.back() == '`')
					cell = cell.substr(1, cell.size() - 2);
				cells.push_back(std::move(cell));
				start = end + 1;
			}
			return cells;
		}

		// The rows of CREDITS.md's table; an error when it has none or its header is not the credits format.
		std::optional<std::vector<CreditsRow>> ReadCredits(const std::string& text, std::vector<std::string>& errors)
		{
			std::vector<CreditsRow> rows;
			bool header = false;
			bool separator = false;
			size_t start = 0;
			while (start <= text.size())
			{
				size_t end = text.find('\n', start);
				if (end == std::string::npos)
					end = text.size();
				std::string line = Trim(std::string_view(text).substr(start, end - start));
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				start = end + 1;
				if (line.empty() || line.front() != '|')
				{
					if (header && separator)
						break; // The table ended
					continue;
				}
				if (!header)
				{
					if (line != c_CreditsHeader)
					{
						errors.push_back(fmt::format("CREDITS.md's table must start with the header '{}'", c_CreditsHeader));
						return std::nullopt;
					}
					header = true;
					continue;
				}
				if (!separator)
				{
					separator = true;
					continue;
				}
				const std::vector<std::string> cells = SplitRow(line);
				if (cells.size() != 6)
				{
					errors.push_back(fmt::format("CREDITS.md has a row without its 6 cells: '{}'", line));
					continue;
				}
				rows.push_back({ cells[0], cells[1], cells[3], cells[4], 0 });
			}
			if (!header)
			{
				errors.push_back(fmt::format("CREDITS.md has no credits table ('{}')", c_CreditsHeader));
				return std::nullopt;
			}
			return rows;
		}

		// The files below a sample (sorted, '/' separated), without hidden entries.
		std::vector<SampleFile> ListFiles(const std::filesystem::path& directory)
		{
			std::vector<SampleFile> files;
			std::error_code error;
			for (std::filesystem::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
			{
				const std::string name = FileSystem::ToUTF8(it->path().filename());
				if (!name.empty() && name.front() == '.')
				{
					if (it->is_directory(error))
						it.disable_recursion_pending();
					continue;
				}
				if (!it->is_regular_file(error))
					continue;
				const uint64_t size = it->file_size(error);
				std::string path = FileSystem::ToUTF8(it->path().lexically_relative(directory));
				std::replace(path.begin(), path.end(), '\\', '/');
				files.push_back({ std::move(path), error ? 0 : size });
				error.clear();
			}
			std::sort(files.begin(), files.end(), [](const SampleFile& a, const SampleFile& b) { return a.Path < b.Path; });
			return files;
		}

		bool Covers(const CreditsRow& row, const std::string& path)
		{
			if (!row.Files.empty() && row.Files.back() == '/')
				return path.compare(0, row.Files.size(), row.Files) == 0;
			return path == row.Files;
		}

		// The first https:// URL of a cell, up to the next space.
		std::string FindURL(const std::string& cell)
		{
			const size_t start = cell.find("https://");
			if (start == std::string::npos)
				return {};
			const size_t end = cell.find_first_of(" \t)", start);
			return cell.substr(start, end == std::string::npos ? std::string::npos : end - start);
		}

		// The text of ThirdPartyNotices.md's "Sample assets" section (up to the next heading of its level or above).
		std::string_view FindSampleAssetsSection(const std::string& notices)
		{
			const size_t start = notices.find("## Sample assets");
			if (start == std::string::npos)
				return {};
			const size_t end = notices.find("\n## ", start + 1);
			return std::string_view(notices).substr(start, end == std::string::npos ? std::string::npos : end - start);
		}

		// The targets of a Markdown document's inline links and images: what follows "](" up to ')'.
		std::vector<std::string> FindLinkTargets(const std::string& text)
		{
			std::vector<std::string> targets;
			for (size_t found = text.find("]("); found != std::string::npos; found = text.find("](", found + 2))
			{
				const size_t start = found + 2;
				const size_t end = text.find_first_of(") \t\n", start);
				if (end != std::string::npos && text[end] == ')')
					targets.push_back(text.substr(start, end - start));
			}
			return targets;
		}

		// What is wrong with the links of a sample's document `name`: each names a page on the web or a file of the sample.
		// Copies of a sample open in any directory and release archives lay out the engine's documents differently from the
		// repository, so a relative link that leaves the sample breaks in both.
		void CheckLinks(const std::filesystem::path& directory, const char* name, std::vector<std::string>& errors)
		{
			const std::optional<std::string> text = FileSystem::ReadText(directory / name);
			if (!text)
				return;
			for (const std::string& target : FindLinkTargets(*text))
			{
				if (target.empty() || target.front() == '#' || target.find("://") != std::string::npos || target.rfind("mailto:", 0) == 0)
					continue;
				const std::string file = target.substr(0, target.find('#'));
				const std::filesystem::path relative = FileSystem::FromUTF8(file);
				const bool leaves = relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()
					|| std::any_of(relative.begin(), relative.end(), [](const std::filesystem::path& part) { return part == ".."; });
				if (leaves)
					errors.push_back(fmt::format("{} links to '{}' outside the sample (link to its https:// page instead)", name, target));
				else if (!FileSystem::Exists(directory / relative))
					errors.push_back(fmt::format("{} links to '{}', which the sample does not hold", name, target));
			}
		}

		// The width and height of a PNG from its header, or nothing when it is not one.
		std::optional<std::pair<uint32_t, uint32_t>> ReadPNGSize(const std::filesystem::path& path)
		{
			const std::optional<std::vector<uint8_t>> header = FileSystem::ReadBytesPrefix(path, 24);
			constexpr std::array<uint8_t, 8> c_Signature = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
			if (!header || header->size() < 24 || !std::equal(c_Signature.begin(), c_Signature.end(), header->begin()))
				return std::nullopt;
			if (std::string_view(reinterpret_cast<const char*>(header->data() + 12), 4) != "IHDR")
				return std::nullopt;
			const auto readU32 = [&](size_t offset)
			{
				return (static_cast<uint32_t>((*header)[offset]) << 24) | (static_cast<uint32_t>((*header)[offset + 1]) << 16)
					| (static_cast<uint32_t>((*header)[offset + 2]) << 8) | static_cast<uint32_t>((*header)[offset + 3]);
			};
			return std::make_pair(readU32(16), readU32(20));
		}

	}

	bool IsSampleBinaryFile(const std::filesystem::path& path)
	{
		const std::string extension = StringUtils::ToLower(FileSystem::ToUTF8(path.extension()));
		return std::find(c_BinaryExtensions.begin(), c_BinaryExtensions.end(), extension) != c_BinaryExtensions.end();
	}

	std::vector<std::string> CheckSampleContent(const std::filesystem::path& directory, const std::string& notices, const SampleBudgets& budgets)
	{
		std::vector<std::string> errors;
		for (const char* required : { "README.md", "CREDITS.md", "Picture.png" })
		{
			if (!FileSystem::IsRegularFile(directory / required))
				errors.push_back(fmt::format("missing {}", required));
		}
		if (FileSystem::IsRegularFile(directory / "Picture.png"))
		{
			const std::optional<std::pair<uint32_t, uint32_t>> size = ReadPNGSize(directory / "Picture.png");
			if (!size)
				errors.push_back("Picture.png is not a PNG");
			else if (size->first != c_SamplePictureWidth || size->second != c_SamplePictureHeight)
				errors.push_back(fmt::format("Picture.png is {} x {}, not {} x {}", size->first, size->second, c_SamplePictureWidth, c_SamplePictureHeight));
		}
		CheckLinks(directory, "README.md", errors);
		CheckLinks(directory, "CREDITS.md", errors);

		const std::vector<SampleFile> files = ListFiles(directory);
		std::vector<CreditsRow> rows;
		if (const std::optional<std::string> credits = FileSystem::ReadText(directory / "CREDITS.md"))
		{
			if (std::optional<std::vector<CreditsRow>> read = ReadCredits(*credits, errors))
				rows = std::move(*read);
		}

		uint64_t sourceBytes = 0;
		uint64_t documentBytes = 0;
		uint64_t screenshotBytes = 0;
		uint32_t screenshots = 0;
		for (const SampleFile& file : files)
		{
			const bool binary = IsSampleBinaryFile(FileSystem::FromUTF8(file.Path));
			if (file.Path.rfind("Screenshots/", 0) == 0)
			{
				screenshots++;
				screenshotBytes += file.Size;
				if (StringUtils::ToLower(FileSystem::ToUTF8(FileSystem::FromUTF8(file.Path).extension())) != ".png")
					errors.push_back(fmt::format("'{}' is not a PNG (Screenshots/ holds PNGs only)", file.Path));
				continue;
			}
			if (!binary)
			{
				documentBytes += file.Size;
				continue;
			}
			if (file.Path.rfind("Assets/", 0) != 0)
				continue; // Picture.png
			sourceBytes += file.Size;
			size_t credited = 0;
			for (CreditsRow& row : rows)
			{
				if (Covers(row, file.Path))
				{
					credited++;
					row.Covered++;
				}
			}
			if (credited == 0)
				errors.push_back(fmt::format("'{}' is credited by no row of CREDITS.md", file.Path));
			else if (credited > 1)
				errors.push_back(fmt::format("'{}' is credited by {} rows of CREDITS.md", file.Path, credited));
		}

		const std::string_view section = FindSampleAssetsSection(notices);
		for (const CreditsRow& row : rows)
		{
			if (row.Covered == 0)
				errors.push_back(fmt::format("the CREDITS.md row of '{}' credits no source asset", row.Files));
			if (std::find(c_Licenses.begin(), c_Licenses.end(), row.License) == c_Licenses.end())
			{
				errors.push_back(fmt::format("the CREDITS.md row of '{}' has the license '{}' (CC0-1.0, CC-BY-4.0 or Strata)", row.Files, row.License));
				continue;
			}
			if (row.License == "CC-BY-4.0" && row.Attribution.empty())
				errors.push_back(fmt::format("the CREDITS.md row of '{}' is CC-BY-4.0 without an attribution", row.Files));
			if (row.License == "Strata")
				continue;
			const std::string url = FindURL(row.Source);
			if (url.empty())
				errors.push_back(fmt::format("the CREDITS.md row of '{}' names no https:// source", row.Files));
			else if (section.find(url) == std::string_view::npos)
				errors.push_back(fmt::format("the source '{}' of '{}' is not in ThirdPartyNotices.md's \"Sample assets\" table", url, row.Files));
		}

		if (sourceBytes > budgets.SourceAssetBytes)
			errors.push_back(fmt::format("the source assets take {} (at most {})", FormatMB(sourceBytes), FormatMB(budgets.SourceAssetBytes)));
		if (documentBytes > budgets.DocumentBytes)
			errors.push_back(fmt::format("the documents take {} (at most {})", FormatMB(documentBytes), FormatMB(budgets.DocumentBytes)));
		if (screenshots > budgets.MaxScreenshots)
			errors.push_back(fmt::format("Screenshots/ holds {} files (at most {})", screenshots, budgets.MaxScreenshots));
		if (screenshotBytes > budgets.ScreenshotBytes)
			errors.push_back(fmt::format("Screenshots/ takes {} (at most {})", FormatMB(screenshotBytes), FormatMB(budgets.ScreenshotBytes)));
		return errors;
	}

	std::vector<std::string> CheckSamplesTotals(const std::vector<std::filesystem::path>& directories, const SampleBudgets& budgets)
	{
		uint64_t binaryBytes = 0;
		uint64_t documentBytes = 0;
		for (const std::filesystem::path& directory : directories)
		{
			for (const SampleFile& file : ListFiles(directory))
				(IsSampleBinaryFile(FileSystem::FromUTF8(file.Path)) ? binaryBytes : documentBytes) += file.Size;
		}
		std::vector<std::string> errors;
		if (binaryBytes > budgets.TotalBinaryBytes)
			errors.push_back(fmt::format("the samples' binary files take {} (at most {})", FormatMB(binaryBytes), FormatMB(budgets.TotalBinaryBytes)));
		if (documentBytes > budgets.TotalDocumentBytes)
			errors.push_back(fmt::format("the samples' documents take {} (at most {})", FormatMB(documentBytes), FormatMB(budgets.TotalDocumentBytes)));
		return errors;
	}

}
