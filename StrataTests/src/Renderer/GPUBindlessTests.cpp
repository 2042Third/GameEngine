#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Strata/Renderer/BindlessTextureTable.h"

#include <vector>

using namespace Strata;

namespace
{

	nvrhi::TextureHandle CreateTestTexture(nvrhi::IDevice* device)
	{
		nvrhi::TextureDesc desc;
		desc.width = 1;
		desc.height = 1;
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.debugName = "BindlessTestTexture";
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		return device->createTexture(desc);
	}

}

TEST_SUITE("GPU.Renderer")
{
	TEST_CASE("Bindless texture slots are recycled only after frames in flight")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		nvrhi::TextureHandle texture = CreateTestTexture(device);
		REQUIRE(texture);

		constexpr uint32_t framesInFlight = 2;
		BindlessTextureTable table(device, 64, framesInFlight);
		CHECK(table.GetLayout() != nullptr);
		CHECK(table.GetTable() != nullptr);

		const uint32_t first = table.Allocate(texture);
		const uint32_t second = table.Allocate(texture);
		CHECK(first >= BindlessTextureTable::c_ReservedSlots);
		CHECK(second != first);
		CHECK(table.GetAllocatedCount() == 2);

		table.BeginFrame(10);
		table.Release(first);
		table.Release(first); // Double release is ignored
		CHECK(table.GetAllocatedCount() == 1);

		// Until frame 10 + framesInFlight + 1 the slot may still be sampled and is not handed out again.
		table.BeginFrame(12);
		const uint32_t third = table.Allocate(texture);
		CHECK(third != first);

		table.BeginFrame(13);
		CHECK(table.Allocate(texture) == first);
		CHECK(table.GetAllocatedCount() == 3);

		table.Release(BindlessTextureTable::c_WhiteSlot); // Reserved slots are never released
		table.Release(BindlessTextureTable::c_InvalidSlot);
		CHECK(table.GetAllocatedCount() == 3);
		CHECK(table.Allocate(nullptr) == BindlessTextureTable::c_InvalidSlot);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Bindless texture table grows up to its capacity")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		nvrhi::TextureHandle texture = CreateTestTexture(device);
		REQUIRE(texture);

		BindlessTextureTable large(device, 4096, 2);
		const uint32_t initialCapacity = large.GetCapacity();
		for (uint32_t index = 0; index < initialCapacity + 10; index++)
			REQUIRE(large.Allocate(texture) != BindlessTextureTable::c_InvalidSlot);
		CHECK(large.GetCapacity() > initialCapacity);
		CHECK(large.GetCapacity() <= 4096);

		BindlessTextureTable small(device, 8, 2);
		for (uint32_t index = BindlessTextureTable::c_ReservedSlots; index < 8; index++)
			CHECK(small.Allocate(texture) == index);
		CHECK(small.Allocate(texture) == BindlessTextureTable::c_InvalidSlot);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Bindless slots change generation when released, so holders of slot indices can tell they no longer name their texture")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		nvrhi::TextureHandle texture = CreateTestTexture(device);
		REQUIRE(texture);
		BindlessTextureTable table(device, 64, 2);

		const std::vector<uint32_t> slots = { table.Allocate(texture), table.Allocate(texture), BindlessTextureTable::c_WhiteSlot };
		std::vector<uint32_t> generations;
		table.GetSlotGenerations(slots, generations);
		REQUIRE(generations.size() == slots.size());
		CHECK(table.AreSlotsUnchanged(slots, generations));

		// Released: no longer the texture it was, also once the slot is handed out again.
		table.BeginFrame(1);
		table.Release(slots[1]);
		CHECK_FALSE(table.AreSlotsUnchanged(slots, generations));
		table.BeginFrame(10);
		CHECK(table.Allocate(texture) == slots[1]);
		CHECK_FALSE(table.AreSlotsUnchanged(slots, generations));
		const std::vector<uint32_t> first = { slots[0], slots[2] };
		const std::vector<uint32_t> firstGenerations = { generations[0], generations[2] };
		CHECK(table.AreSlotsUnchanged(first, firstGenerations));

		// Slots the table does not have, and mismatched lists, are never unchanged.
		const std::vector<uint32_t> beyond = { 1000 };
		const std::vector<uint32_t> zero = { 0 };
		CHECK_FALSE(table.AreSlotsUnchanged(beyond, zero));
		CHECK_FALSE(table.AreSlotsUnchanged(first, zero));
		CHECK(table.AreSlotsUnchanged({}, {}));
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
