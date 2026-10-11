#include "stpch.h"
#include "Strata/Renderer/CookedTexture.h"

#include "Strata/Core/BinaryStream.h"

namespace Strata
{

	namespace
	{

		struct CookedTextureHeader
		{
			uint32_t Magic;
			uint32_t Version;
			uint32_t Format;
			uint32_t Width;
			uint32_t Height;
			uint32_t MipCount;
			uint32_t Flags;
			uint8_t Filter;
			uint8_t Wrap;
			uint16_t NameLength;
			uint64_t DataOffset;
		};
		static_assert(sizeof(CookedTextureHeader) == 40, "The cooked texture header is part of the file format");

		struct CookedMipEntry
		{
			uint32_t Width;
			uint32_t Height;
			uint64_t Offset;
			uint64_t Size;
		};
		static_assert(sizeof(CookedMipEntry) == 24, "The cooked mip table is part of the file format");

		// Version 1: the header, the debug name (length-prefixed), the mip table, then the levels largest first.
		struct CookedTextureHeaderV1
		{
			uint32_t Magic;
			uint32_t Version;
			uint32_t Format;
			uint32_t Filter;
			uint32_t Wrap;
			uint32_t MipCount;
		};
		static_assert(sizeof(CookedTextureHeaderV1) == 24, "The cooked texture header is part of the file format");

		struct CookedMipEntryV1
		{
			uint32_t Width;
			uint32_t Height;
			uint64_t Size;
		};
		static_assert(sizeof(CookedMipEntryV1) == 16, "The cooked mip table is part of the file format");

		uint64_t AlignUp(uint64_t value, uint64_t alignment)
		{
			return (value + alignment - 1) / alignment * alignment;
		}

		uint32_t GetFullMipCount(uint32_t width, uint32_t height)
		{
			uint32_t levels = 1;
			for (uint32_t size = std::max(width, height); size > 1; size >>= 1)
				levels++;
			return levels;
		}

		bool IsValidDescription(TextureFormat format, uint32_t width, uint32_t height, uint32_t mipCount)
		{
			return IsValidTextureFormat(static_cast<uint32_t>(format)) && width > 0 && height > 0 && width <= c_MaxCookedTextureSize
				&& height <= c_MaxCookedTextureSize && mipCount > 0 && mipCount <= GetFullMipCount(width, height);
		}

		std::optional<CookedTextureView> Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		}

		// The checks every version shares: each level has the size its dimensions give in the format and lies within the
		// cooked data's `dataSize` bytes.
		std::optional<std::string> FindLevelProblem(const CookedTextureView& view, uint64_t dataSize)
		{
			for (uint32_t level = 0; level < view.Layout.MipCount; level++)
			{
				const CookedTextureLevel& entry = view.Layout.Levels[level];
				const uint32_t expectedWidth = std::max(1u, view.Width >> level);
				const uint32_t expectedHeight = std::max(1u, view.Height >> level);
				if (entry.Width != expectedWidth || entry.Height != expectedHeight)
					return fmt::format("Mip {} is {}x{}, expected {}x{}", level, entry.Width, entry.Height, expectedWidth, expectedHeight);
				const uint64_t expectedSize = GetMipByteSize(view.Format, entry.Width, entry.Height);
				if (entry.Size != expectedSize)
					return fmt::format("Mip {} has {} bytes, expected {}", level, entry.Size, expectedSize);
				if (entry.Offset > dataSize || entry.Size > dataSize - entry.Offset)
					return fmt::format("Mip {} lies outside the cooked data", level);
			}
			return std::nullopt;
		}

