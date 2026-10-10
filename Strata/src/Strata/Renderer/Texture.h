#pragma once

#include "Strata/Asset/Asset.h"

#include <nvrhi/nvrhi.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	// Pixel formats of cooked textures. The numeric values are part of the cooked file format: never reorder.
	enum class TextureFormat : uint32_t
	{
		None = 0,
		RGBA8 = 1,     // Linear 8-bit (normal maps, masks, data)
		RGBA8SRGB = 2, // sRGB-encoded 8-bit color
		RGBA16F = 3,   // HDR (environment maps)
		RGBA32F = 4,
		R8 = 5,
		RG8 = 6
	};

	const char* TextureFormatToString(TextureFormat format);
	uint32_t GetTextureFormatBytesPerPixel(TextureFormat format);
	nvrhi::Format ToNvrhiFormat(TextureFormat format);
	bool IsHDRFormat(TextureFormat format);

	enum class TextureFilter : uint8_t
	{
		Linear = 0,
		Nearest
	};

	enum class TextureWrap : uint8_t
	{
		Repeat = 0,
		Clamp,
		Mirror
	};

	struct TextureMip
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<uint8_t> Data; // Tightly packed rows
	};

	struct TextureSpecification
	{
		TextureFormat Format = TextureFormat::RGBA8SRGB;
		TextureFilter Filter = TextureFilter::Linear;
		TextureWrap Wrap = TextureWrap::Repeat;
		std::string DebugName;
	};

	// 2D texture asset: CPU mip chain plus (when a renderer is running) a GPU texture registered in the bindless
	// table. Created from cooked data by the asset system or directly from pixels.
	class Texture : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::Texture; }
		AssetType GetType() const override { return GetStaticType(); }

		// mips[0] is the full-resolution level; every level must match the format's size.
		static Ref<Texture> Create(const TextureSpecification& specification, std::vector<TextureMip> mips, std::string* outError = nullptr);

		// Cooked format: "STTX" header, specification, mip table, pixel data. Returns an empty vector once the CPU
		// copy was released after the GPU upload.
		static constexpr uint32_t c_CookedVersion = 1;
		std::vector<uint8_t> Serialize() const;
		static Ref<Texture> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);
		// The same, keeping the cooked bytes and reading the pixels in place instead of copying them (asset loads, where the
		// bytes are not needed otherwise): GetMips then has the levels' sizes, GetMipData their pixels.
		static Ref<Texture> Deserialize(std::vector<uint8_t>&& data, std::string* outError = nullptr);

		~Texture() override;

		// Creates the GPU texture and its bindless slot, then releases the CPU copy.
		bool FinalizeOnMainThread(const AssetFinalizeContext& context) override;
		// The CPU mip chain (until it is uploaded) and the GPU texture.
		AssetMemoryUsage GetMemoryUsage() const override;

		uint32_t GetWidth() const { return m_Mips.empty() ? m_Width : m_Mips[0].Width; }
		uint32_t GetHeight() const { return m_Mips.empty() ? m_Height : m_Mips[0].Height; }
		uint32_t GetMipCount() const { return m_MipCount; }
		const TextureSpecification& GetSpecification() const { return m_Specification; }
		// The levels until the CPU copy is released: their sizes, and their pixels unless the texture keeps cooked bytes
		// (GetMipData has the pixels either way).
		const std::vector<TextureMip>& GetMips() const { return m_Mips; }
		// A level's pixels while the CPU copy exists (empty after it was released, and for levels out of range).
		std::span<const uint8_t> GetMipData(uint32_t level) const;

		// GPU state (main thread, after the asset became Ready).
		nvrhi::ITexture* GetGPUTexture() const { return m_GPUTexture; }
		uint32_t GetBindlessSlot() const { return m_BindlessSlot; }
	private:
		Texture() = default;
	private:
		TextureSpecification m_Specification;
		std::vector<TextureMip> m_Mips;
		// Cooked bytes the texture took over (Deserialize from a vector): they hold the pixels, level i at m_CookedOffsets[i].
		std::vector<uint8_t> m_Cooked;
		std::vector<size_t> m_CookedOffsets;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		uint32_t m_MipCount = 0;

		nvrhi::TextureHandle m_GPUTexture;
		uint32_t m_BindlessSlot = UINT32_MAX;
	};

	namespace TextureUtils
	{
		// Number of mips in a full chain down to 1x1.
		uint32_t CalculateMipCount(uint32_t width, uint32_t height);
		// Builds the full mip chain from level 0 (box-filtered; sRGB data is averaged in linear space, normal maps are
		// renormalized). Supports RGBA8, RGBA8SRGB, RGBA16F and RGBA32F.
		bool GenerateMips(std::vector<TextureMip>& mips, TextureFormat format, bool normalMap);
	}

}
