#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/StagingPool.h"
#include "Strata/Renderer/Texture.h"

#include <nvrhi/nvrhi.h>

#include <chrono>
#include <cstring>
#include <limits>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A level of a test texture: every texel encodes its column, row and level, so misplaced bands show.
	glm::u8vec4 GetPatternTexel(uint32_t x, uint32_t y, uint32_t level)
	{
		return glm::u8vec4(static_cast<uint8_t>(x * 7 + level), static_cast<uint8_t>(y), static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(level * 20));
	}

	std::vector<TextureMip> CreatePatternChain(uint32_t width, uint32_t height)
	{
		std::vector<TextureMip> mips;
		for (uint32_t level = 0; level < TextureUtils::CalculateMipCount(width, height); level++)
		{
			TextureMip mip;
			mip.Width = std::max(1u, width >> level);
			mip.Height = std::max(1u, height >> level);
			mip.Data.reserve(static_cast<size_t>(mip.Width) * mip.Height * 4);
			for (uint32_t y = 0; y < mip.Height; y++)
			{
				for (uint32_t x = 0; x < mip.Width; x++)
				{
					const glm::u8vec4 texel = GetPatternTexel(x, y, level);
					mip.Data.insert(mip.Data.end(), { texel.r, texel.g, texel.b, texel.a });
				}
			}
			mips.push_back(std::move(mip));
		}
		return mips;
	}

	// One finalization call on its own command list, executed and waited for.
	AssetFinalizeResult FinalizeOnce(GPUContext& gpu, Asset& asset, uint64_t uploadBudget, uint64_t& outUploaded)
	{
		RendererUploadContext& upload = Renderer::GetUploadContext();
		upload.Begin();
		AssetFinalizeContext context { &upload, nullptr };
		context.UploadBudget = uploadBudget;
		outUploaded = 0;
		context.UploadedBytes = &outUploaded;
		const AssetFinalizeResult result = asset.FinalizeOnMainThread(context);
		upload.Submit();
		gpu.GetDevice().WaitForIdle();
		return result;
	}

	// Device frames without work, until releases deferred for frames in flight have happened.
	void SettleDevice(GPUContext& gpu)
	{
		for (uint32_t frame = 0; frame < gpu.GetDevice().GetMaxFramesInFlight() + 2; frame++)
		{
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			gpu.GetDevice().EndFrame();
		}
		gpu.GetDevice().WaitForIdle();
		gpu.GetNvrhiDevice()->runGarbageCollection();
	}

	// The blocks of a level of a block-compressed GPU texture, copied through a staging texture whose rows are whole blocks.
	std::vector<uint8_t> ReadBlocks(GPUContext& gpu, nvrhi::ITexture* texture, uint32_t level)
	{
		const nvrhi::TextureDesc& textureDesc = texture->getDesc();
		const nvrhi::FormatInfo& format = nvrhi::getFormatInfo(textureDesc.format);
		const uint32_t width = std::max(1u, textureDesc.width >> level);
		const uint32_t height = std::max(1u, textureDesc.height >> level);
		const uint32_t blocksX = (width + format.blockSize - 1) / format.blockSize;
		const uint32_t blocksY = (height + format.blockSize - 1) / format.blockSize;
		nvrhi::TextureDesc stagingDesc;
		stagingDesc.width = blocksX * format.blockSize;
		stagingDesc.height = blocksY * format.blockSize;
		stagingDesc.format = textureDesc.format;
		stagingDesc.debugName = "BlockReadback";
		nvrhi::StagingTextureHandle staging = gpu.GetNvrhiDevice()->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
		REQUIRE(staging);
		nvrhi::CommandListHandle commandList = gpu.GetNvrhiDevice()->createCommandList();
		commandList->open();
		commandList->copyTexture(staging, nvrhi::TextureSlice().setSize(width, height, 1), texture, nvrhi::TextureSlice().setMipLevel(level).setSize(width, height, 1));
		commandList->close();
		REQUIRE(gpu.ExecuteAndWait(commandList));

		size_t rowPitch = 0;
		const auto* mapped = static_cast<const uint8_t*>(gpu.GetNvrhiDevice()->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
		REQUIRE(mapped);
		const size_t blockRowBytes = static_cast<size_t>(blocksX) * format.bytesPerBlock;
		std::vector<uint8_t> blocks(blockRowBytes * blocksY);
		for (uint32_t blockRow = 0; blockRow < blocksY; blockRow++)
			std::memcpy(blocks.data() + blockRow * blockRowBytes, mapped + blockRow * rowPitch, blockRowBytes);
		gpu.GetNvrhiDevice()->unmapStagingTexture(staging);
		return blocks;
	}

	// Copies a GPU buffer into CPU memory.
	std::vector<uint8_t> ReadBuffer(GPUContext& gpu, nvrhi::IBuffer* buffer, size_t size)
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = size;
		desc.cpuAccess = nvrhi::CpuAccessMode::Read;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		desc.keepInitialState = true;
		desc.debugName = "ReadbackBuffer";
		nvrhi::BufferHandle readback = gpu.GetNvrhiDevice()->createBuffer(desc);
		REQUIRE(readback);
		nvrhi::CommandListHandle commandList = gpu.GetNvrhiDevice()->createCommandList();
		commandList->open();
		commandList->copyBuffer(readback, 0, buffer, 0, size);
		commandList->close();
		REQUIRE(gpu.ExecuteAndWait(commandList));
		const void* mapped = gpu.GetNvrhiDevice()->mapBuffer(readback, nvrhi::CpuAccessMode::Read);
		REQUIRE(mapped);
		std::vector<uint8_t> bytes(size);
		std::memcpy(bytes.data(), mapped, size);
		gpu.GetNvrhiDevice()->unmapBuffer(readback);
		return bytes;
	}

}

