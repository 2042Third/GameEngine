#include <doctest/doctest.h>

#include "Strata/Core/BinaryStream.h"
#include "Strata/Renderer/CookedTexture.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/Texture.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <tuple>
#include <vector>

using namespace Strata;

namespace
{

	// A full mip chain in `format` whose bytes differ per level and position.
	Ref<Texture> CreateChain(TextureFormat format, uint32_t width, uint32_t height, const std::string& name, bool grayscale = false)
	{
		std::vector<TextureMip> mips;
		for (uint32_t level = 0; level < TextureUtils::CalculateMipCount(width, height); level++)
		{
			TextureMip mip;
			mip.Width = std::max(1u, width >> level);
			mip.Height = std::max(1u, height >> level);
			mip.Data.resize(static_cast<size_t>(GetMipByteSize(format, mip.Width, mip.Height)));
			for (size_t index = 0; index < mip.Data.size(); index++)
				mip.Data[index] = static_cast<uint8_t>(index * 31 + level * 7);
			mips.push_back(std::move(mip));
		}
		TextureSpecification specification;
		specification.Format = format;
		specification.Filter = TextureFilter::Nearest;
		specification.Wrap = TextureWrap::Clamp;
		specification.Grayscale = grayscale;
		specification.DebugName = name;
		std::string error;
		Ref<Texture> texture = Texture::Create(specification, std::move(mips), &error);
		REQUIRE_MESSAGE(texture, error);
		return texture;
	}

	// Version 1 data as engines before texture cooking v2 wrote it: header, name, mip table, levels largest first.
	std::vector<uint8_t> WriteVersion1(const Texture& texture)
	{
		BinaryWriter writer;
		const TextureSpecification& specification = texture.GetSpecification();
		for (uint32_t value : { c_CookedTextureMagic, 1u, static_cast<uint32_t>(specification.Format), static_cast<uint32_t>(specification.Filter),
			static_cast<uint32_t>(specification.Wrap), texture.GetMipCount() })
			writer.Write(value);
		writer.WriteString(specification.DebugName);
		for (const TextureMip& mip : texture.GetMips())
		{
			writer.Write(mip.Width);
			writer.Write(mip.Height);
			writer.Write(static_cast<uint64_t>(mip.Data.size()));
		}
		for (const TextureMip& mip : texture.GetMips())
			writer.WriteBytes(mip.Data.data(), mip.Data.size());
		return writer.TakeData();
	}

	// What every accepted cooked texture guarantees: each level has its size and lies inside the data, smallest first.
	void CheckViewInvariants(const CookedTextureView& view)
	{
		REQUIRE(view.Layout.MipCount >= 1);
		REQUIRE(view.Layout.MipCount <= c_MaxCookedTextureMips);
		for (uint32_t level = 0; level < view.Layout.MipCount; level++)
		{
			const CookedTextureLevel& entry = view.Layout.Levels[level];
			REQUIRE(entry.Width == std::max(1u, view.Width >> level));
			REQUIRE(entry.Height == std::max(1u, view.Height >> level));
			REQUIRE(entry.Size == GetMipByteSize(view.Format, entry.Width, entry.Height));
			REQUIRE(entry.Offset <= view.Data.size());
			REQUIRE(entry.Size <= view.Data.size() - entry.Offset);
			REQUIRE(view.GetLevelData(level).size() == entry.Size);
		}
	}

	template<typename T>
	void Poke(std::vector<uint8_t>& data, size_t offset, T value)
	{
		REQUIRE(offset + sizeof(T) <= data.size());
		std::memcpy(data.data() + offset, &value, sizeof(T));
	}

	// Offsets of the version 2 header's fields and of mip table entries.
	constexpr size_t c_HeaderSize = 40;
	constexpr size_t c_MipEntrySize = 24;
	size_t GetMipEntryOffset(uint32_t level) { return c_HeaderSize + level * c_MipEntrySize; }

}

