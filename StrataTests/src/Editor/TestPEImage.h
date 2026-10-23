#pragma once

#include "Editor/PEResources.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strata::Tests
{

	// The runtime's resources in small: the icon group GLFW_ICON naming icons 1 and 2, the group 101 naming icon 3, the
	// three icons, a version (RT_VERSION 1), a manifest (RT_MANIFEST 1), data of a named type and data in two languages.
	std::vector<PEResources::Resource> MakeTestRuntimeResources();

	struct TestPEImageOptions
	{
		std::vector<PEResources::Resource> Resources = MakeTestRuntimeResources();
		uint16_t Machine = 0x8664; // AMD64
		bool SectionAfterRelocations = false; // A section '.extra' after .reloc
		uint32_t OverlayBytes = 0;            // Bytes after the last section
		bool CertificateTable = false;        // A certificate table at the end of the file (a signed image)
	};

	// A valid PE32+ image laid out as the linker lays out the runtime: headers, .text (with an exception directory), .rsrc
	// with the resources, then .reloc, whose base relocations name .text. The resource tree is written depth first (each
	// type's directory, then its names'), then the names, the data entries and the data on 4-byte boundaries: another
	// layout than PEResources::Write's, so reading is tested on both. Written here without PEResources (whose resource
	// order it borrows), and with the checksum of ComputeTestPEChecksum.
	std::vector<uint8_t> BuildTestPEImage(const TestPEImageOptions& options = {});

	struct TestPESection
	{
		std::string Name;
		uint32_t VirtualSize = 0;
		uint32_t VirtualAddress = 0;
		uint32_t SizeOfRawData = 0;
		uint32_t PointerToRawData = 0;
	};

	// The headers of an image (fields out of the file's bounds read as zero), read independently of PEResources.
	struct TestPEHeaders
	{
		uint32_t SizeOfInitializedData = 0;
		uint32_t SectionAlignment = 0;
		uint32_t FileAlignment = 0;
		uint32_t SizeOfImage = 0;
		uint32_t SizeOfHeaders = 0;
		uint32_t CheckSum = 0;
		std::array<std::pair<uint32_t, uint32_t>, 16> Directories = {}; // RVA (a file offset for the certificate table), size
		std::vector<TestPESection> Sections;

		const TestPESection* FindSection(std::string_view name) const;
	};
	TestPEHeaders ReadTestPEHeaders(std::span<const uint8_t> image);
	// The checksum as the PE specification defines it, computed independently of PEResources::ComputeChecksum.
	uint32_t ComputeTestPEChecksum(std::span<const uint8_t> image);

	// What a VS_VERSIONINFO resource holds, read independently of PEResources::BuildVersionInfo.
	struct TestVersionInfo
	{
		std::array<uint16_t, 4> FileVersion = {};
		std::array<uint16_t, 4> ProductVersion = {};
		std::u16string StringTableKey;
		std::map<std::u16string, std::u16string> Strings;
		uint32_t Translation = 0; // Language in the low word, code page in the high word
	};
	std::optional<TestVersionInfo> ParseTestVersionInfo(std::span<const uint8_t> data, std::string* outError = nullptr);

}