TEST_SUITE("GPU.Assets.Uploads")
{
	TEST_CASE("Staging is reused once the GPU has run the copies fenced with it, and released when uploads stop")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		{
			StagingPool pool(gpu.GetNvrhiDevice());
			nvrhi::TextureDesc band;
			band.width = 256;
			band.height = 256;
			band.format = nvrhi::Format::RGBA8_UNORM;
			const uint64_t bandBytes = 256 * 256 * 4;
			CHECK(StagingPool::GetByteSize(band) == bandBytes);

			uint64_t frame = 100;
			pool.BeginFrame(frame);
			nvrhi::StagingTextureHandle first = pool.AcquireTexture(band);
			REQUIRE(first);
			nvrhi::IStagingTexture* firstObject = first.Get();
			CHECK(pool.GetCreatedCount() == 1);
			pool.Release(std::move(first));
			CHECK(pool.GetPendingBytes() == bandBytes);
			constexpr uint64_t c_Limit = 64ull << 20;
			CHECK(pool.HasRoomFor(c_Limit - bandBytes, c_Limit));
			CHECK_FALSE(pool.HasRoomFor(c_Limit - bandBytes + 1, c_Limit));
			CHECK_FALSE(pool.HasRoomFor(1, bandBytes - 1)); // A limit below what is in flight already
			CHECK(pool.HasRoomFor(std::numeric_limits<uint64_t>::max() - bandBytes, std::numeric_limits<uint64_t>::max()));

			// Until a query covers released staging (FenceReleased once the command lists with its copies were executed, else
			// the next BeginFrame), those copies may not even have been submitted: whatever the GPU has done, a new one is
			// created, and the released one keeps its room.
			gpu.GetDevice().WaitForIdle();
			nvrhi::StagingTextureHandle second = pool.AcquireTexture(band);
			REQUIRE(second);
			nvrhi::IStagingTexture* secondObject = second.Get();
			CHECK(secondObject != firstObject);
			CHECK(pool.GetCreatedCount() == 2);
			CHECK_FALSE(pool.HasRoomFor(1, bandBytes));

			// The second one's copy, executed, then fenced: once the GPU has run what was submitted before the query, both are
			// reusable in the same frame (a budgeted upload that finds no room, or an acquisition, polls the query).
			nvrhi::TextureDesc destinationDesc = band;
			destinationDesc.initialState = nvrhi::ResourceStates::ShaderResource;
			destinationDesc.keepInitialState = true;
			destinationDesc.debugName = "StagingDestination";
			nvrhi::TextureHandle destination = gpu.GetNvrhiDevice()->createTexture(destinationDesc);
			REQUIRE(destination);
			nvrhi::CommandListHandle commandList = gpu.GetNvrhiDevice()->createCommandList();
			commandList->open();
			commandList->copyTexture(destination, nvrhi::TextureSlice(), second, nvrhi::TextureSlice());
			commandList->close();
			pool.Release(std::move(second));
			REQUIRE(gpu.ExecuteAndWait(commandList));
			pool.FenceReleased();
			CHECK(pool.GetPendingBytes() == 2 * bandBytes);
			CHECK(pool.HasRoomFor(2 * bandBytes, 2 * bandBytes));
			CHECK(pool.GetPendingBytes() == 0);
			CHECK(pool.GetIdleBytes() == 2 * bandBytes);
			pool.FenceReleased(); // Nothing released since: no query
			CHECK(pool.GetIdleBytes() == 2 * bandBytes);

			// The most recently released one comes back first; another shape gets its own.
			nvrhi::StagingTextureHandle reused = pool.AcquireTexture(band);
			CHECK(reused.Get() == secondObject);
			CHECK(pool.GetCreatedCount() == 2);
			nvrhi::TextureDesc other = band;
			other.height = 128;
			nvrhi::StagingTextureHandle otherShape = pool.AcquireTexture(other);
			REQUIRE(otherShape);
			CHECK(pool.GetCreatedCount() == 3);
			pool.Release(std::move(reused));
			pool.Release(std::move(otherShape));

			// Idle staging stays within its limit, the most recently released kept.
			frame += 10;
			pool.BeginFrame(frame);
			nvrhi::TextureDesc large = band;
			large.width = 1024;
			large.height = 1024; // 4 MiB
			std::vector<nvrhi::StagingTextureHandle> larges;
			for (int index = 0; index < 6; index++)
				larges.push_back(pool.AcquireTexture(large));
			for (nvrhi::StagingTextureHandle& staging : larges)
				pool.Release(std::move(staging));
			pool.BeginFrame(++frame);
			gpu.GetDevice().WaitForIdle();
			pool.BeginFrame(++frame);
			CHECK(pool.GetPendingBytes() == 0);
			CHECK(pool.GetIdleBytes() <= StagingPool::c_MaxIdleBytes);
			CHECK(pool.GetIdleBytes() >= StagingPool::c_MaxIdleBytes - (4ull << 20));

			// After a while without uploads, everything is returned.
			pool.BeginFrame(frame + StagingPool::c_IdleReleaseFrames);
			CHECK(pool.GetIdleBytes() == 0);

			// Without queries (no FenceReleased, no frames) nothing becomes reusable: beyond the pending limit, staging is left
			// to the GPU to free.
			nvrhi::TextureDesc huge = band;
			huge.width = 2048;
			huge.height = 4096; // 32 MiB
			const uint64_t hugeBytes = StagingPool::GetByteSize(huge);
			const uint64_t fitting = StagingPool::c_MaxPendingBytes / hugeBytes;
			for (uint64_t index = 0; index <= fitting; index++)
				pool.Release(pool.AcquireTexture(huge));
			CHECK(pool.GetPendingBytes() == fitting * hugeBytes);
		}
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Staging buffers come in powers of two and are reused like staging textures")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		CHECK(StagingPool::GetBufferByteSize(1) == StagingPool::c_MinBufferBytes);
		CHECK(StagingPool::GetBufferByteSize(StagingPool::c_MinBufferBytes + 1) == 2 * StagingPool::c_MinBufferBytes);
		CHECK(StagingPool::GetBufferByteSize(3ull << 20) == 4ull << 20);
		CHECK(StagingPool::GetBufferByteSize(c_AssetUploadStepBytes) == c_AssetUploadStepBytes);
		{
			StagingPool pool(gpu.GetNvrhiDevice());
			uint64_t frame = 100;
			pool.BeginFrame(frame);
			nvrhi::BufferHandle first = pool.AcquireBuffer(3ull << 20);
			REQUIRE(first);
			nvrhi::IBuffer* firstObject = first.Get();
			CHECK(first->getDesc().byteSize == 4ull << 20);
			CHECK(first->getDesc().cpuAccess == nvrhi::CpuAccessMode::Write);
			pool.Release(std::move(first));
			CHECK(pool.GetPendingBytes() == 4ull << 20);

			// Not covered by a query yet: another one. Once the GPU has run what was submitted before the next frame's query, a
			// request of the same rounded size gets one back, and staging textures never come from buffers (nor the other way
			// round).
			nvrhi::BufferHandle second = pool.AcquireBuffer(4ull << 20);
			REQUIRE(second);
			nvrhi::IBuffer* secondObject = second.Get();
			CHECK(secondObject != firstObject);
			pool.Release(std::move(second));
			pool.BeginFrame(++frame);
			gpu.GetDevice().WaitForIdle();
			nvrhi::TextureDesc band;
			band.width = 1024;
			band.height = 1024;
			band.format = nvrhi::Format::RGBA8_UNORM; // 4 MiB as well
			nvrhi::StagingTextureHandle texture = pool.AcquireTexture(band);
			REQUIRE(texture);
			CHECK(pool.GetIdleBytes() == 8ull << 20);
			const uint64_t created = pool.GetCreatedCount();
			nvrhi::BufferHandle reused = pool.AcquireBuffer((3ull << 20) + 1);
			CHECK(reused.Get() == secondObject);
			CHECK(pool.GetCreatedCount() == created);
			CHECK(pool.GetIdleBytes() == 4ull << 20);
			pool.Release(std::move(reused));
			pool.Release(std::move(texture));
		}
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("New staging made within a limit first releases idle staging, so the pool as a whole stays within the limit")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		{
			StagingPool pool(gpu.GetNvrhiDevice());
			constexpr uint64_t c_Step = 4ull << 20;
			nvrhi::TextureDesc band;
			band.width = 1024;
			band.height = 1024;
			band.format = nvrhi::Format::RGBA8_UNORM; // 4 MiB
			REQUIRE(StagingPool::GetByteSize(band) == c_Step);

			// Three idle staging textures (12 MiB) and one in flight (4 MiB).
			uint64_t frame = 100;
			pool.BeginFrame(frame);
			std::vector<nvrhi::StagingTextureHandle> bands;
			for (int index = 0; index < 3; index++)
				bands.push_back(pool.AcquireTexture(band));
			for (nvrhi::StagingTextureHandle& staging : bands)
				pool.Release(std::move(staging));
			pool.BeginFrame(++frame);
			gpu.GetDevice().WaitForIdle();
			pool.BeginFrame(++frame);
			pool.Release(pool.AcquireTexture(band)); // Reuses an idle one, then is in flight
			REQUIRE(pool.GetIdleBytes() == 2 * c_Step);
			REQUIRE(pool.GetPendingBytes() == c_Step);

			// A new shape within a limit of 12 MiB: the pool would hold 16 MiB with it, so an idle texture goes first.
			nvrhi::BufferHandle buffer = pool.AcquireBuffer(c_Step, 3 * c_Step);
			REQUIRE(buffer);
			CHECK(pool.GetIdleBytes() == c_Step);
			CHECK(pool.GetIdleBytes() + pool.GetPendingBytes() + buffer->getDesc().byteSize <= 3 * c_Step);
			pool.Release(std::move(buffer));

			// Without a limit idle staging stays; staging in flight is never released for room, whatever the limit.
			nvrhi::TextureDesc other = band;
			other.height = 512;
			nvrhi::StagingTextureHandle unlimited = pool.AcquireTexture(other);
			REQUIRE(unlimited);
			CHECK(pool.GetIdleBytes() == c_Step);
			nvrhi::StagingTextureHandle tight = pool.AcquireTexture(other, c_Step);
			REQUIRE(tight);
			CHECK(pool.GetIdleBytes() == 0);
			CHECK(pool.GetPendingBytes() == 2 * c_Step);
			pool.Release(std::move(unlimited));
			pool.Release(std::move(tight));
		}
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Textures upload in bands within each call's upload budget")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SettleDevice(gpu); // Staging of earlier tests is reusable again: budgeted uploads wait for staging room
		BindlessTextureTable& bindless = Renderer::GetBindlessTextures();
		const uint32_t slotsBefore = bindless.GetAllocatedCount();

		// 2048 x 1024 RGBA8: level 0 is 8 MiB, two bands of 512 rows; the rest of the chain (2.7 MB) is one more step.
		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA8;
		specification.DebugName = "BandedTexture";
		const Ref<Texture> source = Texture::Create(specification, CreatePatternChain(2048, 1024));
		REQUIRE(source);
		const uint64_t chainBytes = source->GetMemoryUsage().Cpu;
		// Through the asset pipeline's path: the texture takes the cooked bytes over.
		Ref<Texture> texture = Texture::Deserialize(source->Serialize());
		REQUIRE(texture);
		CHECK(texture->GetFinalizedMemoryUsage() == AssetMemoryUsage { 0, chainBytes, 0 });

		uint64_t uploaded = 0;
		CHECK(FinalizeOnce(gpu, *texture, 1, uploaded) == AssetFinalizeResult::Pending);
		CHECK(uploaded == Texture::c_UploadBandBytes);
		CHECK(texture->GetBindlessSlot() == BindlessTextureTable::c_InvalidSlot); // Not usable until it is complete
		CHECK(FinalizeOnce(gpu, *texture, 1, uploaded) == AssetFinalizeResult::Pending);
		CHECK(uploaded == Texture::c_UploadBandBytes);
		CHECK(FinalizeOnce(gpu, *texture, 1, uploaded) == AssetFinalizeResult::Done);
		CHECK(uploaded == chainBytes - 2 * Texture::c_UploadBandBytes);
		CHECK(texture->GetBindlessSlot() != BindlessTextureTable::c_InvalidSlot);
		CHECK(texture->GetMemoryUsage() == AssetMemoryUsage { 0, chainBytes, 0 });
		CHECK(texture->GetMipData(0).empty()); // The CPU copy is gone

		// Every band landed where it belongs: both sides of the band boundary, the last row, and the small levels.
		ReadbackImage level0;
		REQUIRE(Renderer::ReadTexture(texture->GetGPUTexture(), level0, 0));
		for (uint32_t y : { 0u, 511u, 512u, 1023u })
		{
			for (uint32_t x : { 0u, 1000u, 2047u })
			{
				CAPTURE(x);
				CAPTURE(y);
				CHECK(GetPixelRGBA8(level0, x, y) == GetPatternTexel(x, y, 0));
			}
		}
		for (uint32_t level : { 1u, 5u, 11u })
		{
			CAPTURE(level);
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(texture->GetGPUTexture(), image, level));
			CHECK(GetPixelRGBA8(image, image.Width - 1, image.Height - 1) == GetPatternTexel(image.Width - 1, image.Height - 1, level));
		}

		// Without a budget the whole chain goes in one call.
		Ref<Texture> unbudgeted = Texture::Create(specification, CreatePatternChain(2048, 1024));
		REQUIRE(unbudgeted);
		CHECK(FinalizeOnce(gpu, *unbudgeted, std::numeric_limits<uint64_t>::max(), uploaded) == AssetFinalizeResult::Done);
		CHECK(uploaded == chainBytes);
		ReadbackImage unbudgetedLevel0;
		REQUIRE(Renderer::ReadTexture(unbudgeted->GetGPUTexture(), unbudgetedLevel0, 0));
		CHECK(GetPixelRGBA8(unbudgetedLevel0, 2047, 1023) == GetPatternTexel(2047, 1023, 0));

		texture.reset();
		unbudgeted.reset();
		SettleDevice(gpu);
		CHECK(bindless.GetAllocatedCount() == slotsBefore);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Block-compressed textures upload in bands of whole block rows")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SettleDevice(gpu);
		BindlessTextureTable& bindless = Renderer::GetBindlessTextures();
		const uint32_t slotsBefore = bindless.GetAllocatedCount();

		// 2050 x 2050 BC7: level 0 is 513 x 513 blocks (4.2 MB), one band of 511 block rows and one of the last two, which end
		// inside their blocks; the rest of the chain (1.4 MB, sizes like 1025 and 3 that are not whole blocks) is one more
		// step through one staging texture.
		std::vector<TextureMip> mips;
		for (uint32_t level = 0; level < TextureUtils::CalculateMipCount(2050, 2050); level++)
		{
			TextureMip mip;
			mip.Width = std::max(1u, 2050u >> level);
			mip.Height = mip.Width;
			mip.Data.resize(static_cast<size_t>(GetMipByteSize(TextureFormat::BC7, mip.Width, mip.Height)));
			for (size_t index = 0; index < mip.Data.size(); index++)
				mip.Data[index] = static_cast<uint8_t>(index * 13 + index / 4096 + level * 29);
			mips.push_back(std::move(mip));
		}
		TextureSpecification specification;
		specification.Format = TextureFormat::BC7SRGB;
		specification.DebugName = "BandedBlocks";
		const Ref<Texture> source = Texture::Create(specification, mips);
		REQUIRE(source);
		const uint64_t chainBytes = source->GetMemoryUsage().Cpu;
		const uint64_t level0Bytes = GetMipByteSize(TextureFormat::BC7, 2050, 2050);
		const uint64_t firstBandBytes = 511ull * 513 * 16;
		Ref<Texture> texture = Texture::Deserialize(source->Serialize());
		REQUIRE(texture);
		// Residency counts the block-compressed chain.
		CHECK(texture->GetFinalizedMemoryUsage() == AssetMemoryUsage { 0, chainBytes, 0 });

		uint64_t uploaded = 0;
		CHECK(FinalizeOnce(gpu, *texture, 1, uploaded) == AssetFinalizeResult::Pending);
		CHECK(uploaded == firstBandBytes);
		CHECK(FinalizeOnce(gpu, *texture, 1, uploaded) == AssetFinalizeResult::Pending);
		CHECK(uploaded == level0Bytes - firstBandBytes);
		CHECK(FinalizeOnce(gpu, *texture, 1, uploaded) == AssetFinalizeResult::Done);
		CHECK(uploaded == chainBytes - level0Bytes);
		CHECK(texture->GetBindlessSlot() != BindlessTextureTable::c_InvalidSlot);
		CHECK(texture->GetMemoryUsage() == AssetMemoryUsage { 0, chainBytes, 0 });

		// Every block arrived where it belongs, in every level.
		for (uint32_t level = 0; level < source->GetMipCount(); level++)
		{
			CAPTURE(level);
			CHECK(ReadBlocks(gpu, texture->GetGPUTexture(), level) == mips[level].Data);
		}

		// Without a budget the whole chain goes in one call.
		Ref<Texture> unbudgeted = Texture::Create(specification, mips);
		REQUIRE(unbudgeted);
		CHECK(FinalizeOnce(gpu, *unbudgeted, std::numeric_limits<uint64_t>::max(), uploaded) == AssetFinalizeResult::Done);
		CHECK(uploaded == chainBytes);
		CHECK(ReadBlocks(gpu, unbudgeted->GetGPUTexture(), 0) == mips[0].Data);
		CHECK(ReadBlocks(gpu, unbudgeted->GetGPUTexture(), 10) == mips[10].Data);

		texture.reset();
		unbudgeted.reset();
		SettleDevice(gpu);
		CHECK(bindless.GetAllocatedCount() == slotsBefore);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A block-compressed tail goes up in one step only once its staging fits in a band")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SettleDevice(gpu); // Nothing pending: the pool's pending bytes below are this test's staging
		const StagingPool& pool = Renderer::GetStagingPool();

		// 2048 x 2048 BC1: the whole chain (2.8 MB) is less than a band, but stacked in one staging texture as wide as level
		// 0 it would take 1025 block rows of 4096 bytes (4.2 MB, 1.5 times its blocks and more than a band). Level 0 goes up
		// alone, then the rest stacks at half the width: 513 block rows of 2048 bytes for 0.7 MB of blocks.
		std::vector<TextureMip> mips;
		for (uint32_t level = 0; level < TextureUtils::CalculateMipCount(2048, 2048); level++)
		{
			TextureMip mip;
			mip.Width = 2048u >> level;
			mip.Height = mip.Width;
			mip.Data.resize(static_cast<size_t>(GetMipByteSize(TextureFormat::BC1, mip.Width, mip.Height)));
			for (size_t index = 0; index < mip.Data.size(); index++)
				mip.Data[index] = static_cast<uint8_t>(index * 7 + index / 8192 + level * 31);
			mips.push_back(std::move(mip));
		}
		TextureSpecification specification;
		specification.Format = TextureFormat::BC1SRGB;
		specification.DebugName = "TailStaging";
		Ref<Texture> texture = Texture::Create(specification, mips);
		REQUIRE(texture);
		const uint64_t chainBytes = texture->GetMemoryUsage().Cpu;
		const uint64_t level0Bytes = GetMipByteSize(TextureFormat::BC1, 2048, 2048);
		REQUIRE(chainBytes == 2796216);
		REQUIRE(chainBytes <= Texture::c_UploadBandBytes);

		const uint64_t acquiredBefore = pool.GetAcquiredBytes();
		uint64_t uploaded = 0;
		CHECK(FinalizeOnce(gpu, *texture, 1, uploaded) == AssetFinalizeResult::Pending);
		CHECK(uploaded == level0Bytes);
		const uint64_t level0Staging = pool.GetAcquiredBytes() - acquiredBefore;
		CHECK(level0Staging == level0Bytes);
		CHECK(FinalizeOnce(gpu, *texture, 1, uploaded) == AssetFinalizeResult::Done);
		CHECK(uploaded == chainBytes - level0Bytes);
		const uint64_t tailStaging = pool.GetAcquiredBytes() - acquiredBefore - level0Staging;
		CHECK(tailStaging == 513ull * 2048);
		// No step stages more than a band, and the chain as a whole about an eighth more than its blocks.
		CHECK(level0Staging <= Texture::c_UploadBandBytes);
		CHECK(tailStaging <= Texture::c_UploadBandBytes);
		CHECK(level0Staging + tailStaging == 3147776);

		for (uint32_t level = 0; level < texture->GetMipCount(); level++)
		{
			CAPTURE(level);
			CHECK(ReadBlocks(gpu, texture->GetGPUTexture(), level) == mips[level].Data);
		}
		texture.reset();
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A frame's first texture band goes up while staging is full; its later bands wait for room")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SettleDevice(gpu);
		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA8;
		specification.DebugName = "StagingWaitTexture";
		const Ref<Texture> source = Texture::Create(specification, CreatePatternChain(2048, 1024));
		REQUIRE(source);
		const std::vector<uint8_t> cooked = source->Serialize();
		const uint64_t chainBytes = source->GetMemoryUsage().Cpu;

		// Staging released in this frame and not covered by a query yet (its copies' command list is not executed): the
		// budget leaves no room beside it.
		StagingPool& pool = Renderer::GetStagingPool();
		nvrhi::TextureDesc band;
		band.width = 1024;
		band.height = 1024;
		band.format = nvrhi::Format::RGBA8_UNORM;
		pool.Release(pool.AcquireTexture(band));
		const uint64_t stagingBudget = pool.GetPendingBytes();
		REQUIRE(stagingBudget >= 4ull << 20);
		CHECK_FALSE(pool.HasRoomFor(1, stagingBudget));

		Ref<Texture> first = Texture::Deserialize(std::vector<uint8_t>(cooked));
		Ref<Texture> second = Texture::Deserialize(std::vector<uint8_t>(cooked));
		REQUIRE(first);
		REQUIRE(second);
		RendererUploadContext& upload = Renderer::GetUploadContext();
		upload.Begin();
		uint64_t frameUploaded = 0;
		AssetFinalizeContext context { &upload, nullptr };
		context.UploadBudget = 64ull << 20;
		context.StagingBytes = stagingBudget;
		context.UploadedBytes = &frameUploaded;
		// The frame's first step goes, so every frame makes progress; the next one waits for staging room.
		CHECK(first->FinalizeOnMainThread(context) == AssetFinalizeResult::Pending);
		CHECK(frameUploaded == Texture::c_UploadBandBytes);
		// A later call of the same frame takes no step at all.
		CHECK(second->FinalizeOnMainThread(context) == AssetFinalizeResult::Pending);
		CHECK(frameUploaded == Texture::c_UploadBandBytes);
		upload.Submit();
		gpu.GetDevice().WaitForIdle();

		// With room in a larger staging budget, both go on to the end.
		uint64_t uploaded = 0;
		CHECK(FinalizeOnce(gpu, *first, 64ull << 20, uploaded) == AssetFinalizeResult::Done);
		CHECK(uploaded == chainBytes - Texture::c_UploadBandBytes);
		CHECK(FinalizeOnce(gpu, *second, 64ull << 20, uploaded) == AssetFinalizeResult::Done);
		CHECK(uploaded == chainBytes);
		ReadbackImage level0;
		REQUIRE(Renderer::ReadTexture(first->GetGPUTexture(), level0, 0));
		CHECK(GetPixelRGBA8(level0, 2047, 1023) == GetPatternTexel(2047, 1023, 0));

		first.reset();
		second.reset();
		SettleDevice(gpu);
		CHECK(pool.GetPendingBytes() == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A frame's later texture bands leave a step of the staging budget to the next frame's first")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SettleDevice(gpu);
		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA8;
		specification.DebugName = "StagingRoomTexture";
		const Ref<Texture> source = Texture::Create(specification, CreatePatternChain(2048, 1024));
		REQUIRE(source);
		const uint64_t chainBytes = source->GetMemoryUsage().Cpu;
		Ref<Texture> texture = Texture::Deserialize(source->Serialize());
		REQUIRE(texture);

		// Level 0 is two bands of 4 MiB, the smaller levels one step of 2.7 MiB. Within 12 MiB of staging the steps after the
		// frame's first leave 4 MiB: both bands of level 0 go (8 MiB), the rest does not.
		StagingPool& pool = Renderer::GetStagingPool();
		constexpr uint64_t c_StagingBudget = 12ull << 20;
		RendererUploadContext& upload = Renderer::GetUploadContext();
		upload.Begin();
		uint64_t frameUploaded = 0;
		AssetFinalizeContext context { &upload, nullptr };
		context.UploadBudget = 64ull << 20;
		context.StagingBytes = c_StagingBudget;
		context.UploadedBytes = &frameUploaded;
		CHECK(context.GetStagingRoom() == c_StagingBudget - c_AssetUploadStepBytes);
		CHECK(texture->FinalizeOnMainThread(context) == AssetFinalizeResult::Pending);
		CHECK(frameUploaded == 2 * Texture::c_UploadBandBytes);
		CHECK(pool.GetPendingBytes() == 2 * Texture::c_UploadBandBytes);

		// The next frame's first step goes whatever is still in flight (here: nothing has run), and fits in the step left.
		uint64_t nextFrameUploaded = 0;
		context.UploadedBytes = &nextFrameUploaded;
		CHECK(texture->FinalizeOnMainThread(context) == AssetFinalizeResult::Done);
		CHECK(nextFrameUploaded == chainBytes - 2 * Texture::c_UploadBandBytes);
		CHECK(pool.GetPendingBytes() == chainBytes);
		CHECK(pool.GetPendingBytes() <= c_StagingBudget);
		upload.Submit();
		gpu.GetDevice().WaitForIdle();

		// Every band is in place.
		ReadbackImage level0;
		REQUIRE(Renderer::ReadTexture(texture->GetGPUTexture(), level0, 0));
		CHECK(GetPixelRGBA8(level0, 2047, 1023) == GetPatternTexel(2047, 1023, 0));
		ReadbackImage level1;
		REQUIRE(Renderer::ReadTexture(texture->GetGPUTexture(), level1, 1));
		CHECK(GetPixelRGBA8(level1, 1023, 511) == GetPatternTexel(1023, 511, 1));

		texture.reset();
		SettleDevice(gpu);
		CHECK(pool.GetPendingBytes() == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Freeing a large CPU copy waits for the next call when it would end after the deadline")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SettleDevice(gpu);
		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA8;
		specification.DebugName = "ReleasedTexture";
		// 512 x 512 RGBA8: the whole chain (1.3 MB) is one step, and the cooked bytes the texture holds are large enough to
		// take time to free.
		const Ref<Texture> source = Texture::Create(specification, CreatePatternChain(512, 512));
		REQUIRE(source);
		const uint64_t chainBytes = source->GetMemoryUsage().Cpu;
		Ref<Texture> texture = Texture::Deserialize(source->Serialize());
		REQUIRE(texture);
		REQUIRE(texture->GetMemoryUsage().Cpu >= 1ull << 20);

		RendererUploadContext& upload = Renderer::GetUploadContext();
		upload.Begin();
		uint64_t uploaded = 0;
		AssetFinalizeContext context { &upload, nullptr };
		context.UploadBudget = 64ull << 20;
		context.Deadline = std::chrono::steady_clock::now(); // Passed: only each call's first piece of work goes
		context.UploadedBytes = &uploaded;
		// The call's first step uploads the chain; freeing the CPU copy would end after the deadline, so it waits.
		CHECK(texture->FinalizeOnMainThread(context) == AssetFinalizeResult::Pending);
		CHECK(uploaded == chainBytes);
		CHECK(texture->GetMemoryUsage().Cpu > 0);
		CHECK(texture->GetBindlessSlot() == BindlessTextureTable::c_InvalidSlot);
		// It is the next call's first piece of work: that call frees it and finishes, uploading nothing.
		uploaded = 0;
		CHECK(texture->FinalizeOnMainThread(context) == AssetFinalizeResult::Done);
		CHECK(uploaded == 0);
		CHECK(texture->GetMemoryUsage() == AssetMemoryUsage { 0, chainBytes, 0 });
		CHECK(texture->GetBindlessSlot() != BindlessTextureTable::c_InvalidSlot);

		// A small CPU copy costs too little to wait: a 256 x 256 texture finishes in one call after the deadline as well.
		Ref<Texture> smallTexture = Texture::Deserialize(Texture::Create(specification, CreatePatternChain(256, 256))->Serialize());
		REQUIRE(smallTexture);
		CHECK(smallTexture->FinalizeOnMainThread(context) == AssetFinalizeResult::Done);
		CHECK(smallTexture->GetMemoryUsage().Cpu == 0);
		upload.Submit();
		gpu.GetDevice().WaitForIdle();

		ReadbackImage level0;
		REQUIRE(Renderer::ReadTexture(texture->GetGPUTexture(), level0, 0));
		CHECK(GetPixelRGBA8(level0, 511, 511) == GetPatternTexel(511, 511, 0));
		texture.reset();
		smallTexture.reset();
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Past the deadline only the frame's first finalization works: later calls neither allocate nor upload")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SettleDevice(gpu);
		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA8;
		specification.DebugName = "LateTexture";
		const Ref<Texture> source = Texture::Create(specification, CreatePatternChain(1024, 1024));
		REQUIRE(source);
		Ref<Texture> texture = Texture::Deserialize(source->Serialize());
		REQUIRE(texture);
		const Ref<Mesh> sourceMesh = MeshFactory::CreateCube();
		Ref<Mesh> mesh = Mesh::Deserialize(sourceMesh->Serialize());
		REQUIRE(mesh);

		RendererUploadContext& upload = Renderer::GetUploadContext();
		upload.Begin();
		uint64_t uploaded = 0;
		AssetFinalizeContext context { &upload, nullptr };
		context.UploadBudget = 64ull << 20;
		context.Deadline = std::chrono::steady_clock::now(); // Passed
		context.UploadedBytes = &uploaded;
		// Not the frame's first finalization: nothing happens, not even the GPU texture's or buffers' creation.
		context.FirstOfFrame = false;
		CHECK(texture->FinalizeOnMainThread(context) == AssetFinalizeResult::Pending);
		CHECK(texture->GetGPUTexture() == nullptr);
		CHECK(mesh->FinalizeOnMainThread(context) == AssetFinalizeResult::Pending);
		CHECK(mesh->GetIndexBuffer() == nullptr);
		CHECK(uploaded == 0);
		// The frame's first finalization takes its first step whatever the time.
		context.FirstOfFrame = true;
		CHECK(texture->FinalizeOnMainThread(context) == AssetFinalizeResult::Pending);
		CHECK(texture->GetGPUTexture() != nullptr);
		CHECK(uploaded == Texture::c_UploadBandBytes);
		CHECK(mesh->FinalizeOnMainThread(context) == AssetFinalizeResult::Done);
		CHECK(mesh->GetIndexBuffer() != nullptr);
		upload.Submit();
		gpu.GetDevice().WaitForIdle();

		texture.reset();
		mesh.reset();
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Meshes upload their geometry in steps within each call's upload budget")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());

		// 150,000 vertices: positions 1.8 MB, attributes 5.4 MB, indices 0.6 MB; 7.8 MB in two steps, each spanning streams.
		constexpr uint32_t c_Vertices = 150000;
		std::vector<glm::vec3> positions(c_Vertices);
		std::vector<MeshVertexAttributes> attributes(c_Vertices);
		std::vector<uint32_t> indices(c_Vertices);
		for (uint32_t vertex = 0; vertex < c_Vertices; vertex++)
		{
			positions[vertex] = glm::vec3(static_cast<float>(vertex), static_cast<float>(vertex % 7), 0.0f);
			attributes[vertex].TexCoord = glm::vec2(static_cast<float>(vertex), 0.5f);
			indices[vertex] = c_Vertices - 1 - vertex;
		}
		Submesh submesh;
		submesh.VertexCount = c_Vertices;
		submesh.LODs.push_back(MeshLOD { 0, c_Vertices });
		std::string error;
		const Ref<Mesh> mesh = Mesh::Create(positions, attributes, indices, { submesh }, &error);
		REQUIRE_MESSAGE(mesh, error);
		const uint64_t positionBytes = c_Vertices * sizeof(glm::vec3);
		const uint64_t attributeBytes = c_Vertices * sizeof(MeshVertexAttributes);
		const uint64_t indexBytes = c_Vertices * sizeof(uint32_t);
		CHECK(mesh->GetFinalizedMemoryUsage().GpuBuffers == positionBytes + attributeBytes + indexBytes);

		uint64_t uploaded = 0;
		uint64_t total = 0;
		uint32_t calls = 0;
		AssetFinalizeResult result = AssetFinalizeResult::Pending;
		while (result == AssetFinalizeResult::Pending && calls < 10)
		{
			result = FinalizeOnce(gpu, *mesh, 1, uploaded);
			CHECK(uploaded <= c_AssetUploadStepBytes);
			total += uploaded;
			calls++;
		}
		CHECK(result == AssetFinalizeResult::Done);
		CHECK(calls == 2);
		CHECK(total == positionBytes + attributeBytes + indexBytes);
		CHECK(mesh->GetMemoryUsage().GpuBuffers == positionBytes + attributeBytes + indexBytes);

		const std::vector<uint8_t> gpuPositions = ReadBuffer(gpu, mesh->GetPositionBuffer(), positionBytes);
		CHECK(std::memcmp(gpuPositions.data(), positions.data(), positionBytes) == 0);
		const std::vector<uint8_t> gpuAttributes = ReadBuffer(gpu, mesh->GetAttributeBuffer(), attributeBytes);
		CHECK(std::memcmp(gpuAttributes.data(), attributes.data(), attributeBytes) == 0);
		const std::vector<uint8_t> gpuIndices = ReadBuffer(gpu, mesh->GetIndexBuffer(), indexBytes);
		CHECK(std::memcmp(gpuIndices.data(), indices.data(), indexBytes) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("An uploaded mesh keeps only the positions and indices physics reads on the CPU")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());

		// 40,002 vertices (13,334 triangles): attributes 1.44 MB, positions 0.48 MB, indices 0.16 MB, one step.
		constexpr uint32_t c_Vertices = 40002;
		std::vector<glm::vec3> positions(c_Vertices);
		std::vector<MeshVertexAttributes> attributes(c_Vertices);
		std::vector<uint32_t> indices(c_Vertices);
		for (uint32_t vertex = 0; vertex < c_Vertices; vertex++)
		{
			positions[vertex] = glm::vec3(static_cast<float>(vertex), 0.0f, static_cast<float>(vertex % 11));
			attributes[vertex].TexCoord = glm::vec2(0.25f, static_cast<float>(vertex));
			indices[vertex] = vertex;
		}
		Submesh submesh;
		submesh.VertexCount = c_Vertices;
		submesh.LODs.push_back(MeshLOD { 0, c_Vertices });
		std::string error;
		const Ref<Mesh> mesh = Mesh::Create(positions, attributes, indices, { submesh }, &error);
		REQUIRE_MESSAGE(mesh, error);
		const uint64_t attributeBytes = c_Vertices * sizeof(MeshVertexAttributes);
		const AssetMemoryUsage finalized = mesh->GetFinalizedMemoryUsage();
		CHECK(finalized.Cpu == mesh->GetMemoryUsage().Cpu - attributeBytes);

		// The step that uploads everything ends past the deadline: the vertex attributes are freed by the next call, which
		// uploads nothing more.
		RendererUploadContext& upload = Renderer::GetUploadContext();
		upload.Begin();
		uint64_t uploaded = 0;
		AssetFinalizeContext context { &upload, nullptr };
		context.Deadline = std::chrono::steady_clock::now(); // Passed: only each call's first piece of work goes
		context.UploadedBytes = &uploaded;
		CHECK(mesh->FinalizeOnMainThread(context) == AssetFinalizeResult::Pending);
		CHECK(uploaded == finalized.GpuBuffers);
		CHECK(mesh->GetAttributes().size() == c_Vertices);
		uploaded = 0;
		CHECK(mesh->FinalizeOnMainThread(context) == AssetFinalizeResult::Done);
		CHECK(uploaded == 0);
		upload.Submit();
		gpu.GetDevice().WaitForIdle();

		// The GPU has the attributes; the CPU keeps positions and indices, and reports what it said it would.
		CHECK(mesh->GetAttributes().empty());
		CHECK(mesh->GetPositions() == positions);
		CHECK(mesh->GetIndices() == indices);
		CHECK(mesh->GetMemoryUsage() == finalized);
		const std::vector<uint8_t> gpuAttributes = ReadBuffer(gpu, mesh->GetAttributeBuffer(), attributeBytes);
		CHECK(std::memcmp(gpuAttributes.data(), attributes.data(), attributeBytes) == 0);
		CHECK(mesh->FinalizeOnMainThread(context) == AssetFinalizeResult::Done); // Finalized: nothing more to do
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Mesh steps go through pooled staging buffers and wait for staging room like texture bands")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SettleDevice(gpu);

		// 300,000 vertices: 15.6 MB of geometry, four steps.
		constexpr uint32_t c_Vertices = 300000;
		std::vector<glm::vec3> positions(c_Vertices);
		std::vector<MeshVertexAttributes> attributes(c_Vertices);
		std::vector<uint32_t> indices(c_Vertices);
		for (uint32_t vertex = 0; vertex < c_Vertices; vertex++)
		{
			positions[vertex] = glm::vec3(static_cast<float>(vertex % 1000), static_cast<float>(vertex / 1000), 1.0f);
			attributes[vertex].Normal = glm::vec3(0.0f, 0.0f, static_cast<float>(vertex % 3));
			indices[vertex] = (vertex * 7) % c_Vertices;
		}
		Submesh submesh;
		submesh.VertexCount = c_Vertices;
		submesh.LODs.push_back(MeshLOD { 0, c_Vertices });
		std::string error;
		const Ref<Mesh> first = Mesh::Create(positions, attributes, indices, { submesh }, &error);
		REQUIRE_MESSAGE(first, error);
		const Ref<Mesh> second = Mesh::Create(positions, attributes, indices, { submesh }, &error);
		REQUIRE_MESSAGE(second, error);
		const uint64_t positionBytes = c_Vertices * sizeof(glm::vec3);
		const uint64_t attributeBytes = c_Vertices * sizeof(MeshVertexAttributes);
		const uint64_t indexBytes = c_Vertices * sizeof(uint32_t);

		// Staging released in this frame and not covered by a query yet fills the staging budget, as in the texture test.
		StagingPool& pool = Renderer::GetStagingPool();
		pool.Release(pool.AcquireBuffer(c_AssetUploadStepBytes));
		const uint64_t stagingBudget = pool.GetPendingBytes();
		REQUIRE(stagingBudget >= c_AssetUploadStepBytes);
		CHECK_FALSE(pool.HasRoomFor(1, stagingBudget));

		RendererUploadContext& upload = Renderer::GetUploadContext();
		upload.Begin();
		uint64_t frameUploaded = 0;
		AssetFinalizeContext context { &upload, nullptr };
		context.UploadBudget = 64ull << 20;
		context.StagingBytes = stagingBudget;
		context.UploadedBytes = &frameUploaded;
		// The frame's first step goes through a staging buffer of the pool; the next one waits for staging room, and a later
		// call of the frame takes no step at all.
		CHECK(first->FinalizeOnMainThread(context) == AssetFinalizeResult::Pending);
		CHECK(frameUploaded == c_AssetUploadStepBytes);
		CHECK(pool.GetPendingBytes() == stagingBudget + c_AssetUploadStepBytes);
		CHECK(second->FinalizeOnMainThread(context) == AssetFinalizeResult::Pending);
		CHECK(frameUploaded == c_AssetUploadStepBytes);
		upload.Submit();
		gpu.GetDevice().WaitForIdle();

		// Without a staging budget both finish, and every byte arrived where it belongs.
		uint64_t uploaded = 0;
		CHECK(FinalizeOnce(gpu, *first, std::numeric_limits<uint64_t>::max(), uploaded) == AssetFinalizeResult::Done);
		CHECK(uploaded == positionBytes + attributeBytes + indexBytes - c_AssetUploadStepBytes);
		CHECK(FinalizeOnce(gpu, *second, std::numeric_limits<uint64_t>::max(), uploaded) == AssetFinalizeResult::Done);
		CHECK(uploaded == positionBytes + attributeBytes + indexBytes);
		for (const Ref<Mesh>& mesh : { first, second })
		{
			const std::vector<uint8_t> gpuPositions = ReadBuffer(gpu, mesh->GetPositionBuffer(), positionBytes);
			CHECK(std::memcmp(gpuPositions.data(), positions.data(), positionBytes) == 0);
			const std::vector<uint8_t> gpuAttributes = ReadBuffer(gpu, mesh->GetAttributeBuffer(), attributeBytes);
			CHECK(std::memcmp(gpuAttributes.data(), attributes.data(), attributeBytes) == 0);
			const std::vector<uint8_t> gpuIndices = ReadBuffer(gpu, mesh->GetIndexBuffer(), indexBytes);
			CHECK(std::memcmp(gpuIndices.data(), indices.data(), indexBytes) == 0);
		}
		SettleDevice(gpu);
		CHECK(pool.GetPendingBytes() == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
