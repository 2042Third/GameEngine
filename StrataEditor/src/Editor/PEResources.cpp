#include "Editor/PEResources.h"

#include <Strata/Project/GameInfo.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace Strata
{

	namespace
	{

		// The parts of the PE32+ format the reader and the writer touch (Microsoft's "PE Format" specification).
		constexpr size_t c_DosHeaderSize = 64;
		constexpr size_t c_NewHeaderOffsetField = 0x3C; // e_lfanew
		constexpr uint32_t c_PESignature = 0x00004550;  // "PE\0\0"
		constexpr size_t c_FileHeaderSize = 20;
		constexpr uint16_t c_MachineAMD64 = 0x8664;
		constexpr uint16_t c_MachineARM64 = 0xAA64;
		constexpr uint16_t c_MaxSections = 96;
		constexpr uint16_t c_PE32PlusMagic = 0x20B;
		// The PE32+ optional header up to its data directories, and the offsets of the fields read or written in it.
		constexpr size_t c_OptionalHeaderFixedSize = 112;
		constexpr size_t c_SizeOfInitializedDataField = 8;
		constexpr size_t c_SectionAlignmentField = 32;
		constexpr size_t c_FileAlignmentField = 36;
		constexpr size_t c_SizeOfImageField = 56;
		constexpr size_t c_SizeOfHeadersField = 60;
		constexpr size_t c_CheckSumField = 64;
		constexpr size_t c_NumberOfRvaAndSizesField = 108;
		constexpr size_t c_DataDirectorySize = 8;
		constexpr uint32_t c_MaxDataDirectories = 16;
		constexpr uint32_t c_ResourceDirectory = 2;
		constexpr uint32_t c_CertificateDirectory = 4;
		constexpr uint32_t c_RelocationDirectory = 5;
		constexpr std::array<std::string_view, c_MaxDataDirectories> c_DirectoryNames = { "export", "import", "resource", "exception", "certificate",
			"base relocation", "debug", "architecture", "global pointer", "TLS", "load configuration", "bound import", "import address table",
			"delay import", "CLR runtime header", "reserved" };
		// Section headers and the fields of them that are read or written.
		constexpr size_t c_SectionHeaderSize = 40;
		constexpr size_t c_VirtualSizeField = 8;
		constexpr size_t c_VirtualAddressField = 12;
		constexpr size_t c_SizeOfRawDataField = 16;
		constexpr size_t c_PointerToRawDataField = 20;
		constexpr size_t c_CharacteristicsField = 36;
		constexpr uint32_t c_SectionInitializedData = 0x40; // IMAGE_SCN_CNT_INITIALIZED_DATA
		// The resource tree: directories (a 16-byte header, then 8-byte entries), data entries, names, data.
		constexpr size_t c_ResourceDirectoryHeaderSize = 16;
		constexpr size_t c_ResourceEntrySize = 8;
		constexpr size_t c_ResourceDataEntrySize = 16;
		constexpr uint32_t c_ResourceHighBit = 0x80000000u; // A name (in the id field) or a subdirectory (in the offset field)
		constexpr uint32_t c_MaxTreeOffset = 0x7FFFFFFFu;
		constexpr size_t c_ResourceLevels = 3; // Type, name, language
		constexpr std::array<std::string_view, c_ResourceLevels> c_LevelNames = { "type", "name", "language" };
		// Resource data starts on 8-byte boundaries (the version resource's structures are read as 32-bit aligned).
		constexpr size_t c_ResourceDataAlignment = 8;
		// Base relocation blocks: the page's RVA and the block's size, then 16-bit entries (type in the top 4 bits).
		constexpr size_t c_RelocationBlockHeaderSize = 8;
		constexpr uint16_t c_RelocationAbsolute = 0; // Padding, names nothing
		constexpr uint32_t c_LargestRelocatedSize = 8;
		// Icons: a group directory (reserved, type 1, count) with 14-byte entries, each image's header a BITMAPINFOHEADER.
		constexpr size_t c_IconGroupHeaderSize = 6;
		constexpr size_t c_IconGroupEntrySize = 14;
		constexpr uint32_t c_BitmapInfoHeaderSize = 40;
		constexpr uint32_t c_PNGIconSize = 256;
		// VS_VERSIONINFO.
		constexpr uint32_t c_FixedFileInfoSignature = 0xFEEF04BD;
		constexpr uint32_t c_FixedFileInfoVersion = 0x00010000;
		constexpr uint32_t c_FileFlagsMask = 0x3F;       // VS_FFI_FILEFLAGSMASK
		constexpr uint32_t c_FileOSWindows32 = 0x40004;  // VOS_NT_WINDOWS32
		constexpr uint32_t c_FileTypeApplication = 1;    // VFT_APP
		constexpr uint16_t c_VersionNodeBinary = 0;
		constexpr uint16_t c_VersionNodeText = 1;
		constexpr uint16_t c_CodePageUnicode = 1200;
		constexpr std::u16string_view c_StringTableKey = u"040904B0"; // US English, Unicode

		uint16_t Read16(std::span<const uint8_t> bytes, size_t offset)
		{
			return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
		}

		uint32_t Read32(std::span<const uint8_t> bytes, size_t offset)
		{
			return static_cast<uint32_t>(bytes[offset]) | static_cast<uint32_t>(bytes[offset + 1]) << 8 | static_cast<uint32_t>(bytes[offset + 2]) << 16
				| static_cast<uint32_t>(bytes[offset + 3]) << 24;
		}

		void Write16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value)
		{
			bytes[offset] = static_cast<uint8_t>(value);
			bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
		}

		void Write32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
		{
			for (size_t index = 0; index < 4; index++)
				bytes[offset + index] = static_cast<uint8_t>(value >> (8 * index));
		}

		void Append16(std::vector<uint8_t>& bytes, uint16_t value)
		{
			bytes.push_back(static_cast<uint8_t>(value));
			bytes.push_back(static_cast<uint8_t>(value >> 8));
		}

		void Append32(std::vector<uint8_t>& bytes, uint32_t value)
		{
			for (size_t index = 0; index < 4; index++)
				bytes.push_back(static_cast<uint8_t>(value >> (8 * index)));
		}

		void AppendZeros(std::vector<uint8_t>& bytes, size_t count)
		{
			bytes.insert(bytes.end(), count, 0);
		}

		void PadTo(std::vector<uint8_t>& bytes, size_t alignment)
		{
			AppendZeros(bytes, (alignment - bytes.size() % alignment) % alignment);
		}

		uint64_t AlignUp(uint64_t value, uint64_t alignment)
		{
			return (value + alignment - 1) / alignment * alignment;
		}

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

		// UTF-8 as UTF-16; a byte sequence that is no character becomes U+FFFD (the strings come validated, see
		// ProjectGameInfo::ValidateText, so this only guards against misuse).
		std::u16string ToUTF16(std::string_view text)
		{
			std::u16string result;
			result.reserve(text.size());
			size_t index = 0;
			while (index < text.size())
			{
				const unsigned char lead = static_cast<unsigned char>(text[index]);
				size_t length = 0;
				uint32_t codePoint = 0;
				if (lead < 0x80)
				{
					length = 1;
					codePoint = lead;
				}
				else if ((lead & 0xE0) == 0xC0)
				{
					length = 2;
					codePoint = lead & 0x1F;
				}
				else if ((lead & 0xF0) == 0xE0)
				{
					length = 3;
					codePoint = lead & 0x0F;
				}
				else if ((lead & 0xF8) == 0xF0)
				{
					length = 4;
					codePoint = lead & 0x07;
				}
				bool valid = length != 0 && index + length <= text.size();
				for (size_t offset = 1; valid && offset < length; offset++)
				{
					const unsigned char continuation = static_cast<unsigned char>(text[index + offset]);
					valid = (continuation & 0xC0) == 0x80;
					codePoint = (codePoint << 6) | (continuation & 0x3F);
				}
				constexpr uint32_t c_MinimumByLength[] = { 0, 0, 0x80, 0x800, 0x10000 };
				valid = valid && codePoint >= c_MinimumByLength[length] && !(codePoint >= 0xD800 && codePoint <= 0xDFFF) && codePoint <= 0x10FFFF;
				if (!valid)
				{
					result.push_back(u'�');
					index++;
					continue;
				}
				if (codePoint >= 0x10000)
				{
					// A surrogate pair: the high ten bits, then the low ten bits, of the code point less 0x10000.
					codePoint -= 0x10000;
					result.push_back(static_cast<char16_t>(0xD800 + (codePoint >> 10)));
					result.push_back(static_cast<char16_t>(0xDC00 + (codePoint & 0x3FF)));
				}
				else
				{
					result.push_back(static_cast<char16_t>(codePoint));
				}
				index += length;
			}
			return result;
		}

		// A resource name for messages: printable ASCII as it is, other code units as \uXXXX.
		std::string DescribeId(const PEResources::ResourceId& id)
		{
			if (!id.IsNamed())
				return std::to_string(id.Number);
			std::string text;
			for (const char16_t unit : id.Name)
			{
				if (unit >= 0x20 && unit < 0x7F)
					text += static_cast<char>(unit);
				else
					text += fmt::format("\\u{:04X}", static_cast<uint32_t>(unit));
			}
			return text;
		}

		std::string DescribeResource(const PEResources::Resource& resource)
		{
			return fmt::format("{}/{}/{}", DescribeId(resource.Type), DescribeId(resource.Name), resource.Language);
		}

		struct Section
		{
			std::string Name; // For messages: the header's 8 bytes up to the first zero, other than printable ASCII as '?'
			size_t HeaderOffset = 0;
			uint32_t VirtualSize = 0;
			uint32_t VirtualAddress = 0;
			uint32_t SizeOfRawData = 0;
			uint32_t PointerToRawData = 0;
			uint32_t Characteristics = 0;

			bool ContainsRVA(uint32_t rva) const { return rva >= VirtualAddress && rva - VirtualAddress < std::max(VirtualSize, SizeOfRawData); }
		};

		struct DataDirectory
		{
			uint32_t RVA = 0;
			uint32_t Size = 0;
		};

		// What the headers of a PE32+ image say, every field checked against the file.
		struct ImageLayout
		{
			size_t OptionalHeader = 0; // File offset
			uint32_t SectionAlignment = 0;
			uint32_t FileAlignment = 0;
			uint32_t SizeOfHeaders = 0;
			uint32_t DirectoryCount = 0;
			std::array<DataDirectory, c_MaxDataDirectories> Directories = {};
			std::vector<Section> Sections;
		};

		std::optional<ImageLayout> ReadLayout(std::span<const uint8_t> image, std::string* outError)
		{
			auto fail = [outError](std::string message) -> std::optional<ImageLayout>
			{
				Fail(outError, std::move(message));
				return std::nullopt;
			};
			if (image.size() < c_DosHeaderSize || image[0] != 'M' || image[1] != 'Z')
				return fail("it is no executable: the MZ header is missing");
			const uint32_t peOffset = Read32(image, c_NewHeaderOffsetField);
			if (peOffset > image.size() || image.size() - peOffset < 4 + c_FileHeaderSize)
				return fail(fmt::format("e_lfanew (0x{:x}) points past the end of the file", peOffset));
			if (Read32(image, peOffset) != c_PESignature)
				return fail(fmt::format("the PE signature is missing at e_lfanew (0x{:x})", peOffset));

			const size_t fileHeader = static_cast<size_t>(peOffset) + 4;
			const uint16_t machine = Read16(image, fileHeader);
			if (machine != c_MachineAMD64 && machine != c_MachineARM64)
				return fail(fmt::format("the COFF header's Machine (0x{:04x}) is neither AMD64 nor ARM64", machine));
			const uint16_t sectionCount = Read16(image, fileHeader + 2);
			if (sectionCount == 0 || sectionCount > c_MaxSections)
				return fail(fmt::format("NumberOfSections ({}) is not between 1 and {}", sectionCount, c_MaxSections));
			const uint16_t optionalHeaderSize = Read16(image, fileHeader + 16);
			if (optionalHeaderSize < c_OptionalHeaderFixedSize)
				return fail(fmt::format("SizeOfOptionalHeader ({}) is too small for a PE32+ optional header", optionalHeaderSize));
			ImageLayout layout;
			layout.OptionalHeader = fileHeader + c_FileHeaderSize;
			if (layout.OptionalHeader + optionalHeaderSize > image.size())
				return fail("the optional header runs past the end of the file");
			const uint16_t magic = Read16(image, layout.OptionalHeader);
			if (magic != c_PE32PlusMagic)
				return fail(fmt::format("the optional header's Magic (0x{:x}) is not PE32+ (0x{:x})", magic, c_PE32PlusMagic));

			layout.SectionAlignment = Read32(image, layout.OptionalHeader + c_SectionAlignmentField);
			layout.FileAlignment = Read32(image, layout.OptionalHeader + c_FileAlignmentField);
			const auto isPowerOfTwo = [](uint32_t value) { return value != 0 && (value & (value - 1)) == 0; };
			if (!isPowerOfTwo(layout.FileAlignment) || !isPowerOfTwo(layout.SectionAlignment) || layout.SectionAlignment < layout.FileAlignment)
			{
				return fail(fmt::format("FileAlignment (0x{:x}) and SectionAlignment (0x{:x}) are not powers of two with SectionAlignment the larger",
					layout.FileAlignment, layout.SectionAlignment));
			}
			layout.SizeOfHeaders = Read32(image, layout.OptionalHeader + c_SizeOfHeadersField);
			layout.DirectoryCount = Read32(image, layout.OptionalHeader + c_NumberOfRvaAndSizesField);
			if (layout.DirectoryCount <= c_ResourceDirectory || layout.DirectoryCount > c_MaxDataDirectories)
				return fail(fmt::format("NumberOfRvaAndSizes ({}) is not between {} and {}", layout.DirectoryCount, c_ResourceDirectory + 1, c_MaxDataDirectories));
			if (optionalHeaderSize < c_OptionalHeaderFixedSize + layout.DirectoryCount * c_DataDirectorySize)
				return fail(fmt::format("SizeOfOptionalHeader ({}) is too small for its {} data directories", optionalHeaderSize, layout.DirectoryCount));
			for (uint32_t index = 0; index < layout.DirectoryCount; index++)
			{
				const size_t entry = layout.OptionalHeader + c_OptionalHeaderFixedSize + index * c_DataDirectorySize;
				layout.Directories[index] = { Read32(image, entry), Read32(image, entry + 4) };
				const DataDirectory& directory = layout.Directories[index];
				// The certificate table's "RVA" is a file offset.
				if (index != c_CertificateDirectory && uint64_t(directory.RVA) + directory.Size > std::numeric_limits<uint32_t>::max())
				{
					return fail(fmt::format("the {} directory (RVA 0x{:x}, 0x{:x} bytes) runs past the address space", c_DirectoryNames[index], directory.RVA,
						directory.Size));
				}
			}

			const size_t sectionTable = layout.OptionalHeader + optionalHeaderSize;
			const size_t sectionTableEnd = sectionTable + size_t(sectionCount) * c_SectionHeaderSize;
			if (layout.SizeOfHeaders > image.size())
				return fail(fmt::format("SizeOfHeaders (0x{:x}) is larger than the file (0x{:x} bytes)", layout.SizeOfHeaders, image.size()));
			if (sectionTableEnd > layout.SizeOfHeaders)
				return fail(fmt::format("the section table ends (0x{:x}) past SizeOfHeaders (0x{:x})", sectionTableEnd, layout.SizeOfHeaders));
			for (uint16_t index = 0; index < sectionCount; index++)
			{
				Section section;
				section.HeaderOffset = sectionTable + size_t(index) * c_SectionHeaderSize;
				for (size_t character = 0; character < 8 && image[section.HeaderOffset + character] != 0; character++)
				{
					const uint8_t value = image[section.HeaderOffset + character];
					section.Name += value >= 0x20 && value < 0x7F ? static_cast<char>(value) : '?';
				}
				section.VirtualSize = Read32(image, section.HeaderOffset + c_VirtualSizeField);
				section.VirtualAddress = Read32(image, section.HeaderOffset + c_VirtualAddressField);
				section.SizeOfRawData = Read32(image, section.HeaderOffset + c_SizeOfRawDataField);
				section.PointerToRawData = Read32(image, section.HeaderOffset + c_PointerToRawDataField);
				section.Characteristics = Read32(image, section.HeaderOffset + c_CharacteristicsField);
				if (uint64_t(section.VirtualAddress) + std::max(section.VirtualSize, section.SizeOfRawData) > std::numeric_limits<uint32_t>::max())
					return fail(fmt::format("the section '{}' (VirtualAddress 0x{:x}) runs past the address space", section.Name, section.VirtualAddress));
				if (section.SizeOfRawData > 0)
				{
					if (section.PointerToRawData < layout.SizeOfHeaders)
						return fail(fmt::format("the section '{}' starts its data (PointerToRawData 0x{:x}) inside the headers", section.Name, section.PointerToRawData));
					if (uint64_t(section.PointerToRawData) + section.SizeOfRawData > image.size())
					{
						return fail(fmt::format("the section '{}' (PointerToRawData 0x{:x}, SizeOfRawData 0x{:x}) runs past the end of the file (0x{:x} bytes)",
							section.Name, section.PointerToRawData, section.SizeOfRawData, image.size()));
					}
				}
				if (!layout.Sections.empty())
				{
					const Section& previous = layout.Sections.back();
					if (section.VirtualAddress < uint64_t(previous.VirtualAddress) + std::max(previous.VirtualSize, previous.SizeOfRawData))
						return fail(fmt::format("the section '{}' (VirtualAddress 0x{:x}) overlaps or precedes '{}'", section.Name, section.VirtualAddress, previous.Name));
				}
				layout.Sections.push_back(std::move(section));
			}
			return layout;
		}

		// Reads the resource tree of the section holding the resource directory, checking every directory, entry, name and
		// data entry before it is read and each directory being reached once.
		class ResourceTreeReader
		{
		public:
			ResourceTreeReader(std::span<const uint8_t> tree, std::span<const uint8_t> section, uint32_t sectionRVA, std::string* outError)
				: m_Tree(tree), m_Section(section), m_SectionRVA(sectionRVA), m_Error(outError)
			{
			}

			bool ReadDirectory(uint32_t offset, size_t level, const PEResources::ResourceId& type, const PEResources::ResourceId& name)
			{
				if (!m_Visited.insert(offset).second)
					return Fail(m_Error, fmt::format("the resource directory at offset 0x{:x} is reached twice (a cycle or a shared directory)", offset));
				if (uint64_t(offset) + c_ResourceDirectoryHeaderSize > m_Tree.size())
					return Fail(m_Error, fmt::format("the resource directory at offset 0x{:x} runs past the resource section", offset));
				const uint32_t namedCount = Read16(m_Tree, offset + 12);
				const uint32_t count = namedCount + Read16(m_Tree, offset + 14);
				if (uint64_t(offset) + c_ResourceDirectoryHeaderSize + uint64_t(count) * c_ResourceEntrySize > m_Tree.size())
					return Fail(m_Error, fmt::format("the resource directory at offset 0x{:x} has more entries ({}) than the section holds", offset, count));

				for (uint32_t index = 0; index < count; index++)
				{
					const size_t entry = offset + c_ResourceDirectoryHeaderSize + size_t(index) * c_ResourceEntrySize;
					const uint32_t idField = Read32(m_Tree, entry);
					const uint32_t targetField = Read32(m_Tree, entry + 4);
					const bool named = (idField & c_ResourceHighBit) != 0;
					if (named != (index < namedCount))
						return Fail(m_Error, fmt::format("the resource directory at offset 0x{:x} lists a number among its names or a name among its numbers", offset));
					PEResources::ResourceId id;
					if (named)
					{
						if (level == c_ResourceLevels - 1)
							return Fail(m_Error, fmt::format("a language of resource {}/{} is a name, not a number", DescribeId(type), DescribeId(name)));
						if (!ReadName(idField & ~c_ResourceHighBit, id.Name))
							return false;
					}
					else
					{
						if (idField > std::numeric_limits<uint16_t>::max())
							return Fail(m_Error, fmt::format("the resource {} {} at offset 0x{:x} does not fit 16 bits", c_LevelNames[level], idField, entry));
						id.Number = static_cast<uint16_t>(idField);
					}

					const bool directory = (targetField & c_ResourceHighBit) != 0;
					if (level < c_ResourceLevels - 1)
					{
						if (!directory)
							return Fail(m_Error, fmt::format("the resource {} {} points to data, not to a directory", c_LevelNames[level], DescribeId(id)));
						const bool read = level == 0 ? ReadDirectory(targetField & ~c_ResourceHighBit, level + 1, id, {})
							: ReadDirectory(targetField & ~c_ResourceHighBit, level + 1, type, id);
						if (!read)
							return false;
					}
					else
					{
						if (directory)
							return Fail(m_Error, fmt::format("language {} of resource {}/{} points to a directory, not to data", id.Number, DescribeId(type), DescribeId(name)));
						if (!ReadData(targetField, type, name, id.Number))
							return false;
					}
				}
				return true;
			}

			std::vector<PEResources::Resource> TakeResources() { return std::move(m_Resources); }

		private:
			bool ReadName(uint32_t offset, std::u16string& outName)
			{
				if (uint64_t(offset) + 2 > m_Tree.size())
					return Fail(m_Error, fmt::format("the resource name at offset 0x{:x} lies outside the resource section", offset));
				const uint16_t length = Read16(m_Tree, offset);
				if (length == 0)
					return Fail(m_Error, fmt::format("the resource name at offset 0x{:x} is empty", offset));
				if (uint64_t(offset) + 2 + uint64_t(length) * 2 > m_Tree.size())
					return Fail(m_Error, fmt::format("the resource name at offset 0x{:x} ({} characters) runs past the resource section", offset, length));
				outName.resize(length);
				for (uint16_t index = 0; index < length; index++)
					outName[index] = static_cast<char16_t>(Read16(m_Tree, offset + 2 + size_t(index) * 2));
				return true;
			}

			bool ReadData(uint32_t offset, const PEResources::ResourceId& type, const PEResources::ResourceId& name, uint16_t language)
			{
				PEResources::Resource resource;
				resource.Type = type;
				resource.Name = name;
				resource.Language = language;
				if (uint64_t(offset) + c_ResourceDataEntrySize > m_Tree.size())
					return Fail(m_Error, fmt::format("the data entry of resource {} (offset 0x{:x}) runs past the resource section", DescribeResource(resource), offset));
				const uint32_t rva = Read32(m_Tree, offset);
				const uint32_t size = Read32(m_Tree, offset + 4);
				resource.CodePage = Read32(m_Tree, offset + 8);
				if (rva < m_SectionRVA || uint64_t(rva - m_SectionRVA) + size > m_Section.size())
				{
					return Fail(m_Error, fmt::format("the data of resource {} (RVA 0x{:x}, 0x{:x} bytes) lies outside the resource section", DescribeResource(resource),
						rva, size));
				}
				// The data of distinct resources never overlaps, so together it fits the section: more means entries share data,
				// which a damaged tree could repeat until copying it exhausts memory.
				m_DataBytes += size;
				if (m_DataBytes > m_Section.size())
					return Fail(m_Error, fmt::format("the resources' data adds up to more than the resource section holds (at resource {})", DescribeResource(resource)));
				const auto begin = m_Section.begin() + (rva - m_SectionRVA);
				resource.Data.assign(begin, begin + size);
				m_Resources.push_back(std::move(resource));
				return true;
			}

			std::span<const uint8_t> m_Tree;
			std::span<const uint8_t> m_Section;
			uint32_t m_SectionRVA = 0;
			std::string* m_Error = nullptr;
			std::set<uint32_t> m_Visited;
			uint64_t m_DataBytes = 0;
			std::vector<PEResources::Resource> m_Resources;
		};

		// The section holding the resource directory, which must lie within the section's data.
		std::optional<size_t> FindResourceSection(const ImageLayout& layout, std::string* outError)
		{
			const DataDirectory& directory = layout.Directories[c_ResourceDirectory];
			for (size_t index = 0; index < layout.Sections.size(); index++)
			{
				const Section& section = layout.Sections[index];
				if (!section.ContainsRVA(directory.RVA))
					continue;
				if (uint64_t(directory.RVA - section.VirtualAddress) + std::max<uint32_t>(directory.Size, 1) > section.SizeOfRawData)
				{
					Fail(outError, fmt::format("the resource directory (RVA 0x{:x}, 0x{:x} bytes) runs past the data of its section '{}'", directory.RVA,
						directory.Size, section.Name));
					return std::nullopt;
				}
				return index;
			}
			Fail(outError, fmt::format("the resource directory (RVA 0x{:x}) lies in no section", directory.RVA));
			return std::nullopt;
		}

		std::optional<std::vector<PEResources::Resource>> ReadResources(std::span<const uint8_t> image, const ImageLayout& layout, std::string* outError)
		{
			const DataDirectory& directory = layout.Directories[c_ResourceDirectory];
			if (directory.RVA == 0 && directory.Size == 0)
				return std::vector<PEResources::Resource>();
			const std::optional<size_t> sectionIndex = FindResourceSection(layout, outError);
			if (!sectionIndex)
				return std::nullopt;
			const Section& section = layout.Sections[*sectionIndex];
			const std::span<const uint8_t> sectionData = image.subspan(section.PointerToRawData, section.SizeOfRawData);
			// Offsets in the tree count from the root directory.
			ResourceTreeReader reader(sectionData.subspan(directory.RVA - section.VirtualAddress), sectionData, section.VirtualAddress, outError);
			if (!reader.ReadDirectory(0, 0, {}, {}))
				return std::nullopt;
			return reader.TakeResources();
		}

		// The resource section's contents for the resources, its first byte at `sectionRVA`: every directory (the root, the
		// types', then the names'), the data entries, the names, then the data, each piece on an 8-byte boundary. Entries go
		// in the order the loader searches them (ResourceId's order; languages ascending).
		std::optional<std::vector<uint8_t>> BuildResourceSection(std::span<const PEResources::Resource> resources, uint32_t sectionRVA, std::string* outError)
		{
			using LanguageMap = std::map<uint16_t, const PEResources::Resource*>;
			using NameMap = std::map<PEResources::ResourceId, LanguageMap>;
			std::map<PEResources::ResourceId, NameMap> tree;
			for (const PEResources::Resource& resource : resources)
			{
				if (resource.Type.Name.size() > std::numeric_limits<uint16_t>::max() || resource.Name.Name.size() > std::numeric_limits<uint16_t>::max())
				{
					Fail(outError, fmt::format("the name of resource {} is longer than 65,535 characters", DescribeResource(resource)));
					return std::nullopt;
				}
				if (resource.Data.size() > c_MaxTreeOffset)
				{
					Fail(outError, fmt::format("resource {} is larger than 2 GiB", DescribeResource(resource)));
					return std::nullopt;
				}
				if (!tree[resource.Type][resource.Name].emplace(resource.Language, &resource).second)
				{
					Fail(outError, fmt::format("two resources are {}", DescribeResource(resource)));
					return std::nullopt;
				}
			}

			// A directory counts its entries in 16 bits.
			bool fits = tree.size() <= std::numeric_limits<uint16_t>::max();
			for (const auto& [type, names] : tree)
			{
				fits = fits && names.size() <= std::numeric_limits<uint16_t>::max();
				for (const auto& [name, languages] : names)
					fits = fits && languages.size() <= std::numeric_limits<uint16_t>::max();
			}
			if (!fits)
			{
				Fail(outError, "a resource directory would have more than 65,535 entries");
				return std::nullopt;
			}

			// Where everything goes.
			const auto directorySize = [](size_t entries) { return c_ResourceDirectoryHeaderSize + entries * c_ResourceEntrySize; };
			uint64_t end = directorySize(tree.size());
			std::vector<uint64_t> typeDirectories;
			for (const auto& [type, names] : tree)
			{
				typeDirectories.push_back(end);
				end += directorySize(names.size());
			}
			std::vector<uint64_t> nameDirectories;
			for (const auto& [type, names] : tree)
			{
				for (const auto& [name, languages] : names)
				{
					nameDirectories.push_back(end);
					end += directorySize(languages.size());
				}
			}
			const uint64_t dataEntries = end;
			end += uint64_t(resources.size()) * c_ResourceDataEntrySize;
			std::vector<uint64_t> nameStrings; // One per named id, in the order the directories list them
			const auto placeName = [&end, &nameStrings](const PEResources::ResourceId& id)
			{
				if (!id.IsNamed())
					return;
				nameStrings.push_back(end);
				end += 2 + uint64_t(id.Name.size()) * 2;
			};
			for (const auto& [type, names] : tree)
				placeName(type);
			for (const auto& [type, names] : tree)
			{
				for (const auto& [name, languages] : names)
					placeName(name);
			}
			std::vector<uint64_t> data;
			for (const auto& [type, names] : tree)
			{
				for (const auto& [name, languages] : names)
				{
					for (const auto& [language, resource] : languages)
					{
						end = AlignUp(end, c_ResourceDataAlignment);
						data.push_back(end);
						end += resource->Data.size();
					}
				}
			}
			if (end > c_MaxTreeOffset || uint64_t(sectionRVA) + end > std::numeric_limits<uint32_t>::max())
			{
				Fail(outError, "the resources do not fit a resource section (2 GiB at most)");
				return std::nullopt;
			}

			std::vector<uint8_t> bytes;
			bytes.reserve(static_cast<size_t>(end));
			const auto appendDirectory = [&bytes](size_t namedCount, size_t numberCount)
			{
				AppendZeros(bytes, 12); // Characteristics, TimeDateStamp and the version: zero, so the bytes depend on the resources only
				Append16(bytes, static_cast<uint16_t>(namedCount));
				Append16(bytes, static_cast<uint16_t>(numberCount));
			};
			size_t nextName = 0;
			const auto appendEntry = [&bytes, &nameStrings, &nextName](const PEResources::ResourceId& id, uint32_t target)
			{
				Append32(bytes, id.IsNamed() ? static_cast<uint32_t>(nameStrings[nextName++]) | c_ResourceHighBit : id.Number);
				Append32(bytes, target);
			};
			const auto countNamed = [](const auto& map)
			{
				return static_cast<size_t>(std::count_if(map.begin(), map.end(), [](const auto& item) { return item.first.IsNamed(); }));
			};
			// The root, then each type's directory, then each name's directory: the names are numbered in that order too.
			appendDirectory(countNamed(tree), tree.size() - countNamed(tree));
			size_t typeIndex = 0;
			for (const auto& [type, names] : tree)
				appendEntry(type, static_cast<uint32_t>(typeDirectories[typeIndex++]) | c_ResourceHighBit);
			size_t nameIndex = 0;
			for (const auto& [type, names] : tree)
			{
				appendDirectory(countNamed(names), names.size() - countNamed(names));
				for (const auto& [name, languages] : names)
					appendEntry(name, static_cast<uint32_t>(nameDirectories[nameIndex++]) | c_ResourceHighBit);
			}
			size_t resourceIndex = 0;
			for (const auto& [type, names] : tree)
			{
				for (const auto& [name, languages] : names)
				{
					appendDirectory(0, languages.size());
					for (const auto& [language, resource] : languages)
					{
						Append32(bytes, language);
						Append32(bytes, static_cast<uint32_t>(dataEntries + resourceIndex++ * c_ResourceDataEntrySize));
					}
				}
			}
			resourceIndex = 0;
			for (const auto& [type, names] : tree)
			{
				for (const auto& [name, languages] : names)
				{
					for (const auto& [language, resource] : languages)
					{
						Append32(bytes, static_cast<uint32_t>(sectionRVA + data[resourceIndex++]));
						Append32(bytes, static_cast<uint32_t>(resource->Data.size()));
						Append32(bytes, resource->CodePage);
						Append32(bytes, 0);
					}
				}
			}
			const auto appendName = [&bytes](const PEResources::ResourceId& id)
			{
				if (!id.IsNamed())
					return;
				Append16(bytes, static_cast<uint16_t>(id.Name.size()));
				for (const char16_t unit : id.Name)
					Append16(bytes, static_cast<uint16_t>(unit));
			};
			for (const auto& [type, names] : tree)
				appendName(type);
			for (const auto& [type, names] : tree)
			{
				for (const auto& [name, languages] : names)
					appendName(name);
			}
			for (const auto& [type, names] : tree)
			{
				for (const auto& [name, languages] : names)
				{
					for (const auto& [language, resource] : languages)
					{
						PadTo(bytes, c_ResourceDataAlignment);
						bytes.insert(bytes.end(), resource->Data.begin(), resource->Data.end());
					}
				}
			}
			return bytes;
		}

		// The base relocations name only RVAs before the resource section (whose contents are rebuilt and after which the
		// relocations' own section moves).
		bool CheckRelocations(std::span<const uint8_t> image, const ImageLayout& layout, const Section& relocations, uint32_t resourceRVA, std::string* outError)
		{
			const DataDirectory& directory = layout.Directories[c_RelocationDirectory];
			if (uint64_t(directory.RVA - relocations.VirtualAddress) + directory.Size > relocations.SizeOfRawData)
			{
				return Fail(outError, fmt::format("the base relocation directory (RVA 0x{:x}, 0x{:x} bytes) runs past the data of its section '{}'", directory.RVA,
					directory.Size, relocations.Name));
			}
			const std::span<const uint8_t> blocks = image.subspan(relocations.PointerToRawData + (directory.RVA - relocations.VirtualAddress), directory.Size);
			size_t offset = 0;
			while (offset < blocks.size())
			{
				if (blocks.size() - offset < c_RelocationBlockHeaderSize)
					return Fail(outError, fmt::format("the base relocation block at offset 0x{:x} is cut off", offset));
				const uint32_t page = Read32(blocks, offset);
				const uint32_t blockSize = Read32(blocks, offset + 4);
				if (blockSize < c_RelocationBlockHeaderSize || blockSize % 2 != 0 || blockSize > blocks.size() - offset)
					return Fail(outError, fmt::format("the base relocation block at offset 0x{:x} has an invalid SizeOfBlock (0x{:x})", offset, blockSize));
				for (size_t entry = offset + c_RelocationBlockHeaderSize; entry < offset + blockSize; entry += 2)
				{
					const uint16_t value = Read16(blocks, entry);
					if ((value >> 12) == c_RelocationAbsolute)
						continue;
					const uint64_t target = uint64_t(page) + (value & 0xFFF);
					if (target + c_LargestRelocatedSize > resourceRVA)
						return Fail(outError, fmt::format("a base relocation names RVA 0x{:x}, in or after the resource section", target));
				}
				offset += blockSize;
			}
			return true;
		}

		// The groups of an icon resource: the ids of the RT_ICON images an RT_GROUP_ICON names.
		std::optional<std::vector<uint16_t>> ReadIconGroup(std::span<const uint8_t> data)
		{
			if (data.size() < c_IconGroupHeaderSize || Read16(data, 0) != 0 || Read16(data, 2) != 1)
				return std::nullopt;
			const size_t count = Read16(data, 4);
			if (data.size() < c_IconGroupHeaderSize + count * c_IconGroupEntrySize)
				return std::nullopt;
			std::vector<uint16_t> ids;
			for (size_t index = 0; index < count; index++)
				ids.push_back(Read16(data, c_IconGroupHeaderSize + index * c_IconGroupEntrySize + 12));
			return ids;
		}

		bool IsGameIconGroup(const PEResources::Resource& resource)
		{
			return resource.Type == PEResources::ResourceId::FromNumber(PEResources::c_TypeGroupIcon) && resource.Name.Name == PEResources::c_GameIconGroup;
		}

		// One node of VS_VERSIONINFO's tree: its length, its value's length (bytes, or characters for text), its type, its key
		// (UTF-16, terminated), padding to 32 bits, its value, then each child on a 32-bit boundary.
		struct VersionNode
		{
			std::u16string Key;
			uint16_t Type = c_VersionNodeText;
			std::vector<uint8_t> Value;
			uint16_t ValueLength = 0;
			std::vector<VersionNode> Children;
		};

		VersionNode MakeStringNode(std::u16string_view key, std::string_view value)
		{
			VersionNode node;
			node.Key = std::u16string(key);
			const std::u16string text = ToUTF16(value);
			for (const char16_t unit : text)
				Append16(node.Value, static_cast<uint16_t>(unit));
			Append16(node.Value, 0);
			// Text values count characters, the terminator included; text too long for the field fails the node's length.
			node.ValueLength = static_cast<uint16_t>(std::min<size_t>(text.size() + 1, std::numeric_limits<uint16_t>::max()));
			return node;
		}

		bool AppendVersionNode(std::vector<uint8_t>& bytes, const VersionNode& node)
		{
			PadTo(bytes, 4);
			const size_t start = bytes.size();
			Append16(bytes, 0); // The length, known at the end
			Append16(bytes, node.ValueLength);
			Append16(bytes, node.Type);
			for (const char16_t unit : node.Key)
				Append16(bytes, static_cast<uint16_t>(unit));
			Append16(bytes, 0);
			if (!node.Value.empty())
			{
				PadTo(bytes, 4);
				bytes.insert(bytes.end(), node.Value.begin(), node.Value.end());
			}
			for (const VersionNode& child : node.Children)
			{
				if (!AppendVersionNode(bytes, child))
					return false;
			}
			const size_t length = bytes.size() - start;
			if (length > std::numeric_limits<uint16_t>::max())
				return false;
			Write16(bytes, start, static_cast<uint16_t>(length));
			return true;
		}

	}

	namespace PEResources
	{

		bool ResourceId::operator<(const ResourceId& other) const
		{
			if (IsNamed() != other.IsNamed())
				return IsNamed();
			if (IsNamed())
				return Name < other.Name;
			return Number < other.Number;
		}

		std::optional<std::vector<Resource>> Read(std::span<const uint8_t> image, std::string* outError)
		{
			const std::optional<ImageLayout> layout = ReadLayout(image, outError);
			if (!layout)
				return std::nullopt;
			return ReadResources(image, *layout, outError);
		}

		std::optional<std::vector<uint8_t>> Write(std::span<const uint8_t> image, std::span<const Resource> resources, std::string* outError)
		{
			auto fail = [outError](std::string message) -> std::optional<std::vector<uint8_t>>
			{
				Fail(outError, std::move(message));
				return std::nullopt;
			};
			const std::optional<ImageLayout> layout = ReadLayout(image, outError);
			if (!layout)
				return std::nullopt;
			const DataDirectory& resourceDirectory = layout->Directories[c_ResourceDirectory];
			if (resourceDirectory.RVA == 0 && resourceDirectory.Size == 0)
				return fail("the image has no resource section to rebuild");
			// The tree is read in full first: a damaged one is refused before anything is written.
			if (!ReadResources(image, *layout, outError))
				return std::nullopt;
			const std::optional<size_t> resourceIndex = FindResourceSection(*layout, outError);
			if (!resourceIndex)
				return std::nullopt;
			const Section& resourceSection = layout->Sections[*resourceIndex];
			if (resourceDirectory.RVA != resourceSection.VirtualAddress)
				return fail(fmt::format("the resource directory (RVA 0x{:x}) does not start its section '{}'", resourceDirectory.RVA, resourceSection.Name));
			if (resourceSection.SizeOfRawData == 0 || resourceSection.SizeOfRawData % layout->FileAlignment != 0)
				return fail(fmt::format("the resource section's SizeOfRawData (0x{:x}) is not a multiple of FileAlignment", resourceSection.SizeOfRawData));

			// The supported layout: unsigned, nothing after the last section, and only the base relocations after the resources.
			// Directories past NumberOfRvaAndSizes stay zero in the layout: absent.
			if (layout->Directories[c_CertificateDirectory].Size != 0)
				return fail("the image is signed (it has a certificate table), and stamping would break the signature: sign the game after it is exported");
			uint64_t dataEnd = layout->SizeOfHeaders;
			for (const Section& section : layout->Sections)
			{
				if (section.SizeOfRawData > 0)
					dataEnd = std::max<uint64_t>(dataEnd, uint64_t(section.PointerToRawData) + section.SizeOfRawData);
			}
			if (image.size() > dataEnd)
				return fail(fmt::format("the image has 0x{:x} bytes after its last section (overlay data), which the stamp cannot keep", image.size() - dataEnd));
			for (size_t index = 0; index < *resourceIndex; index++)
			{
				const Section& section = layout->Sections[index];
				if (section.SizeOfRawData > 0 && uint64_t(section.PointerToRawData) + section.SizeOfRawData > resourceSection.PointerToRawData)
					return fail(fmt::format("the data of the section '{}' lies after the resource section's", section.Name));
			}
			const Section* moved = nullptr;
			if (layout->Sections.size() > *resourceIndex + 2)
			{
				return fail(fmt::format("the sections '{}' and '{}' follow the resource section '{}': only the base relocations may", layout->Sections[*resourceIndex + 1].Name,
					layout->Sections[*resourceIndex + 2].Name, resourceSection.Name));
			}
			const DataDirectory& relocationDirectory = layout->Directories[c_RelocationDirectory];
			if (layout->Sections.size() == *resourceIndex + 2)
			{
				moved = &layout->Sections[*resourceIndex + 1];
				if (relocationDirectory.Size == 0 || !moved->ContainsRVA(relocationDirectory.RVA))
				{
					return fail(fmt::format("the section '{}' after the resource section '{}' holds no base relocations: only those may follow it", moved->Name,
						resourceSection.Name));
				}
				if (moved->VirtualAddress != AlignUp(uint64_t(resourceSection.VirtualAddress) + resourceSection.VirtualSize, layout->SectionAlignment)
					|| moved->PointerToRawData != uint64_t(resourceSection.PointerToRawData) + resourceSection.SizeOfRawData)
					return fail(fmt::format("the section '{}' does not directly follow the resource section '{}'", moved->Name, resourceSection.Name));
				if (!CheckRelocations(image, *layout, *moved, resourceSection.VirtualAddress, outError))
					return std::nullopt;
			}
			else if (relocationDirectory.Size != 0 && uint64_t(relocationDirectory.RVA) + relocationDirectory.Size > resourceSection.VirtualAddress)
			{
				return fail("the base relocation directory lies in or after the resource section");
			}
			for (uint32_t index = 0; index < layout->DirectoryCount; index++)
			{
				const DataDirectory& directory = layout->Directories[index];
				if (index == c_ResourceDirectory || index == c_CertificateDirectory || index == c_RelocationDirectory || directory.Size == 0)
					continue;
				if (uint64_t(directory.RVA) + directory.Size > resourceSection.VirtualAddress)
				{
					return fail(fmt::format("the {} directory (RVA 0x{:x}, 0x{:x} bytes) reaches into or past the resource section, which the stamp rebuilds",
						c_DirectoryNames[index], directory.RVA, directory.Size));
				}
			}

			const std::optional<std::vector<uint8_t>> contents = BuildResourceSection(resources, resourceSection.VirtualAddress, outError);
			if (!contents)
				return std::nullopt;
			const uint64_t newVirtualSize = contents->size();
			const uint64_t newRawSize = AlignUp(newVirtualSize, layout->FileAlignment);
			const uint64_t oldEnd = AlignUp(uint64_t(resourceSection.VirtualAddress) + resourceSection.VirtualSize, layout->SectionAlignment);
			const uint64_t newEnd = AlignUp(uint64_t(resourceSection.VirtualAddress) + newVirtualSize, layout->SectionAlignment);
			const int64_t rvaShift = static_cast<int64_t>(newEnd) - static_cast<int64_t>(oldEnd);
			const int64_t fileShift = static_cast<int64_t>(newRawSize) - static_cast<int64_t>(resourceSection.SizeOfRawData);
			const uint64_t imageEnd = moved ? AlignUp(uint64_t(int64_t(moved->VirtualAddress) + rvaShift) + moved->VirtualSize, layout->SectionAlignment) : newEnd;
			if (imageEnd > std::numeric_limits<uint32_t>::max() || uint64_t(resourceSection.PointerToRawData) + newRawSize + (moved ? moved->SizeOfRawData : 0)
				> std::numeric_limits<uint32_t>::max())
				return fail("the stamped image would be larger than 4 GiB");

			const size_t optionalHeader = layout->OptionalHeader;
			const uint32_t initializedData = Read32(image, optionalHeader + c_SizeOfInitializedDataField);
			uint64_t newInitializedData = initializedData;
			if ((resourceSection.Characteristics & c_SectionInitializedData) != 0)
			{
				if (initializedData < resourceSection.SizeOfRawData)
				{
					return fail(fmt::format("SizeOfInitializedData (0x{:x}) is smaller than the resource section's data (0x{:x})", initializedData,
						resourceSection.SizeOfRawData));
				}
				newInitializedData = initializedData - resourceSection.SizeOfRawData + newRawSize;
				if (newInitializedData > std::numeric_limits<uint32_t>::max())
					return fail("SizeOfInitializedData would pass 4 GiB");
			}

			// The image up to the resource section's data as it is, the new resource section, then the moved section's data.
			std::vector<uint8_t> output(image.begin(), image.begin() + resourceSection.PointerToRawData);
			output.insert(output.end(), contents->begin(), contents->end());
			PadTo(output, layout->FileAlignment);
			if (moved)
			{
				const auto begin = image.begin() + moved->PointerToRawData;
				output.insert(output.end(), begin, begin + moved->SizeOfRawData);
			}

			Write32(output, resourceSection.HeaderOffset + c_VirtualSizeField, static_cast<uint32_t>(newVirtualSize));
			Write32(output, resourceSection.HeaderOffset + c_SizeOfRawDataField, static_cast<uint32_t>(newRawSize));
			const size_t directories = optionalHeader + c_OptionalHeaderFixedSize;
			Write32(output, directories + c_ResourceDirectory * c_DataDirectorySize + 4, static_cast<uint32_t>(newVirtualSize));
			if (moved)
			{
				Write32(output, moved->HeaderOffset + c_VirtualAddressField, static_cast<uint32_t>(int64_t(moved->VirtualAddress) + rvaShift));
				Write32(output, moved->HeaderOffset + c_PointerToRawDataField, static_cast<uint32_t>(int64_t(moved->PointerToRawData) + fileShift));
				Write32(output, directories + c_RelocationDirectory * c_DataDirectorySize, static_cast<uint32_t>(int64_t(relocationDirectory.RVA) + rvaShift));
			}
			Write32(output, optionalHeader + c_SizeOfImageField, static_cast<uint32_t>(imageEnd));
			Write32(output, optionalHeader + c_SizeOfInitializedDataField, static_cast<uint32_t>(newInitializedData));
			Write32(output, optionalHeader + c_CheckSumField, 0);
			Write32(output, optionalHeader + c_CheckSumField, ComputeChecksum(output));
			return output;
		}

		uint32_t ComputeChecksum(std::span<const uint8_t> image)
		{
			// Where CheckSum lies, when the image has headers to hold one: it counts as zero.
			size_t checkSumOffset = std::numeric_limits<size_t>::max();
			if (image.size() >= c_DosHeaderSize)
			{
				const uint64_t offset = uint64_t(Read32(image, c_NewHeaderOffsetField)) + 4 + c_FileHeaderSize + c_CheckSumField;
				if (offset + 4 <= image.size())
					checkSumOffset = static_cast<size_t>(offset);
			}
			const auto byteAt = [&image, checkSumOffset](size_t offset) -> uint32_t
			{
				const bool inCheckSum = offset >= checkSumOffset && offset - checkSumOffset < 4;
				return offset < image.size() && !inCheckSum ? image[offset] : 0u;
			};
			uint64_t sum = 0;
			for (size_t offset = 0; offset < image.size(); offset += 2)
			{
				const uint32_t word = byteAt(offset) | byteAt(offset + 1) << 8;
				sum += word;
				sum = (sum & 0xFFFF) + (sum >> 16);
			}
			sum = (sum & 0xFFFF) + (sum >> 16);
			return static_cast<uint32_t>(sum + image.size());
		}

		std::optional<std::vector<uint8_t>> BuildVersionInfo(const GameVersionInfo& info, std::string* outError)
		{
			const std::optional<std::array<uint16_t, ProjectGameInfo::c_MaxVersionParts>> version = ProjectGameInfo::ParseVersion(info.Version);
			if (!version)
			{
				Fail(outError, fmt::format("'{}' is no version: {}", info.Version, ProjectGameInfo::ValidateVersion(info.Version).value_or("")));
				return std::nullopt;
			}
			const uint32_t versionHigh = uint32_t((*version)[0]) << 16 | (*version)[1];
			const uint32_t versionLow = uint32_t((*version)[2]) << 16 | (*version)[3];

			VersionNode root;
			root.Key = u"VS_VERSION_INFO";
			root.Type = c_VersionNodeBinary;
			// VS_FIXEDFILEINFO: the file's and the product's version, then flags, OS and type of an application.
			for (const uint32_t value : { c_FixedFileInfoSignature, c_FixedFileInfoVersion, versionHigh, versionLow, versionHigh, versionLow, c_FileFlagsMask,
					 0u, c_FileOSWindows32, c_FileTypeApplication, 0u, 0u, 0u })
				Append32(root.Value, value);
			root.ValueLength = static_cast<uint16_t>(root.Value.size());

			VersionNode table;
			table.Key = std::u16string(c_StringTableKey);
			if (!info.Company.empty())
				table.Children.push_back(MakeStringNode(u"CompanyName", info.Company));
			table.Children.push_back(MakeStringNode(u"FileDescription", info.Title));
			table.Children.push_back(MakeStringNode(u"FileVersion", info.Version));
			table.Children.push_back(MakeStringNode(u"InternalName", info.InternalName));
			if (!info.Copyright.empty())
				table.Children.push_back(MakeStringNode(u"LegalCopyright", info.Copyright));
			table.Children.push_back(MakeStringNode(u"OriginalFilename", info.OriginalFilename));
			table.Children.push_back(MakeStringNode(u"ProductName", info.Title));
			table.Children.push_back(MakeStringNode(u"ProductVersion", info.Version));
			VersionNode strings;
			strings.Key = u"StringFileInfo";
			strings.Children.push_back(std::move(table));

			VersionNode translation;
			translation.Key = u"Translation";
			translation.Type = c_VersionNodeBinary;
			Append16(translation.Value, c_LanguageEnglishUS);
			Append16(translation.Value, c_CodePageUnicode);
			translation.ValueLength = static_cast<uint16_t>(translation.Value.size());
			VersionNode variables;
			variables.Key = u"VarFileInfo";
			variables.Children.push_back(std::move(translation));

			root.Children.push_back(std::move(strings));
			root.Children.push_back(std::move(variables));
			std::vector<uint8_t> bytes;
			if (!AppendVersionNode(bytes, root))
			{
				Fail(outError, "the version information is longer than its 64 KiB limit: shorten the title, company, copyright or name");
				return std::nullopt;
			}
			return bytes;
		}

		std::optional<std::vector<uint8_t>> BuildIconImage(const GameIcon::Image& image, std::string* outError)
		{
			if (image.Size == 0 || image.Size > c_PNGIconSize || image.Pixels.size() != size_t(image.Size) * image.Size * 4)
			{
				Fail(outError, fmt::format("an icon image of {} pixels with {} bytes cannot be an icon", image.Size, image.Pixels.size()));
				return std::nullopt;
			}
			if (image.Size == c_PNGIconSize)
				return GameIcon::EncodePNG(image, outError);

			// BITMAPINFOHEADER: the height counts the color rows and the mask's; BI_RGB, no palette.
			const uint32_t size = image.Size;
			const uint32_t maskRowBytes = (size + 31) / 32 * 4;
			std::vector<uint8_t> bytes;
			bytes.reserve(c_BitmapInfoHeaderSize + size_t(size) * size * 4 + size_t(maskRowBytes) * size);
			Append32(bytes, c_BitmapInfoHeaderSize);
			Append32(bytes, size);
			Append32(bytes, size * 2);
			Append16(bytes, 1);  // Planes
			Append16(bytes, 32); // Bits per pixel
			Append32(bytes, 0);  // BI_RGB
			Append32(bytes, size * size * 4 + maskRowBytes * size);
			AppendZeros(bytes, 16); // Resolution and palette: none
			for (uint32_t row = size; row-- > 0;)
			{
				for (uint32_t column = 0; column < size; column++)
				{
					const uint8_t* pixel = image.Pixels.data() + (size_t(row) * size + column) * 4;
					bytes.push_back(pixel[2]);
					bytes.push_back(pixel[1]);
					bytes.push_back(pixel[0]);
					bytes.push_back(pixel[3]);
				}
			}
			// The AND mask: a set bit (most significant first) is a transparent pixel for whatever ignores the alpha channel.
			for (uint32_t row = size; row-- > 0;)
			{
				const size_t start = bytes.size();
				AppendZeros(bytes, maskRowBytes);
				for (uint32_t column = 0; column < size; column++)
				{
					if (image.Pixels[(size_t(row) * size + column) * 4 + 3] == 0)
						bytes[start + column / 8] = static_cast<uint8_t>(bytes[start + column / 8] | (0x80 >> (column % 8)));
				}
			}
			return bytes;
		}

		std::optional<std::vector<uint8_t>> StampGame(std::span<const uint8_t> runtime, const GameVersionInfo& info, const GameIcon::Images* icon, std::string* outError)
		{
			auto fail = [outError](std::string message) -> std::optional<std::vector<uint8_t>>
			{
				Fail(outError, std::move(message));
				return std::nullopt;
			};
			std::string readError;
			std::optional<std::vector<Resource>> resources = Read(runtime, &readError);
			if (!resources)
				return fail(fmt::format("the runtime is no Windows executable the stamp can read: {}", readError));
			const std::optional<std::vector<uint8_t>> version = BuildVersionInfo(info, outError);
			if (!version)
				return std::nullopt;

			// The version replaces RT_VERSION 1 in the language it had.
			const ResourceId versionType = ResourceId::FromNumber(c_TypeVersion);
			const ResourceId versionName = ResourceId::FromNumber(c_VersionResource);
			uint16_t versionLanguage = c_LanguageEnglishUS;
			const auto oldVersion = std::find_if(resources->begin(), resources->end(),
				[&](const Resource& resource) { return resource.Type == versionType && resource.Name == versionName; });
			if (oldVersion != resources->end())
				versionLanguage = oldVersion->Language;
			std::erase_if(*resources, [&](const Resource& resource) { return resource.Type == versionType && resource.Name == versionName; });
			resources->push_back(Resource { versionType, versionName, versionLanguage, 0, *version });

			if (icon && icon->Sizes.empty())
				return fail("the game's icon has no images");
			if (icon)
			{
				// The images the game's group names and no other group does make way for the icon's.
				const ResourceId iconType = ResourceId::FromNumber(c_TypeIcon);
				uint16_t iconLanguage = c_LanguageEnglishUS;
				std::set<uint16_t> replaced;
				std::set<uint16_t> kept;
				bool foundGroup = false;
				for (const Resource& resource : *resources)
				{
					if (resource.Type != ResourceId::FromNumber(c_TypeGroupIcon))
						continue;
					const std::optional<std::vector<uint16_t>> ids = ReadIconGroup(resource.Data);
					if (!ids)
						return fail(fmt::format("the runtime's icon group {} is damaged", DescribeResource(resource)));
					const bool game = IsGameIconGroup(resource);
					if (game && !foundGroup)
					{
						iconLanguage = resource.Language;
						foundGroup = true;
					}
					(game ? replaced : kept).insert(ids->begin(), ids->end());
				}
				std::erase_if(*resources, [&](const Resource& resource)
				{
					return IsGameIconGroup(resource) || (resource.Type == iconType && !resource.Name.IsNamed() && replaced.contains(resource.Name.Number)
						&& !kept.contains(resource.Name.Number));
				});
				std::set<uint16_t> used;
				for (const Resource& resource : *resources)
				{
					if (resource.Type == iconType && !resource.Name.IsNamed())
						used.insert(resource.Name.Number);
				}

				std::vector<uint8_t> group;
				Append16(group, 0);
				Append16(group, 1); // Icons
				Append16(group, static_cast<uint16_t>(icon->Sizes.size()));
				uint32_t nextId = 1;
				for (const GameIcon::Image& image : icon->Sizes)
				{
					std::optional<std::vector<uint8_t>> data = BuildIconImage(image, outError);
					if (!data)
						return std::nullopt;
					while (nextId <= std::numeric_limits<uint16_t>::max() && used.contains(static_cast<uint16_t>(nextId)))
						nextId++;
					if (nextId > std::numeric_limits<uint16_t>::max() || icon->Sizes.size() > std::numeric_limits<uint16_t>::max())
						return fail("the runtime has no free icon ids left");
					const uint16_t id = static_cast<uint16_t>(nextId++);
					// GRPICONDIRENTRY: width and height (0 for 256), no palette, one plane of 32 bits, the image's size and id.
					const uint8_t dimension = image.Size >= c_PNGIconSize ? 0 : static_cast<uint8_t>(image.Size);
					group.push_back(dimension);
					group.push_back(dimension);
					group.push_back(0);
					group.push_back(0);
					Append16(group, 1);
					Append16(group, 32);
					Append32(group, static_cast<uint32_t>(data->size()));
					Append16(group, id);
					resources->push_back(Resource { iconType, ResourceId::FromNumber(id), iconLanguage, 0, std::move(*data) });
				}
				resources->push_back(Resource { ResourceId::FromNumber(c_TypeGroupIcon), ResourceId::FromName(c_GameIconGroup), iconLanguage, 0, std::move(group) });
			}
			return Write(runtime, *resources, outError);
		}

	}

}
