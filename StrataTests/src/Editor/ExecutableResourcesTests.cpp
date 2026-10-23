#include <doctest/doctest.h>

#include "Editor/GameIcon.h"
#include "Editor/PEResources.h"
#include "Editor/TestPEImage.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/PlatformDetection.h>
#include <Strata/Core/Process.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Exported Windows games carry their version information and icon (PEResources, Docs/DeveloperGuide.md, "Exporting
// games"): the writer is platform-independent, so it is tested on every platform with images made by Tests::BuildTestPEImage;
// on Windows the built runtime is stamped too, and Windows reads the result and runs it.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	PEResources::GameVersionInfo MakeVersionInfo()
	{
		PEResources::GameVersionInfo info;
		info.Title = "Smoke Adventure";
		info.Version = "2.0.1";
		info.Company = "Strata Tests";
		info.Copyright = "(c) 2026 Strata Tests";
		info.InternalName = "SmokeGame";
		info.OriginalFilename = "SmokeGame.exe";
		return info;
	}

	// An icon of four colored quadrants at every size (GameIcon's conversion), made once.
	const GameIcon::Images& GetTestIcon()
	{
		static const GameIcon::Images s_Icon = []()
		{
			std::vector<uint8_t> pixels;
			for (uint32_t y = 0; y < 256; y++)
			{
				for (uint32_t x = 0; x < 256; x++)
				{
					// Red, green, blue and white quadrants; a transparent corner, so the bitmaps' masks have set bits.
					const bool right = x >= 128;
					const bool bottom = y >= 128;
					constexpr uint8_t c_On = 255;
					constexpr uint8_t c_Off = 0;
					pixels.push_back(right == bottom ? c_On : c_Off);
					pixels.push_back(right ? c_On : c_Off);
					pixels.push_back(bottom ? c_On : c_Off);
					pixels.push_back(x < 64 && y < 64 ? c_Off : c_On);
				}
			}
			std::string error;
			std::optional<GameIcon::Images> icon = GameIcon::Convert(EncodePNG(256, 256, pixels), &error);
			REQUIRE_MESSAGE(icon, error);
			return std::move(*icon);
		}();
		return s_Icon;
	}

	const PEResources::Resource* FindEntry(const std::vector<PEResources::Resource>& resources, const PEResources::ResourceId& type,
		const PEResources::ResourceId& name, uint16_t language)
	{
		const auto found = std::find_if(resources.begin(), resources.end(), [&](const PEResources::Resource& resource)
		{
			return resource.Type == type && resource.Name == name && resource.Language == language;
		});
		return found != resources.end() ? &*found : nullptr;
	}

	PEResources::ResourceId Number(uint16_t number)
	{
		return PEResources::ResourceId::FromNumber(number);
	}

	uint32_t AlignUp(uint32_t value, uint32_t alignment)
	{
		return (value + alignment - 1) / alignment * alignment;
	}

	std::vector<uint8_t> Stamp(std::span<const uint8_t> image, const GameIcon::Images* icon)
	{
		std::string error;
		std::optional<std::vector<uint8_t>> stamped = PEResources::StampGame(image, MakeVersionInfo(), icon, &error);
		REQUIRE_MESSAGE(stamped, error);
		return std::move(*stamped);
	}

	std::string StampError(const TestPEImageOptions& options)
	{
		std::string error;
		CHECK_FALSE(PEResources::StampGame(BuildTestPEImage(options), MakeVersionInfo(), &GetTestIcon(), &error));
		CHECK_FALSE(error.empty());
		return error;
	}

	bool Contains(const std::string& text, const std::string& part)
	{
		return text.find(part) != std::string::npos;
	}

	void Patch32(std::vector<uint8_t>& image, size_t offset, uint32_t value)
	{
		for (size_t index = 0; index < 4; index++)
			image[offset + index] = static_cast<uint8_t>(value >> (8 * index));
	}

	// Where Tests::BuildTestPEImage puts the data directories and the section table (its headers start at 0x80).
	constexpr size_t c_TestDataDirectories = 0x80 + 4 + 20 + 112;
	constexpr size_t c_TestSectionTable = c_TestDataDirectories + 16 * 8;

}

