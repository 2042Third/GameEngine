#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"

#include "Strata/Renderer/DeferredReleaseQueue.h"

#include <chrono>
#include <cstddef>
#include <vector>

using namespace Strata;

namespace
{

	nvrhi::TextureHandle CreateTarget(nvrhi::IDevice* device, const char* name)
	{
		nvrhi::TextureDesc desc;
		desc.width = 64;
		desc.height = 64;
		desc.format = nvrhi::Format::R32_UINT;
		desc.isRenderTarget = true;
		desc.debugName = name;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		nvrhi::TextureHandle texture = device->createTexture(desc);
		REQUIRE(texture);
		return texture;
	}

	// Clears the texture in a command list that is executed without waiting for it. NVRHI's Vulkan backend does not keep
	// cleared textures alive for the GPU, so dropping the last reference before the clear completes would destroy the
	// image in use (a validation error).
	void SubmitClear(nvrhi::IDevice* device, nvrhi::ITexture* texture)
	{
		nvrhi::CommandListHandle commandList = device->createCommandList();
		REQUIRE(commandList);
		commandList->open();
		commandList->clearTextureUInt(texture, nvrhi::AllSubresources, 7);
		commandList->close();
		device->executeCommandList(commandList);
	}

	// The references held on a resource (NVRHI's AddRef and Release return the count they leave).
	unsigned long GetReferenceCount(nvrhi::IResource* resource)
	{
		resource->AddRef();
		return resource->Release();
	}

}

