#include "stpch.h"
#include "Strata/Renderer/Texture.h"

#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/BlockCompression.h"
#include "Strata/Renderer/GPUUploadQueue.h"
#include "Strata/Renderer/Renderer.h"

#include <glm/gtc/packing.hpp>

#include <bit>

namespace Strata
{

	namespace
	{

		// Whether loaded textures may keep block-compressed data (see Texture::IsBlockCompressionSupported). Read by loaders
		// on worker threads.
		std::atomic<bool> s_BlockCompressionSupported = true;

		// Freeing a large block hands its pages back to the system, which takes milliseconds for tens of megabytes (2 to
		// 3 ms for the 89.5 MB of a 4096x4096 RGBA8 chain). A CPU copy from this size on is freed like an upload step: not
		// started when it would end after the finalization deadline, at the speed of the latest frees. It is not handed to
		// a worker: the next allocations of the frame would then come before the memory is back, raising the peak.
		constexpr uint64_t c_TimedReleaseBytes = 1ull << 20;
		// How fast freeing CPU copies goes (bytes per millisecond), averaged over the latest frees. Main thread only.
		float s_ReleaseBytesPerMs = 3.0e7f;

		uint32_t AlignUp(uint32_t value, uint32_t alignment)
		{
			return (value + alignment - 1) / alignment * alignment;
		}

		// Why a mip chain cannot make a texture of the specification's format (dimensions, level count, byte counts of the
		// levels in `sizes`), or nullopt when it can.
		std::optional<std::string> FindMipChainProblem(const TextureSpecification& specification, std::span<const TextureMip> mips, std::span<const size_t> sizes)
		{
			if (!IsValidTextureFormat(static_cast<uint32_t>(specification.Format)))
				return std::string("Texture has no valid format");
			if (specification.Grayscale && GetTextureFormatInfo(specification.Format).Channels != 1)
				return fmt::format("Only one-channel formats can be grayscale, not {}", TextureFormatToString(specification.Format));
			if (mips.empty())
				return std::string("Texture has no pixel data");
			if (mips[0].Width == 0 || mips[0].Height == 0 || mips[0].Width > c_MaxCookedTextureSize || mips[0].Height > c_MaxCookedTextureSize)
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
				const uint64_t expectedSize = GetMipByteSize(specification.Format, mip.Width, mip.Height);
				if (sizes[level] != expectedSize)
					return fmt::format("Mip {} has {} bytes, expected {}", level, sizes[level], expectedSize);
			}
			return std::nullopt;
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

		// Reads/writes RGBA pixels of the formats GenerateMips supports as linear floats.
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
		texture->m_CookedLayout = ComputeCookedTextureLayout(texture->GetCookedDescription());
		return texture;
	}

	Ref<Texture> Texture::CreateFromView(const CookedTextureView& view, std::vector<uint8_t>* takeOver, std::string* outError)
	{
		TextureSpecification specification;
		specification.Format = view.Format;
		specification.Filter = view.Filter;
		specification.Wrap = view.Wrap;
		specification.Grayscale = HasFlag(view.Flags, CookedTextureFlags::Grayscale);
		specification.DebugName = std::string(view.DebugName);

		std::vector<TextureMip> mips(view.Layout.MipCount);
		std::vector<size_t> sizes(view.Layout.MipCount);
		for (uint32_t level = 0; level < view.Layout.MipCount; level++)
		{
			const CookedTextureLevel& cooked = view.Layout.Levels[level];
			mips[level].Width = cooked.Width;
			mips[level].Height = cooked.Height;
			sizes[level] = static_cast<size_t>(cooked.Size);
			// Without taking the bytes over, the levels are copied out of the cooked data.
			if (!takeOver)
			{
				const std::span<const uint8_t> pixels = view.GetLevelData(level);
				mips[level].Data.assign(pixels.begin(), pixels.end());
			}
		}
		if (const std::optional<std::string> problem = FindMipChainProblem(specification, mips, sizes))
		{
			if (outError)
				*outError = *problem;
			return nullptr;
		}

		Ref<Texture> texture(new Texture());
		texture->m_Specification = std::move(specification);
		texture->m_Width = view.Width;
		texture->m_Height = view.Height;
		texture->m_MipCount = view.Layout.MipCount;
		texture->m_Mips = std::move(mips);
		texture->m_CookedLayout = view.Layout;
		texture->m_FromCookedData = true;
		texture->m_LoadedLevel = view.FirstLevelInData;
		texture->m_ResidentLevel.store(view.FirstLevelInData, std::memory_order_release);
		// The view refers to the taken-over buffer, which moves along without being copied: its offsets stay valid.
		if (takeOver)
			texture->m_Cooked = std::move(*takeOver);
		return texture;
	}

