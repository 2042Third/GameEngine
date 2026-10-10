#include "stpch.h"
#include "Strata/Renderer/Texture.h"

#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/BlockCompression.h"
#include "Strata/Renderer/Renderer.h"

#include <glm/gtc/packing.hpp>

namespace Strata
{

	namespace
	{

		// Whether loaded textures may keep block-compressed data (see Texture::IsBlockCompressionSupported). Read by loaders
		// on worker threads.
		std::atomic<bool> s_BlockCompressionSupported = true;

		// How fast upload steps copy pixels into staging memory (bytes per millisecond), averaged over the latest steps, to
		// predict whether the next step still fits before a finalization deadline. Main thread only (finalization).
		float s_UploadBytesPerMs = 4.0e6f;

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
		// Uploading releases the CPU copy.
		if (m_Mips.empty() || m_UploadLevel > 0)
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
		if (m_Mips.empty() || m_UploadLevel > 0)
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
		if (level >= m_Mips.size())
			return {};
		if (m_Cooked.empty())
			return m_Mips[level].Data;
		const CookedTextureLevel& cooked = m_CookedLayout.Levels[level];
		return std::span<const uint8_t>(m_Cooked.data() + cooked.Offset, static_cast<size_t>(cooked.Size));
	}

	AssetFinalizeResult Texture::FinalizeOnMainThread(const AssetFinalizeContext& context)
	{
		nvrhi::ICommandList* commandList = context.CommandList;
		if (!commandList || m_BindlessSlot != BindlessTextureTable::c_InvalidSlot)
			return AssetFinalizeResult::Done; // No renderer (tools, tests): CPU data only; or finalized before

		if (!m_GPUTexture)
		{
			// Loaders decode block-compressed data for such devices (IsBlockCompressionSupported); textures created from
			// blocks in memory cannot be shown on them.
			if (IsBlockCompressedFormat(m_Specification.Format) && !Renderer::GetGraphicsDevice().GetInfo().SupportsBCCompression)
			{
				ST_CORE_ERROR("Texture '{}' is {}, which the graphics device cannot sample", m_Specification.DebugName,
					TextureFormatToString(m_Specification.Format));
				return AssetFinalizeResult::Failed;
			}

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
				return AssetFinalizeResult::Failed;
			m_UploadLevel = 0;
			m_UploadRow = 0;
		}

		// Steps until the chain is uploaded or the budget is used up; the first step of a call is always taken as far as the
		// upload budget and the deadline go, so a texture larger than any frame's budget still arrives. Steps also wait for
		// staging memory (it frees up as frames complete) beyond the context's StagingBytes, so a burst of arrivals cannot
		// pile up staging, except the frame's first step (nothing uploaded yet): every frame makes progress, also where no
		// device frames recycle staging (tools, tests). Unbudgeted calls (LoadAssetSync, memory assets) have no staging
		// limit and finish.
		const StagingTexturePool& stagingPool = Renderer::GetStagingTextures();
		const bool framesFirstCall = !context.UploadedBytes || *context.UploadedBytes == 0;
		uint64_t uploaded = 0;
		bool firstStep = true;
		while (m_UploadLevel < m_MipCount)
		{
			const UploadStep step = GetNextUploadStep();
			if (!(firstStep && framesFirstCall) && !stagingPool.HasRoomFor(step.StagingBytes, context.StagingBytes))
				break;
			// A step that would end after the deadline (at the copy speed seen so far) waits for the next call.
			const auto now = std::chrono::steady_clock::now();
			const auto predicted = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
				std::chrono::duration<float, std::milli>(static_cast<float>(step.Bytes) / s_UploadBytesPerMs));
			if (!firstStep && (uploaded + step.Bytes > context.UploadBudget || now >= context.Deadline || context.Deadline - now < predicted))
				break;
			const bool recorded = IsBlockCompressedFormat(m_Specification.Format) ? RecordBlockUploadStep(commandList, step) : RecordUploadStep(commandList, step);
			if (!recorded)
			{
				ST_CORE_ERROR("Texture '{}': no staging memory for its upload", m_Specification.DebugName);
				m_GPUTexture = nullptr;
				return AssetFinalizeResult::Failed;
			}
			const float stepMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - now).count();
			if (stepMs > 0.01f)
				s_UploadBytesPerMs = 0.8f * s_UploadBytesPerMs + 0.2f * (static_cast<float>(step.Bytes) / stepMs);
			uploaded += step.Bytes;
			firstStep = false;
		}
		if (context.UploadedBytes)
			*context.UploadedBytes += uploaded;
		if (m_UploadLevel < m_MipCount)
			return AssetFinalizeResult::Pending;

		// The chain is uploaded; the CPU copy goes before the texture is published. Freeing a large one takes time like a
		// step: when it would end after the deadline it is the next call's work (a call that took no step does it anyway).
		const uint64_t cpuCopyBytes = GetCPUCopyBytes();
		if (!firstStep && cpuCopyBytes >= c_TimedReleaseBytes)
		{
			const auto now = std::chrono::steady_clock::now();
			const auto predicted = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
				std::chrono::duration<float, std::milli>(static_cast<float>(cpuCopyBytes) / s_ReleaseBytesPerMs));
			if (now >= context.Deadline || context.Deadline - now < predicted)
				return AssetFinalizeResult::Pending;
		}

		m_BindlessSlot = Renderer::GetBindlessTextures().Allocate(m_GPUTexture);
		if (m_BindlessSlot == BindlessTextureTable::c_InvalidSlot)
		{
			m_GPUTexture = nullptr;
			return AssetFinalizeResult::Failed; // Table full (logged by the table)
		}

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

	uint64_t Texture::GetCPUCopyBytes() const
	{
		uint64_t bytes = m_Cooked.capacity();
		for (const TextureMip& mip : m_Mips)
			bytes += mip.Data.capacity();
		return bytes;
	}

	Texture::UploadStep Texture::GetNextUploadStep() const
	{
		UploadStep step;
		step.Level = m_UploadLevel;
		step.Row = m_UploadRow;
		const TextureFormatInfo info = GetTextureFormatInfo(m_Specification.Format);
		uint64_t tailBytes = 0;
		uint64_t tailBlockRows = 0;
		for (uint32_t level = m_UploadLevel; level < m_MipCount; level++)
		{
			tailBytes += GetMipByteSize(m_Specification.Format, m_Mips[level].Width, m_Mips[level].Height);
			tailBlockRows += (m_Mips[level].Height + info.BlockHeight - 1) / info.BlockHeight;
		}
		// The rest of the chain goes in one step once its staging fits in a band. Block-compressed tails are stacked in one
		// staging texture as wide as their first level (RecordBlockUploadStep), which takes more than their bytes (1.5 times
		// for a square chain); a level whose tail does not fit goes up in bands first (level 0 of a 2048x2048 BC1 chain in
		// one), so the tail that follows stacks at half its width.
		const uint64_t tailStagingBytes = info.IsBlockCompressed ? tailBlockRows * GetBlockRowByteSize(m_Specification.Format, m_Mips[m_UploadLevel].Width)
			: tailBytes;
		if (m_UploadRow == 0 && tailStagingBytes <= c_UploadBandBytes)
		{
			step.Tail = true;
			step.Rows = m_Mips[m_UploadLevel].Height;
			step.Bytes = tailBytes;
			step.StagingBytes = tailStagingBytes;
			return step;
		}

		// Bands of whole rows of blocks (texel rows for uncompressed formats); the level's last band may end inside a block.
		const TextureMip& mip = m_Mips[m_UploadLevel];
		const uint64_t blockRowBytes = GetBlockRowByteSize(m_Specification.Format, mip.Width);
		const uint64_t bandBlockRows = std::max<uint64_t>(1, c_UploadBandBytes / std::max<uint64_t>(1, blockRowBytes));
		const uint64_t remainingBlockRows = (mip.Height - m_UploadRow + info.BlockHeight - 1) / info.BlockHeight;
		const uint64_t blockRows = std::min(remainingBlockRows, bandBlockRows);
		step.Rows = static_cast<uint32_t>(std::min<uint64_t>(mip.Height - m_UploadRow, blockRows * info.BlockHeight));
		step.Bytes = blockRows * blockRowBytes;
		step.StagingBytes = step.Bytes;
		return step;
	}

	bool Texture::RecordUploadStep(nvrhi::ICommandList* commandList, const UploadStep& step)
	{
		nvrhi::IDevice* device = Renderer::GetDevice();
		const uint32_t levels = step.Tail ? m_MipCount - step.Level : 1;
		nvrhi::TextureDesc stagingDesc;
		stagingDesc.width = m_Mips[step.Level].Width;
		stagingDesc.height = step.Rows;
		stagingDesc.mipLevels = levels;
		stagingDesc.format = ToNvrhiFormat(m_Specification.Format);
		// The pool hands it out again once the frames that may copy from it are done.
		StagingTexturePool& stagingPool = Renderer::GetStagingTextures();
		nvrhi::StagingTextureHandle staging = stagingPool.Acquire(stagingDesc);
		if (!staging)
			return false;

		const size_t bytesPerTexel = GetTextureFormatInfo(m_Specification.Format).BytesPerBlock;
		for (uint32_t index = 0; index < levels; index++)
		{
			const TextureMip& source = m_Mips[step.Level + index];
			const uint32_t firstRow = step.Tail ? 0 : step.Row;
			const uint32_t rows = step.Tail ? source.Height : step.Rows;
			size_t rowPitch = 0;
			auto* destination = static_cast<uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice().setMipLevel(index), nvrhi::CpuAccessMode::Write,
				&rowPitch));
			if (!destination)
			{
				stagingPool.Release(std::move(staging));
				return false;
			}
			const size_t rowBytes = static_cast<size_t>(source.Width) * bytesPerTexel;
			const uint8_t* sourceRows = GetMipData(step.Level + index).data() + static_cast<size_t>(firstRow) * rowBytes;
			if (rowPitch == rowBytes)
			{
				std::memcpy(destination, sourceRows, rows * rowBytes);
			}
			else
			{
				for (uint32_t row = 0; row < rows; row++)
					std::memcpy(destination + row * rowPitch, sourceRows + row * rowBytes, rowBytes);
			}
			device->unmapStagingTexture(staging);
			commandList->copyTexture(m_GPUTexture, nvrhi::TextureSlice().setOrigin(0, firstRow, 0).setSize(source.Width, rows, 1).setMipLevel(step.Level + index),
				staging, nvrhi::TextureSlice().setSize(source.Width, rows, 1).setMipLevel(index));
		}
		stagingPool.Release(std::move(staging));
		CompleteUploadStep(step, levels);
		return true;
	}

	bool Texture::RecordBlockUploadStep(nvrhi::ICommandList* commandList, const UploadStep& step)
	{
		// Copies between buffers and block-compressed images must cover whole blocks of the buffer: the staging rows are a
		// whole number of blocks wide and every level starts on a block row. One single-level staging texture holds the
		// band, or the tail's levels below each other (a staging texture with mips would have levels below 4 texels, whose
		// rows are not whole blocks).
		const TextureFormatInfo info = GetTextureFormatInfo(m_Specification.Format);
		const uint32_t levels = step.Tail ? m_MipCount - step.Level : 1;
		auto getRows = [&](uint32_t index) { return step.Tail ? m_Mips[step.Level + index].Height : step.Rows; };
		nvrhi::TextureDesc stagingDesc;
		stagingDesc.width = AlignUp(m_Mips[step.Level].Width, info.BlockWidth);
		stagingDesc.height = 0;
		for (uint32_t index = 0; index < levels; index++)
			stagingDesc.height += AlignUp(getRows(index), info.BlockHeight);
		stagingDesc.mipLevels = 1;
		stagingDesc.format = ToNvrhiFormat(m_Specification.Format);
		StagingTexturePool& stagingPool = Renderer::GetStagingTextures();
		nvrhi::StagingTextureHandle staging = stagingPool.Acquire(stagingDesc);
		if (!staging)
			return false;

		nvrhi::IDevice* device = Renderer::GetDevice();
		size_t rowPitch = 0;
		auto* destination = static_cast<uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Write, &rowPitch));
		if (!destination)
		{
			stagingPool.Release(std::move(staging));
			return false;
		}
		uint32_t stagingBlockRow = 0;
		for (uint32_t index = 0; index < levels; index++)
		{
			const TextureMip& source = m_Mips[step.Level + index];
			const uint32_t firstRow = step.Tail ? 0 : step.Row;
			const uint32_t blockRows = AlignUp(getRows(index), info.BlockHeight) / info.BlockHeight;
			const size_t blockRowBytes = static_cast<size_t>(GetBlockRowByteSize(m_Specification.Format, source.Width));
			const uint8_t* sourceRows = GetMipData(step.Level + index).data() + static_cast<size_t>(firstRow / info.BlockHeight) * blockRowBytes;
			for (uint32_t blockRow = 0; blockRow < blockRows; blockRow++)
				std::memcpy(destination + static_cast<size_t>(stagingBlockRow + blockRow) * rowPitch, sourceRows + blockRow * blockRowBytes, blockRowBytes);
			stagingBlockRow += blockRows;
		}
		device->unmapStagingTexture(staging);

		uint32_t stagingRow = 0;
		for (uint32_t index = 0; index < levels; index++)
		{
			const TextureMip& source = m_Mips[step.Level + index];
			const uint32_t firstRow = step.Tail ? 0 : step.Row;
			const uint32_t rows = getRows(index);
			commandList->copyTexture(m_GPUTexture, nvrhi::TextureSlice().setOrigin(0, firstRow, 0).setSize(source.Width, rows, 1).setMipLevel(step.Level + index),
				staging, nvrhi::TextureSlice().setOrigin(0, stagingRow, 0).setSize(source.Width, rows, 1));
			stagingRow += AlignUp(rows, info.BlockHeight);
		}
		stagingPool.Release(std::move(staging));
		CompleteUploadStep(step, levels);
		return true;
	}

	void Texture::CompleteUploadStep(const UploadStep& step, uint32_t levels)
	{
		// Uploaded levels are not needed on the CPU any more.
		const uint32_t endRow = step.Row + step.Rows;
		const bool levelComplete = step.Tail || endRow == m_Mips[step.Level].Height;
		for (uint32_t index = 0; levelComplete && index < levels; index++)
			std::vector<uint8_t>().swap(m_Mips[step.Level + index].Data);
		m_UploadLevel = levelComplete ? step.Level + levels : step.Level;
		m_UploadRow = levelComplete ? 0 : endRow;
	}

	uint64_t Texture::GetChainBytes() const
	{
		uint64_t bytes = 0;
		for (uint32_t level = 0; level < m_MipCount; level++)
			bytes += GetMipByteSize(m_Specification.Format, std::max(1u, m_Width >> level), std::max(1u, m_Height >> level));
		return bytes;
	}

	AssetMemoryUsage Texture::GetFinalizedMemoryUsage() const
	{
		if (!Renderer::IsInitialized())
			return GetMemoryUsage();
		return AssetMemoryUsage { 0, GetChainBytes(), 0 };
	}

	AssetMemoryUsage Texture::GetMemoryUsage() const
	{
		AssetMemoryUsage usage;
		usage.Cpu = m_Cooked.size();
		for (const TextureMip& mip : m_Mips)
			usage.Cpu += mip.Data.size();
		if (m_GPUTexture)
			usage.GpuTextures = GetChainBytes();
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