		std::optional<CookedTextureView> ParseVersion1(std::span<const uint8_t> data, std::string* outError)
		{
			BinaryReader reader(data);
			const CookedTextureHeaderV1 header = reader.Read<CookedTextureHeaderV1>();
			// Version 1 only knew the uncompressed formats.
			if (!reader.IsValid() || header.Format > static_cast<uint32_t>(TextureFormat::RG8) || !IsValidTextureFormat(header.Format)
				|| header.Filter > static_cast<uint32_t>(TextureFilter::Nearest) || header.Wrap > static_cast<uint32_t>(TextureWrap::Mirror)
				|| header.MipCount == 0 || header.MipCount > c_MaxCookedTextureMips)
				return Fail(outError, "Cooked texture header is corrupt");

			CookedTextureView view;
			view.Format = static_cast<TextureFormat>(header.Format);
			view.Filter = static_cast<TextureFilter>(header.Filter);
			view.Wrap = static_cast<TextureWrap>(header.Wrap);
			view.Data = data;
			const uint32_t nameLength = reader.Read<uint32_t>();
			const std::span<const uint8_t> name = nameLength <= c_MaxCookedTextureNameLength ? reader.ReadView(nameLength) : std::span<const uint8_t>();
			if (!reader.IsValid() || nameLength > c_MaxCookedTextureNameLength)
				return Fail(outError, "Cooked texture is truncated");
			view.DebugName = std::string_view(reinterpret_cast<const char*>(name.data()), name.size());

			std::array<CookedMipEntryV1, c_MaxCookedTextureMips> entries {};
			for (uint32_t level = 0; level < header.MipCount; level++)
				entries[level] = reader.Read<CookedMipEntryV1>();
			if (!reader.IsValid())
				return Fail(outError, "Cooked texture is truncated");

			view.Width = entries[0].Width;
			view.Height = entries[0].Height;
			if (!IsValidDescription(view.Format, view.Width, view.Height, header.MipCount))
				return Fail(outError, fmt::format("Texture size {}x{} with {} mips is out of range", view.Width, view.Height, header.MipCount));

			view.Layout.Version = 1;
			view.Layout.Format = view.Format;
			view.Layout.MipCount = header.MipCount;
			view.Layout.HeaderSize = reader.GetPosition();
			for (uint32_t level = 0; level < header.MipCount; level++)
			{
				CookedTextureLevel& entry = view.Layout.Levels[level];
				entry.Width = entries[level].Width;
				entry.Height = entries[level].Height;
				entry.Offset = reader.GetPosition();
				entry.Size = entries[level].Size;
				reader.ReadView(static_cast<size_t>(std::min<uint64_t>(entry.Size, SIZE_MAX)));
				if (!reader.IsValid())
					return Fail(outError, "Cooked texture pixel data is truncated");
			}
			if (const std::optional<std::string> problem = FindLevelProblem(view, data.size()))
				return Fail(outError, *problem);
			return view;
		}