TEST_SUITE("Renderer.CookedTexture")
{
	TEST_CASE("Textures of every format round trip through the cooked form")
	{
		for (uint32_t value = 1; value <= static_cast<uint32_t>(c_LastTextureFormat); value++)
		{
			const TextureFormat format = static_cast<TextureFormat>(value);
			const std::string formatName = TextureFormatToString(format);
			CAPTURE(formatName);
			// Odd sizes: block-compressed levels end in partial blocks.
			const bool grayscale = GetTextureFormatInfo(format).Channels == 1;
			const Ref<Texture> source = CreateChain(format, 37, 10, "Chain", grayscale);
			const std::vector<uint8_t> cooked = source->Serialize();
			REQUIRE_FALSE(cooked.empty());

			std::string error;
			const std::optional<CookedTextureView> view = ParseCookedTexture(cooked, &error);
			REQUIRE_MESSAGE(view, error);
			CHECK(view->Layout.Version == 2);
			CHECK(view->Format == format);
			CHECK(view->Width == 37);
			CHECK(view->Height == 10);
			CHECK(view->Filter == TextureFilter::Nearest);
			CHECK(view->Wrap == TextureWrap::Clamp);
			CHECK(view->DebugName == "Chain");
			CHECK(HasFlag(view->Flags, CookedTextureFlags::Grayscale) == grayscale);
			CheckViewInvariants(*view);

			for (const Ref<Texture>& loaded : { Texture::Deserialize(std::span<const uint8_t>(cooked), &error), Texture::Deserialize(std::vector<uint8_t>(cooked), &error) })
			{
				REQUIRE_MESSAGE(loaded, error);
				CHECK(loaded->GetSpecification().Format == format);
				CHECK(loaded->GetSpecification().Grayscale == grayscale);
				CHECK(loaded->GetMipCount() == source->GetMipCount());
				for (uint32_t level = 0; level < source->GetMipCount(); level++)
				{
					CAPTURE(level);
					CHECK(std::ranges::equal(loaded->GetMipData(level), source->GetMips()[level].Data));
				}
				CHECK(loaded->Serialize() == cooked);
				// Finalized, the texture holds its chain at its block-compressed size: on the GPU where a renderer runs (GPU suites
				// that ran earlier in this process leave the shared one initialized), else in system memory.
				const AssetMemoryUsage finalized = loaded->GetFinalizedMemoryUsage();
				uint64_t chainBytes = 0;
				for (const TextureMip& mip : loaded->GetMips())
					chainBytes += GetMipByteSize(format, mip.Width, mip.Height);
				if (Renderer::IsInitialized())
					CHECK(finalized == AssetMemoryUsage { 0, chainBytes, 0 });
				else
					CHECK(finalized == loaded->GetMemoryUsage());
			}
		}
	}

	TEST_CASE("The mip tail is one contiguous range right after the header")
	{
		for (const auto& [format, width, height] : { std::tuple { TextureFormat::BC1SRGB, 2048u, 2048u }, std::tuple { TextureFormat::BC7, 1024u, 256u },
			std::tuple { TextureFormat::RGBA8, 300u, 200u }, std::tuple { TextureFormat::BC4, 64u, 64u } })
		{
			const std::string formatName = TextureFormatToString(format);
			CAPTURE(formatName);
			CAPTURE(width);
			const Ref<Texture> texture = CreateChain(format, width, height, "Layout");
			const std::vector<uint8_t> cooked = texture->Serialize();
			const std::optional<CookedTextureView> view = ParseCookedTexture(cooked);
			REQUIRE(view);

			// The texture's layout (what Serialize writes) is the parsed one.
			const CookedTextureLayout& layout = texture->GetCookedLayout();
			CHECK(layout.HeaderSize == view->Layout.HeaderSize);
			for (uint32_t level = 0; level < layout.MipCount; level++)
			{
				CHECK(layout.Levels[level].Offset == view->Layout.Levels[level].Offset);
				CHECK(layout.Levels[level].Size == view->Layout.Levels[level].Size);
			}

			for (uint32_t maxDimension : { 1u, 4u, 128u, 256u, 100000u })
			{
				CAPTURE(maxDimension);
				const CookedTextureByteRange tail = texture->GetMipTailByteRange(maxDimension);
				CHECK(tail.Offset == view->Layout.HeaderSize);
				CHECK(tail.Offset % c_CookedTextureAlignment == 0);
				// Every level that fits lies in the range, every larger one after it.
				uint32_t inside = 0;
				for (uint32_t level = 0; level < view->Layout.MipCount; level++)
				{
					const CookedTextureLevel& entry = view->Layout.Levels[level];
					const bool fits = entry.Width <= maxDimension && entry.Height <= maxDimension;
					if (fits)
					{
						inside++;
						CHECK(entry.Offset >= tail.Offset);
						CHECK(entry.Offset + entry.Size <= tail.Offset + tail.Size);
					}
					else
					{
						CHECK(entry.Offset >= tail.Offset + tail.Size);
					}
				}
				CHECK(tail.LevelCount == inside);
				CHECK(tail.FirstLevel == view->Layout.MipCount - inside);
				// The range holds nothing but those levels and the alignment between them.
				uint64_t levelBytes = 0;
				for (uint32_t level = tail.FirstLevel; level < view->Layout.MipCount; level++)
					levelBytes += view->Layout.Levels[level].Size;
				CHECK(tail.Size >= levelBytes);
				CHECK(tail.Size <= levelBytes + inside * (c_CookedTextureAlignment - 1));
			}
		}
		// The default is the levels up to 128x128.
		const Ref<Texture> texture = CreateChain(TextureFormat::BC1, 2048, 2048, "Default");
		CHECK(texture->GetMipTailByteRange().FirstLevel == 4);
		CHECK(texture->GetMipTailByteRange().LevelCount == 8);
	}

	TEST_CASE("A partial load reads the header and the resident tail, and the levels above it are one range each")
	{
		const Ref<Texture> source = CreateChain(TextureFormat::BC7SRGB, 2048, 2048, "Partial");
		const std::vector<uint8_t> cooked = source->Serialize();
		const uint64_t storedSize = cooked.size();
		const CookedTextureLayout& layout = source->GetCookedLayout();
		REQUIRE(layout.MipCount == 12);

		// The prefix: header and the levels of at most 32x32 texels (levels 6 to 11), found from the first 64 KiB.
		const std::span<const uint8_t> head = std::span<const uint8_t>(cooked).first(std::min<size_t>(cooked.size(), 64 * 1024));
		const std::optional<uint64_t> prefix = Texture::GetLoadPrefixSize(head, storedSize);
		REQUIRE(prefix);
		const CookedTextureByteRange tail = layout.GetMipTailByteRange(Texture::c_ResidentTailDimension);
		CHECK(tail.FirstLevel == 6);
		CHECK(*prefix == tail.Offset + tail.Size);
		CHECK(*prefix < 2 * 1024);
		CHECK(GetCookedTexturePrefixSize(head, storedSize, 128) == layout.GetMipTailByteRange(128).Offset + layout.GetMipTailByteRange(128).Size);

		// The prefix parses on its own, checked against the whole stored size; it holds the tail's levels and no others.
		const std::span<const uint8_t> bytes = std::span<const uint8_t>(cooked).first(static_cast<size_t>(*prefix));
		const std::optional<CookedTextureView> view = ParseCookedTexturePrefix(bytes, storedSize);
		REQUIRE(view);
		CHECK(view->FirstLevelInData == 6);
		CHECK(view->StoredSize == storedSize);
		for (uint32_t level = 0; level < 12; level++)
		{
			CAPTURE(level);
			CHECK(view->GetLevelData(level).empty() == (level < 6));
			if (level >= 6)
				CHECK(std::equal(view->GetLevelData(level).begin(), view->GetLevelData(level).end(), source->GetMipData(level).begin()));
		}
		// A whole cooked texture holds every level.
		CHECK(ParseCookedTexture(cooked)->FirstLevelInData == 0);
		// Prefixes too short for a level, stored sizes smaller than the prefix and version 1 data are refused.
		CHECK_FALSE(ParseCookedTexturePrefix(std::span<const uint8_t>(cooked).first(static_cast<size_t>(layout.HeaderSize)), storedSize));
		CHECK_FALSE(ParseCookedTexturePrefix(bytes, bytes.size() - 1));
		CHECK_FALSE(ParseCookedTexturePrefix(bytes, 16));
		const std::vector<uint8_t> version1 = WriteVersion1(*CreateChain(TextureFormat::RGBA8, 64, 64, "Old"));
		CHECK_FALSE(GetCookedTexturePrefixSize(version1, version1.size(), 32));
		CHECK_FALSE(ParseCookedTexturePrefix(version1, version1.size() + 100));
		// Textures no larger than the tail have no levels to stream: a prefix would be all of them.
		const std::vector<uint8_t> small = CreateChain(TextureFormat::RGBA8, 32, 32, "Small")->Serialize();
		CHECK(GetCookedTexturePrefixSize(small, small.size(), 32) == small.size());

		// The levels above the tail are read as one range each run of them: [first, end) is contiguous, the smaller first.
		for (const auto& [first, end] : { std::pair { 5u, 6u }, std::pair { 0u, 6u }, std::pair { 3u, 5u } })
		{
			CAPTURE(first);
			const CookedTextureByteRange range = layout.GetLevelsByteRange(first, end);
			CHECK(range.FirstLevel == first);
			CHECK(range.LevelCount == end - first);
			CHECK(range.Offset == layout.Levels[end - 1].Offset);
			CHECK(range.Offset + range.Size == layout.Levels[first].Offset + layout.Levels[first].Size);
		}
		CHECK(layout.GetLevelsByteRange(4, 4).Size == 0);

		// A texture made from the prefix: the tail's levels, its full size and layout, nothing to serialize or decode.
		std::string error;
		const Ref<Texture> partial = Texture::Deserialize(std::vector<uint8_t>(bytes.begin(), bytes.end()), storedSize, &error);
		REQUIRE_MESSAGE(partial, error);
		CHECK(partial->GetWidth() == 2048);
		CHECK(partial->GetMipCount() == 12);
		CHECK(partial->GetLoadedLevel() == 6);
		CHECK(partial->GetResidentLevel() == 6);
		CHECK(partial->GetTailLevel() == 6);
		CHECK(partial->GetMipData(5).empty());
		CHECK(partial->GetMipData(6).size() == source->GetMipData(6).size());
		CHECK(partial->Serialize().empty());
		CHECK_FALSE(partial->Decompress());
		AssetDetailLevels detail;
		REQUIRE(partial->GetDetailLevels(detail));
		CHECK(detail.Resident == 6);
		CHECK(detail.Wanted == 6);
		CHECK(detail.Count == 12);
		partial->SetWantedLevel(2);
		REQUIRE(partial->GetDetailLevels(detail));
		CHECK(detail.Wanted == 2);
		// Without a renderer nothing changes levels.
		CHECK_FALSE(partial->CanChangeLevels());
		CHECK_FALSE(partial->DropLevels(8));
		CHECK_FALSE(partial->RaiseLevels(5, std::vector<uint8_t>(static_cast<size_t>(layout.GetLevelsByteRange(5, 6).Size))));
	}

	TEST_CASE("Version 1 data still loads")
	{
		const Ref<Texture> source = CreateChain(TextureFormat::RGBA8SRGB, 8, 4, "Old");
		const std::vector<uint8_t> version1 = WriteVersion1(*source);
		std::string error;
		const std::optional<CookedTextureView> view = ParseCookedTexture(version1, &error);
		REQUIRE_MESSAGE(view, error);
		CHECK(view->Layout.Version == 1);
		CHECK(view->DebugName == "Old");
		CheckViewInvariants(*view);
		// Version 1 stores the levels largest first; the tail is still one range, at the end.
		const CookedTextureByteRange tail = view->Layout.GetMipTailByteRange(2);
		CHECK(tail.FirstLevel == 2);
		CHECK(tail.Offset + tail.Size == version1.size());

		for (const Ref<Texture>& loaded : { Texture::Deserialize(std::span<const uint8_t>(version1), &error), Texture::Deserialize(std::vector<uint8_t>(version1), &error) })
		{
			REQUIRE_MESSAGE(loaded, error);
			CHECK(loaded->GetSpecification().Format == TextureFormat::RGBA8SRGB);
			CHECK(loaded->GetSpecification().Filter == TextureFilter::Nearest);
			CHECK(loaded->GetSpecification().Wrap == TextureWrap::Clamp);
			CHECK(loaded->GetSpecification().DebugName == "Old");
			for (uint32_t level = 0; level < source->GetMipCount(); level++)
				CHECK(std::ranges::equal(loaded->GetMipData(level), source->GetMips()[level].Data));
			// Saving writes the current version.
			CHECK(loaded->Serialize() == source->Serialize());
		}

		// Version 1 knew only uncompressed formats.
		std::vector<uint8_t> compressed = version1;
		Poke<uint32_t>(compressed, 8, static_cast<uint32_t>(TextureFormat::BC1));
		CHECK_FALSE(ParseCookedTexture(compressed));
	}

	TEST_CASE("Corrupt headers and mip tables are rejected")
	{
		const Ref<Texture> texture = CreateChain(TextureFormat::BC1, 64, 32, "Corrupt");
		const std::vector<uint8_t> cooked = texture->Serialize();
		REQUIRE(ParseCookedTexture(cooked));
		const uint32_t mipCount = texture->GetMipCount();

		auto rejects = [&](const char* what, auto&& mutate)
		{
			CAPTURE(what);
			std::vector<uint8_t> data = cooked;
			mutate(data);
			std::string error;
			CHECK_FALSE(ParseCookedTexture(data, &error));
			CHECK_FALSE(error.empty());
			CHECK_FALSE(Texture::Deserialize(std::span<const uint8_t>(data)));
			CHECK_FALSE(Texture::Deserialize(std::move(data)));
		};
		rejects("magic", [](std::vector<uint8_t>& data) { data[0] ^= 0xFF; });
		rejects("version", [](std::vector<uint8_t>& data) { Poke<uint32_t>(data, 4, 3); });
		rejects("no format", [](std::vector<uint8_t>& data) { Poke<uint32_t>(data, 8, 0); });
		rejects("unknown format", [](std::vector<uint8_t>& data) { Poke<uint32_t>(data, 8, 99); });
		rejects("format with other block sizes", [](std::vector<uint8_t>& data) { Poke<uint32_t>(data, 8, static_cast<uint32_t>(TextureFormat::BC7)); });
		rejects("zero width", [](std::vector<uint8_t>& data) { Poke<uint32_t>(data, 12, 0); });
		rejects("huge width", [](std::vector<uint8_t>& data) { Poke<uint32_t>(data, 12, 20000); });
		rejects("other width", [](std::vector<uint8_t>& data) { Poke<uint32_t>(data, 12, 128); });
		rejects("other height", [](std::vector<uint8_t>& data) { Poke<uint32_t>(data, 16, 64); });
		rejects("no mips", [](std::vector<uint8_t>& data) { Poke<uint32_t>(data, 20, 0); });
		rejects("too many mips", [&](std::vector<uint8_t>& data) { Poke<uint32_t>(data, 20, mipCount + 1); });
		rejects("unknown flag", [](std::vector<uint8_t>& data) { Poke<uint32_t>(data, 24, 1u << 7); });
		rejects("grayscale with four channels", [](std::vector<uint8_t>& data) { Poke<uint32_t>(data, 24, 1u); });
		rejects("filter", [](std::vector<uint8_t>& data) { data[28] = 2; });
		rejects("wrap", [](std::vector<uint8_t>& data) { data[29] = 3; });
		rejects("long name", [](std::vector<uint8_t>& data) { Poke<uint16_t>(data, 30, 2000); });
		rejects("data offset inside the table", [](std::vector<uint8_t>& data) { Poke<uint64_t>(data, 32, 16); });
		rejects("data offset misaligned", [&](std::vector<uint8_t>& data) { Poke<uint64_t>(data, 32, texture->GetCookedLayout().HeaderSize + 8); });
		rejects("data offset past the end", [](std::vector<uint8_t>& data) { Poke<uint64_t>(data, 32, data.size() + 16); });
		for (uint32_t level = 0; level < mipCount; level++)
		{
			CAPTURE(level);
			const size_t entry = GetMipEntryOffset(level);
			const CookedTextureLevel& cookedLevel = texture->GetCookedLayout().Levels[level];
			rejects("level width", [&](std::vector<uint8_t>& data) { Poke<uint32_t>(data, entry, cookedLevel.Width + 1); });
			rejects("level height", [&](std::vector<uint8_t>& data) { Poke<uint32_t>(data, entry + 4, cookedLevel.Height * 2); });
			rejects("misaligned offset", [&](std::vector<uint8_t>& data) { Poke<uint64_t>(data, entry + 8, cookedLevel.Offset + 8); });
			rejects("overlapping offset", [&](std::vector<uint8_t>& data) { Poke<uint64_t>(data, entry + 8, cookedLevel.Offset + c_CookedTextureAlignment); });
			rejects("offset moved down by the header's size", [&](std::vector<uint8_t>& data) { Poke<uint64_t>(data, entry + 8, cookedLevel.Offset - texture->GetCookedLayout().HeaderSize); });
			rejects("offset past the end", [&](std::vector<uint8_t>& data) { Poke<uint64_t>(data, entry + 8, UINT64_MAX - 15); });
			rejects("size", [&](std::vector<uint8_t>& data) { Poke<uint64_t>(data, entry + 16, cookedLevel.Size + 8); });
			rejects("huge size", [&](std::vector<uint8_t>& data) { Poke<uint64_t>(data, entry + 16, UINT64_MAX); });
		}
		// Levels must be stored smallest first: swapping two levels' places is rejected even though both fit.
		rejects("largest first", [&](std::vector<uint8_t>& data)
		{
			const CookedTextureLayout& layout = texture->GetCookedLayout();
			Poke<uint64_t>(data, GetMipEntryOffset(mipCount - 1), layout.Levels[0].Offset);
			Poke<uint64_t>(data, GetMipEntryOffset(0), layout.Levels[mipCount - 1].Offset);
		});
		CHECK_FALSE(ParseCookedTexture(std::span<const uint8_t>()));
	}

	TEST_CASE("Random mutations and truncations never get past validation unchecked")
	{
		// 10,000 corrupted copies of cooked textures: every truncation is rejected; a mutation is rejected or yields a
		// texture whose every level lies inside the data with its exact size (pixel bytes carry no checksum, so changed
		// pixels, names or a shorter mip chain can be valid). Nothing reads outside the data either way. The same holds for
		// the partial loads' path: the prefix found from the data's first bytes (Texture::GetLoadPrefixSize), parsed and
		// loaded against the stored size of the original (ParseCookedTexturePrefix, Texture::Deserialize).
		const std::vector<std::vector<uint8_t>> sources = {
			CreateChain(TextureFormat::BC1SRGB, 64, 64, "Color")->Serialize(),
			CreateChain(TextureFormat::BC7, 40, 12, "Odd")->Serialize(),
			CreateChain(TextureFormat::BC4, 16, 16, "Gray", true)->Serialize(),
			CreateChain(TextureFormat::RGBA8, 8, 8, "Small")->Serialize(),
			CreateChain(TextureFormat::BC7SRGB, 256, 128, "Streamed")->Serialize() };
		uint32_t prefixesAccepted = 0;
		std::mt19937 random(1234);
		uint32_t accepted = 0;
		uint32_t rejected = 0;
		for (uint32_t iteration = 0; iteration < 10000; iteration++)
		{
			const std::vector<uint8_t>& source = sources[iteration % sources.size()];
			std::vector<uint8_t> data = source;
			const bool truncate = iteration % 4 == 0;
			if (truncate)
			{
				data.resize(std::uniform_int_distribution<size_t>(0, source.size() - 1)(random));
			}
			else
			{
				// Most mutations hit the header and mip table, where they matter; some hit the pixels.
				const size_t region = iteration % 3 == 0 ? data.size() : std::min<size_t>(data.size(), 256);
				const uint32_t changes = std::uniform_int_distribution<uint32_t>(1, 4)(random);
				for (uint32_t change = 0; change < changes; change++)
				{
					const size_t position = std::uniform_int_distribution<size_t>(0, region - 1)(random);
					data[position] = static_cast<uint8_t>(data[position] ^ std::uniform_int_distribution<uint32_t>(1, 255)(random));
				}
			}

			// The partial load's path first: it reads only the prefix, so truncations after it may load.
			{
				CAPTURE(iteration);
				const std::span<const uint8_t> head = std::span<const uint8_t>(data).first(std::min<size_t>(data.size(), 64 * 1024));
				const std::optional<uint64_t> prefixSize = Texture::GetLoadPrefixSize(head, source.size());
				if (data.size() < c_HeaderSize)
					CHECK_FALSE(prefixSize);
				if (prefixSize && *prefixSize <= data.size())
				{
					const std::span<const uint8_t> prefix = std::span<const uint8_t>(data).first(static_cast<size_t>(*prefixSize));
					const std::optional<CookedTextureView> partial = ParseCookedTexturePrefix(prefix, source.size());
					const Ref<Texture> loaded = Texture::Deserialize(std::vector<uint8_t>(prefix.begin(), prefix.end()), source.size());
					if (partial)
					{
						prefixesAccepted++;
						REQUIRE(partial->Layout.MipCount >= 1);
						REQUIRE(partial->FirstLevelInData < partial->Layout.MipCount);
						for (uint32_t level = 0; level < partial->Layout.MipCount; level++)
						{
							const CookedTextureLevel& entry = partial->Layout.Levels[level];
							REQUIRE(entry.Size == GetMipByteSize(partial->Format, entry.Width, entry.Height));
							if (level < partial->FirstLevelInData)
							{
								REQUIRE(partial->GetLevelData(level).empty());
								continue;
							}
							REQUIRE(entry.Offset <= prefix.size());
							REQUIRE(entry.Size <= prefix.size() - entry.Offset);
							REQUIRE(partial->GetLevelData(level).size() == entry.Size);
						}
						if (loaded)
						{
							REQUIRE(loaded->GetLoadedLevel() == partial->FirstLevelInData);
							for (uint32_t level = 0; level < loaded->GetMipCount(); level++)
								REQUIRE(loaded->GetMipData(level).size() == (level < loaded->GetLoadedLevel() ? 0 : partial->Layout.Levels[level].Size));
						}
					}
					else
					{
						CHECK_FALSE(loaded);
					}
				}
			}

			const std::optional<CookedTextureView> view = ParseCookedTexture(data);
			if (truncate)
			{
				CAPTURE(iteration);
				CHECK_FALSE(view);
			}
			if (!view)
			{
				rejected++;
				CHECK_FALSE(Texture::Deserialize(std::span<const uint8_t>(data)));
				CHECK_FALSE(Texture::Deserialize(std::vector<uint8_t>(data)));
				continue;
			}
			accepted++;
			CheckViewInvariants(*view);
			const Ref<Texture> texture = Texture::Deserialize(std::vector<uint8_t>(data));
			REQUIRE(texture);
			for (uint32_t level = 0; level < texture->GetMipCount(); level++)
				REQUIRE(texture->GetMipData(level).size() == GetMipByteSize(texture->GetSpecification().Format, texture->GetMips()[level].Width, texture->GetMips()[level].Height));
			if (IsBlockCompressedFormat(texture->GetSpecification().Format))
				CHECK(texture->Decompress());
		}
		MESSAGE(rejected, " corrupted copies rejected, ", accepted, " accepted with valid levels; ", prefixesAccepted, " prefixes accepted");
		CHECK(rejected > 2500); // Every truncation and many mutations
		CHECK(accepted > 0);
		CHECK(prefixesAccepted > 0);
	}

	TEST_CASE("Writing refuses levels that do not match the description")
	{
		CookedTextureDescription description;
		description.Format = TextureFormat::BC1;
		description.Width = 8;
		description.Height = 8;
		description.MipCount = 2;
		const std::vector<uint8_t> level0(32);
		const std::vector<uint8_t> level1(8);
		const std::span<const uint8_t> levels[] = { level0, level1 };
		CHECK_FALSE(WriteCookedTexture(description, levels).empty());
		CHECK(WriteCookedTexture(description, std::span<const std::span<const uint8_t>>(levels, 1)).empty());
		const std::vector<uint8_t> wrongSize(16);
		const std::span<const uint8_t> wrongLevels[] = { wrongSize, level1 };
		CHECK(WriteCookedTexture(description, wrongLevels).empty());
		description.MipCount = 5; // An 8x8 texture has 4 levels
		CHECK(ComputeCookedTextureLayout(description).MipCount == 0);
		description.MipCount = 2;
		description.Format = TextureFormat::None;
		CHECK(WriteCookedTexture(description, levels).empty());
	}
}
