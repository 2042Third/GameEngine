#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/StagingTexturePool.h"
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
		nvrhi::CommandListHandle commandList = gpu.GetNvrhiDevice()->createCommandList();
		commandList->open();
		AssetFinalizeContext context { commandList, nullptr };
		context.UploadBudget = uploadBudget;
		outUploaded = 0;
		context.UploadedBytes = &outUploaded;
		const AssetFinalizeResult result = asset.FinalizeOnMainThread(context);
		commandList->close();
		REQUIRE(gpu.ExecuteAndWait(commandList));
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
	TEST_CASE("Staging textures are reused once frames in flight are done, and released when uploads stop")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const uint32_t framesInFlight = gpu.GetDevice().GetMaxFramesInFlight();
		{
			StagingTexturePool pool(gpu.GetNvrhiDevice(), framesInFlight);
			nvrhi::TextureDesc band;
			band.width = 256;
			band.height = 256;
			band.format = nvrhi::Format::RGBA8_UNORM;
			const uint64_t bandBytes = 256 * 256 * 4;
			CHECK(StagingTexturePool::GetByteSize(band) == bandBytes);

			uint64_t frame = 100;
			pool.BeginFrame(frame);
			nvrhi::StagingTextureHandle first = pool.Acquire(band);
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

			// Frames that may still copy from it are in flight: a new one is created.
			pool.BeginFrame(frame + framesInFlight);
			nvrhi::StagingTextureHandle second = pool.Acquire(band);
			REQUIRE(second);
			CHECK(second.Get() != firstObject);
			CHECK(pool.GetCreatedCount() == 2);
			pool.Release(std::move(second));

			// Once they are done, it is handed out again; another shape gets its own.
			pool.BeginFrame(frame + framesInFlight + 1);
			CHECK(pool.GetIdleBytes() == bandBytes);
			nvrhi::StagingTextureHandle reused = pool.Acquire(band);
			CHECK(reused.Get() == firstObject);
			CHECK(pool.GetCreatedCount() == 2);
			nvrhi::TextureDesc other = band;
			other.height = 128;
			nvrhi::StagingTextureHandle otherShape = pool.Acquire(other);
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
				larges.push_back(pool.Acquire(large));
			for (nvrhi::StagingTextureHandle& staging : larges)
				pool.Release(std::move(staging));
			pool.BeginFrame(frame + framesInFlight + 1);
			CHECK(pool.GetPendingBytes() == 0);
			CHECK(pool.GetIdleBytes() <= StagingTexturePool::c_MaxIdleBytes);
			CHECK(pool.GetIdleBytes() >= StagingTexturePool::c_MaxIdleBytes - (4ull << 20));

			// After a while without uploads, everything is returned.
			pool.BeginFrame(frame + StagingTexturePool::c_IdleReleaseFrames);
			CHECK(pool.GetIdleBytes() == 0);

			// Without frames nothing becomes reusable: beyond the pending limit, staging is left to the GPU to free.
			nvrhi::TextureDesc huge = band;
			huge.width = 2048;
			huge.height = 4096; // 32 MiB
			const uint64_t hugeBytes = StagingTexturePool::GetByteSize(huge);
			const uint64_t fitting = StagingTexturePool::c_MaxPendingBytes / hugeBytes;
			for (uint64_t index = 0; index <= fitting; index++)
				pool.Release(pool.Acquire(huge));
			CHECK(pool.GetPendingBytes() == fitting * hugeBytes);
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

		// Staging of this frame that no device frame has recycled yet (as in tools and tests, which run none): the
		// budget leaves no room beside it.
		StagingTexturePool& pool = Renderer::GetStagingTextures();
		nvrhi::TextureDesc band;
		band.width = 1024;
		band.height = 1024;
		band.format = nvrhi::Format::RGBA8_UNORM;
		pool.Release(pool.Acquire(band));
		const uint64_t stagingBudget = pool.GetPendingBytes();
		REQUIRE(stagingBudget >= 4ull << 20);
		CHECK_FALSE(pool.HasRoomFor(1, stagingBudget));

		Ref<Texture> first = Texture::Deserialize(std::vector<uint8_t>(cooked));
		Ref<Texture> second = Texture::Deserialize(std::vector<uint8_t>(cooked));
		REQUIRE(first);
		REQUIRE(second);
		nvrhi::CommandListHandle commandList = gpu.GetNvrhiDevice()->createCommandList();
		commandList->open();
		uint64_t frameUploaded = 0;
		AssetFinalizeContext context { commandList, nullptr };
		context.UploadBudget = 64ull << 20;
		context.StagingBytes = stagingBudget;
		context.UploadedBytes = &frameUploaded;
		// The frame's first step goes, so every frame makes progress; the next one waits for staging room.
		CHECK(first->FinalizeOnMainThread(context) == AssetFinalizeResult::Pending);
		CHECK(frameUploaded == Texture::c_UploadBandBytes);
		// A later call of the same frame takes no step at all.
		CHECK(second->FinalizeOnMainThread(context) == AssetFinalizeResult::Pending);
		CHECK(frameUploaded == Texture::c_UploadBandBytes);
		commandList->close();
		REQUIRE(gpu.ExecuteAndWait(commandList));

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

		nvrhi::CommandListHandle commandList = gpu.GetNvrhiDevice()->createCommandList();
		commandList->open();
		uint64_t uploaded = 0;
		AssetFinalizeContext context { commandList, nullptr };
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
		commandList->close();
		REQUIRE(gpu.ExecuteAndWait(commandList));

		ReadbackImage level0;
		REQUIRE(Renderer::ReadTexture(texture->GetGPUTexture(), level0, 0));
		CHECK(GetPixelRGBA8(level0, 511, 511) == GetPatternTexel(511, 511, 0));
		texture.reset();
		smallTexture.reset();
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
}
