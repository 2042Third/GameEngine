#include "stpch.h"
#include "Strata/Renderer/Texture.h"

#include "Strata/Core/BinaryStream.h"
#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/Renderer.h"

#include <glm/gtc/packing.hpp>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_TextureMagic = 0x58545453; // "STTX"
		constexpr uint32_t c_MaxTextureSize = 16384;

		struct CookedTextureHeader
		{
			uint32_t Magic;
			uint32_t Version;
			uint32_t Format;
			uint32_t Filter;
			uint32_t Wrap;
			uint32_t MipCount;
		};

		struct CookedMipHeader
		{
			uint32_t Width;
			uint32_t Height;
			uint64_t Size;
		};

		bool IsValidFormat(uint32_t format)
		{
			return format >= static_cast<uint32_t>(TextureFormat::RGBA8) && format <= static_cast<uint32_t>(TextureFormat::RG8);
		}

		// Where a level's pixels are in cooked data.
		struct CookedLevel
		{
			uint32_t Width = 0;
			uint32_t Height = 0;
			size_t Offset = 0;
			size_t Size = 0;
		};

		struct CookedTextureLayout
		{
			TextureSpecification Specification;
			std::vector<CookedLevel> Levels;
		};

		// Why a mip chain cannot make a texture of the specification's format (dimensions, level count, pixel byte counts in
		// `sizes`), or nullopt when it can.
		std::optional<std::string> FindMipChainProblem(const TextureSpecification& specification, std::span<const TextureMip> mips, std::span<const size_t> sizes)
		{
			const uint32_t bytesPerPixel = GetTextureFormatBytesPerPixel(specification.Format);
			if (bytesPerPixel == 0)
				return std::string("Texture has no valid format");
			if (mips.empty())
				return std::string("Texture has no pixel data");
			if (mips[0].Width == 0 || mips[0].Height == 0 || mips[0].Width > c_MaxTextureSize || mips[0].Height > c_MaxTextureSize)
				return fmt::format("Texture size {}x{} is out of range", mips[0].Width, mips[0].Height);
			if (mips.size() > TextureUtils::CalculateMipCount(mips[0].Width, mips[0].Height))
				return std::string("Texture has more mips than its size allows");

			for (size_t level = 0; level < mips.size(); level++)
			{
				const TextureMip& mip = mips[level];
				const uint32_t expectedWidth = std::max(1u, mips[0].Width >> level);
				const uint32_t expectedHeight = std::max(1u, mips[0].Height >> level);
				if (mip.Width != expectedWidth || mip.Height != expectedHeight)
					return fmt::format("Mip {} is {}x{}, expected {}x{}", level, mip.Width, mip.Height, expectedWidth, expectedHeight);
				const size_t expectedSize = static_cast<size_t>(mip.Width) * mip.Height * bytesPerPixel;
				if (sizes[level] != expectedSize)
					return fmt::format("Mip {} has {} bytes, expected {}", level, sizes[level], expectedSize);
			}
			return std::nullopt;
		}

		// Reads the header and mip table of cooked data (Texture::Serialize) and checks that every level's pixels are there;
		// the levels' dimensions and sizes are checked by FindMipChainProblem.
		std::optional<CookedTextureLayout> ParseCookedTexture(std::span<const uint8_t> data, std::string* outError)
		{
			auto fail = [outError](const std::string& message) -> std::optional<CookedTextureLayout>
			{
				if (outError)
					*outError = message;
				return std::nullopt;
			};

			BinaryReader reader(data);
			const CookedTextureHeader header = reader.Read<CookedTextureHeader>();
			if (!reader.IsValid() || header.Magic != c_TextureMagic)
				return fail("Not a cooked Strata texture");
			if (header.Version != Texture::c_CookedVersion)
				return fail(fmt::format("Unsupported cooked texture version {}", header.Version));
			if (!IsValidFormat(header.Format) || header.Filter > static_cast<uint32_t>(TextureFilter::Nearest)
				|| header.Wrap > static_cast<uint32_t>(TextureWrap::Mirror) || header.MipCount == 0 || header.MipCount > 15)
				return fail("Cooked texture header is corrupt");

			CookedTextureLayout layout;
			layout.Specification.Format = static_cast<TextureFormat>(header.Format);
			layout.Specification.Filter = static_cast<TextureFilter>(header.Filter);
			layout.Specification.Wrap = static_cast<TextureWrap>(header.Wrap);
			layout.Specification.DebugName = reader.ReadString(1024);

			std::vector<CookedMipHeader> mipHeaders(header.MipCount);
			for (CookedMipHeader& mipHeader : mipHeaders)
				mipHeader = reader.Read<CookedMipHeader>();
			if (!reader.IsValid())
				return fail("Cooked texture is truncated");

			layout.Levels.resize(header.MipCount);
			for (uint32_t level = 0; level < header.MipCount; level++)
			{
				const std::span<const uint8_t> bytes = reader.ReadView(static_cast<size_t>(mipHeaders[level].Size));
				if (!reader.IsValid())
					return fail("Cooked texture pixel data is truncated");
				CookedLevel& cooked = layout.Levels[level];
				cooked.Width = mipHeaders[level].Width;
				cooked.Height = mipHeaders[level].Height;
				cooked.Offset = static_cast<size_t>(bytes.data() - data.data());
				cooked.Size = bytes.size();
			}
			return layout;
		}

		float SRGBToLinear(float value)
		{
			return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
		}

		float LinearToSRGB(float value)
		{
			return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
		}

		const std::array<float, 256>& GetSRGBToLinearTable()
		{
			static const std::array<float, 256> s_Table = []()
			{
				std::array<float, 256> table {};
				for (size_t index = 0; index < table.size(); index++)
					table[index] = SRGBToLinear(static_cast<float>(index) / 255.0f);
				return table;
			}();
			return s_Table;
		}

		// Reads/writes RGBA pixels of any supported uncompressed format as linear floats.
		glm::vec4 LoadPixel(const TextureMip& mip, TextureFormat format, uint32_t x, uint32_t y)
		{
			const size_t pixelIndex = static_cast<size_t>(y) * mip.Width + x;
			switch (format)
			{
				case TextureFormat::RGBA8:
				{
					const uint8_t* pixel = mip.Data.data() + pixelIndex * 4;
					return glm::vec4(pixel[0], pixel[1], pixel[2], pixel[3]) / 255.0f;
				}
				case TextureFormat::RGBA8SRGB:
				{
					const uint8_t* pixel = mip.Data.data() + pixelIndex * 4;
					const std::array<float, 256>& table = GetSRGBToLinearTable();
					return glm::vec4(table[pixel[0]], table[pixel[1]], table[pixel[2]], pixel[3] / 255.0f);
				}
				case TextureFormat::RGBA16F:
				{
					// Byte storage is reinterpreted through memcpy (no aliasing through other pointer types).
					uint16_t pixel[4];
					std::memcpy(pixel, mip.Data.data() + pixelIndex * sizeof(pixel), sizeof(pixel));
					return glm::vec4(glm::unpackHalf1x16(pixel[0]), glm::unpackHalf1x16(pixel[1]), glm::unpackHalf1x16(pixel[2]), glm::unpackHalf1x16(pixel[3]));
				}
				case TextureFormat::RGBA32F:
				{
					float pixel[4];
					std::memcpy(pixel, mip.Data.data() + pixelIndex * sizeof(pixel), sizeof(pixel));
					return glm::vec4(pixel[0], pixel[1], pixel[2], pixel[3]);
				}
				default:
					return glm::vec4(0.0f);
			}
		}

		void StorePixel(TextureMip& mip, TextureFormat format, uint32_t x, uint32_t y, const glm::vec4& value)
		{
			const size_t pixelIndex = static_cast<size_t>(y) * mip.Width + x;
			auto toByte = [](float component) { return static_cast<uint8_t>(std::clamp(component * 255.0f + 0.5f, 0.0f, 255.0f)); };
			switch (format)
			{
				case TextureFormat::RGBA8:
				{
					uint8_t* pixel = mip.Data.data() + pixelIndex * 4;
					for (int channel = 0; channel < 4; channel++)
						pixel[channel] = toByte(value[channel]);
					break;
				}
				case TextureFormat::RGBA8SRGB:
				{
					uint8_t* pixel = mip.Data.data() + pixelIndex * 4;
					for (int channel = 0; channel < 3; channel++)
						pixel[channel] = toByte(LinearToSRGB(std::clamp(value[channel], 0.0f, 1.0f)));
					pixel[3] = toByte(value.a);
					break;
				}
				case TextureFormat::RGBA16F:
				{
					uint16_t pixel[4];
					for (int channel = 0; channel < 4; channel++)
						pixel[channel] = glm::packHalf1x16(value[channel]);
					std::memcpy(mip.Data.data() + pixelIndex * sizeof(pixel), pixel, sizeof(pixel));
					break;
				}
				case TextureFormat::RGBA32F:
				{
					float pixel[4] = { value[0], value[1], value[2], value[3] };
					std::memcpy(mip.Data.data() + pixelIndex * sizeof(pixel), pixel, sizeof(pixel));
					break;
				}
				default:
					break;
			}
		}

	}

	const char* TextureFormatToString(TextureFormat format)
	{
		switch (format)
		{
			case TextureFormat::None:      return "None";
			case TextureFormat::RGBA8:     return "RGBA8";
			case TextureFormat::RGBA8SRGB: return "RGBA8SRGB";
			case TextureFormat::RGBA16F:   return "RGBA16F";
			case TextureFormat::RGBA32F:   return "RGBA32F";
			case TextureFormat::R8:        return "R8";
			case TextureFormat::RG8:       return "RG8";
		}
		return "Unknown";
	}

	uint32_t GetTextureFormatBytesPerPixel(TextureFormat format)
	{
		switch (format)
		{
			case TextureFormat::RGBA8:
			case TextureFormat::RGBA8SRGB: return 4;
			case TextureFormat::RGBA16F:   return 8;
			case TextureFormat::RGBA32F:   return 16;
			case TextureFormat::R8:        return 1;
			case TextureFormat::RG8:       return 2;
			case TextureFormat::None:      return 0;
		}
		return 0;
	}

	nvrhi::Format ToNvrhiFormat(TextureFormat format)
	{
		switch (format)
		{
			case TextureFormat::RGBA8:     return nvrhi::Format::RGBA8_UNORM;
			case TextureFormat::RGBA8SRGB: return nvrhi::Format::SRGBA8_UNORM;
			case TextureFormat::RGBA16F:   return nvrhi::Format::RGBA16_FLOAT;
			case TextureFormat::RGBA32F:   return nvrhi::Format::RGBA32_FLOAT;
			case TextureFormat::R8:        return nvrhi::Format::R8_UNORM;
			case TextureFormat::RG8:       return nvrhi::Format::RG8_UNORM;
			case TextureFormat::None:      return nvrhi::Format::UNKNOWN;
		}
		return nvrhi::Format::UNKNOWN;
	}

	bool IsHDRFormat(TextureFormat format)
	{
		return format == TextureFormat::RGBA16F || format == TextureFormat::RGBA32F;
	}

	Ref<Texture> Texture::Create(const TextureSpecification& specification, std::vector<TextureMip> mips, std::string* outError)
	{
		std::vector<size_t> sizes;
		sizes.reserve(mips.size());
		for (const TextureMip& mip : mips)
			sizes.push_back(mip.Data.size());
		if (const std::optional<std::string> problem = FindMipChainProblem(specification, mips, sizes))
		{
			if (outError)
				*outError = *problem;
			return nullptr;
		}

		Ref<Texture> texture(new Texture());
		texture->m_Specification = specification;
		texture->m_Width = mips[0].Width;
		texture->m_Height = mips[0].Height;
		texture->m_MipCount = static_cast<uint32_t>(mips.size());
		texture->m_Mips = std::move(mips);
		return texture;
	}

	Texture::~Texture()
	{
		if (m_BindlessSlot != UINT32_MAX && Renderer::IsInitialized())
			Renderer::GetBindlessTextures().Release(m_BindlessSlot);
	}

	std::vector<uint8_t> Texture::Serialize() const
	{
		// Uploading releases the CPU copy.
		if (m_Mips.empty())
			return {};
		// Bytes taken over from cooked data are exactly what serializing would write.
		if (!m_Cooked.empty())
			return m_Cooked;

		BinaryWriter writer;
		writer.Write(CookedTextureHeader { c_TextureMagic, c_CookedVersion, static_cast<uint32_t>(m_Specification.Format),
			static_cast<uint32_t>(m_Specification.Filter), static_cast<uint32_t>(m_Specification.Wrap), static_cast<uint32_t>(m_Mips.size()) });
		writer.WriteString(m_Specification.DebugName);
		for (const TextureMip& mip : m_Mips)
			writer.Write(CookedMipHeader { mip.Width, mip.Height, mip.Data.size() });
		for (const TextureMip& mip : m_Mips)
			writer.WriteBytes(mip.Data.data(), mip.Data.size());
		return writer.TakeData();
	}

	Ref<Texture> Texture::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		std::optional<CookedTextureLayout> layout = ParseCookedTexture(data, outError);
		if (!layout)
			return nullptr;

		std::vector<TextureMip> mips(layout->Levels.size());
		for (size_t level = 0; level < mips.size(); level++)
		{
			const CookedLevel& cooked = layout->Levels[level];
			mips[level].Width = cooked.Width;
			mips[level].Height = cooked.Height;
			mips[level].Data.assign(data.begin() + cooked.Offset, data.begin() + cooked.Offset + cooked.Size);
		}
		return Create(layout->Specification, std::move(mips), outError);
	}

	Ref<Texture> Texture::Deserialize(std::vector<uint8_t>&& data, std::string* outError)
	{
		std::optional<CookedTextureLayout> layout = ParseCookedTexture(data, outError);
		if (!layout)
			return nullptr;

		// The texture keeps the cooked bytes and reads its levels' pixels in place: loading never copies them.
		std::vector<TextureMip> mips(layout->Levels.size());
		std::vector<size_t> sizes(layout->Levels.size());
		std::vector<size_t> offsets(layout->Levels.size());
		for (size_t level = 0; level < mips.size(); level++)
		{
			const CookedLevel& cooked = layout->Levels[level];
			mips[level].Width = cooked.Width;
			mips[level].Height = cooked.Height;
			sizes[level] = cooked.Size;
			offsets[level] = cooked.Offset;
		}
		if (const std::optional<std::string> problem = FindMipChainProblem(layout->Specification, mips, sizes))
		{
			if (outError)
				*outError = *problem;
			return nullptr;
		}

		Ref<Texture> texture(new Texture());
		texture->m_Specification = layout->Specification;
		texture->m_Width = mips[0].Width;
		texture->m_Height = mips[0].Height;
		texture->m_MipCount = static_cast<uint32_t>(mips.size());
		texture->m_Mips = std::move(mips);
		texture->m_Cooked = std::move(data);
		texture->m_CookedOffsets = std::move(offsets);
		return texture;
	}

	std::span<const uint8_t> Texture::GetMipData(uint32_t level) const
	{
		if (level >= m_Mips.size())
			return {};
		if (m_Cooked.empty())
			return m_Mips[level].Data;
		const TextureMip& mip = m_Mips[level];
		const size_t size = static_cast<size_t>(mip.Width) * mip.Height * GetTextureFormatBytesPerPixel(m_Specification.Format);
		return std::span<const uint8_t>(m_Cooked.data() + m_CookedOffsets[level], size);
	}

	bool Texture::FinalizeOnMainThread(const AssetFinalizeContext& context)
	{
		nvrhi::ICommandList* commandList = context.CommandList;
		if (!commandList || m_GPUTexture)
			return true; // No renderer (tools, tests): CPU data only

		nvrhi::TextureDesc desc;
		desc.width = m_Width;
		desc.height = m_Height;
		desc.mipLevels = m_MipCount;
		desc.format = ToNvrhiFormat(m_Specification.Format);
		desc.debugName = m_Specification.DebugName.empty() ? std::string("Texture") : m_Specification.DebugName;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;

		m_GPUTexture = Renderer::GetDevice()->createTexture(desc);
		if (!m_GPUTexture)
			return false;

		const uint32_t bytesPerPixel = GetTextureFormatBytesPerPixel(m_Specification.Format);
		for (uint32_t level = 0; level < m_MipCount; level++)
			commandList->writeTexture(m_GPUTexture, 0, level, GetMipData(level).data(), static_cast<size_t>(m_Mips[level].Width) * bytesPerPixel);

		m_BindlessSlot = Renderer::GetBindlessTextures().Allocate(m_GPUTexture);
		if (m_BindlessSlot == BindlessTextureTable::c_InvalidSlot)
		{
			m_GPUTexture = nullptr;
			return false; // Table full (logged by the table)
		}

		// The GPU copy is authoritative from now on; streaming re-reads cooked data when it needs pixels again.
		m_Mips.clear();
		m_Mips.shrink_to_fit();
		std::vector<uint8_t>().swap(m_Cooked);
		m_CookedOffsets.clear();
		return true;
	}

	AssetMemoryUsage Texture::GetMemoryUsage() const
	{
		AssetMemoryUsage usage;
		usage.Cpu = m_Cooked.size();
		for (const TextureMip& mip : m_Mips)
			usage.Cpu += mip.Data.size();

		if (m_GPUTexture)
		{
			const uint64_t bytesPerPixel = GetTextureFormatBytesPerPixel(m_Specification.Format);
			for (uint32_t level = 0; level < m_MipCount; level++)
				usage.GpuTextures += static_cast<uint64_t>(std::max(1u, m_Width >> level)) * std::max(1u, m_Height >> level) * bytesPerPixel;
		}
		return usage;
	}

	namespace TextureUtils
	{

		uint32_t CalculateMipCount(uint32_t width, uint32_t height)
		{
			uint32_t levels = 1;
			uint32_t size = std::max(width, height);
			while (size > 1)
			{
				size >>= 1;
				levels++;
			}
			return levels;
		}

		bool GenerateMips(std::vector<TextureMip>& mips, TextureFormat format, bool normalMap)
		{
			if (mips.empty() || GetTextureFormatBytesPerPixel(format) == 0 || format == TextureFormat::R8 || format == TextureFormat::RG8)
				return false;

			mips.resize(1);
			const uint32_t bytesPerPixel = GetTextureFormatBytesPerPixel(format);
			if (mips[0].Width == 0 || mips[0].Height == 0 || mips[0].Data.size() != static_cast<size_t>(mips[0].Width) * mips[0].Height * bytesPerPixel)
				return false;
			const uint32_t mipCount = CalculateMipCount(mips[0].Width, mips[0].Height);
			for (uint32_t level = 1; level < mipCount; level++)
			{
				const TextureMip& source = mips[level - 1];
				TextureMip destination;
				destination.Width = std::max(1u, source.Width / 2);
				destination.Height = std::max(1u, source.Height / 2);
				destination.Data.resize(static_cast<size_t>(destination.Width) * destination.Height * bytesPerPixel);

				for (uint32_t y = 0; y < destination.Height; y++)
				{
					for (uint32_t x = 0; x < destination.Width; x++)
					{
						// 2x2 box filter; odd edges (and 1-pixel dimensions) clamp to the last row/column.
						const uint32_t x0 = std::min(x * 2, source.Width - 1);
						const uint32_t x1 = std::min(x * 2 + 1, source.Width - 1);
						const uint32_t y0 = std::min(y * 2, source.Height - 1);
						const uint32_t y1 = std::min(y * 2 + 1, source.Height - 1);
						glm::vec4 sum = LoadPixel(source, format, x0, y0) + LoadPixel(source, format, x1, y0)
							+ LoadPixel(source, format, x0, y1) + LoadPixel(source, format, x1, y1);
						glm::vec4 value = sum * 0.25f;

						if (normalMap)
						{
							glm::vec3 normal = glm::vec3(value) * 2.0f - 1.0f;
							const float length = glm::length(normal);
							normal = length > 1e-6f ? normal / length : glm::vec3(0.0f, 0.0f, 1.0f);
							value = glm::vec4(normal * 0.5f + 0.5f, value.a);
						}
						StorePixel(destination, format, x, y, value);
					}
				}
				mips.push_back(std::move(destination));
			}
			return true;
		}

	}

}
