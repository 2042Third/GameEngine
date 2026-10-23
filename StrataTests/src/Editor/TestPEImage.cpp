#include "Editor/TestPEImage.h"

#include <algorithm>
#include <string_view>

namespace Strata::Tests
{

	namespace
	{

		constexpr uint32_t c_SectionAlignment = 0x1000;
		constexpr uint32_t c_FileAlignment = 0x200;
		constexpr uint32_t c_PEOffset = 0x80;
		constexpr uint32_t c_OptionalHeaderSize = 112 + 16 * 8;
		constexpr uint32_t c_TextRVA = 0x1000;
		constexpr uint32_t c_TextSize = 0x2F0;
		constexpr uint32_t c_InitializedData = 0x40;

		uint32_t AlignUp(uint32_t value, uint32_t alignment)
		{
			return (value + alignment - 1) / alignment * alignment;
		}

		void Put16(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
		{
			if (bytes.size() < offset + 2)
				bytes.resize(offset + 2, 0);
			bytes[offset] = static_cast<uint8_t>(value);
			bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
		}

		void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
		{
			Put16(bytes, offset, value & 0xFFFF);
			Put16(bytes, offset + 2, value >> 16);
		}

		void Put64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value)
		{
			Put32(bytes, offset, static_cast<uint32_t>(value));
			Put32(bytes, offset + 4, static_cast<uint32_t>(value >> 32));
		}

		uint32_t Get16(std::span<const uint8_t> bytes, size_t offset)
		{
			if (offset + 2 > bytes.size())
				return 0;
			return static_cast<uint32_t>(bytes[offset] | bytes[offset + 1] << 8);
		}

		uint32_t Get32(std::span<const uint8_t> bytes, size_t offset)
		{
			return Get16(bytes, offset) | Get16(bytes, offset + 2) << 16;
		}

		std::vector<uint8_t> Bytes(std::string_view text)
		{
			return std::vector<uint8_t>(text.begin(), text.end());
		}

		std::vector<uint8_t> Sequence(size_t count, uint8_t first)
		{
			std::vector<uint8_t> bytes(count);
			for (size_t index = 0; index < count; index++)
				bytes[index] = static_cast<uint8_t>(first + index);
			return bytes;
		}

		// An icon group naming images by size and id (GRPICONDIR and its entries).
		std::vector<uint8_t> MakeGroup(const std::vector<std::pair<uint8_t, uint16_t>>& images, const std::vector<PEResources::Resource>& resources)
		{
			std::vector<uint8_t> group;
			Put16(group, 0, 0);
			Put16(group, 2, 1);
			Put16(group, 4, static_cast<uint32_t>(images.size()));
			for (size_t index = 0; index < images.size(); index++)
			{
				const size_t entry = 6 + index * 14;
				size_t bytes = 0;
				for (const PEResources::Resource& resource : resources)
				{
					if (resource.Type.Number == PEResources::c_TypeIcon && resource.Name.Number == images[index].second)
						bytes = resource.Data.size();
				}
				group.resize(entry + 14, 0);
				group[entry] = images[index].first;
				group[entry + 1] = images[index].first;
				Put16(group, entry + 4, 1);
				Put16(group, entry + 6, 32);
				Put32(group, entry + 8, static_cast<uint32_t>(bytes));
				Put16(group, entry + 12, images[index].second);
			}
			return group;
		}

