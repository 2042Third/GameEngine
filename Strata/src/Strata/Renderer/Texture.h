#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Renderer/CookedTexture.h"
#include "Strata/Renderer/TextureFormat.h"

#include <nvrhi/nvrhi.h>

#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	class GPUUploadQueue;
	enum class GPUUploadQueueKind : uint8_t;

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
	//
	// Partial residency: a texture loaded from a prefix of its cooked form (a partial load, AssetLoadData::IsPartial) holds
	// its mip tail only; its GPU texture holds the levels from GetResidentLevel() to the smallest. The renderer's texture
	// streamer (TextureStreamer) changes that: RaiseLevels with the cooked bytes of finer levels read by range, DropLevels to
	// release them, each continued within the frame's upload budgets (ContinueLevelChange) and finished by swapping in a new
	// GPU texture with a new bindless slot (the old slot is released once no frame in flight samples it), so what samples
	// the texture never sees the loading fallback again.
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
		// bytes are not needed otherwise): GetMips then has the levels' sizes, GetMipData their pixels. With a storedSize
		// larger than the data, the data is the first bytes of a version 2 cooked texture of that size (a partial load): the
		// texture holds the levels they contain (GetLoadedLevel), its mip tail.
		static Ref<Texture> Deserialize(std::vector<uint8_t>&& data, std::string* outError = nullptr);
		static Ref<Texture> Deserialize(std::vector<uint8_t>&& data, uint64_t storedSize, std::string* outError = nullptr);
		// The texture loader's AssetPrefixFunction: the bytes a partial load of a cooked texture reads (its header and mip
		// tail), from its first bytes. nullopt (load all of it) for data that cannot be read in parts (version 1, no tail) and
		// for block-compressed data while the device cannot sample it (it is decoded whole).
		static std::optional<uint64_t> GetLoadPrefixSize(std::span<const uint8_t> head, uint64_t storedSize);

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

		// Creates the GPU texture and uploads the mip chain through the renderer's upload queue (GPUUploadQueue), smallest
		// level first, in steps the queue sizes (PlanStep) to what is left of the context's upload bytes, upload time and
		// deadline: several whole levels stacked in one staging buffer while their staging fits a step, else runs of rows of a
		// level (rows of 4x4 blocks for block-compressed formats, a power of two of them, filling staging buffers of a power of
		// two of bytes), as far as the context allows (Pending: the next call continues where this one stopped). The frame's
		// first step is one row of blocks at least, whatever the budgets. Steps wait while the staging in flight leaves no
		// room within the context's staging budget (the frame's first) or room (the others), and the pool keeps all of its
		// staging within StagingBytes when it creates more (see StagingPool). Once the chain is uploaded, takes a bindless slot
		// and releases the CPU copy (copied levels of textures made from pixels go as soon as they are uploaded, cooked bytes
		// at the end). Freeing a large CPU copy is timed like a step: it waits for the next call (Pending) when it would end
		// after the deadline. Fails for block-compressed formats the device cannot sample. Without the renderer's upload
		// context (no device, or a test's context) the texture keeps its CPU data and is done.
		AssetFinalizeResult FinalizeOnMainThread(const AssetFinalizeContext& context) override;
		// The most one upload step copies (GPUUploadQueue::c_MaxStepBytes).
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
		// Its index in the renderer's texture table (Renderer::GetTextureTable), which material data names it by: the same for
		// the texture's life, its entry following the bindless slot as levels stream (TextureTable::c_InvalidIndex before the
		// texture was uploaded).
		uint32_t GetTableIndex() const { return m_TableIndex; }
		// Its texture table entry: the bindless slot with the sampler and grayscale bit (TextureTable::PackEntry).
		uint32_t GetTableEntry() const;

		// Partial residency (see the class comment; main thread unless noted).
		//
		// The finest level its GPU texture holds (it holds every level from this one on; GetMipCount() before it was
		// uploaded); without a renderer the finest level of its CPU copy. Any thread.
		uint32_t GetResidentLevel() const { return m_ResidentLevel.load(std::memory_order_acquire); }
		// The finest level the cooked data it was made from held: 0, or the first level of the mip tail for a partial load.
		uint32_t GetLoadedLevel() const { return m_LoadedLevel; }
		// The largest side of the levels that stay resident whatever the streaming (the resident tail): what a first, partial
		// load holds (the 32x32 level of a 2048x2048 BC7 texture and its smaller ones: 1.4 KiB), and what dropping levels
		// keeps.
		static constexpr uint32_t c_ResidentTailDimension = 32;
		// The first level of the resident tail (levels of at most c_ResidentTailDimension texels a side): a partial load holds
		// it, and dropping levels never goes further.
		uint32_t GetTailLevel() const;
		// Whether its levels can change: it was made from cooked data (whose levels can be read again) and is resident on the
		// GPU, and no change is under way.
		bool CanChangeLevels() const;
		// The finest level the change under way makes resident (GetResidentLevel() while none is under way).
		uint32_t GetChangeLevel() const;
		// Starts making levels [firstLevel, GetResidentLevel()) resident, from `data`: the cooked bytes of the range
		// GetCookedLayout().GetLevelsByteRange(firstLevel, GetResidentLevel()). False (nothing changes) when the levels
		// cannot change, firstLevel is not finer than the resident level, or the data does not have the range's size. With
		// `fadeIn` the new levels fade in once swapped in (TextureTable::SetFadeIn); views that need the texture whole
		// (exact pixels) raise without.
		bool RaiseLevels(uint32_t firstLevel, std::vector<uint8_t> data, bool fadeIn = true);
		// Starts keeping only levels [firstLevel, GetMipCount()), at most down to the tail (GetTailLevel). False when the
		// levels cannot change or firstLevel is not coarser than the resident level.
		bool DropLevels(uint32_t firstLevel);
		// Continues the change under way within the context's budgets (Pending until its uploads are done): copies the
		// resident levels it keeps into a new GPU texture on the GPU, uploads the raised ones through the upload queue, and,
		// once done, swaps the new texture in (Done). Failed leaves the resident levels as they were. Within an open batch of
		// the renderer's upload context.
		AssetFinalizeResult ContinueLevelChange(const AssetFinalizeContext& context);
		// Abandons the change under way (its new GPU texture goes once the GPU is done with it).
		void CancelLevelChange();
		// Bytes of levels [firstLevel, end) of the mip chain, in the format it is stored in.
		uint64_t GetLevelBytes(uint32_t firstLevel, uint32_t end) const;
		// The finest level something needs, as the texture streamer reports it (AssetDetailLevels::Wanted). Any thread.
		void SetWantedLevel(uint32_t level) { m_WantedLevel.store(level, std::memory_order_release); }
		bool GetDetailLevels(AssetDetailLevels& outLevels) const override;
	private:
		Texture() = default;

		// How far an upload of levels from the CPU copy got: levels [FirstLevel, FirstLevel + Remaining) are left (they go up
		// smallest first, so the next is FirstLevel + Remaining - 1) and, while a level goes up in runs of rows, the next of
		// its rows.
		struct UploadPosition
		{
			uint32_t FirstLevel = 0;
			uint32_t Remaining = 0;
			uint32_t NextRow = 0;
		};
		// The result of a run of upload steps.
		enum class UploadProgress : uint8_t
		{
			Complete = 0,
			Partial,  // The context's budget, time or staging room ran out first
			Failed
		};

		static Ref<Texture> CreateFromView(const CookedTextureView& view, std::vector<uint8_t>* takeOver, std::string* outError);
		CookedTextureDescription GetCookedDescription() const;
		// Bytes the CPU copy (cooked bytes and level data) holds allocated.
		uint64_t GetCPUCopyBytes() const;
		// Uploads levels of the CPU copy into `texture` (which holds levels from position.FirstLevel on at its level 0) in
		// steps within the context, advancing the position; frees the data of levels made from pixels once they are uploaded.
		// outBytes: what the call uploaded.
		UploadProgress UploadLevels(GPUUploadQueue& queue, const AssetFinalizeContext& context, nvrhi::ITexture* texture, UploadPosition& position,
			uint64_t& outBytes);
		// Uploads levels (UploadLevels) on the upload's queue and, once every level is copied there, hands an upload on the
		// transfer queue over to the graphics queue: Done once `texture` may be sampled, AwaitingGPU while the transfer
		// queue's copies run (only when the context allows it; else it waits for them), Pending or Failed like the steps.
		AssetFinalizeResult ContinueUpload(GPUUploadQueue& queue, const AssetFinalizeContext& context, nvrhi::ITexture* texture, UploadPosition& position,
			uint64_t& outBytes);
		// A GPU texture for levels [firstLevel, m_MipCount): its level 0 is level firstLevel. Null when it cannot be created.
		nvrhi::TextureHandle CreateGPUTexture(uint32_t firstLevel) const;
		// Makes `texture` (levels [firstLevel, m_MipCount)) the resident one: a new bindless slot, the old one released once
		// no frame in flight samples it. False when the table is full (nothing changes).
		bool SwapResidentTexture(nvrhi::TextureHandle texture, uint32_t firstLevel, bool fadeIn);

		// A change of the resident levels under way (RaiseLevels, DropLevels).
		struct LevelChange
		{
			uint32_t FirstLevel = 0;      // The new texture holds levels [FirstLevel, m_MipCount)
			nvrhi::TextureHandle Texture; // Created by the first ContinueLevelChange
			bool Copied = false;          // The levels it keeps of the resident texture were copied into it
			UploadPosition Position;      // The raised levels left to upload
			std::vector<uint8_t> Data;    // The raised levels' cooked bytes
			uint64_t DataOffset = 0;      // Where Data starts in the cooked form
			bool FadeIn = false;          // The raised levels fade in (RaiseLevels)
		};
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
		uint32_t m_TableIndex = UINT32_MAX;
		// While the texture is finalized over several calls: the GPU texture being filled and how far its upload got.
		nvrhi::TextureHandle m_UploadTexture;
		UploadPosition m_UploadPosition;
		GPUUploadQueueKind m_UploadQueue {}; // The queue the upload's copies run on (the graphics queue until one is chosen)
		uint64_t m_TransferTicket = 0; // While the transfer queue's copies complete: the ticket of the release (GPUUploadTicket)
		bool m_UploadStarted = false; // The CPU copy started to go (Serialize and Decompress need all of it)

		// Partial residency
		bool m_FromCookedData = false; // Made by Deserialize: its levels can be read again from the stored form
		uint32_t m_LoadedLevel = 0;    // The finest level the cooked data it was made from held
		std::atomic<uint32_t> m_ResidentLevel = 0;
		std::atomic<uint32_t> m_WantedLevel = UINT32_MAX; // UINT32_MAX: nothing reported
		std::atomic<uint64_t> m_ResidentBytes = 0;          // Bytes of the resident levels
		std::unique_ptr<LevelChange> m_LevelChange;
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
