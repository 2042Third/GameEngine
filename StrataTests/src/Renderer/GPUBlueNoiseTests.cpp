#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Strata/Renderer/BlueNoise.h"
#include "Strata/Renderer/Renderer.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// What the sun shadows' shaders read from the blue-noise tile (Include/BlueNoise.glsl's BlueNoise, through
// Debug/BlueNoise.comp): the tile's byte k at the shifted pixel as (k + 0.5) / 256, so its values lie evenly in (0, 1).

TEST_SUITE("GPU.Renderer.BlueNoise")
{
	TEST_CASE("Shaders read the blue-noise tile as (k + 0.5) / 256, evenly in (0, 1) and shifted on the torus")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		constexpr uint32_t size = RenderData::c_BlueNoiseSize;
		constexpr uint32_t count = size * size;

		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Compute;
		layoutDesc.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(33), nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
			nvrhi::BindingLayoutItem::PushConstants(0, 4 * sizeof(int32_t)) };
		const nvrhi::BindingLayoutHandle layout = device->createBindingLayout(layoutDesc);
		REQUIRE(layout);
		nvrhi::ComputePipelineDesc pipelineDesc;
		pipelineDesc.CS = Renderer::GetShaderLibrary().Get("Debug/BlueNoise.comp");
		pipelineDesc.bindingLayouts = { layout };
		const nvrhi::ComputePipelineHandle pipeline = device->createComputePipeline(pipelineDesc);
		REQUIRE(pipeline);
		const nvrhi::BufferHandle output = device->createBuffer(nvrhi::BufferDesc().setByteSize(count * sizeof(float)).setStructStride(sizeof(float))
			.setCanHaveUAVs(true).setInitialState(nvrhi::ResourceStates::UnorderedAccess).setKeepInitialState(true).setDebugName("BlueNoiseValues"));
		REQUIRE(output);
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings = { nvrhi::BindingSetItem::Texture_SRV(33, Renderer::GetBlueNoiseTexture()), nvrhi::BindingSetItem::StructuredBuffer_UAV(0, output),
			nvrhi::BindingSetItem::PushConstants(0, 4 * sizeof(int32_t)) };
		const nvrhi::BindingSetHandle set = device->createBindingSet(setDesc, layout);
		REQUIRE(set);

		// No shift (the disk's turn) and the contact shadows' (32, 17).
		for (const glm::ivec2 shift : { glm::ivec2(0), glm::ivec2(32, 17) })
		{
			CAPTURE(shift.x);
			CAPTURE(shift.y);
			nvrhi::CommandListHandle commandList = device->createCommandList();
			commandList->open();
			nvrhi::ComputeState state;
			state.pipeline = pipeline;
			state.bindings = { set };
			commandList->setComputeState(state);
			const std::array<int32_t, 4> parameters = { shift.x, shift.y, 0, 0 };
			commandList->setPushConstants(parameters.data(), sizeof(parameters));
			commandList->dispatch(size / 8, size / 8);
			commandList->close();
			REQUIRE(gpu.ExecuteAndWait(commandList));
			const std::vector<uint8_t> bytes = ReadBufferRange(gpu, output, 0, count * sizeof(float));
			REQUIRE(bytes.size() == count * sizeof(float));
			std::vector<float> values(count);
			std::memcpy(values.data(), bytes.data(), bytes.size());

			double sum = 0.0;
			float least = 1.0f;
			float largest = 0.0f;
			uint32_t mismatched = 0;
			for (uint32_t y = 0; y < size; y++)
			{
				for (uint32_t x = 0; x < size; x++)
				{
					const float value = values[y * size + x];
					const uint32_t byte = RenderData::c_BlueNoise[((y + static_cast<uint32_t>(shift.y)) % size) * size + (x + static_cast<uint32_t>(shift.x)) % size];
					const float expected = (static_cast<float>(byte) + 0.5f) / 256.0f;
					mismatched += std::abs(value - expected) <= 1e-5f ? 0 : 1;
					sum += value;
					least = std::min(least, value);
					largest = std::max(largest, value);
				}
			}
			CAPTURE(least);
			CAPTURE(largest);
			CHECK(mismatched == 0);
			CHECK(least > 0.0f);
			CHECK(largest < 1.0f);
			CHECK(std::abs(sum / count - 0.5) <= 1e-5);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
