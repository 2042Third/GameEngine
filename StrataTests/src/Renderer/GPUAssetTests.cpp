#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Strata/Asset/AssetPack.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/Texture.h"
#include "TestHelpers.h"

#include <glm/gtc/packing.hpp>

#include <map>
#include <vector>

using namespace Strata;

namespace
{

	// 4x2 RGBA8 texture whose texels all differ.
	Ref<Texture> CreatePatternTexture(TextureFormat format, std::vector<uint8_t>& outLevel0)
	{
		TextureMip level0;
		level0.Width = 4;
		level0.Height = 2;
		for (uint32_t texel = 0; texel < 8; texel++)
			level0.Data.insert(level0.Data.end(), { static_cast<uint8_t>(texel * 30), static_cast<uint8_t>(255 - texel * 20), static_cast<uint8_t>(texel * 7), 255 });
		outLevel0 = level0.Data;

		std::vector<TextureMip> mips = { level0 };
		REQUIRE(TextureUtils::GenerateMips(mips, format, false));
		TextureSpecification specification;
		specification.Format = format;
		specification.DebugName = "PatternTexture";
		return Texture::Create(specification, std::move(mips));
	}

	bool Upload(GraphicsDevice& device, Asset& asset)
	{
		nvrhi::CommandListHandle commandList = device.GetDevice()->createCommandList();
		commandList->open();
		const bool finalized = asset.FinalizeOnMainThread(AssetFinalizeContext { commandList, nullptr }) == AssetFinalizeResult::Done;
		commandList->close();
		device.GetDevice()->executeCommandList(commandList);
		return finalized;
	}

}

