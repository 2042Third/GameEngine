#pragma once

#include "Editor/GameIcon.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// The resources of Windows executables (PE32+ images; an exported Windows game is a copy of the runtime), read and
	// replaced by engine code without the Win32 API, so the writer builds and is tested on every platform. The export
	// stamps every Windows game with its version information and icon (Docs/DeveloperGuide.md, "Exporting games").
	namespace PEResources
	{

		// Resource types (the Windows SDK's RT_ values) and the language of the runtime's resources (US English).
		constexpr uint16_t c_TypeIcon = 3;
		constexpr uint16_t c_TypeGroupIcon = 14;
		constexpr uint16_t c_TypeVersion = 16;
		constexpr uint16_t c_TypeManifest = 24;
		constexpr uint16_t c_LanguageEnglishUS = 0x0409;
		// The runtime's icon group (StrataRuntime.rc.in): Explorer shows it, and GLFW's Win32 backend gives it to windows.
		constexpr std::u16string_view c_GameIconGroup = u"GLFW_ICON";
		// The name of the version resource.
		constexpr uint16_t c_VersionResource = 1;

		// A resource's type or name: a number, or a name (UTF-16 as stored; the resource compiler stores names in upper case).
		struct ResourceId
		{
			uint16_t Number = 0;
			std::u16string Name; // Not empty for a named id (whose Number is 0)

			static ResourceId FromNumber(uint16_t number) { return ResourceId { number, {} }; }
			static ResourceId FromName(std::u16string_view name) { return ResourceId { 0, std::u16string(name) }; }

			bool IsNamed() const { return !Name.empty(); }
			bool operator==(const ResourceId& other) const = default;
			// The order of a resource directory's entries: names (by their UTF-16 code units) before numbers (ascending).
			bool operator<(const ResourceId& other) const;
		};

		struct Resource
		{
			ResourceId Type;
			ResourceId Name;
			uint16_t Language = 0;
			uint32_t CodePage = 0;
			std::vector<uint8_t> Data;

			bool operator==(const Resource& other) const = default;
		};

		// The resources of a PE32+ image for AMD64 or ARM64, in the order of its resource tree (none for an image without a
		// resource directory). Nullopt, with the reason naming the field, for anything else or a damaged image: the headers,
		// the section table, each section's data and the whole resource tree (three levels, every directory once, names and
		// data within its section) are checked before they are read. Never asserts.
		std::optional<std::vector<Resource>> Read(std::span<const uint8_t> image, std::string* outError = nullptr);

		// A copy of the image with `resources` in place of all of its own. The resource section is rebuilt where it is; the
		// section after it, which may only be the base relocations' (as the linker lays images out), moves by the growth with
		// its file offset, RVA and data directory entry (relocations name RVAs of the sections before, so none changes);
		// SizeOfImage, SizeOfInitializedData and the resource directory's size follow and the checksum is recomputed. Every
		// other byte is kept. Nullopt, with the reason, for a damaged image or a layout this does not support: another section
		// after the resource section, data after the last section (overlay), a certificate table (a signed image: sign the
		// game after it is stamped), a resource directory that does not start its section, other data directories or
		// relocations in or after it, or two resources of one type, name and language.
		std::optional<std::vector<uint8_t>> Write(std::span<const uint8_t> image, std::span<const Resource> resources, std::string* outError = nullptr);

		// The image's checksum, as the optional header's CheckSum holds it: the sum of its 16-bit little-endian words with the
		// carries folded back in (CheckSum itself counted as zero), plus the file's size.
		uint32_t ComputeChecksum(std::span<const uint8_t> image);

		// What an exported game's executable tells about the game (from its manifest).
		struct GameVersionInfo
		{
			std::string Title;            // FileDescription and ProductName
			std::string Version;          // A ProjectGameInfo version: FILEVERSION and PRODUCTVERSION, FileVersion and ProductVersion
			std::string Company;          // CompanyName; empty: left out
			std::string Copyright;        // LegalCopyright; empty: left out
			std::string InternalName;     // The project's name
			std::string OriginalFilename; // The executable's file name
		};

		// The VS_VERSIONINFO resource (RT_VERSION) of a game: the version quad from Version (missing parts 0), a US English
		// string table (040904B0) of the strings converted from UTF-8 to UTF-16, and the translation 0x409, 1200. Nullopt,
		// with the reason, for an invalid version or strings too long for the resource's 16-bit lengths.
		std::optional<std::vector<uint8_t>> BuildVersionInfo(const GameVersionInfo& info, std::string* outError = nullptr);
		// The RT_ICON resource of one image of the icon: a 32-bit bitmap (BGRA rows bottom first, then the AND mask, set
		// where a pixel is fully transparent) below 256 pixels, a PNG at 256.
		std::optional<std::vector<uint8_t>> BuildIconImage(const GameIcon::Image& image, std::string* outError = nullptr);

		// The executable of a game: `runtime` with its version resource (RT_VERSION 1) replaced by the game's and, when `icon`
		// is given, its icon group GLFW_ICON (added when missing) and the icon images only that group names replaced by one
		// image per size of the icon, under the lowest ids no kept image has. The replacements keep the language of what
		// they replace (US English when new); every other resource is kept byte for byte (Write).
		std::optional<std::vector<uint8_t>> StampGame(std::span<const uint8_t> runtime, const GameVersionInfo& info, const GameIcon::Images* icon,
			std::string* outError = nullptr);

	}

}
