#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Renderer/UploadQueueTestUtils.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Asset/BC7Encoder.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Renderer/CookedTexture.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Renderer/TextureStreamer.h"
#include "Strata/Scene/Scene.h"
#include "TestHelpers.h"

#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// Mip streaming end to end: a 2048x2048 BC7 texture in an asset pack, drawn on a quad by a SceneRenderer, loaded partly
// (its resident tail) and streamed to the level its size on screen needs by the asset manager's TextureStreamer.
namespace
{

	constexpr uint32_t c_TextureSize = 2048;
	constexpr uint32_t c_TextureMips = 12;
	constexpr uint32_t c_ViewSize = 256;
	constexpr AssetHandle c_TextureHandle = AssetHandle(0xA1000);
	constexpr AssetHandle c_MaterialHandle = AssetHandle(0xA1001);

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

	// A smooth gradient: the color of a texel at (u, v) in [0, 1], the same function at every level, so each level is the
	// texture seen from further away and the levels next to each other differ little.
	std::array<uint8_t, 4> GetGradient(float u, float v)
	{
		return { static_cast<uint8_t>(40.0f + 150.0f * u), static_cast<uint8_t>(40.0f + 150.0f * v), static_cast<uint8_t>(60.0f + 60.0f * (u + v)), 255 };
	}

	// The texture's cooked form: every level of the gradient, one color per 4x4 block, in BC7 (solid blocks are exact).
	std::vector<uint8_t> CookGradientTexture()
	{
		std::map<uint32_t, std::array<uint8_t, 16>> encoded; // Per color
		std::vector<std::vector<uint8_t>> levels(c_TextureMips);
		std::vector<std::span<const uint8_t>> spans;
		for (uint32_t level = 0; level < c_TextureMips; level++)
		{
			const uint32_t size = std::max(1u, c_TextureSize >> level);
			const uint32_t blocks = (size + 3) / 4;
			std::vector<uint8_t>& data = levels[level];
			data.resize(static_cast<size_t>(blocks) * blocks * 16);
			for (uint32_t y = 0; y < blocks; y++)
			{
				for (uint32_t x = 0; x < blocks; x++)
				{
					const std::array<uint8_t, 4> color = GetGradient((static_cast<float>(x) + 0.5f) / static_cast<float>(blocks), (static_cast<float>(y) + 0.5f) / static_cast<float>(blocks));
					const uint32_t key = static_cast<uint32_t>(color[0]) | (static_cast<uint32_t>(color[1]) << 8) | (static_cast<uint32_t>(color[2]) << 16);
					auto it = encoded.find(key);
					if (it == encoded.end())
					{
						std::array<uint8_t, 64> texels {};
						for (uint32_t texel = 0; texel < 16; texel++)
							std::memcpy(texels.data() + texel * 4, color.data(), 4);
						std::array<uint8_t, 16> block {};
						BC7Encoder::EncodeBlock(texels.data(), block.data());
						it = encoded.emplace(key, block).first;
					}
					std::memcpy(data.data() + (static_cast<size_t>(y) * blocks + x) * 16, it->second.data(), 16);
				}
			}
			spans.push_back(data);
		}
		CookedTextureDescription description;
		description.Format = TextureFormat::BC7SRGB;
		description.Width = c_TextureSize;
		description.Height = c_TextureSize;
		description.MipCount = c_TextureMips;
		description.DebugName = "StreamedGradient";
		std::vector<uint8_t> cooked = WriteCookedTexture(description, spans);
		REQUIRE_FALSE(cooked.empty());
		return cooked;
	}