		// The resource tree depth first: the root, then each type's directory followed by its names' (language) directories;
		// then the names, the data entries and the data, on 4-byte boundaries.
		std::vector<uint8_t> BuildResourceTree(const std::vector<PEResources::Resource>& resources, uint32_t sectionRVA)
		{
			std::map<PEResources::ResourceId, std::map<PEResources::ResourceId, std::map<uint16_t, const PEResources::Resource*>>> tree;
			for (const PEResources::Resource& resource : resources)
				tree[resource.Type][resource.Name][resource.Language] = &resource;
			const auto countNamed = [](const auto& map)
			{
				return static_cast<uint32_t>(std::count_if(map.begin(), map.end(), [](const auto& item) { return item.first.IsNamed(); }));
			};

			// Every offset first: directories, names, data entries, data.
			uint32_t end = 16 + 8 * static_cast<uint32_t>(tree.size());
			std::map<const void*, uint32_t> directoryOffsets;
			for (const auto& [type, names] : tree)
			{
				directoryOffsets[&names] = end;
				end += 16 + 8 * static_cast<uint32_t>(names.size());
				for (const auto& [name, languages] : names)
				{
					directoryOffsets[&languages] = end;
					end += 16 + 8 * static_cast<uint32_t>(languages.size());
				}
			}
			std::map<const std::u16string*, uint32_t> nameOffsets;
			for (const auto& [type, names] : tree)
			{
				if (type.IsNamed())
				{
					nameOffsets[&type.Name] = end;
					end += 2 + 2 * static_cast<uint32_t>(type.Name.size());
				}
				for (const auto& [name, languages] : names)
				{
					if (name.IsNamed())
					{
						nameOffsets[&name.Name] = end;
						end += 2 + 2 * static_cast<uint32_t>(name.Name.size());
					}
				}
			}
			end = AlignUp(end, 4);
			std::map<const PEResources::Resource*, uint32_t> entryOffsets;
			for (const PEResources::Resource& resource : resources)
			{
				entryOffsets[&resource] = end;
				end += 16;
			}
			std::map<const PEResources::Resource*, uint32_t> dataOffsets;
			for (const PEResources::Resource& resource : resources)
			{
				end = AlignUp(end, 4);
				dataOffsets[&resource] = end;
				end += static_cast<uint32_t>(resource.Data.size());
			}

			std::vector<uint8_t> bytes(end, 0);
			const auto writeDirectory = [&bytes](uint32_t offset, uint32_t named, uint32_t numbered)
			{
				Put32(bytes, offset + 4, 0x5F000000); // A time stamp, as the resource compiler writes one
				Put16(bytes, offset + 8, 4);
				Put16(bytes, offset + 12, named);
				Put16(bytes, offset + 14, numbered);
			};
			const auto idField = [&nameOffsets](const PEResources::ResourceId& id)
			{
				return id.IsNamed() ? nameOffsets.at(&id.Name) | 0x80000000u : uint32_t(id.Number);
			};
			writeDirectory(0, countNamed(tree), static_cast<uint32_t>(tree.size()) - countNamed(tree));
			uint32_t rootEntry = 16;
			for (const auto& [type, names] : tree)
			{
				Put32(bytes, rootEntry, idField(type));
				Put32(bytes, rootEntry + 4, directoryOffsets.at(&names) | 0x80000000u);
				rootEntry += 8;
				const uint32_t typeDirectory = directoryOffsets.at(&names);
				writeDirectory(typeDirectory, countNamed(names), static_cast<uint32_t>(names.size()) - countNamed(names));
				uint32_t typeEntry = typeDirectory + 16;
				for (const auto& [name, languages] : names)
				{
					Put32(bytes, typeEntry, idField(name));
					Put32(bytes, typeEntry + 4, directoryOffsets.at(&languages) | 0x80000000u);
					typeEntry += 8;
					const uint32_t nameDirectory = directoryOffsets.at(&languages);
					writeDirectory(nameDirectory, 0, static_cast<uint32_t>(languages.size()));
					uint32_t nameEntry = nameDirectory + 16;
					for (const auto& [language, resource] : languages)
					{
						Put32(bytes, nameEntry, language);
						Put32(bytes, nameEntry + 4, entryOffsets.at(resource));
						nameEntry += 8;
					}
				}
			}
			for (const auto& [text, offset] : nameOffsets)
			{
				Put16(bytes, offset, static_cast<uint32_t>(text->size()));
				for (size_t index = 0; index < text->size(); index++)
					Put16(bytes, offset + 2 + index * 2, (*text)[index]);
			}
			for (const PEResources::Resource& resource : resources)
			{
				const uint32_t entry = entryOffsets.at(&resource);
				Put32(bytes, entry, sectionRVA + dataOffsets.at(&resource));
				Put32(bytes, entry + 4, static_cast<uint32_t>(resource.Data.size()));
				Put32(bytes, entry + 8, resource.CodePage);
				std::copy(resource.Data.begin(), resource.Data.end(), bytes.begin() + dataOffsets.at(&resource));
			}
			return bytes;
		}

