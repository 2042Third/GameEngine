#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Renderer/TextureReadback.h"

#include <chrono>
#include <cstring>
#include <thread>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// RGBA8 texture whose texel (x, y) of mip `mip` is (x, y, mip, slice) with alpha 255.
	nvrhi::TextureHandle CreatePatternTexture(nvrhi::IDevice* device, uint32_t width, uint32_t height, uint32_t mips, uint32_t slices)
	{
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.mipLevels = mips;
		desc.arraySize = slices;
		desc.dimension = slices > 1 ? nvrhi::TextureDimension::Texture2DArray : nvrhi::TextureDimension::Texture2D;
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.debugName = "ReadbackPattern";
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		nvrhi::TextureHandle texture = device->createTexture(desc);
		REQUIRE(texture);

		nvrhi::CommandListHandle commandList = device->createCommandList();
		commandList->open();
		for (uint32_t slice = 0; slice < slices; slice++)
		{
			for (uint32_t mip = 0; mip < mips; mip++)
			{
				const uint32_t mipWidth = std::max(width >> mip, 1u);
				const uint32_t mipHeight = std::max(height >> mip, 1u);
				std::vector<uint8_t> pixels;
				for (uint32_t y = 0; y < mipHeight; y++)
				{
					for (uint32_t x = 0; x < mipWidth; x++)
						AppendPixel(pixels, static_cast<uint8_t>(x), static_cast<uint8_t>(y), static_cast<uint8_t>(mip), static_cast<uint8_t>(slice));
				}
				commandList->writeTexture(texture, slice, mip, pixels.data(), static_cast<size_t>(mipWidth) * 4);
			}
		}
		commandList->close();
		device->executeCommandList(commandList);
		return texture;
	}

	// Polls like a frame loop would, without blocking on the GPU.
	bool PollUntilReady(const TextureReadback& readback)
	{
		for (int attempt = 0; attempt < 5000; attempt++)
		{
			if (readback.IsReady())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

}

TEST_SUITE("GPU.TextureReadback")
{
	TEST_CASE("Regions, mips and array slices are read back without blocking")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::TextureHandle texture = CreatePatternTexture(gpu.GetNvrhiDevice(), 16, 8, 2, 3);

		std::string error;
		TextureReadbackRegion region;
		region.X = 3;
		region.Y = 2;
		region.Width = 4;
		region.Height = 5;
		region.ArraySlice = 2;
		Scope<TextureReadback> readback = TextureReadback::Create(texture, region, &error);
		REQUIRE_MESSAGE(readback, error);
		CHECK(readback->GetWidth() == 4);
		CHECK(readback->GetHeight() == 5);
		CHECK(readback->GetFormat() == nvrhi::Format::RGBA8_UNORM);
		REQUIRE(PollUntilReady(*readback));
		ReadbackImage image;
		REQUIRE(readback->GetResult(image));
		REQUIRE(image.Width == 4);
		REQUIRE(image.Height == 5);
		CHECK(image.BytesPerPixel == 4);
		CHECK(GetPixelRGBA8(image, 0, 0) == glm::u8vec4(3, 2, 0, 2));
		CHECK(GetPixelRGBA8(image, 3, 4) == glm::u8vec4(6, 6, 0, 2));

		// The whole of mip 1: an empty region extends to the edges.
		region = {};
		region.MipLevel = 1;
		readback = TextureReadback::Create(texture, region, &error);
		REQUIRE_MESSAGE(readback, error);
		readback->Wait();
		REQUIRE(readback->GetResult(image));
		CHECK(image.Width == 8);
		CHECK(image.Height == 4);
		CHECK(GetPixelRGBA8(image, 7, 3) == glm::u8vec4(7, 3, 1, 0));

		// A region from an origin to the edge.
		region = {};
		region.X = 14;
		region.Y = 7;
		readback = TextureReadback::Create(texture, region, &error);
		REQUIRE_MESSAGE(readback, error);
		readback->Wait();
		REQUIRE(readback->GetResult(image));
		CHECK(image.Width == 2);
		CHECK(image.Height == 1);
		CHECK(GetPixelRGBA8(image, 1, 0) == glm::u8vec4(15, 7, 0, 0));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Invalid requests are refused and pending readbacks can be dropped")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::TextureHandle texture = CreatePatternTexture(gpu.GetNvrhiDevice(), 8, 8, 1, 1);

		auto refused = [&](const TextureReadbackRegion& region)
		{
			std::string error;
			const bool created = TextureReadback::Create(texture, region, &error) != nullptr;
			CHECK_FALSE(error.empty());
			return !created;
		};
		CHECK(refused({ 8, 0, 0, 0, 0, 0 }));       // Origin outside
		CHECK(refused({ 0, 0, 9, 1, 0, 0 }));       // Wider than the texture
		CHECK(refused({ 4, 4, 5, 1, 0, 0 }));       // Reaches past the edge
		CHECK(refused({ 1, 0, 0xFFFFFFFFu, 1, 0, 0 })); // Would overflow X + Width
		CHECK(refused({ 0, 0, 0, 0, 1, 0 }));       // No such mip
		CHECK(refused({ 0, 0, 0, 0, 0, 1 }));       // No such slice
		std::string error;
		CHECK_FALSE(TextureReadback::Create(nullptr, {}, &error));
		CHECK_FALSE(error.empty());

		nvrhi::TextureDesc compressedDesc;
		compressedDesc.width = 8;
		compressedDesc.height = 8;
		compressedDesc.format = nvrhi::Format::BC1_UNORM;
		compressedDesc.debugName = "Compressed";
		compressedDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		compressedDesc.keepInitialState = true;
		if (gpu.GetDevice().GetInfo().SupportsBCCompression)
		{
			nvrhi::TextureHandle compressed = gpu.GetNvrhiDevice()->createTexture(compressedDesc);
			REQUIRE(compressed);
			CHECK_FALSE(TextureReadback::Create(compressed, {}, &error));
		}

		// Multisampled images and combined depth-stencil formats have no plain rows of pixels to copy.
		nvrhi::TextureDesc multisampledDesc;
		multisampledDesc.width = 8;
		multisampledDesc.height = 8;
		multisampledDesc.format = nvrhi::Format::RGBA8_UNORM;
		multisampledDesc.sampleCount = 4;
		multisampledDesc.dimension = nvrhi::TextureDimension::Texture2DMS;
		multisampledDesc.isRenderTarget = true;
		multisampledDesc.debugName = "Multisampled";
		multisampledDesc.initialState = nvrhi::ResourceStates::RenderTarget;
		multisampledDesc.keepInitialState = true;
		nvrhi::TextureHandle multisampled = gpu.GetNvrhiDevice()->createTexture(multisampledDesc);
		REQUIRE(multisampled);
		error.clear();
		CHECK_FALSE(TextureReadback::Create(multisampled, {}, &error));
		CHECK(error.find("multisampled") != std::string::npos);
		nvrhi::TextureDesc depthStencilDesc;
		depthStencilDesc.width = 8;
		depthStencilDesc.height = 8;
		depthStencilDesc.format = nvrhi::Format::D32S8;
		depthStencilDesc.isRenderTarget = true;
		depthStencilDesc.debugName = "DepthStencil";
		depthStencilDesc.initialState = nvrhi::ResourceStates::DepthWrite;
		depthStencilDesc.keepInitialState = true;
		nvrhi::TextureHandle depthStencil = gpu.GetNvrhiDevice()->createTexture(depthStencilDesc);
		REQUIRE(depthStencil);
		error.clear();
		CHECK_FALSE(TextureReadback::Create(depthStencil, {}, &error));
		CHECK(error.find("cannot be read back") != std::string::npos);

		// Dropped before the GPU finished: the copy still completes safely.
		for (int index = 0; index < 8; index++)
		{
			Scope<TextureReadback> dropped = TextureReadback::Create(texture);
			REQUIRE(dropped);
		}
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The entity under a pixel is read without waiting for the GPU")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		Entity cube = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f), "Cube");
		SceneRenderer renderer;
		renderer.SetViewportSize(c_SceneTestSize, c_SceneTestSize);
		CHECK_FALSE(renderer.ReadEntityIDAsync(1, 1)); // Nothing rendered yet

		REQUIRE(renderer.Render(scene, LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f))));
		CHECK_FALSE(renderer.ReadEntityIDAsync(c_SceneTestSize, 0)); // Outside the viewport

		auto pick = [&](uint32_t x, uint32_t y)
		{
			Scope<TextureReadback> readback = renderer.ReadEntityIDAsync(x, y);
			REQUIRE(readback);
			REQUIRE(PollUntilReady(*readback));
			ReadbackImage image;
			REQUIRE(readback->GetResult(image));
			REQUIRE(image.Pixels.size() == sizeof(uint32_t));
			uint32_t id = 0;
			std::memcpy(&id, image.Pixels.data(), sizeof(id));
			return id;
		};
		const uint32_t cubeID = pick(c_SceneTestSize / 2, c_SceneTestSize / 2);
		CHECK(SceneRenderer::GetEntityFromID(scene, cubeID) == cube);
		CHECK(renderer.GetEntityAt(scene, c_SceneTestSize / 2, c_SceneTestSize / 2) == cube);
		const uint32_t emptyID = pick(1, 1);
		CHECK(emptyID == 0);
		CHECK_FALSE(SceneRenderer::GetEntityFromID(scene, emptyID));

		// A result that arrives after the entity was destroyed resolves to nothing, even when its slot is reused.
		scene.DestroyEntity(cube);
		CHECK_FALSE(SceneRenderer::GetEntityFromID(scene, cubeID));
		scene.CreateEntity("Reuse");
		CHECK_FALSE(SceneRenderer::GetEntityFromID(scene, cubeID));
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