	// A pack with the texture and an unlit material showing it.
	std::filesystem::path WriteStreamingPack()
	{
		MaterialProperties properties = UnlitColor(glm::vec4(1.0f));
		properties.BaseColorMap = c_TextureHandle;
		const std::string material = JsonUtils::Dump(Material::Create(properties)->Serialize());

		std::unordered_map<AssetHandle, std::vector<uint8_t>> data;
		data[c_TextureHandle] = CookGradientTexture();
		data[c_MaterialHandle] = std::vector<uint8_t>(material.begin(), material.end());
		std::vector<AssetMetadata> metadata(2);
		metadata[0].Handle = c_TextureHandle;
		metadata[0].Type = AssetType::Texture;
		metadata[0].Path = "Textures/Gradient.png";
		metadata[0].Name = "Gradient";
		metadata[1].Handle = c_MaterialHandle;
		metadata[1].Type = AssetType::Material;
		metadata[1].Path = "Materials/Gradient.stmat";
		metadata[1].Name = "GradientMaterial";

		const std::filesystem::path pack = CreateTemporaryDirectory("TextureStreaming") / "Streaming.stpak";
		std::string error;
		REQUIRE_MESSAGE(AssetPackWriter::Write(pack, metadata, [&data](const AssetMetadata& asset, std::vector<uint8_t>& outData, std::string*)
		{
			outData = data.at(asset.Handle);
			return true;
		}, &error), error);
		return pack;
	}

	// A scene with the quad (1x1, facing +Z, at the origin) and neutral post-processing.
	Scope<Scene> CreateQuadScene()
	{
		Scope<Scene> scene = CreateScope<Scene>("Streaming");
		AddNeutralPostProcess(*scene);
		AddMesh(*scene, BuiltinAssets::QuadMesh, c_MaterialHandle, glm::vec3(0.0f), "Quad");
		return scene;
	}

	// The camera looking at the quad from `distance` on its axis: the quad's bounding sphere (radius 0.707) is
	// 0.707 * 256 / (distance * tan(30 degrees)) pixels tall.
	SceneCamera CameraAt(float distance)
	{
		return LookAt(glm::vec3(0.0f, 0.0f, distance), glm::vec3(0.0f));
	}

	Ref<Texture> PeekTexture(AssetManagerBase& manager)
	{
		return std::dynamic_pointer_cast<Texture>(manager.PeekLoadedAsset(c_TextureHandle));
	}

	// One application frame: the device's frame, the manager's update (which runs its streamer), the renderer's frame.
	struct StreamingFrame
	{
		GPUContext& Gpu;
		Ref<RuntimeAssetManager> Manager;
		SceneRenderer& Renderer;
		Scene& QuadScene;

		ReadbackImage Run(const SceneCamera& camera)
		{
			AssetManager::SetActive(Manager);
			ReadbackImage image;
			REQUIRE(Gpu.GetDevice().BeginFrame());
			Strata::Renderer::BeginFrame();
			Manager->Update();
			image = RenderToImage(Renderer, QuadScene, camera);
			Gpu.GetDevice().EndFrame();
			return image;
		}
	};

	// The quad's center shows the texture, not the white of a texture still loading.
	bool ShowsTexture(const ReadbackImage& image)
	{
		const glm::u8vec4 center = GetPixelRGBA8(image, c_ViewSize / 2, c_ViewSize / 2);
		return center.r < 230 && center.g < 230;
	}

}

