#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"

#include "Strata/Renderer/DeferredReleaseQueue.h"

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

		// A release collects what completed before it: without frames, the queue holds only what the GPU may still use.
		gpu.GetDevice().WaitForIdle();
		nvrhi::TextureHandle third = CreateTarget(device, "DeferredRelease.Third");
		SubmitClear(device, third);
		queue.Release({ third.Get() });
		third = nullptr;
		CHECK(queue.GetPendingCount() == 1);

		// Later rounds reuse the event queries of the batches that completed.
		for (int round = 0; round < 3; round++)
		{
			gpu.GetDevice().WaitForIdle();
			nvrhi::TextureHandle texture = CreateTarget(device, "DeferredRelease.Round");
			SubmitClear(device, texture);
			queue.Release({ texture.Get() });
			CHECK(queue.GetPendingCount() == 1);
		}
		gpu.GetDevice().WaitForIdle();
		queue.Collect();
		CHECK(queue.GetPendingCount() == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The renderer drops deferred releases at the frame after the GPU finished them")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		DeferredReleaseQueue& releases = Renderer::GetDeferredReleases();

		nvrhi::TextureHandle texture = CreateTarget(device, "DeferredRelease.Renderer");
		SubmitClear(device, texture);
		Renderer::ReleaseDeferred({ texture.Get() });
		texture = nullptr;
		CHECK(releases.GetPendingCount() >= 1);

		gpu.GetDevice().WaitForIdle();
		REQUIRE(gpu.GetDevice().BeginFrame());
		Renderer::BeginFrame();
		gpu.GetDevice().EndFrame();
		CHECK(releases.GetPendingCount() == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