TEST_SUITE("Editor.Export.ExecutableResources")
{
	TEST_CASE("Stamping replaces the icon group and version and keeps every other resource byte for byte")
	{
		const std::vector<uint8_t> image = BuildTestPEImage();
		const std::vector<PEResources::Resource> original = MakeTestRuntimeResources();
		std::string error;
		const std::optional<std::vector<PEResources::Resource>> read = PEResources::Read(image, &error);
		REQUIRE_MESSAGE(read, error);
		CHECK(read->size() == original.size());
		for (const PEResources::Resource& resource : original)
		{
			const PEResources::Resource* found = FindEntry(*read, resource.Type, resource.Name, resource.Language);
			REQUIRE(found);
			CHECK(*found == resource);
		}

		const GameIcon::Images& icon = GetTestIcon();
		const std::vector<uint8_t> stamped = Stamp(image, &icon);
		const std::optional<std::vector<PEResources::Resource>> resources = PEResources::Read(stamped, &error);
		REQUIRE_MESSAGE(resources, error);

		// The game's version.
		const std::optional<std::vector<uint8_t>> version = PEResources::BuildVersionInfo(MakeVersionInfo(), &error);
		REQUIRE_MESSAGE(version, error);
		const PEResources::Resource* versionResource = FindEntry(*resources, Number(PEResources::c_TypeVersion), Number(1), 0x409);
		REQUIRE(versionResource);
		CHECK(versionResource->Data == *version);

		// The game's icon group names one new image per size, under the lowest ids the kept icon 3 leaves free.
		const PEResources::Resource* group = FindEntry(*resources, Number(PEResources::c_TypeGroupIcon),
			PEResources::ResourceId::FromName(PEResources::c_GameIconGroup), 0x409);
		REQUIRE(group);
		REQUIRE(group->Data.size() == 6 + icon.Sizes.size() * 14);
		CHECK(group->Data[2] == 1);
		CHECK(static_cast<size_t>(group->Data[4]) == icon.Sizes.size());
		const std::array<uint16_t, 10> expectedIds = { 1, 2, 4, 5, 6, 7, 8, 9, 10, 11 };
		REQUIRE(icon.Sizes.size() == expectedIds.size());
		for (size_t index = 0; index < icon.Sizes.size(); index++)
		{
			const uint8_t* entry = group->Data.data() + 6 + index * 14;
			const uint32_t size = icon.Sizes[index].Size;
			CAPTURE(size);
			CHECK(static_cast<uint32_t>(entry[0]) == (size == 256 ? 0 : size));
			CHECK(static_cast<uint32_t>(entry[1]) == (size == 256 ? 0 : size));
			CHECK((entry[6] | entry[7] << 8) == 32);
			const uint16_t id = static_cast<uint16_t>(entry[12] | entry[13] << 8);
			CHECK(id == expectedIds[index]);
			const PEResources::Resource* iconResource = FindEntry(*resources, Number(PEResources::c_TypeIcon), Number(id), 0x409);
			REQUIRE(iconResource);
			const std::optional<std::vector<uint8_t>> expected = PEResources::BuildIconImage(icon.Sizes[index], &error);
			REQUIRE_MESSAGE(expected, error);
			CHECK(iconResource->Data == *expected);
			CHECK(static_cast<uint32_t>(entry[8] | entry[9] << 8 | entry[10] << 16 | entry[11] << 24) == iconResource->Data.size());
			// 256 pixels as a PNG; the others as 32-bit bitmaps whose mask marks the transparent corner (bottom row last).
			if (size == 256)
			{
				CHECK(iconResource->Data[0] == 0x89);
				CHECK(iconResource->Data[1] == 'P');
			}
			else
			{
				CHECK(iconResource->Data[0] == 40);
				CHECK(static_cast<uint32_t>(iconResource->Data[8] | iconResource->Data[9] << 8) == size * 2);
				const size_t maskRow = (size + 31) / 32 * 4;
				const size_t lastMaskRow = 40 + size_t(size) * size * 4 + (size - 1) * maskRow;
				REQUIRE(iconResource->Data.size() == 40 + size_t(size) * size * 4 + size * maskRow);
				CHECK((iconResource->Data[lastMaskRow] & 0x80) != 0);
				CHECK((iconResource->Data[40 + size_t(size) * size * 4] & 0x80) == 0);
			}
		}

		// Everything else as it was: the group 101 and the icon 3 it names, the manifest, the named type, both languages.
		size_t kept = 0;
		for (const PEResources::Resource& resource : original)
		{
			const bool replaced = resource.Type == Number(PEResources::c_TypeVersion) || resource.Name.Name == PEResources::c_GameIconGroup
				|| (resource.Type == Number(PEResources::c_TypeIcon) && resource.Name.Number != 3);
			if (replaced)
				continue;
			const PEResources::Resource* found = FindEntry(*resources, resource.Type, resource.Name, resource.Language);
			REQUIRE(found);
			CHECK(*found == resource);
			kept++;
		}
		CHECK(kept == 6);
		CHECK(resources->size() == kept + 2 + icon.Sizes.size());

		// The headers and the sections before the resources are unchanged but for the fields the stamp follows.
		const TestPEHeaders headers = ReadTestPEHeaders(stamped);
		const TestPESection* resourceSection = headers.FindSection(".rsrc");
		REQUIRE(resourceSection);
		CHECK(std::equal(image.begin() + headers.SizeOfHeaders, image.begin() + resourceSection->PointerToRawData, stamped.begin() + headers.SizeOfHeaders));

		// Without an icon, only the version changes.
		const std::vector<uint8_t> versionOnly = Stamp(image, nullptr);
		const std::optional<std::vector<PEResources::Resource>> versionOnlyResources = PEResources::Read(versionOnly, &error);
		REQUIRE_MESSAGE(versionOnlyResources, error);
		CHECK(versionOnlyResources->size() == original.size());
		for (const PEResources::Resource& resource : original)
		{
			const PEResources::Resource* found = FindEntry(*versionOnlyResources, resource.Type, resource.Name, resource.Language);
			REQUIRE(found);
			if (resource.Type == Number(PEResources::c_TypeVersion))
				CHECK(found->Data == *version);
			else
				CHECK(*found == resource);
		}

		// A runtime without the group gets one.
		TestPEImageOptions withoutGroup;
		std::erase_if(withoutGroup.Resources, [](const PEResources::Resource& resource) { return resource.Name.Name == PEResources::c_GameIconGroup; });
		const std::optional<std::vector<PEResources::Resource>> added = PEResources::Read(Stamp(BuildTestPEImage(withoutGroup), &icon), &error);
		REQUIRE_MESSAGE(added, error);
		CHECK(FindEntry(*added, Number(PEResources::c_TypeGroupIcon), PEResources::ResourceId::FromName(PEResources::c_GameIconGroup), 0x409));
		// Icons 1 and 2, which no group names now, stay; the new images take the ids after them.
		const PEResources::Resource* firstIcon = FindEntry(*added, Number(PEResources::c_TypeIcon), Number(1), 0x409);
		REQUIRE(firstIcon);
		CHECK(firstIcon->Data == MakeTestRuntimeResources()[0].Data);
		CHECK(FindEntry(*added, Number(PEResources::c_TypeIcon), Number(13), 0x409));
	}

	TEST_CASE("Sections after .rsrc move with their directories")
	{
		const std::vector<uint8_t> image = BuildTestPEImage();
		const TestPEHeaders before = ReadTestPEHeaders(image);
		const TestPESection* relocationsBefore = before.FindSection(".reloc");
		REQUIRE(relocationsBefore);

		const auto checkLayout = [&](const std::vector<uint8_t>& stamped)
		{
			const TestPEHeaders after = ReadTestPEHeaders(stamped);
			REQUIRE(after.Sections.size() == 3);
			const TestPESection& resources = after.Sections[1];
			const TestPESection& relocations = after.Sections[2];
			CHECK(resources.Name == ".rsrc");
			CHECK(resources.VirtualAddress == before.Sections[1].VirtualAddress);
			CHECK(resources.PointerToRawData == before.Sections[1].PointerToRawData);
			CHECK(resources.SizeOfRawData == AlignUp(resources.VirtualSize, after.FileAlignment));
			CHECK(after.Directories[2] == std::pair<uint32_t, uint32_t>(resources.VirtualAddress, resources.VirtualSize));
			// .reloc follows directly, in memory and in the file, with its bytes and its directory.
			CHECK(relocations.Name == ".reloc");
			CHECK(relocations.VirtualAddress == AlignUp(resources.VirtualAddress + resources.VirtualSize, after.SectionAlignment));
			CHECK(relocations.PointerToRawData == resources.PointerToRawData + resources.SizeOfRawData);
			CHECK(relocations.VirtualSize == relocationsBefore->VirtualSize);
			CHECK(relocations.SizeOfRawData == relocationsBefore->SizeOfRawData);
			CHECK(stamped.size() == relocations.PointerToRawData + relocations.SizeOfRawData);
			CHECK(std::equal(image.begin() + relocationsBefore->PointerToRawData, image.begin() + relocationsBefore->PointerToRawData + relocationsBefore->SizeOfRawData,
				stamped.begin() + relocations.PointerToRawData));
			CHECK(after.Directories[5].first == relocations.VirtualAddress + (before.Directories[5].first - relocationsBefore->VirtualAddress));
			CHECK(after.Directories[5].second == before.Directories[5].second);
			// The directories before the resources stay.
			CHECK(after.Directories[3] == before.Directories[3]);
			return relocations.VirtualAddress;
		};

		// The icon's images take many pages: .reloc moves up.
		const std::vector<uint8_t> grown = Stamp(image, &GetTestIcon());
		const uint32_t grownAddress = checkLayout(grown);
		CHECK(grownAddress > relocationsBefore->VirtualAddress + 0x8000);
		// A one-image icon gives the pages back: .reloc moves down.
		GameIcon::Images oneImage;
		oneImage.Sizes.push_back(GetTestIcon().Sizes.front());
		const std::vector<uint8_t> shrunk = Stamp(grown, &oneImage);
		const uint32_t shrunkAddress = checkLayout(shrunk);
		CHECK(shrunkAddress < grownAddress);
		std::string error;
		CHECK_MESSAGE(PEResources::Read(shrunk, &error), error);
	}

	TEST_CASE("SizeOfImage and the checksum follow")
	{
		const std::vector<uint8_t> image = BuildTestPEImage();
		CHECK(PEResources::ComputeChecksum(image) == ComputeTestPEChecksum(image));
		const TestPEHeaders before = ReadTestPEHeaders(image);
		const std::vector<uint8_t> stamped = Stamp(image, &GetTestIcon());
		const TestPEHeaders after = ReadTestPEHeaders(stamped);
		const TestPESection& last = after.Sections.back();
		CHECK(after.SizeOfImage == AlignUp(last.VirtualAddress + last.VirtualSize, after.SectionAlignment));
		CHECK(after.SizeOfImage > before.SizeOfImage);
		CHECK(after.SizeOfInitializedData == before.SizeOfInitializedData - before.Sections[1].SizeOfRawData + after.Sections[1].SizeOfRawData);
		CHECK(after.CheckSum == ComputeTestPEChecksum(stamped));
		CHECK(after.CheckSum != before.CheckSum);
		// Odd sizes count their last byte as a word of its own.
		const std::vector<uint8_t> odd = { 'M', 'Z', 1 };
		CHECK(PEResources::ComputeChecksum(odd) == ComputeTestPEChecksum(odd));
	}

	TEST_CASE("A second stamp of a stamped image gives the same bytes")
	{
		const std::vector<uint8_t> image = BuildTestPEImage();
		const std::vector<uint8_t> first = Stamp(image, &GetTestIcon());
		CHECK(Stamp(first, &GetTestIcon()) == first);
		const std::vector<uint8_t> versionOnly = Stamp(image, nullptr);
		CHECK(Stamp(versionOnly, nullptr) == versionOnly);
	}

	TEST_CASE("Unsupported layouts (a section after .reloc, overlay, a certificate table) fail with the reason")
	{
		TestPEImageOptions extra;
		extra.SectionAfterRelocations = true;
		const std::string extraError = StampError(extra);
		CHECK_MESSAGE(Contains(extraError, "'.extra'"), extraError);
		CHECK_MESSAGE(Contains(extraError, "only the base relocations may"), extraError);

		TestPEImageOptions overlay;
		overlay.OverlayBytes = 100;
		const std::string overlayError = StampError(overlay);
		CHECK_MESSAGE(Contains(overlayError, "0x64 bytes after its last section (overlay data)"), overlayError);

		TestPEImageOptions signedImage;
		signedImage.CertificateTable = true;
		const std::string signedError = StampError(signedImage);
		CHECK_MESSAGE(Contains(signedError, "signed (it has a certificate table)"), signedError);

		// Moving .reloc must change nothing that names an address: its relocations name only addresses before .rsrc, no other
		// directory lies in or after .rsrc, and .reloc follows .rsrc directly.
		const std::vector<uint8_t> image = BuildTestPEImage();
		const TestPEHeaders headers = ReadTestPEHeaders(image);
		const TestPESection& resourceSection = headers.Sections[1];
		const TestPESection& relocationSection = headers.Sections[2];
		const auto layoutError = [](const std::vector<uint8_t>& changed)
		{
			std::string error;
			CHECK_FALSE(PEResources::StampGame(changed, MakeVersionInfo(), &GetTestIcon(), &error));
			return error;
		};
		std::vector<uint8_t> relocatedResources = image;
		Patch32(relocatedResources, relocationSection.PointerToRawData, resourceSection.VirtualAddress); // The block's page
		const std::string relocatedError = layoutError(relocatedResources);
		CHECK_MESSAGE(Contains(relocatedError, fmt::format("a base relocation names RVA 0x{:x}, in or after the resource section", resourceSection.VirtualAddress + 0x10)),
			relocatedError);
		std::vector<uint8_t> debugAfter = image;
		Patch32(debugAfter, c_TestDataDirectories + 6 * 8, relocationSection.VirtualAddress);
		Patch32(debugAfter, c_TestDataDirectories + 6 * 8 + 4, 28);
		const std::string debugError = layoutError(debugAfter);
		CHECK_MESSAGE(Contains(debugError, fmt::format("the debug directory (RVA 0x{:x}, 0x1c bytes) reaches into or past the resource section",
			relocationSection.VirtualAddress)), debugError);
		std::vector<uint8_t> gap = image;
		Patch32(gap, c_TestSectionTable + 2 * 40 + 12, relocationSection.VirtualAddress + 0x1000);
		Patch32(gap, c_TestDataDirectories + 5 * 8, headers.Directories[5].first + 0x1000);
		const std::string gapError = layoutError(gap);
		CHECK_MESSAGE(Contains(gapError, "the section '.reloc' does not directly follow the resource section '.rsrc'"), gapError);
		std::vector<uint8_t> withoutRelocations = image;
		Patch32(withoutRelocations, c_TestDataDirectories + 5 * 8, 0);
		Patch32(withoutRelocations, c_TestDataDirectories + 5 * 8 + 4, 0);
		const std::string withoutRelocationsError = layoutError(withoutRelocations);
		CHECK_MESSAGE(Contains(withoutRelocationsError, "the section '.reloc' after the resource section '.rsrc' holds no base relocations"),
			withoutRelocationsError);

		// What is no 64-bit Windows executable is refused naming the field.
		TestPEImageOptions x86;
		x86.Machine = 0x14C;
		const std::string machineError = StampError(x86);
		CHECK_MESSAGE(Contains(machineError, "Machine (0x014c) is neither AMD64 nor ARM64"), machineError);
		std::vector<uint8_t> pe32 = BuildTestPEImage();
		pe32[0x80 + 24] = 0x0B;
		pe32[0x80 + 25] = 0x01;
		std::string error;
		CHECK_FALSE(PEResources::Read(pe32, &error));
		CHECK_MESSAGE(Contains(error, "Magic (0x10b) is not PE32+"), error);
		CHECK_FALSE(PEResources::Read(std::vector<uint8_t>(64, 0), &error));
		CHECK_MESSAGE(Contains(error, "MZ header"), error);

		// Two resources of one type, name and language cannot be written.
		std::vector<PEResources::Resource> twice = MakeTestRuntimeResources();
		twice.push_back(twice.front());
		CHECK_FALSE(PEResources::Write(BuildTestPEImage(), twice, &error));
		CHECK_MESSAGE(Contains(error, "two resources are 3/1/1033"), error);
	}

	TEST_CASE("Version strings are UTF-16 and the version quad comes from Version")
	{
		PEResources::GameVersionInfo info = MakeVersionInfo();
		info.Title = "Café Ünïcode \xF0\x9F\x8E\xAE"; // With a character outside the Basic Multilingual Plane: a surrogate pair
		info.Version = "1.2";
		info.Company.clear();
		std::string error;
		const std::optional<std::vector<uint8_t>> data = PEResources::BuildVersionInfo(info, &error);
		REQUIRE_MESSAGE(data, error);
		CHECK(data->size() % 4 == 0);
		const std::optional<TestVersionInfo> parsed = ParseTestVersionInfo(*data, &error);
		REQUIRE_MESSAGE(parsed, error);
		CHECK(parsed->FileVersion == std::array<uint16_t, 4> { 1, 2, 0, 0 });
		CHECK(parsed->ProductVersion == std::array<uint16_t, 4> { 1, 2, 0, 0 });
		CHECK(parsed->StringTableKey == u"040904B0");
		CHECK(parsed->Translation == 0x04B00409u);
		const std::u16string title = u"Café Ünïcode \U0001F3AE";
		CHECK(title.size() == 15);
		const std::map<std::u16string, std::u16string> expected = { { u"FileDescription", title }, { u"FileVersion", u"1.2" },
			{ u"InternalName", u"SmokeGame" }, { u"LegalCopyright", u"(c) 2026 Strata Tests" }, { u"OriginalFilename", u"SmokeGame.exe" },
			{ u"ProductName", title }, { u"ProductVersion", u"1.2" } };
		CHECK(parsed->Strings == expected); // No CompanyName without a company

		info.Version = "2026.10.22.65535";
		info.Company = "Strata Tests";
		const std::optional<std::vector<uint8_t>> full = PEResources::BuildVersionInfo(info, &error);
		REQUIRE_MESSAGE(full, error);
		const std::optional<TestVersionInfo> fullParsed = ParseTestVersionInfo(*full, &error);
		REQUIRE_MESSAGE(fullParsed, error);
		CHECK(fullParsed->FileVersion == std::array<uint16_t, 4> { 2026, 10, 22, 65535 });
		CHECK(fullParsed->Strings.at(u"CompanyName") == u"Strata Tests");
		CHECK(fullParsed->Strings.at(u"FileVersion") == u"2026.10.22.65535");

		info.Version = "1.2.3.4.5";
		CHECK_FALSE(PEResources::BuildVersionInfo(info, &error));
		CHECK_MESSAGE(Contains(error, "'1.2.3.4.5' is no version"), error);
	}

#if defined(ST_PLATFORM_WINDOWS) && defined(STRATA_TESTS_HAVE_RUNTIME)
	TEST_CASE("The runtime stamped with a version and an icon starts, and Windows reads both")
	{
		const std::optional<std::string> variable = Platform::GetEnvVar("STRATA_TEST_RUNTIME_PATH");
		const std::filesystem::path runtime = variable && !variable->empty() ? FileSystem::FromUTF8(*variable)
			: Platform::GetExecutableDirectory() / "StrataRuntime.exe";
		const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(runtime);
		REQUIRE_MESSAGE(bytes, FileSystem::ToUTF8(runtime), " is not built");
		const GameIcon::Images& icon = GetTestIcon();
		const std::vector<uint8_t> stamped = Stamp(*bytes, &icon);
		const std::filesystem::path game = CreateTemporaryDirectory("StampedRuntime") / "SmokeGame.exe";
		REQUIRE(FileSystem::WriteBytes(game, stamped));

		std::string error;
		const std::optional<ExecutableVersion> version = ReadExecutableVersion(game, &error);
		REQUIRE_MESSAGE(version, error);
		CHECK(version->FileVersion == std::array<uint16_t, 4> { 2, 0, 1, 0 });
		CHECK(version->ProductVersion == std::array<uint16_t, 4> { 2, 0, 1, 0 });
		CHECK(version->Strings.at("FileDescription") == "Smoke Adventure");
		CHECK(version->Strings.at("ProductName") == "Smoke Adventure");
		CHECK(version->Strings.at("CompanyName") == "Strata Tests");
		CHECK(version->Strings.at("LegalCopyright") == "(c) 2026 Strata Tests");
		CHECK(version->Strings.at("OriginalFilename") == "SmokeGame.exe");
		CHECK(version->Strings.at("ProductVersion") == "2.0.1");

		const std::optional<std::vector<ExecutableIconImage>> images = ReadExecutableIconImages(game, &error);
		REQUIRE_MESSAGE(images, error);
		REQUIRE(images->size() == icon.Sizes.size());
		for (size_t index = 0; index < images->size(); index++)
		{
			CAPTURE(icon.Sizes[index].Size);
			CHECK((*images)[index].Size == icon.Sizes[index].Size);
			CHECK((*images)[index].Pixels == icon.Sizes[index].Pixels);
		}

		// Windows loads the stamped image (its sections, size and relocations) and runs it.
		ProcessSpecification specification;
		specification.Executable = game;
		specification.Arguments = { "--help" };
		specification.Output = ProcessOutputMode::CaptureSeparate;
		const Process::RunResult result = Process::Run(specification, std::chrono::seconds(60));
		REQUIRE_MESSAGE(result.Started, result.Error);
		REQUIRE_FALSE(result.TimedOut);
		CHECK_MESSAGE(result.ExitCode == 0, result.ErrorOutput);
		CHECK_MESSAGE(Contains(result.Output + result.ErrorOutput, "Usage: "), result.Output);
	}
#endif
}