	Texture::~Texture()
	{
		if (m_BindlessSlot != UINT32_MAX && Renderer::IsInitialized())
			Renderer::GetBindlessTextures().Release(m_BindlessSlot);
	}

	bool Texture::IsBlockCompressionSupported()
	{
		return s_BlockCompressionSupported.load(std::memory_order_relaxed);
	}

	void Texture::SetBlockCompressionSupported(bool supported)
	{
		s_BlockCompressionSupported.store(supported, std::memory_order_relaxed);
	}

	CookedTextureDescription Texture::GetCookedDescription() const
	{
		CookedTextureDescription description;
		description.Format = m_Specification.Format;
		description.Filter = m_Specification.Filter;
		description.Wrap = m_Specification.Wrap;
		description.Flags = m_Specification.Grayscale ? CookedTextureFlags::Grayscale : CookedTextureFlags::None;
		description.Width = m_Width;
		description.Height = m_Height;
		description.MipCount = m_MipCount;
		description.DebugName = m_Specification.DebugName;
		return description;
	}

	std::vector<uint8_t> Texture::Serialize() const
	{
		// Uploading releases the CPU copy; a partial load never had all of it.
		if (m_Mips.empty() || m_UploadStarted || m_LoadedLevel > 0)
			return {};

		std::vector<std::span<const uint8_t>> levels;
		levels.reserve(m_MipCount);
		for (uint32_t level = 0; level < m_MipCount; level++)
			levels.push_back(GetMipData(level));
		return WriteCookedTexture(GetCookedDescription(), levels);
	}