		// `data` is the start of cooked data of `totalSize` bytes (all of it when they are equal): offsets are checked against
		// the whole, and the view refers to what `data` holds.
		std::optional<CookedTextureView> ParseVersion2(std::span<const uint8_t> data, uint64_t totalSize, std::string* outError)
		{
			BinaryReader reader(data);
			const CookedTextureHeader header = reader.Read<CookedTextureHeader>();
			if (!reader.IsValid())
				return Fail(outError, "Cooked texture is truncated");
			if (!IsValidTextureFormat(header.Format) || header.Filter > static_cast<uint8_t>(TextureFilter::Nearest)
				|| header.Wrap > static_cast<uint8_t>(TextureWrap::Mirror) || header.NameLength > c_MaxCookedTextureNameLength
				|| (header.Flags & ~static_cast<uint32_t>(c_KnownCookedTextureFlags)) != 0)
				return Fail(outError, "Cooked texture header is corrupt");
			if (!IsValidDescription(static_cast<TextureFormat>(header.Format), header.Width, header.Height, header.MipCount))
				return Fail(outError, fmt::format("Texture size {}x{} with {} mips is out of range", header.Width, header.Height, header.MipCount));

			CookedTextureView view;
			view.Format = static_cast<TextureFormat>(header.Format);
			view.Filter = static_cast<TextureFilter>(header.Filter);
			view.Wrap = static_cast<TextureWrap>(header.Wrap);
			view.Flags = static_cast<CookedTextureFlags>(header.Flags);
			view.Width = header.Width;
			view.Height = header.Height;
			view.Data = data;
			if (HasFlag(view.Flags, CookedTextureFlags::Grayscale) && GetTextureFormatInfo(view.Format).Channels != 1)
				return Fail(outError, "Cooked texture is marked grayscale but stores more than one channel");

			std::array<CookedMipEntry, c_MaxCookedTextureMips> entries {};
			for (uint32_t level = 0; level < header.MipCount; level++)
				entries[level] = reader.Read<CookedMipEntry>();
			const std::span<const uint8_t> name = reader.ReadView(header.NameLength);
			if (!reader.IsValid())
				return Fail(outError, "Cooked texture is truncated");
			view.DebugName = std::string_view(reinterpret_cast<const char*>(name.data()), name.size());
			if (header.DataOffset < reader.GetPosition() || header.DataOffset > totalSize || header.DataOffset % c_CookedTextureAlignment != 0)
				return Fail(outError, "Cooked texture data offset is invalid");

			view.Layout.Version = 2;
			view.Layout.Format = view.Format;
			view.Layout.MipCount = header.MipCount;
			view.Layout.HeaderSize = header.DataOffset;
			for (uint32_t level = 0; level < header.MipCount; level++)
			{
				CookedTextureLevel& entry = view.Layout.Levels[level];
				entry.Width = entries[level].Width;
				entry.Height = entries[level].Height;
				entry.Offset = entries[level].Offset;
				entry.Size = entries[level].Size;
				if (entry.Offset < header.DataOffset || entry.Offset % c_CookedTextureAlignment != 0)
					return Fail(outError, fmt::format("Mip {} has an invalid offset", level));
			}
			if (const std::optional<std::string> problem = FindLevelProblem(view, totalSize))
				return Fail(outError, *problem);
			// Smallest first: each level ends before the next larger one starts, so the levels do not overlap and the mip tail
			// is a prefix of the pixel data.
			for (uint32_t level = header.MipCount - 1; level > 0; level--)
			{
				const CookedTextureLevel& smaller = view.Layout.Levels[level];
				if (smaller.Offset + smaller.Size > view.Layout.Levels[level - 1].Offset)
					return Fail(outError, fmt::format("Mips {} and {} overlap or are out of order", level, level - 1));
			}
			return view;
		}

	}

	CookedTextureByteRange CookedTextureLayout::GetMipTailByteRange(uint32_t maxDimension) const
	{
		CookedTextureByteRange range;
		range.Offset = HeaderSize;
		range.FirstLevel = MipCount;
		for (uint32_t level = 0; level < MipCount; level++)
		{
			if (Levels[level].Width <= maxDimension && Levels[level].Height <= maxDimension)
			{
				range.FirstLevel = level;
				break;
			}
		}
		if (range.FirstLevel == MipCount)
			return range;

		uint64_t begin = UINT64_MAX;
		uint64_t end = 0;
		for (uint32_t level = range.FirstLevel; level < MipCount; level++)
		{
			begin = std::min(begin, Levels[level].Offset);
			end = std::max(end, Levels[level].Offset + Levels[level].Size);
		}
		range.Offset = begin;
		range.Size = end - begin;
		range.LevelCount = MipCount - range.FirstLevel;
		return range;
	}

	std::span<const uint8_t> CookedTextureView::GetLevelData(uint32_t level) const
	{
		if (level >= Layout.MipCount)
			return {};
		const CookedTextureLevel& entry = Layout.Levels[level];
		// A view of the start of the data (ParseCookedTexturePrefix) lacks the levels beyond it.
		if (entry.Offset > Data.size() || entry.Size > Data.size() - entry.Offset)
			return {};
		return Data.subspan(static_cast<size_t>(entry.Offset), static_cast<size_t>(entry.Size));
	}

	std::optional<CookedTextureView> ParseCookedTexture(std::span<const uint8_t> data, std::string* outError)
	{
		BinaryReader reader(data);
		const uint32_t magic = reader.Read<uint32_t>();
		const uint32_t version = reader.Read<uint32_t>();
		if (!reader.IsValid() || magic != c_CookedTextureMagic)
			return Fail(outError, "Not a cooked Strata texture");
		if (version == 1)
			return ParseVersion1(data, outError);
		if (version == 2)
			return ParseVersion2(data, data.size(), outError);
		return Fail(outError, fmt::format("Unsupported cooked texture version {}", version));
	}

	std::optional<CookedTextureView> ParseCookedTexturePrefix(std::span<const uint8_t> prefix, uint64_t totalSize, std::string* outError)
	{
		BinaryReader reader(prefix);
		const uint32_t magic = reader.Read<uint32_t>();
		const uint32_t version = reader.Read<uint32_t>();
		if (!reader.IsValid() || magic != c_CookedTextureMagic)
			return Fail(outError, "Not a cooked Strata texture");
		if (prefix.size() > totalSize)
			return Fail(outError, "The start of the cooked texture is longer than the texture");
		if (version == 1)
		{
			// Its levels are stored largest first: only the whole data has the small ones.
			if (prefix.size() != totalSize)
				return Fail(outError, "Version 1 cooked textures can only be read whole");
			return ParseVersion1(prefix, outError);
		}
		if (version == 2)
			return ParseVersion2(prefix, totalSize, outError);
		return Fail(outError, fmt::format("Unsupported cooked texture version {}", version));
	}

	CookedTextureLayout ComputeCookedTextureLayout(const CookedTextureDescription& description)
	{
		CookedTextureLayout layout;
		if (!IsValidDescription(description.Format, description.Width, description.Height, description.MipCount))
			return layout;

		const size_t nameLength = std::min<size_t>(description.DebugName.size(), c_MaxCookedTextureNameLength);
		layout.Format = description.Format;
		layout.MipCount = description.MipCount;
		layout.HeaderSize = AlignUp(sizeof(CookedTextureHeader) + description.MipCount * sizeof(CookedMipEntry) + nameLength, c_CookedTextureAlignment);
		uint64_t offset = layout.HeaderSize;
		for (uint32_t level = description.MipCount; level-- > 0;)
		{
			CookedTextureLevel& entry = layout.Levels[level];
			entry.Width = std::max(1u, description.Width >> level);
			entry.Height = std::max(1u, description.Height >> level);
			entry.Offset = offset;
			entry.Size = GetMipByteSize(description.Format, entry.Width, entry.Height);
			offset = AlignUp(offset + entry.Size, c_CookedTextureAlignment);
		}
		return layout;
	}

	std::vector<uint8_t> WriteCookedTexture(const CookedTextureDescription& description, std::span<const std::span<const uint8_t>> levels)
	{
		const CookedTextureLayout layout = ComputeCookedTextureLayout(description);
		if (layout.MipCount == 0 || levels.size() != layout.MipCount)
			return {};
		for (uint32_t level = 0; level < layout.MipCount; level++)
		{
			if (levels[level].size() != layout.Levels[level].Size)
				return {};
		}

		const std::string_view name = description.DebugName.substr(0, c_MaxCookedTextureNameLength);
		std::vector<uint8_t> data(static_cast<size_t>(layout.Levels[0].Offset + layout.Levels[0].Size), 0);
		const CookedTextureHeader header { c_CookedTextureMagic, c_CookedTextureVersion, static_cast<uint32_t>(description.Format), description.Width,
			description.Height, description.MipCount, static_cast<uint32_t>(description.Flags), static_cast<uint8_t>(description.Filter),
			static_cast<uint8_t>(description.Wrap), static_cast<uint16_t>(name.size()), layout.HeaderSize };
		std::memcpy(data.data(), &header, sizeof(header));
		size_t position = sizeof(header);
		for (uint32_t level = 0; level < layout.MipCount; level++)
		{
			const CookedTextureLevel& entry = layout.Levels[level];
			const CookedMipEntry mipEntry { entry.Width, entry.Height, entry.Offset, entry.Size };
			std::memcpy(data.data() + position, &mipEntry, sizeof(mipEntry));
			position += sizeof(mipEntry);
		}
		if (!name.empty())
			std::memcpy(data.data() + position, name.data(), name.size());
		for (uint32_t level = 0; level < layout.MipCount; level++)
		{
			if (!levels[level].empty())
				std::memcpy(data.data() + layout.Levels[level].Offset, levels[level].data(), levels[level].size());
		}
		return data;
	}

}