TEST_SUITE("GPU.Assets")
{
	TEST_CASE("Textures upload their mip chain and own a bindless slot")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		BindlessTextureTable& bindless = Renderer::GetBindlessTextures();
		const uint32_t allocatedBefore = bindless.GetAllocatedCount();

		for (TextureFormat format : { TextureFormat::RGBA8, TextureFormat::RGBA8SRGB })
		{
			std::vector<uint8_t> level0;
			Ref<Texture> texture = CreatePatternTexture(format, level0);
			REQUIRE(texture);
			REQUIRE(Upload(gpu.GetDevice(), *texture));

			REQUIRE(texture->GetGPUTexture());
			CHECK(texture->GetGPUTexture()->getDesc().mipLevels == 3);
			CHECK(texture->GetBindlessSlot() >= BindlessTextureTable::c_ReservedSlots);
			CHECK(texture->GetBindlessSlot() != BindlessTextureTable::c_InvalidSlot);
			CHECK(bindless.GetAllocatedCount() == allocatedBefore + 1);
			CHECK(texture->GetMips().empty()); // CPU copy released after upload
			CHECK(texture->GetWidth() == 4);
			// The mip chain (4x2, 2x1, 1x1) on the GPU, nothing left on the CPU.
			CHECK(texture->GetMemoryUsage().GpuTextures == (4 * 2 + 2 * 1 + 1 * 1) * 4);
			CHECK(texture->GetMemoryUsage().Cpu == 0);
			CHECK(texture->Serialize().empty()); // CPU copy released

			// Finalizing twice is harmless.
			CHECK(Upload(gpu.GetDevice(), *texture));
			CHECK(bindless.GetAllocatedCount() == allocatedBefore + 1);

			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(texture->GetGPUTexture(), image));
			CHECK(image.Pixels == level0);

			texture.reset();
			CHECK(bindless.GetAllocatedCount() == allocatedBefore);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("HDR textures upload as half floats")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());

		TextureMip level0;
		level0.Width = 2;
		level0.Height = 2;
		level0.Data.resize(2 * 2 * 8);
		auto* halves = reinterpret_cast<uint16_t*>(level0.Data.data());
		for (int index = 0; index < 16; index++)
			halves[index] = glm::packHalf1x16(static_cast<float>(index) * 0.5f);
		const std::vector<uint8_t> expected = level0.Data;

		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA16F;
		Ref<Texture> texture = Texture::Create(specification, { level0 });
		REQUIRE(texture);
		REQUIRE(Upload(gpu.GetDevice(), *texture));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(texture->GetGPUTexture(), image));
		CHECK(image.Format == nvrhi::Format::RGBA16_FLOAT);
		CHECK(image.Pixels == expected);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Meshes upload vertex and index buffers")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Ref<Mesh> mesh = MeshFactory::CreateTorus();
		REQUIRE(mesh);
		const AssetMemoryUsage cpuOnly = mesh->GetMemoryUsage();
		CHECK(cpuOnly.GetGpu() == 0);
		REQUIRE(Upload(gpu.GetDevice(), *mesh));

		REQUIRE(mesh->GetPositionBuffer());
		REQUIRE(mesh->GetAttributeBuffer());
		REQUIRE(mesh->GetIndexBuffer());
		CHECK(mesh->GetPositionBuffer()->getDesc().byteSize == mesh->GetPositions().size() * sizeof(glm::vec3));
		CHECK(mesh->GetAttributeBuffer()->getDesc().byteSize == mesh->GetAttributes().size() * sizeof(MeshVertexAttributes));
		CHECK(mesh->GetIndexBuffer()->getDesc().byteSize == mesh->GetIndices().size() * sizeof(uint32_t));
		CHECK(mesh->GetIndexBuffer()->getDesc().isIndexBuffer);
		// Each copy counts once, in its own pool: the CPU geometry stays as it was, the buffers are added on the GPU.
		const AssetMemoryUsage uploaded = mesh->GetMemoryUsage();
		CHECK(uploaded.Cpu == cpuOnly.Cpu);
		CHECK(uploaded.GpuBuffers == mesh->GetPositionBuffer()->getDesc().byteSize + mesh->GetAttributeBuffer()->getDesc().byteSize
			+ mesh->GetIndexBuffer()->getDesc().byteSize);
		CHECK(uploaded.GpuTextures == 0);
		CHECK_FALSE(mesh->GetPositions().empty()); // CPU geometry stays for physics and picking
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Asset managers upload loaded assets")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());

		std::vector<uint8_t> level0;
		Ref<Texture> source = CreatePatternTexture(TextureFormat::RGBA8SRGB, level0);
		REQUIRE(source);
		std::map<uint64_t, std::vector<uint8_t>> data = {
			{ 0x9000, source->Serialize() },
			{ 0x9001, MeshFactory::CreateCone()->Serialize() } };
		AssetMetadata texture;
		texture.Handle = UUID(0x9000);
		texture.Type = AssetType::Texture;
		texture.Path = "Pattern.png";
		AssetMetadata mesh;
		mesh.Handle = UUID(0x9001);
		mesh.Type = AssetType::Mesh;
		mesh.Path = "Cone.mesh";

		const std::filesystem::path path = Tests::CreateTemporaryDirectory("GPUAssetPack") / "Game.stpak";
		REQUIRE(AssetPack::Write(path, { texture, mesh }, [&data](const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string*)
		{
			outData = data.at(static_cast<uint64_t>(metadata.Handle));
			return true;
		}));

		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(path);
		REQUIRE(manager);

		// Built-in assets are uploaded as soon as they are registered.
		Ref<Mesh> cube = std::static_pointer_cast<Mesh>(manager->GetAsset(BuiltinAssets::CubeMesh));
		REQUIRE(cube);
		CHECK(cube->GetIndexBuffer() != nullptr);

		Ref<Asset> loadedTexture = manager->LoadAssetSync(UUID(0x9000));
		REQUIRE(loadedTexture);
		Ref<Texture> gpuTexture = std::static_pointer_cast<Texture>(loadedTexture);
		REQUIRE(gpuTexture->GetGPUTexture());
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(gpuTexture->GetGPUTexture(), image));
		CHECK(image.Pixels == level0);

		Ref<Asset> loadedMesh = manager->LoadAssetSync(UUID(0x9001));
		REQUIRE(loadedMesh);
		CHECK(std::static_pointer_cast<Mesh>(loadedMesh)->GetIndexBuffer() != nullptr);

		// A tiny upload budget spreads finalization over several updates.
		manager->UnloadAsset(UUID(0x9000));
		manager->UnloadAsset(UUID(0x9001));
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.UploadBytesPerFrame = 1;
		manager->SetResidencyBudgets(budgets);
		manager->RequestLoad(UUID(0x9000));
		manager->RequestLoad(UUID(0x9001));
		manager->Update();
		const uint32_t readyAfterFirstUpdate = (manager->GetAssetState(UUID(0x9000)) == AssetState::Ready ? 1u : 0u)
			+ (manager->GetAssetState(UUID(0x9001)) == AssetState::Ready ? 1u : 0u);
		CHECK(readyAfterFirstUpdate == 1);
		manager->Update();
		CHECK(manager->GetAssetState(UUID(0x9000)) == AssetState::Ready);
		CHECK(manager->GetAssetState(UUID(0x9001)) == AssetState::Ready);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
