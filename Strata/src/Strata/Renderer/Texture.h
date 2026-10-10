#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Renderer/CookedTexture.h"
#include "Strata/Renderer/TextureFormat.h"

#include <nvrhi/nvrhi.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	struct TextureMip
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<uint8_t> Data; // Tightly packed rows of texels, or of 4x4 blocks for block-compressed formats
	};

	struct TextureSpecification
	{
		TextureFormat Format = TextureFormat::RGBA8SRGB;
		TextureFilter Filter = TextureFilter::Linear;
		TextureWrap Wrap = TextureWrap::Repeat;
		// One stored channel (R8 or BC4) that stands for gray: shaders sample it as (r, r, r, 1).
		bool Grayscale = false;
		std::string DebugName;
	};

	// 2D texture asset: CPU mip chain plus (when a renderer is running) a GPU texture registered in the bindless
	// table. Created from cooked data by the asset system or directly from pixels.
	class Texture : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::Texture; }
		AssetType GetType() const override { return GetStaticType(); }

		// mips[0] is the full-resolution level; every level must have the size its dimensions give in the format
		// (GetMipByteSize: whole 4x4 blocks for block-compressed formats).
		static Ref<Texture> Create(const TextureSpecification& specification, std::vector<TextureMip> mips, std::string* outError = nullptr);

		// Cooked format (CookedTexture.h): Serialize writes version 2; Deserialize reads versions 1 and 2. Serialize returns
		// an empty vector once the GPU upload has started releasing the CPU copy.
		static constexpr uint32_t c_CookedVersion = c_CookedTextureVersion;
		std::vector<uint8_t> Serialize() const;
		static Ref<Texture> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);
		// The same, keeping the cooked bytes and reading the pixels in place instead of copying them (asset loads, where the
		// bytes are not needed otherwise): GetMips then has the levels' sizes, GetMipData their pixels.
		static Ref<Texture> Deserialize(std::vector<uint8_t>&& data, std::string* outError = nullptr);

		// A copy of a block-compressed texture decoded to GetDecodedFormat (RGBA8 or RGBA8SRGB; R8 for BC4, RG8 for BC5),
		// for devices that cannot sample block-compressed formats. It keeps this texture's cooked layout (GetCookedLayout:
		// where the block-compressed levels are in the data it was loaded from). Null (with the reason) when the texture is
		// not block-compressed or its CPU copy is gone.
		Ref<Texture> Decompress(std::string* outError = nullptr) const;

		// Whether textures loaded as assets may keep block-compressed data: true unless the graphics device cannot sample
		// it (Renderer::Init sets it from the device; true without a renderer). When false, the texture loader decodes
		// block-compressed data on its worker (Decompress). Thread-safe.
		static bool IsBlockCompressionSupported();
		static void SetBlockCompressionSupported(bool supported);

		~Texture() override;

		// Creates the GPU texture and uploads the mip chain in bands of at most c_UploadBandBytes - rows of one level (rows
		// of 4x4 blocks for block-compressed formats), or every remaining level once they fit in one band, staging included
		// (a block-compressed tail's staging is larger than its bytes, see RecordBlockUploadStep) - as far as the
		// context's upload budget allows (Pending: the next call continues where this one stopped). Bands go through staging
		// textures of the renderer's pool (StagingTexturePool), reused once the GPU has copied them; steps wait while the
		// staging in flight is at the context's StagingBytes, except the frame's first. Once the chain is uploaded, takes a
		// bindless slot and releases the CPU copy (copied levels go as soon as they are uploaded, cooked bytes at the end).
		// Freeing a large CPU copy is timed like a step: it waits for the next call (Pending) when it would end after the
		// deadline. Fails for block-compressed formats the device cannot sample.
		AssetFinalizeResult FinalizeOnMainThread(const AssetFinalizeContext& context) override;
		// The most one upload step copies. Steps are not started when they would end after the context's deadline (at the
		// speed of the latest steps), except a call's first.
		static constexpr uint64_t c_UploadBandBytes = c_AssetUploadStepBytes;
		// The CPU mip chain (until it is uploaded) and the GPU texture, in the stored format (block-compressed sizes).
		AssetMemoryUsage GetMemoryUsage() const override;
		// With a renderer, the GPU mip chain (the CPU copy goes); without one, the CPU mip chain.
		AssetMemoryUsage GetFinalizedMemoryUsage() const override;

		uint32_t GetWidth() const { return m_Width; }
		uint32_t GetHeight() const { return m_Height; }
		uint32_t GetMipCount() const { return m_MipCount; }
		const TextureSpecification& GetSpecification() const { return m_Specification; }
		// The levels until the CPU copy is released: their sizes, and their pixels unless the texture keeps cooked bytes
		// (GetMipData has the pixels either way).
		const std::vector<TextureMip>& GetMips() const { return m_Mips; }
		// A level's pixels (or blocks) while the CPU copy exists (empty after it was released, and for levels out of range).
		std::span<const uint8_t> GetMipData(uint32_t level) const;

		// Where the levels are in the cooked form, and in which format (CookedTextureLayout::Format): the data the texture
		// was loaded from - also when it was decoded at load time (Decompress), whose layout is the block-compressed data's
		// - or what Serialize writes for a texture made from pixels. Kept after the CPU copy is released, so streaming can
		// read levels again by byte range.
		const CookedTextureLayout& GetCookedLayout() const { return m_CookedLayout; }
		// The byte range of the cooked form holding the levels of at most maxDimension texels per side (the mip tail): in
		// version 2 data it starts right after the header.
		CookedTextureByteRange GetMipTailByteRange(uint32_t maxDimension = 128) const { return m_CookedLayout.GetMipTailByteRange(maxDimension); }

		// GPU state (main thread, after the asset became Ready).
		nvrhi::ITexture* GetGPUTexture() const { return m_GPUTexture; }
		uint32_t GetBindlessSlot() const { return m_BindlessSlot; }
	private:
		Texture() = default;

		// One step of the upload (see FinalizeOnMainThread): rows [Row, Row + Rows) of Level, or (Tail) every level from
		// Level on. Rows of block-compressed levels start and end on block rows (or the level's last row).
		struct UploadStep
		{
			uint32_t Level = 0;
			uint32_t Row = 0;
			uint32_t Rows = 0;
			bool Tail = false;
			uint64_t Bytes = 0;        // Texture bytes the step uploads
			uint64_t StagingBytes = 0; // Staging memory it uses: more than Bytes for a block-compressed tail (whole-block rows)
		};

		static Ref<Texture> CreateFromView(const CookedTextureView& view, std::vector<uint8_t>* takeOver, std::string* outError);
		CookedTextureDescription GetCookedDescription() const;
		UploadStep GetNextUploadStep() const;
		// Bytes of the whole mip chain.
		uint64_t GetChainBytes() const;
		// Bytes the CPU copy (cooked bytes and level data) holds allocated.
		uint64_t GetCPUCopyBytes() const;
		// Copies the step's pixels into a staging texture of the pool, records its copy into the GPU texture, releases copied
		// levels that are complete and advances the upload position. False if no staging texture could be created or mapped.
		bool RecordUploadStep(nvrhi::ICommandList* commandList, const UploadStep& step);
		// The same for block-compressed formats, whose copies must cover whole blocks of the staging memory: a band of block
		// rows, or the tail's levels stacked in one staging texture.
		bool RecordBlockUploadStep(nvrhi::ICommandList* commandList, const UploadStep& step);
		void CompleteUploadStep(const UploadStep& step, uint32_t levels);
	private:
		TextureSpecification m_Specification;
		std::vector<TextureMip> m_Mips;
		// Cooked bytes the texture took over (Deserialize from a vector): they hold the pixels, level i at
		// m_CookedLayout.Levels[i].Offset.
		std::vector<uint8_t> m_Cooked;
		CookedTextureLayout m_CookedLayout;
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		uint32_t m_MipCount = 0;

		nvrhi::TextureHandle m_GPUTexture;
		uint32_t m_BindlessSlot = UINT32_MAX;
		// Upload position while the texture is finalized over several calls: the next level and row to upload.
		uint32_t m_UploadLevel = 0;
		uint32_t m_UploadRow = 0;
	};

	namespace TextureUtils
	{
		// Number of mips in a full chain down to 1x1.
		uint32_t CalculateMipCount(uint32_t width, uint32_t height);
		// Builds the full mip chain from level 0 (box-filtered; sRGB data is averaged in linear space, normal maps are
		// renormalized) for textures created at runtime; imports filter better (TextureImporter). Supports RGBA8,
		// RGBA8SRGB, RGBA16F and RGBA32F.
		bool GenerateMips(std::vector<TextureMip>& mips, TextureFormat format, bool normalMap);
	}

}