		struct BuiltSection
		{
			std::string Name;
			std::vector<uint8_t> Data;
			uint32_t VirtualSize = 0;
			uint32_t Characteristics = 0;
			uint32_t VirtualAddress = 0;
			uint32_t PointerToRawData = 0;
			uint32_t SizeOfRawData = 0;
		};

	}

	std::vector<PEResources::Resource> MakeTestRuntimeResources()
	{
		using PEResources::ResourceId;
		std::vector<PEResources::Resource> resources;
		const auto add = [&resources](ResourceId type, ResourceId name, uint16_t language, std::vector<uint8_t> data)
		{
			resources.push_back(PEResources::Resource { std::move(type), std::move(name), language, 0, std::move(data) });
		};
		add(ResourceId::FromNumber(PEResources::c_TypeIcon), ResourceId::FromNumber(1), 0x409, Sequence(104, 1));
		add(ResourceId::FromNumber(PEResources::c_TypeIcon), ResourceId::FromNumber(2), 0x409, Sequence(168, 2));
		add(ResourceId::FromNumber(PEResources::c_TypeIcon), ResourceId::FromNumber(3), 0x409, Sequence(77, 3));
		add(ResourceId::FromNumber(PEResources::c_TypeGroupIcon), ResourceId::FromName(PEResources::c_GameIconGroup), 0x409,
			MakeGroup({ { 16, 1 }, { 32, 2 } }, resources));
		add(ResourceId::FromNumber(PEResources::c_TypeGroupIcon), ResourceId::FromNumber(101), 0x409, MakeGroup({ { 48, 3 } }, resources));
		add(ResourceId::FromNumber(PEResources::c_TypeVersion), ResourceId::FromNumber(1), 0x409, Sequence(92, 16));
		add(ResourceId::FromNumber(PEResources::c_TypeManifest), ResourceId::FromNumber(1), 0x409,
			Bytes("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\"/>\n"));
		add(ResourceId::FromName(u"STRATADATA"), ResourceId::FromName(u"BLOB"), 0x409, Sequence(100, 0));
		add(ResourceId::FromNumber(10), ResourceId::FromNumber(7), 0x407, Bytes("Deutsch: 13."));
		add(ResourceId::FromNumber(10), ResourceId::FromNumber(7), 0x409, Bytes("English: 13 bytes"));
		return resources;
	}