	Ref<Texture> Texture::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		const std::optional<CookedTextureView> view = ParseCookedTexture(data, outError);
		if (!view)
			return nullptr;
		return CreateFromView(*view, nullptr, outError);
	}

	Ref<Texture> Texture::Deserialize(std::vector<uint8_t>&& data, std::string* outError)
	{
		const std::optional<CookedTextureView> view = ParseCookedTexture(data, outError);
		if (!view)
			return nullptr;
		// The texture keeps the cooked bytes and reads its levels' pixels in place: loading never copies them.
		return CreateFromView(*view, &data, outError);
	}

	Ref<Texture> Texture::Deserialize(std::vector<uint8_t>&& data, uint64_t storedSize, std::string* outError)
	{
		if (storedSize <= data.size())
			return Deserialize(std::move(data), outError);
		const std::optional<CookedTextureView> view = ParseCookedTexturePrefix(data, storedSize, outError);
		if (!view)
			return nullptr;
		return CreateFromView(*view, &data, outError);
	}

	std::optional<uint64_t> Texture::GetLoadPrefixSize(std::span<const uint8_t> head, uint64_t storedSize)
	{
		const std::optional<uint64_t> prefix = GetCookedTexturePrefixSize(head, storedSize, c_ResidentTailDimension);
		if (!prefix)
			return std::nullopt;
		// Block-compressed levels the device cannot sample are decoded at load (Decompress), all of them at once.
		const std::optional<CookedTextureView> view = ParseCookedTexturePrefix(head.first(std::min<size_t>(head.size(), static_cast<size_t>(*prefix))), storedSize);
		if (view && IsBlockCompressedFormat(view->Format) && !IsBlockCompressionSupported())
			return std::nullopt;
		return prefix;
	}

	Ref<Texture> Texture::Decompress(std::string* outError) const
	{
		auto fail = [outError](std::string message) -> Ref<Texture>
		{
			if (outError)
				*outError = std::move(message);
			return nullptr;
		};
		if (!IsBlockCompressedFormat(m_Specification.Format))
			return fail(fmt::format("Texture '{}' is not block-compressed", m_Specification.DebugName));
		if (m_Mips.empty() || m_UploadStarted || m_LoadedLevel > 0)
			return fail(fmt::format("Texture '{}' has no CPU copy to decode", m_Specification.DebugName));

		TextureSpecification specification = m_Specification;
		specification.Format = GetDecodedFormat(m_Specification.Format);
		const uint32_t bytesPerTexel = GetTextureFormatInfo(specification.Format).BytesPerBlock;
		std::vector<TextureMip> mips(m_MipCount);
		for (uint32_t level = 0; level < m_MipCount; level++)
		{
			TextureMip& mip = mips[level];
			mip.Width = m_Mips[level].Width;
			mip.Height = m_Mips[level].Height;
			mip.Data.resize(static_cast<size_t>(mip.Width) * mip.Height * bytesPerTexel);
			if (!BlockCompression::DecodeLevel(m_Specification.Format, mip.Width, mip.Height, GetMipData(level), mip.Data))
				return fail(fmt::format("Texture '{}': mip {} could not be decoded", m_Specification.DebugName, level));
		}
		Ref<Texture> decoded = Create(specification, std::move(mips), outError);
		// The levels stay where they are in the cooked data, block-compressed: streaming reads them from there again.
		if (decoded)
			decoded->m_CookedLayout = m_CookedLayout;
		return decoded;
	}

	std::span<const uint8_t> Texture::GetMipData(uint32_t level) const
	{
		if (level >= m_MipCount)
			return {};
		const CookedTextureLevel& cooked = m_CookedLayout.Levels[level];
		// Levels a change under way raises, from the bytes read for them.
		if (m_LevelChange && !m_LevelChange->Data.empty() && level >= m_LevelChange->FirstLevel && level < GetResidentLevel())
		{
			const uint64_t offset = cooked.Offset - m_LevelChange->DataOffset;
			if (cooked.Offset < m_LevelChange->DataOffset || offset > m_LevelChange->Data.size() || cooked.Size > m_LevelChange->Data.size() - offset)
				return {};
			return std::span<const uint8_t>(m_LevelChange->Data.data() + offset, static_cast<size_t>(cooked.Size));
		}
		if (level >= m_Mips.size())
			return {};
		if (m_Cooked.empty())
			return m_Mips[level].Data;
		// A partial load's prefix holds the tail only.
		if (cooked.Offset > m_Cooked.size() || cooked.Size > m_Cooked.size() - cooked.Offset)
			return {};
		return std::span<const uint8_t>(m_Cooked.data() + cooked.Offset, static_cast<size_t>(cooked.Size));
	}

	AssetFinalizeResult Texture::FinalizeOnMainThread(const AssetFinalizeContext& context)
	{
		RendererUploadContext* upload = RendererUploadContext::From(context.Upload);
		if (!upload || !upload->GetQueue().IsBatchOpen() || m_BindlessSlot != BindlessTextureTable::c_InvalidSlot)
			return AssetFinalizeResult::Done; // No renderer (tools, tests): CPU data only; or finalized before

		// Past the deadline only the frame's first finalization works (AssetFinalizeContext::FirstOfFrame); creating the GPU
		// texture is work too.
		if (!context.FirstOfFrame && std::chrono::steady_clock::now() >= context.Deadline)
			return AssetFinalizeResult::Pending;

		if (!m_UploadTexture)
		{
			// Loaders decode block-compressed data for such devices (IsBlockCompressionSupported); textures created from
			// blocks in memory cannot be shown on them.
			if (IsBlockCompressedFormat(m_Specification.Format) && !Renderer::GetGraphicsDevice().GetInfo().SupportsBCCompression)
			{
				ST_CORE_ERROR("Texture '{}' is {}, which the graphics device cannot sample", m_Specification.DebugName,
					TextureFormatToString(m_Specification.Format));
				return AssetFinalizeResult::Failed;
			}
			// The levels the cooked data held: all of them, or a partial load's mip tail.
			m_UploadTexture = CreateGPUTexture(m_LoadedLevel);
			if (!m_UploadTexture)
				return AssetFinalizeResult::Failed;
			m_UploadPosition = UploadPosition { m_LoadedLevel, m_MipCount - m_LoadedLevel, 0 };
			m_UploadQueue = upload->GetQueue().ChooseQueue(context);
		}

		uint64_t uploaded = 0;
		const AssetFinalizeResult progress = ContinueUpload(upload->GetQueue(), context, m_UploadTexture, m_UploadPosition, uploaded);
		if (progress == AssetFinalizeResult::Failed)
			m_UploadTexture = nullptr;
		if (progress != AssetFinalizeResult::Done)
			return progress;

		// The chain is uploaded; the CPU copy goes before the texture is published. Freeing a large one takes time like a
		// step: when it would end after the deadline it is the next call's work (the frame's first call that took no step
		// does it anyway).
		const uint64_t cpuCopyBytes = GetCPUCopyBytes();
		if (!(context.FirstOfFrame && uploaded == 0) && cpuCopyBytes >= c_TimedReleaseBytes)
		{
			const auto now = std::chrono::steady_clock::now();
			const auto predicted = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
				std::chrono::duration<float, std::milli>(static_cast<float>(cpuCopyBytes) / s_ReleaseBytesPerMs));
			if (now >= context.Deadline || context.Deadline - now < predicted)
				return AssetFinalizeResult::Pending;
		}

		const uint32_t slot = Renderer::GetBindlessTextures().Allocate(m_UploadTexture);
		if (slot == BindlessTextureTable::c_InvalidSlot)
		{
			m_UploadTexture = nullptr;
			return AssetFinalizeResult::Failed; // Table full (logged by the table)
		}
		m_BindlessSlot = slot;
		m_GPUTexture = std::move(m_UploadTexture);
		m_ResidentLevel.store(m_LoadedLevel, std::memory_order_release);
		m_ResidentBytes.store(GetLevelBytes(m_LoadedLevel, m_MipCount), std::memory_order_release);

		// The GPU copy is authoritative from now on; streaming re-reads cooked data when it needs pixels again.
		const auto releaseStart = std::chrono::steady_clock::now();
		m_Mips.clear();
		m_Mips.shrink_to_fit();
		std::vector<uint8_t>().swap(m_Cooked);
		if (cpuCopyBytes >= c_TimedReleaseBytes)
		{
			const float releaseMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - releaseStart).count();
			if (releaseMs > 0.01f)
				s_ReleaseBytesPerMs = 0.8f * s_ReleaseBytesPerMs + 0.2f * (static_cast<float>(cpuCopyBytes) / releaseMs);
		}
		return AssetFinalizeResult::Done;
	}

	AssetFinalizeResult Texture::ContinueUpload(GPUUploadQueue& queue, const AssetFinalizeContext& context, nvrhi::ITexture* texture,
		UploadPosition& position, uint64_t& outBytes)
	{
		outBytes = 0;
		const GPUQueueTransfer transfer { texture, 0, texture->getDesc().mipLevels, nullptr };
		if (m_TransferTicket == 0)
		{
			const UploadProgress progress = UploadLevels(queue, context, texture, position, outBytes);
			if (progress == UploadProgress::Failed)
			{
				ST_CORE_ERROR("Texture '{}': no staging memory for its upload", m_Specification.DebugName);
				return AssetFinalizeResult::Failed;
			}
			if (progress == UploadProgress::Partial)
				return AssetFinalizeResult::Pending;
			if (m_UploadQueue == GPUUploadQueueKind::Graphics)
				return AssetFinalizeResult::Done;
			// Every level was copied on the transfer queue: the graphics queue takes the texture over once the copies completed.
			m_TransferTicket = queue.ReleaseToGraphics(transfer);
		}
		if (!queue.IsComplete(m_TransferTicket))
		{
			if (context.MayWait)
				return AssetFinalizeResult::AwaitingGPU;
			queue.WaitForTicket(m_TransferTicket);
		}
		queue.AcquireOnGraphics(transfer);
		m_TransferTicket = 0;
		m_UploadQueue = GPUUploadQueueKind::Graphics; // Handed over: nothing is left on the transfer queue
		return AssetFinalizeResult::Done;
	}

	nvrhi::TextureHandle Texture::CreateGPUTexture(uint32_t firstLevel) const
	{
		nvrhi::TextureDesc desc;
		desc.width = std::max(1u, m_Width >> firstLevel);
		desc.height = std::max(1u, m_Height >> firstLevel);
		desc.mipLevels = m_MipCount - firstLevel;
		desc.format = ToNvrhiFormat(m_Specification.Format);
		desc.debugName = m_Specification.DebugName.empty() ? std::string("Texture") : m_Specification.DebugName;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		return Renderer::GetDevice()->createTexture(desc);
	}

	Texture::UploadProgress Texture::UploadLevels(GPUUploadQueue& queue, const AssetFinalizeContext& context, nvrhi::ITexture* texture, UploadPosition& position, uint64_t& outBytes)
	{
		m_UploadStarted = true;
		const TextureFormat format = m_Specification.Format;
		const TextureFormatInfo info = GetTextureFormatInfo(format);
		const auto levelWidth = [this](uint32_t level) { return std::max(1u, m_Width >> level); };
		const auto levelHeight = [this](uint32_t level) { return std::max(1u, m_Height >> level); };
		const auto levelRegion = [&](uint32_t level, uint32_t firstRow, uint32_t rows)
		{
			const uint64_t rowBytes = GetBlockRowByteSize(format, levelWidth(level));
			return GPUTextureRegion { level - position.FirstLevel, levelWidth(level), levelHeight(level), firstRow, rows,
				GetMipData(level).data() + static_cast<size_t>(firstRow / info.BlockHeight) * rowBytes };
		};
		// Levels of textures made from pixels hold their own data: it goes once the level is uploaded.
		const auto releaseLevel = [this](uint32_t level)
		{
			if (level < m_Mips.size())
				std::vector<uint8_t>().swap(m_Mips[level].Data);
		};

		uint64_t& callBytes = outBytes;
		callBytes = 0;
		float callMs = 0.0f;
		std::vector<GPUTextureRegion> regions;
		while (position.Remaining > 0)
		{
			const uint32_t level = position.FirstLevel + position.Remaining - 1;
			const uint64_t rowBytes = GetBlockRowByteSize(format, levelWidth(level));
			const uint32_t levelBlockRows = AlignUp(levelHeight(level), info.BlockHeight) / info.BlockHeight;
			const uint64_t remainingBytes = static_cast<uint64_t>(levelBlockRows - position.NextRow / info.BlockHeight) * rowBytes
				+ GetLevelBytes(position.FirstLevel, level);
			const bool framesFirst = GPUUploadQueue::IsFramesFirstStep(context, callBytes);
			// A step is a row of blocks at least, and not much less than GPUUploadQueue::c_MinStepBytes where the level has more.
			const uint64_t granule = std::max(rowBytes, std::min(GPUUploadQueue::c_MinStepBytes, remainingBytes) / rowBytes * rowBytes);
			const uint64_t limit = queue.PlanStep(context, callBytes, callMs, remainingBytes, granule);
			if (limit == 0)
				return UploadProgress::Partial;

			// Whole levels stacked in one staging buffer while their bytes fit the step and their staging buffer (a power of two
			// of bytes) is a step at most; a level that does not fit alone goes up in runs of rows, a power of two of block rows
			// (whole staging buffers for levels of powers of two).
			regions.clear();
			uint32_t stacked = 0;
			uint64_t stackedBytes = 0;
			if (position.NextRow == 0)
			{
				for (uint32_t candidate = level + 1; candidate-- > position.FirstLevel;)
				{
					const uint64_t candidateBytes = GetLevelBytes(candidate, candidate + 1);
					regions.push_back(levelRegion(candidate, 0, levelHeight(candidate)));
					if (stackedBytes + candidateBytes > limit || GPUUploadQueue::GetStagingBytes(format, regions) > GPUUploadQueue::c_MaxStepBytes)
					{
						regions.pop_back();
						break;
					}
					stackedBytes += candidateBytes;
					stacked++;
				}
			}
			if (stacked == 0)
			{
				const uint32_t remainingBlockRows = levelBlockRows - position.NextRow / info.BlockHeight;
				const uint64_t fitting = std::max<uint64_t>(1, limit / rowBytes);
				const uint32_t blockRows = static_cast<uint32_t>(std::min<uint64_t>(remainingBlockRows, std::bit_floor(fitting)));
				const uint32_t rows = std::min(levelHeight(level) - position.NextRow, blockRows * info.BlockHeight);
				regions.push_back(levelRegion(level, position.NextRow, rows));
			}

			if (!queue.HasStagingRoom(context, GPUUploadQueue::GetStagingBytes(format, regions), framesFirst))
				return UploadProgress::Partial;
			const GPUUploadStep step = queue.UploadTexture(context, texture, format, regions, m_UploadQueue);
			if (!step.Recorded)
				return UploadProgress::Failed;
			callBytes += step.Bytes;
			callMs += step.Milliseconds;

			if (stacked > 0)
			{
				for (uint32_t index = 0; index < stacked; index++)
					releaseLevel(level - index);
				position.Remaining -= stacked;
				continue;
			}
			position.NextRow += regions.front().Rows;
			if (position.NextRow >= levelHeight(level))
			{
				releaseLevel(level);
				position.Remaining--;
				position.NextRow = 0;
			}
		}
		return UploadProgress::Complete;
	}

	uint64_t Texture::GetCPUCopyBytes() const
	{
		uint64_t bytes = m_Cooked.capacity();
		for (const TextureMip& mip : m_Mips)
			bytes += mip.Data.capacity();
		return bytes;
	}

	uint64_t Texture::GetLevelBytes(uint32_t firstLevel, uint32_t end) const
	{
		uint64_t bytes = 0;
		for (uint32_t level = firstLevel; level < end; level++)
			bytes += GetMipByteSize(m_Specification.Format, std::max(1u, m_Width >> level), std::max(1u, m_Height >> level));
		return bytes;
	}

	AssetMemoryUsage Texture::GetFinalizedMemoryUsage() const
	{
		if (!Renderer::IsInitialized())
			return GetMemoryUsage();
		return AssetMemoryUsage { 0, GetLevelBytes(m_LoadedLevel, m_MipCount), 0 };
	}

	AssetMemoryUsage Texture::GetMemoryUsage() const
	{
		AssetMemoryUsage usage;
		usage.Cpu = m_Cooked.size();
		for (const TextureMip& mip : m_Mips)
			usage.Cpu += mip.Data.size();
		if (m_GPUTexture)
			usage.GpuTextures = GetLevelBytes(GetResidentLevel(), m_MipCount);
		else if (m_UploadTexture)
			usage.GpuTextures = GetLevelBytes(m_LoadedLevel, m_MipCount);
		// A change under way holds its new texture beside the resident one, and the raised levels' bytes until they are up.
		if (m_LevelChange)
		{
			usage.Cpu += m_LevelChange->Data.size();
			if (m_LevelChange->Texture)
				usage.GpuTextures += GetLevelBytes(m_LevelChange->FirstLevel, m_MipCount);
		}
		return usage;
	}

	uint32_t Texture::GetTailLevel() const
	{
		const CookedTextureByteRange tail = m_CookedLayout.GetMipTailByteRange(c_ResidentTailDimension);
		return tail.LevelCount > 0 ? tail.FirstLevel : m_MipCount - 1;
	}

	bool Texture::CanChangeLevels() const
	{
		return m_FromCookedData && m_GPUTexture && m_BindlessSlot != BindlessTextureTable::c_InvalidSlot && !m_LevelChange && !m_UploadTexture
			&& m_CookedLayout.Version >= 2;
	}

	uint32_t Texture::GetChangeLevel() const
	{
		return m_LevelChange ? m_LevelChange->FirstLevel : GetResidentLevel();
	}

	bool Texture::RaiseLevels(uint32_t firstLevel, std::vector<uint8_t> data)
	{
		const uint32_t resident = GetResidentLevel();
		if (!CanChangeLevels() || firstLevel >= resident || firstLevel >= m_MipCount)
			return false;
		const CookedTextureByteRange range = m_CookedLayout.GetLevelsByteRange(firstLevel, resident);
		if (range.Size != data.size())
			return false;
		m_LevelChange = std::make_unique<LevelChange>();
		m_LevelChange->FirstLevel = firstLevel;
		m_LevelChange->Position = UploadPosition { firstLevel, resident - firstLevel, 0 };
		m_LevelChange->Data = std::move(data);
		m_LevelChange->DataOffset = range.Offset;
		return true;
	}

	bool Texture::DropLevels(uint32_t firstLevel)
	{
		const uint32_t resident = GetResidentLevel();
		firstLevel = std::min(firstLevel, GetTailLevel());
		if (!CanChangeLevels() || firstLevel <= resident)
			return false;
		m_LevelChange = std::make_unique<LevelChange>();
		m_LevelChange->FirstLevel = firstLevel;
		m_LevelChange->Position = UploadPosition { firstLevel, 0, 0 };
		return true;
	}

	void Texture::CancelLevelChange()
	{
		m_LevelChange.reset();
	}

	AssetFinalizeResult Texture::ContinueLevelChange(const AssetFinalizeContext& context)
	{
		RendererUploadContext* upload = RendererUploadContext::From(context.Upload);
		if (!m_LevelChange || !m_GPUTexture)
			return AssetFinalizeResult::Failed;
		if (!upload || !upload->GetQueue().IsBatchOpen())
			return AssetFinalizeResult::Pending; // Nothing records it now
		if (!context.FirstOfFrame && std::chrono::steady_clock::now() >= context.Deadline)
			return AssetFinalizeResult::Pending;

		GPUUploadQueue& queue = upload->GetQueue();
		LevelChange& change = *m_LevelChange;
		if (!change.Texture)
		{
			change.Texture = CreateGPUTexture(change.FirstLevel);
			if (!change.Texture)
			{
				m_LevelChange.reset();
				return AssetFinalizeResult::Failed;
			}
		}
		if (!change.Copied)
		{
			// The levels both textures hold, from the resident one on the GPU: copies on the graphics queue, which samples it.
			const uint32_t resident = GetResidentLevel();
			const uint32_t kept = std::max(change.FirstLevel, resident);
			queue.CopyTextureLevels(change.Texture, kept - change.FirstLevel, m_GPUTexture, kept - resident, m_MipCount - kept);
			change.Copied = true;
		}
		if (change.Position.Remaining > 0)
		{
			// The raised levels go up on the graphics queue too: the change swaps in as soon as its batch is submitted.
			m_UploadQueue = GPUUploadQueueKind::Graphics;
			uint64_t uploaded = 0;
			const UploadProgress progress = UploadLevels(queue, context, change.Texture, change.Position, uploaded);
			if (progress == UploadProgress::Failed)
			{
				ST_CORE_ERROR("Texture '{}': no staging memory to raise its levels", m_Specification.DebugName);
				m_LevelChange.reset();
				return AssetFinalizeResult::Failed;
			}
			if (progress == UploadProgress::Partial)
				return AssetFinalizeResult::Pending;
		}

		nvrhi::TextureHandle texture = std::move(change.Texture);
		const uint32_t firstLevel = change.FirstLevel;
		m_LevelChange.reset();
		return SwapResidentTexture(std::move(texture), firstLevel) ? AssetFinalizeResult::Done : AssetFinalizeResult::Failed;
	}

	bool Texture::SwapResidentTexture(nvrhi::TextureHandle texture, uint32_t firstLevel)
	{
		BindlessTextureTable& bindless = Renderer::GetBindlessTextures();
		const uint32_t slot = bindless.Allocate(texture);
		if (slot == BindlessTextureTable::c_InvalidSlot)
			return false; // Table full (logged by the table): the resident levels stay
		// The old slot keeps its texture until no frame in flight samples it; frames from now on sample the new one.
		bindless.Release(m_BindlessSlot);
		m_BindlessSlot = slot;
		m_GPUTexture = std::move(texture);
		m_ResidentLevel.store(firstLevel, std::memory_order_release);
		m_ResidentBytes.store(GetLevelBytes(firstLevel, m_MipCount), std::memory_order_release);
		return true;
	}

	bool Texture::GetDetailLevels(AssetDetailLevels& outLevels) const
	{
		outLevels.Resident = GetResidentLevel();
		const uint32_t wanted = m_WantedLevel.load(std::memory_order_acquire);
		outLevels.Wanted = wanted == UINT32_MAX ? outLevels.Resident : wanted;
		outLevels.Count = m_MipCount;
		outLevels.ResidentBytes = m_ResidentBytes.load(std::memory_order_acquire);
		return true;
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
			const bool supported = format == TextureFormat::RGBA8 || format == TextureFormat::RGBA8SRGB || format == TextureFormat::RGBA16F
				|| format == TextureFormat::RGBA32F;
			if (mips.empty() || !supported)
				return false;

			mips.resize(1);
			const uint32_t bytesPerPixel = GetTextureFormatInfo(format).BytesPerBlock;
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
