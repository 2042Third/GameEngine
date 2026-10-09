#include "stpch.h"
#include "Strata/Asset/AssetPack.h"

#include "Strata/Core/BinaryStream.h"
#include "Strata/Core/FileSystem.h"

#include <fstream>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		struct PackHeader
		{
			uint32_t Magic = 0;
			uint32_t Version = 0;
			uint32_t EntryCount = 0;
			uint32_t Reserved = 0;
			uint64_t TableOffset = 0;
			uint64_t TableSize = 0;
		};
		static_assert(sizeof(PackHeader) == 32, "PackHeader layout is part of the pack format");

		// Fixed part of a table entry: handle, type, padding, parent, offset, size, plus three string lengths.
		constexpr uint64_t c_MinimumEntrySize = 8 + 2 + 2 + 8 + 8 + 8 + 3 * 4;
		constexpr uint64_t c_MaxTableSize = 1ull << 30;

		bool IsKnownAssetType(uint16_t value)
		{
			const AssetType type = static_cast<AssetType>(value);
			return type != AssetType::None && AssetTypeFromString(AssetTypeToString(type)) == type;
		}

	}

	bool AssetPack::Write(const std::filesystem::path& path, const std::vector<AssetMetadata>& assets, const DataProvider& provider, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		if (assets.size() > UINT32_MAX)
			return fail("Too many assets for one pack");

		// A unique temporary name: concurrent builds of the same pack never write into one file.
		std::filesystem::path temporaryPath = path;
		temporaryPath += "." + UUID().ToString() + ".tmp";
		if (path.has_parent_path() && !FileSystem::CreateDirectories(path.parent_path()))
			return fail(fmt::format("Could not create '{}'", FileSystem::ToUTF8(path.parent_path())));

		bool success = true;
		std::string error;
		{
			std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
			if (!stream)
				return fail(fmt::format("Could not create '{}'", FileSystem::ToUTF8(temporaryPath)));

			PackHeader header;
			stream.write(reinterpret_cast<const char*>(&header), sizeof(header));

			std::vector<AssetPackEntry> entries;
			entries.reserve(assets.size());
			uint64_t offset = sizeof(PackHeader);
			std::vector<uint8_t> data;
			for (const AssetMetadata& metadata : assets)
			{
				data.clear();
				std::string assetError;
				if (!provider(metadata, data, &assetError))
				{
					success = false;
					error = fmt::format("'{}' ({}): {}", metadata.Path.empty() ? metadata.Name : metadata.Path, metadata.Handle.ToString(), assetError);
					break;
				}
				stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
				entries.push_back(AssetPackEntry { metadata, offset, data.size() });
				offset += data.size();
			}

			if (success)
			{
				BinaryWriter table;
				for (const AssetPackEntry& entry : entries)
				{
					table.Write(static_cast<uint64_t>(entry.Metadata.Handle));
					table.Write(static_cast<uint16_t>(entry.Metadata.Type));
					table.Write(static_cast<uint16_t>(0));
					table.Write(static_cast<uint64_t>(entry.Metadata.Parent));
					table.Write(entry.Offset);
					table.Write(entry.Size);
					table.WriteString(entry.Metadata.Path);
					table.WriteString(entry.Metadata.SubAssetKey);
					table.WriteString(entry.Metadata.Name);
				}
				stream.write(reinterpret_cast<const char*>(table.GetData().data()), static_cast<std::streamsize>(table.GetSize()));

				header.Magic = c_Magic;
				header.Version = c_Version;
				header.EntryCount = static_cast<uint32_t>(entries.size());
				header.TableOffset = offset;
				header.TableSize = table.GetSize();
				stream.seekp(0);
				stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
			}

			stream.flush();
			if (success && !stream)
			{
				success = false;
				error = fmt::format("Writing '{}' failed (disk full?)", FileSystem::ToUTF8(temporaryPath));
			}
		}

		if (!success)
		{
			FileSystem::Remove(temporaryPath);
			return fail(error);
		}
		if (!FileSystem::Rename(temporaryPath, path))
		{
			FileSystem::Remove(temporaryPath);
			return fail(fmt::format("Could not replace '{}'", FileSystem::ToUTF8(path)));
		}
		return true;
	}

	Scope<AssetPack> AssetPack::Open(const std::filesystem::path& path, std::string* outError)
	{
		auto fail = [outError, &path](const std::string& message) -> Scope<AssetPack>
		{
			if (outError)
				*outError = fmt::format("{}: {}", FileSystem::ToUTF8(path), message);
			return nullptr;
		};

		const std::optional<uint64_t> fileSize = FileSystem::GetFileSize(path);
		if (!fileSize)
			return fail("cannot open the asset pack");

		std::ifstream stream(path, std::ios::binary);
		PackHeader header;
		if (!stream || !stream.read(reinterpret_cast<char*>(&header), sizeof(header)))
			return fail("cannot read the pack header");
		if (header.Magic != c_Magic)
			return fail("not a Strata asset pack");
		if (header.Version != c_Version)
			return fail(fmt::format("unsupported pack version {} (expected {})", header.Version, c_Version));
		if (header.TableOffset < sizeof(PackHeader) || header.TableSize > c_MaxTableSize || header.TableOffset > *fileSize
			|| header.TableSize > *fileSize - header.TableOffset || header.EntryCount > header.TableSize / c_MinimumEntrySize)
			return fail("corrupt pack header");

		std::vector<uint8_t> tableData(static_cast<size_t>(header.TableSize));
		stream.seekg(static_cast<std::streamoff>(header.TableOffset));
		if (!stream.read(reinterpret_cast<char*>(tableData.data()), static_cast<std::streamsize>(tableData.size())))
			return fail("cannot read the entry table");

		Scope<AssetPack> pack(new AssetPack());
		pack->m_Path = path;
		pack->m_FileSize = *fileSize;
		pack->m_Entries.reserve(header.EntryCount);

		std::unordered_set<uint64_t> handles;
		BinaryReader reader(tableData);
		for (uint32_t index = 0; index < header.EntryCount; index++)
		{
			AssetPackEntry entry;
			const uint64_t handle = reader.Read<uint64_t>();
			const uint16_t type = reader.Read<uint16_t>();
			reader.Read<uint16_t>();
			const uint64_t parent = reader.Read<uint64_t>();
			entry.Offset = reader.Read<uint64_t>();
			entry.Size = reader.Read<uint64_t>();
			entry.Metadata.Path = reader.ReadString();
			entry.Metadata.SubAssetKey = reader.ReadString();
			entry.Metadata.Name = reader.ReadString();
			if (!reader.IsValid())
				return fail("truncated entry table");
			if (handle == 0 || !IsKnownAssetType(type) || !handles.insert(handle).second)
				return fail(fmt::format("invalid entry {} (handle {:016X})", index, handle));
			if (entry.Offset < sizeof(PackHeader) || entry.Offset > header.TableOffset || entry.Size > header.TableOffset - entry.Offset)
				return fail(fmt::format("entry {} points outside the data section", index));

			entry.Metadata.Handle = UUID(handle);
			entry.Metadata.Type = static_cast<AssetType>(type);
			entry.Metadata.Parent = UUID(parent);
			pack->m_Entries.push_back(std::move(entry));
		}
		if (reader.GetRemaining() != 0)
			return fail("unexpected data after the entry table");
		return pack;
	}

	bool AssetPack::ReadData(const AssetPackEntry& entry, std::vector<uint8_t>& outData, std::string* outError) const
	{
		// Each read opens its own stream, so reads from several I/O threads never share a file position.
		std::ifstream stream(m_Path, std::ios::binary);
		if (stream)
		{
			outData.resize(static_cast<size_t>(entry.Size));
			stream.seekg(static_cast<std::streamoff>(entry.Offset));
			if (entry.Size == 0 || stream.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(entry.Size)))
				return true;
		}
		if (outError)
			*outError = fmt::format("Could not read '{}' from '{}'", entry.Metadata.Path.empty() ? entry.Metadata.Name : entry.Metadata.Path, FileSystem::ToUTF8(m_Path));
		return false;
	}

}