	std::vector<uint8_t> BuildTestPEImage(const TestPEImageOptions& options)
	{
		std::vector<BuiltSection> sections;
		BuiltSection text { ".text", Sequence(c_TextSize, 0x90), c_TextSize, 0x60000020 };
		sections.push_back(std::move(text));
		sections.push_back(BuiltSection { ".rsrc", {}, 0, 0x40000040 });
		// One block of relocations for the first page of .text: five 64-bit addresses, then a padding entry.
		std::vector<uint8_t> relocations;
		Put32(relocations, 0, c_TextRVA);
		Put32(relocations, 4, 8 + 6 * 2);
		for (uint32_t index = 0; index < 5; index++)
			Put16(relocations, 8 + index * 2, (10u << 12) | (0x10 + index * 0x10));
		Put16(relocations, 8 + 5 * 2, 0);
		const uint32_t relocationSize = static_cast<uint32_t>(relocations.size());
		sections.push_back(BuiltSection { ".reloc", relocations, relocationSize, 0x42000040 });
		if (options.SectionAfterRelocations)
			sections.push_back(BuiltSection { ".extra", Sequence(16, 0xE0), 16, 0x40000040 });

		const uint32_t sectionTable = c_PEOffset + 4 + 20 + c_OptionalHeaderSize;
		const uint32_t sizeOfHeaders = AlignUp(sectionTable + 40 * static_cast<uint32_t>(sections.size()), c_FileAlignment);
		uint32_t rva = c_TextRVA;
		uint32_t fileOffset = sizeOfHeaders;
		for (BuiltSection& section : sections)
		{
			section.VirtualAddress = rva;
			if (section.Name == ".rsrc")
			{
				section.Data = BuildResourceTree(options.Resources, rva);
				section.VirtualSize = static_cast<uint32_t>(section.Data.size());
			}
			section.PointerToRawData = fileOffset;
			section.SizeOfRawData = AlignUp(static_cast<uint32_t>(section.Data.size()), c_FileAlignment);
			rva = AlignUp(rva + section.VirtualSize, c_SectionAlignment);
			fileOffset += section.SizeOfRawData;
		}

		std::vector<uint8_t> image(fileOffset, 0);
		image[0] = 'M';
		image[1] = 'Z';
		Put32(image, 0x3C, c_PEOffset);
		const std::string_view stub = "This program cannot be run in DOS mode.";
		std::copy(stub.begin(), stub.end(), image.begin() + 0x4E);
		Put32(image, c_PEOffset, 0x00004550);
		const uint32_t fileHeader = c_PEOffset + 4;
		Put16(image, fileHeader, options.Machine);
		Put16(image, fileHeader + 2, static_cast<uint32_t>(sections.size()));
		Put32(image, fileHeader + 4, 0x5F000000);
		Put16(image, fileHeader + 16, c_OptionalHeaderSize);
		Put16(image, fileHeader + 18, 0x22); // An executable that handles addresses above 2 GiB

		const uint32_t optionalHeader = fileHeader + 20;
		uint32_t initializedData = 0;
		for (const BuiltSection& section : sections)
		{
			if (section.Characteristics & c_InitializedData)
				initializedData += section.SizeOfRawData;
		}
		Put16(image, optionalHeader, 0x20B);
		image[optionalHeader + 2] = 14;
		Put32(image, optionalHeader + 4, sections[0].SizeOfRawData);
		Put32(image, optionalHeader + 8, initializedData);
		Put32(image, optionalHeader + 16, c_TextRVA);
		Put32(image, optionalHeader + 20, c_TextRVA);
		Put64(image, optionalHeader + 24, 0x140000000ull);
		Put32(image, optionalHeader + 32, c_SectionAlignment);
		Put32(image, optionalHeader + 36, c_FileAlignment);
		Put16(image, optionalHeader + 40, 6);
		Put16(image, optionalHeader + 48, 6);
		Put32(image, optionalHeader + 56, rva);
		Put32(image, optionalHeader + 60, sizeOfHeaders);
		Put16(image, optionalHeader + 68, 2);      // Windows GUI
		Put16(image, optionalHeader + 70, 0x8160); // High-entropy ASLR, relocatable, NX, terminal server aware
		Put64(image, optionalHeader + 72, 0x100000);
		Put64(image, optionalHeader + 80, 0x1000);
		Put64(image, optionalHeader + 88, 0x100000);
		Put64(image, optionalHeader + 96, 0x1000);
		Put32(image, optionalHeader + 108, 16);
		const uint32_t directories = optionalHeader + 112;
		Put32(image, directories + 3 * 8, c_TextRVA + 0x100); // The exception directory, in .text
		Put32(image, directories + 3 * 8 + 4, 0x30);
		Put32(image, directories + 2 * 8, sections[1].VirtualAddress);
		Put32(image, directories + 2 * 8 + 4, sections[1].VirtualSize);
		Put32(image, directories + 5 * 8, sections[2].VirtualAddress);
		Put32(image, directories + 5 * 8 + 4, relocationSize);

		for (size_t index = 0; index < sections.size(); index++)
		{
			const BuiltSection& section = sections[index];
			const size_t header = sectionTable + index * 40;
			std::copy(section.Name.begin(), section.Name.end(), image.begin() + header);
			Put32(image, header + 8, section.VirtualSize);
			Put32(image, header + 12, section.VirtualAddress);
			Put32(image, header + 16, section.SizeOfRawData);
			Put32(image, header + 20, section.PointerToRawData);
			Put32(image, header + 36, section.Characteristics);
			std::copy(section.Data.begin(), section.Data.end(), image.begin() + section.PointerToRawData);
		}
		for (uint32_t index = 0; index < options.OverlayBytes; index++)
			image.push_back(0xAB);
		if (options.CertificateTable)
		{
			// WIN_CERTIFICATE: its length, revision 2 and type PKCS #7, then the signature's bytes.
			image.resize(AlignUp(static_cast<uint32_t>(image.size()), 8), 0);
			const uint32_t certificate = static_cast<uint32_t>(image.size());
			Put32(image, certificate, 32);
			Put16(image, certificate + 4, 0x200);
			Put16(image, certificate + 6, 2);
			image.resize(certificate + 32, 0x30);
			Put32(image, directories + 4 * 8, certificate);
			Put32(image, directories + 4 * 8 + 4, 32);
		}
		Put32(image, optionalHeader + 64, ComputeTestPEChecksum(image));
		return image;
	}