TEST_SUITE("GPU.TextureStreaming")
{
	TEST_CASE("A texture streams from its tail to the level the screen needs, to level 0 filling it, and back to its tail on a tight budget")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path pack = WriteStreamingPack();
		const float c_SmallDistance = 5.6f; // 56 pixels: level 5 (64x64) is what a 2048x2048 texture needs there
		const float c_FillDistance = 0.15f; // Beyond the view's 256 pixels: 2.2 times the texture's 2048 texels

		for (const bool transfer : GetQueueModes(gpu))
		{
			CAPTURE(transfer);
			SettleDevice(gpu);
			const ScopedTransferQueue queueMode(transfer);
			REQUIRE(queueMode.IsEnabled() == transfer);

			// The reference: every level loaded and drawn by a renderer without streaming, from its own manager.
			ReadbackImage reference;
			{
				std::string error;
				Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
				REQUIRE_MESSAGE(manager, error);
				manager->SetPartialLoads(false);
				SceneRendererSpecification specification;
				specification.TextureStreaming = false;
				SceneRenderer renderer(specification);
				renderer.SetViewportSize(c_ViewSize, c_ViewSize);
				Scope<Scene> scene = CreateQuadScene();
				StreamingFrame frame { gpu, manager, renderer, *scene };
				for (int attempt = 0; attempt < 100; attempt++)
				{
					reference = frame.Run(CameraAt(c_SmallDistance));
					const Ref<Texture> texture = PeekTexture(*manager);
					if (texture && texture->GetResidentLevel() == 0 && renderer.GetStats().PendingAssets == 0)
						break;
				}
				REQUIRE(renderer.GetStats().PendingAssets == 0);
				REQUIRE(PeekTexture(*manager)->GetResidentLevel() == 0);
				CHECK(manager->GetStats().PartialLoads == 0);
				AssetManager::SetActive(nullptr);
			}

			std::string error;
			Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
			REQUIRE_MESSAGE(manager, error);
			REQUIRE(TextureStreamer::Find(*manager));
			SceneRenderer renderer;
			CHECK(renderer.GetSpecification().TextureStreaming);
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			Scope<Scene> scene = CreateQuadScene();
			StreamingFrame frame { gpu, manager, renderer, *scene };
			const auto streamingDone = [&manager]()
			{
				const AssetManagerStats stats = manager->GetStats();
				return stats.Detail.Reads == 0 && stats.Detail.Changes == 0;
			};

			// Drawn at 56 pixels: the first load holds the resident tail (32x32 and below, level 6), and the streamer raises
			// it to level 5, not further. From its arrival on the quad never shows the loading fallback.
			ReadbackImage image;
			std::optional<uint32_t> arrivedLevel;
			bool fallbackAfterArrival = false;
			for (int attempt = 0; attempt < 100; attempt++)
			{
				image = frame.Run(CameraAt(c_SmallDistance));
				const Ref<Texture> texture = PeekTexture(*manager);
				if (!texture || texture->GetBindlessSlot() == BindlessTextureTable::c_InvalidSlot)
					continue;
				if (!arrivedLevel)
					arrivedLevel = texture->GetResidentLevel();
				else
					fallbackAfterArrival = fallbackAfterArrival || !ShowsTexture(image);
				if (texture->GetResidentLevel() == 5 && streamingDone() && attempt > 10)
					break;
			}
			REQUIRE(arrivedLevel);
			CHECK(*arrivedLevel == 6);
			CHECK(PeekTexture(*manager)->GetTailLevel() == 6);
			CHECK(PeekTexture(*manager)->GetResidentLevel() == 5);
			CHECK_FALSE(fallbackAfterArrival);
			CHECK(manager->GetStats().PartialLoads == 1);
			CHECK(manager->GetStats().Detail.LevelsRaised == 1);
			CHECK(manager->GetStats().Resident.GpuTextures == PeekTexture(*manager)->GetLevelBytes(5, c_TextureMips));
			AssetDetailLevels detail;
			REQUIRE(PeekTexture(*manager)->GetDetailLevels(detail));
			CHECK(detail.Resident == 5);
			CHECK(detail.Wanted == 5);
			CHECK(detail.Count == c_TextureMips);
			// Its pixels are those of the fully resident texture.
			CHECK(MaxDifference(image, reference) <= 2);

			// Filling the screen streams it to level 0 within 30 frames.
			uint32_t frames = 0;
			for (; frames < 30 && PeekTexture(*manager)->GetResidentLevel() > 0; frames++)
			{
				image = frame.Run(CameraAt(c_FillDistance));
				CHECK(ShowsTexture(image));
			}
			CHECK(PeekTexture(*manager)->GetResidentLevel() == 0);
			CHECK(frames <= 30);
			CHECK(manager->GetStats().Resident.GpuTextures == PeekTexture(*manager)->GetLevelBytes(0, c_TextureMips));

			// A budget of 1 MiB of GPU textures drops it back to its tail at once, without the fallback, and it stays there
			// while the budget cannot hold what the view needs.
			AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
			budgets.GpuTextures = 1ull << 20;
			manager->SetResidencyBudgets(budgets);
			for (uint32_t step = 0; step < 10; step++)
			{
				image = frame.Run(CameraAt(c_FillDistance));
				CHECK(ShowsTexture(image));
				CHECK(manager->GetAssetState(c_TextureHandle) == AssetState::Ready);
			}
			CHECK(PeekTexture(*manager)->GetResidentLevel() == 6);
			CHECK(manager->GetStats().Resident.GpuTextures <= budgets.GpuTextures);
			CHECK(manager->GetStats().Detail.LevelsDropped >= 6);

			AssetManager::SetActive(nullptr);
			scene.reset();
			manager.reset();
			SettleDevice(gpu);
			CHECK(gpu.GetNewErrorCount() == 0);
		}
	}
}