// The reader refuses every damaged image without crashing (a sample of offsets in the core shard, all of them in
// StrataTests.Fuzz: Tests::SelectSweepOffsets).
TEST_SUITE("Editor.Export.ExecutableResources.Sweep")
{
	TEST_CASE("Images truncated anywhere, or with a byte flipped in the headers or the resource tree, fail cleanly")
	{
		const std::vector<uint8_t> image = BuildTestPEImage();
		const TestPEHeaders headers = ReadTestPEHeaders(image);
		const TestPESection& resources = headers.Sections[1];
		// The tree ends where the data of its first resource begins.
		std::string error;
		const std::optional<std::vector<PEResources::Resource>> read = PEResources::Read(image, &error);
		REQUIRE_MESSAGE(read, error);
		size_t treeEnd = resources.VirtualSize;
		for (const PEResources::Resource& resource : *read)
		{
			const auto found = std::search(image.begin() + resources.PointerToRawData, image.end(), resource.Data.begin(), resource.Data.end());
			treeEnd = std::min<size_t>(treeEnd, static_cast<size_t>(found - image.begin()) - resources.PointerToRawData);
		}
		const std::vector<SweepRange> regions = { { 0, headers.SizeOfHeaders }, { resources.PointerToRawData, resources.PointerToRawData + treeEnd } };

		// A smaller icon keeps each stamp quick.
		GameIcon::Images icon;
		icon.Sizes.push_back(GetTestIcon().Sizes.front());
		for (const size_t length : SelectSweepOffsets(image.size(), regions, 0))
		{
			CAPTURE(length);
			const std::span<const uint8_t> truncated(image.data(), length);
			error.clear();
			CHECK_FALSE(PEResources::Read(truncated, &error));
			CHECK_FALSE(error.empty());
			error.clear();
			CHECK_FALSE(PEResources::StampGame(truncated, MakeVersionInfo(), &icon, &error));
			CHECK_FALSE(error.empty());
		}
		for (const size_t offset : SelectSweepOffsets(image.size(), regions, 0))
		{
			for (const uint8_t flip : { uint8_t(0xFF), uint8_t(0x80), uint8_t(0x01) })
			{
				std::vector<uint8_t> damaged = image;
				damaged[offset] ^= flip;
				CAPTURE(offset);
				CAPTURE(flip);
				error.clear();
				if (!PEResources::Read(damaged, &error))
					CHECK_FALSE(error.empty());
				error.clear();
				const std::optional<std::vector<uint8_t>> stamped = PEResources::StampGame(damaged, MakeVersionInfo(), &icon, &error);
				if (stamped)
					CHECK_MESSAGE(PEResources::Read(*stamped, &error), error);
				else
					CHECK_FALSE(error.empty());
			}
		}
	}

	TEST_CASE("A resource directory that names itself is refused")
	{
		std::vector<uint8_t> image = BuildTestPEImage();
		const TestPEHeaders headers = ReadTestPEHeaders(image);
		// The root's first entry (a type) points back at the root.
		const size_t target = headers.Sections[1].PointerToRawData + 16 + 4;
		image[target] = 0;
		image[target + 1] = 0;
		image[target + 2] = 0;
		image[target + 3] = 0x80;
		std::string error;
		CHECK_FALSE(PEResources::Read(image, &error));
		CHECK_MESSAGE(Contains(error, "the resource directory at offset 0x0 is reached twice"), error);
		CHECK_FALSE(PEResources::StampGame(image, MakeVersionInfo(), nullptr, &error));
		CHECK_MESSAGE(Contains(error, "reached twice"), error);
	}
}