	const TestPESection* TestPEHeaders::FindSection(std::string_view name) const
	{
		for (const TestPESection& section : Sections)
		{
			if (section.Name == name)
				return &section;
		}
		return nullptr;
	}

	TestPEHeaders ReadTestPEHeaders(std::span<const uint8_t> image)
	{
		TestPEHeaders headers;
		const uint32_t fileHeader = Get32(image, 0x3C) + 4;
		const uint32_t optionalHeader = fileHeader + 20;
		headers.SizeOfInitializedData = Get32(image, optionalHeader + 8);
		headers.SectionAlignment = Get32(image, optionalHeader + 32);
		headers.FileAlignment = Get32(image, optionalHeader + 36);
		headers.SizeOfImage = Get32(image, optionalHeader + 56);
		headers.SizeOfHeaders = Get32(image, optionalHeader + 60);
		headers.CheckSum = Get32(image, optionalHeader + 64);
		for (size_t index = 0; index < headers.Directories.size(); index++)
			headers.Directories[index] = { Get32(image, optionalHeader + 112 + index * 8), Get32(image, optionalHeader + 116 + index * 8) };
		const uint32_t sectionTable = optionalHeader + Get16(image, fileHeader + 16);
		const uint32_t sectionCount = Get16(image, fileHeader + 2);
		for (uint32_t index = 0; index < sectionCount; index++)
		{
			const size_t header = sectionTable + size_t(index) * 40;
			TestPESection section;
			for (size_t character = 0; character < 8 && header + character < image.size() && image[header + character] != 0; character++)
				section.Name += static_cast<char>(image[header + character]);
			section.VirtualSize = Get32(image, header + 8);
			section.VirtualAddress = Get32(image, header + 12);
			section.SizeOfRawData = Get32(image, header + 16);
			section.PointerToRawData = Get32(image, header + 20);
			headers.Sections.push_back(std::move(section));
		}
		return headers;
	}

	uint32_t ComputeTestPEChecksum(std::span<const uint8_t> image)
	{
		// The file as 16-bit words with the CheckSum field zeroed, summed with every carry out of 16 bits added back, plus its size.
		std::vector<uint8_t> copy(image.begin(), image.end());
		const size_t checkSum = Get32(image, 0x3C) + 4 + 20 + 64;
		if (checkSum + 4 <= copy.size())
			std::fill(copy.begin() + checkSum, copy.begin() + checkSum + 4, uint8_t(0));
		if (copy.size() % 2 != 0)
			copy.push_back(0);
		uint32_t sum = 0;
		for (size_t offset = 0; offset < copy.size(); offset += 2)
		{
			sum += copy[offset] | copy[offset + 1] << 8;
			if (sum > 0xFFFF)
				sum = (sum & 0xFFFF) + 1;
		}
		return sum + static_cast<uint32_t>(image.size());
	}

