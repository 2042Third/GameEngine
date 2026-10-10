#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

using namespace Strata;

namespace
{

	// The number of references to a resource, ours included.
	unsigned long GetReferenceCount(nvrhi::IResource* resource)
	{
		resource->AddRef();
		return resource->Release();
	}

}

TEST_SUITE("GPU.TestUtils")
{
	TEST_CASE("ExecuteAndWait recycles the command buffer")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();

		nvrhi::BufferDesc bufferDesc;
		bufferDesc.byteSize = 16;
		bufferDesc.debugName = "ExecuteAndWaitBuffer";
		bufferDesc.initialState = nvrhi::ResourceStates::CopyDest;
		bufferDesc.keepInitialState = true;
		nvrhi::BufferHandle buffer = device->createBuffer(bufferDesc);
		REQUIRE(buffer);
		REQUIRE(GetReferenceCount(buffer) == 1);

		// A submitted command buffer keeps the resources it used alive until NVRHI recycles it (runGarbageCollection once
		// the GPU finished it), which also returns it to the queue's pool for the next open(). After ExecuteAndWait the
		// command buffer is recycled, so our handle is the only reference to the written buffer left; without recycling,
		// every submission would add one.
		nvrhi::CommandListHandle commandList = device->createCommandList();
		REQUIRE(commandList);
		const uint32_t values[4] = { 1, 2, 3, 4 };
		for (int submission = 0; submission < 3; submission++)
		{
			CAPTURE(submission);
			commandList->open();
			commandList->writeBuffer(buffer, values, sizeof(values));
			commandList->close();
			REQUIRE(gpu.ExecuteAndWait(commandList));
			CHECK(GetReferenceCount(buffer) == 1);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