TEST_SUITE("GPU.Renderer.DeferredRelease")
{
	TEST_CASE("Released resources stay alive until the GPU has finished the work submitted before")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		DeferredReleaseQueue queue(device);

		nvrhi::TextureHandle texture = CreateTarget(device, "DeferredRelease.Cleared");
		SubmitClear(device, texture);
		queue.Release({ texture.Get() });
		texture = nullptr;
		// The queue holds the only reference: a release is collected by later calls only, once the GPU is done.
		CHECK(queue.GetPendingCount() == 1);

		gpu.GetDevice().WaitForIdle();
		queue.Collect();
		CHECK(queue.GetPendingCount() == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Releases are collected in order, also without frames")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		DeferredReleaseQueue queue(device);

		// Nothing to hold.
		queue.Release({});
		queue.Release({ nullptr });
		CHECK(queue.GetPendingCount() == 0);

		nvrhi::TextureHandle first = CreateTarget(device, "DeferredRelease.First");
		nvrhi::TextureHandle second = CreateTarget(device, "DeferredRelease.Second");
		SubmitClear(device, first);
		SubmitClear(device, second);
		queue.Release({ first.Get(), second.Get() });
		first = nullptr;
		second = nullptr;
		CHECK(queue.GetPendingCount() == 2);

		// A release drops as many references to resources that completed before it as it takes over, the oldest first:
		// without frames, the queue does not grow beyond what the GPU may still use and what earlier releases left.
		gpu.GetDevice().WaitForIdle();
		nvrhi::TextureHandle third = CreateTarget(device, "DeferredRelease.Third");
		SubmitClear(device, third);
		queue.Release({ third.Get() });
		third = nullptr;
		CHECK(queue.GetPendingCount() == 2);
		CHECK(queue.GetCompletedCount() == 1);

		// Later rounds reuse the event queries of the batches that completed.
		for (int round = 0; round < 3; round++)
		{
			gpu.GetDevice().WaitForIdle();
			nvrhi::TextureHandle texture = CreateTarget(device, "DeferredRelease.Round");
			SubmitClear(device, texture);
			queue.Release({ texture.Get() });
			CHECK(queue.GetPendingCount() == 2);
			CHECK(queue.GetCompletedCount() == 1);
		}
		gpu.GetDevice().WaitForIdle();
		queue.Collect();
		CHECK(queue.GetPendingCount() == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A collection within a budget destroys the completed resources over several calls, the oldest first")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		DeferredReleaseQueue queue(device);

		// Eight textures released at once (a cache letting go of its thumbnails), done on the GPU. The test keeps references
		// of its own to see which ones the queue let go of.
		std::vector<nvrhi::TextureHandle> textures;
		for (int index = 0; index < 8; index++)
			textures.push_back(CreateTarget(device, "DeferredRelease.Budget"));
		queue.Release(std::vector<nvrhi::ResourceHandle>(textures.begin(), textures.end()));
		CHECK(queue.GetPendingCount() == 8);
		CHECK(queue.GetCompletedCount() == 0);
		gpu.GetDevice().WaitForIdle();

		// A budget no destruction fits in still drops one reference per call, so the queue drains; the rest waits for later
		// calls.
		queue.Collect(std::chrono::nanoseconds(0));
		CHECK(queue.GetPendingCount() == 7);
		CHECK(queue.GetCompletedCount() == 7);
		CHECK(GetReferenceCount(textures[0]) == 1);
		CHECK(GetReferenceCount(textures[1]) == 2);
		queue.Collect(std::chrono::nanoseconds(0));
		CHECK(queue.GetPendingCount() == 6);
		CHECK(GetReferenceCount(textures[1]) == 1);
		CHECK(GetReferenceCount(textures[2]) == 2);
		// Without a budget, everything completed goes.
		queue.Collect();
		CHECK(queue.GetPendingCount() == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A frame destroys completed releases within its budget and leaves the rest to the frames after")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		DeferredReleaseQueue& releases = Renderer::GetDeferredReleases();
		gpu.GetDevice().WaitForIdle();
		releases.Collect();
		REQUIRE(releases.GetPendingCount() == 0);

		// Hundreds of textures released at once, as when a cache lets go of its thumbnails, all done on the GPU. Destroying
		// one costs the driver microseconds to a tenth of a millisecond, so one frame's budget cannot hold them all: a frame
		// that destroyed every completed resource held the editor for milliseconds.
		constexpr size_t c_Count = 512;
		std::vector<nvrhi::TextureHandle> textures;
		for (size_t index = 0; index < c_Count; index++)
			textures.push_back(CreateTarget(device, "DeferredRelease.FrameBudget"));
		Renderer::ReleaseDeferred(std::vector<nvrhi::ResourceHandle>(textures.begin(), textures.end()));
		textures.clear(); // The queue holds the only references
		CHECK(releases.GetPendingCount() == c_Count);
		gpu.GetDevice().WaitForIdle();

		const auto frame = [&gpu]()
		{
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			gpu.GetDevice().EndFrame();
		};
		frame();
		CHECK(releases.GetPendingCount() < c_Count);
		CHECK(releases.GetCompletedCount() > 0);
		CHECK(releases.GetCompletedCount() == releases.GetPendingCount());
		// Every frame destroys at least one, so the frames after drain the rest.
		for (size_t round = 0; round < c_Count && releases.GetPendingCount() > 0; round++)
			frame();
		CHECK(releases.GetPendingCount() == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The renderer drops deferred releases at the frame after the GPU finished them")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		DeferredReleaseQueue& releases = Renderer::GetDeferredReleases();
		// Without what earlier tests left, which a frame's budget might not cover.
		gpu.GetDevice().WaitForIdle();
		releases.Collect();
		REQUIRE(releases.GetPendingCount() == 0);

		nvrhi::TextureHandle texture = CreateTarget(device, "DeferredRelease.Renderer");
		SubmitClear(device, texture);
		Renderer::ReleaseDeferred({ texture.Get() });
		texture = nullptr;
		CHECK(releases.GetPendingCount() == 1);

		gpu.GetDevice().WaitForIdle();
		REQUIRE(gpu.GetDevice().BeginFrame());
		Renderer::BeginFrame();
		gpu.GetDevice().EndFrame();
		CHECK(releases.GetPendingCount() == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