	std::optional<TestVersionInfo> ParseTestVersionInfo(std::span<const uint8_t> data, std::string* outError)
	{
		struct Node
		{
			uint32_t Length = 0;
			uint32_t ValueLength = 0;
			uint32_t Type = 0;
			std::u16string Key;
			size_t Value = 0;
			size_t Children = 0;
			size_t End = 0;
		};
		const auto fail = [outError](const char* message) -> std::optional<TestVersionInfo>
		{
			if (outError)
				*outError = message;
			return std::nullopt;
		};
		const auto readNode = [&data](size_t offset) -> std::optional<Node>
		{
			Node node;
			if (offset + 6 > data.size())
				return std::nullopt;
			node.Length = Get16(data, offset);
			node.ValueLength = Get16(data, offset + 2);
			node.Type = Get16(data, offset + 4);
			node.End = offset + node.Length;
			if (node.Length < 6 || node.End > data.size())
				return std::nullopt;
			size_t position = offset + 6;
			while (true)
			{
				if (position + 2 > node.End)
					return std::nullopt;
				const char16_t unit = static_cast<char16_t>(Get16(data, position));
				position += 2;
				if (unit == 0)
					break;
				node.Key += unit;
			}
			node.Value = AlignUp(static_cast<uint32_t>(position), 4);
			const size_t valueBytes = node.Type == 1 ? node.ValueLength * 2 : node.ValueLength;
			if (node.Value + valueBytes > node.End && valueBytes > 0)
				return std::nullopt;
			node.Children = AlignUp(static_cast<uint32_t>(node.Value + valueBytes), 4);
			return node;
		};
		const auto children = [&readNode](const Node& parent) -> std::optional<std::vector<Node>>
		{
			std::vector<Node> nodes;
			size_t offset = parent.Children;
			while (offset < parent.End)
			{
				std::optional<Node> node = readNode(offset);
				if (!node || node->End > parent.End)
					return std::nullopt;
				offset = AlignUp(static_cast<uint32_t>(node->End), 4);
				nodes.push_back(std::move(*node));
			}
			return nodes;
		};

		const std::optional<Node> root = readNode(0);
		if (!root || root->Key != u"VS_VERSION_INFO" || root->ValueLength != 52 || Get32(data, root->Value) != 0xFEEF04BD)
			return fail("no VS_VERSION_INFO with a VS_FIXEDFILEINFO");
		TestVersionInfo info;
		const auto quad = [&data](size_t offset)
		{
			const uint32_t high = Get32(data, offset);
			const uint32_t low = Get32(data, offset + 4);
			return std::array<uint16_t, 4> { uint16_t(high >> 16), uint16_t(high & 0xFFFF), uint16_t(low >> 16), uint16_t(low & 0xFFFF) };
		};
		info.FileVersion = quad(root->Value + 8);
		info.ProductVersion = quad(root->Value + 16);
		const std::optional<std::vector<Node>> blocks = children(*root);
		if (!blocks)
			return fail("the blocks of VS_VERSION_INFO do not fit it");
		for (const Node& block : *blocks)
		{
			const std::optional<std::vector<Node>> items = children(block);
			if (!items)
				return fail("the items of a block do not fit it");
			if (block.Key == u"StringFileInfo")
			{
				if (items->size() != 1)
					return fail("StringFileInfo holds other than one string table");
				info.StringTableKey = items->front().Key;
				const std::optional<std::vector<Node>> strings = children(items->front());
				if (!strings)
					return fail("the strings do not fit their table");
				for (const Node& string : *strings)
				{
					if (string.Type != 1 || string.ValueLength == 0 || Get16(data, string.Value + (string.ValueLength - 1) * 2) != 0)
						return fail("a string is no terminated text");
					std::u16string value;
					for (size_t index = 0; index + 1 < string.ValueLength; index++)
						value += static_cast<char16_t>(Get16(data, string.Value + index * 2));
					info.Strings[string.Key] = value;
				}
			}
			else if (block.Key == u"VarFileInfo")
			{
				if (items->size() != 1 || items->front().Key != u"Translation" || items->front().ValueLength != 4)
					return fail("VarFileInfo holds no translation");
				info.Translation = Get32(data, items->front().Value);
			}
			else
			{
				return fail("VS_VERSION_INFO holds an unknown block");
			}
		}
		return info;
	}

}
