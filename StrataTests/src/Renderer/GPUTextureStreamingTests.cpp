#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Renderer/TextureStreamingTestUtils.h"
#include "Renderer/UploadQueueTestUtils.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Renderer/TextureStreamer.h"
#include "Strata/Scene/Scene.h"
#include "TestHelpers.h"

#include <optional>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

// Mip streaming end to end: a 2048x2048 BC7 texture in an asset pack, drawn on a quad by a SceneRenderer, loaded partly
// (its resident tail) and streamed to the level its size on screen needs by the asset manager's TextureStreamer.
namespace
{

	constexpr uint32_t c_TextureMips = c_GradientTextureMips;
	constexpr uint32_t c_ViewSize = 256;
	constexpr AssetHandle c_TextureHandle = c_GradientTextureHandle;
	constexpr AssetHandle c_MaterialHandle = c_GradientMaterialHandle;

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

	// A scene with the quad (1x1, facing +Z, at the origin) and neutral post-processing.
	Scope<Scene> CreateQuadScene()
	{
		Scope<Scene> scene = CreateScope<Scene>("Streaming");
		AddNeutralPostProcess(*scene);
		AddMesh(*scene, BuiltinAssets::QuadMesh, c_MaterialHandle, glm::vec3(0.0f), "Quad");
		return scene;
	}

	// The camera looking at the quad from `distance` on its axis: the quad (one texture across a unit) is
	// 256 / (2 * distance * tan(30 degrees)) pixels tall.
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
		const std::filesystem::path pack = WriteGradientStreamingPack();
		// The quad spans 40 pixels: level 5 (64x64) is what a 2048x2048 texture needs there (51 texels a pixel).
		const float c_SmallDistance = 5.6f;
		const float c_FillDistance = 0.15f; // Beyond the view's 256 pixels: 1478 pixels for the texture's 2048 texels

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

			// Drawn at 40 pixels: the first load holds the resident tail (32x32 and below, level 6), and the streamer raises
			// it to level 5, not further. From its arrival on the quad never shows the loading fallback. A loading screen's
			// group load counts the texture as loaded as soon as its tail is resident (its finer levels stream as views need
			// them).
			std::optional<AssetGroupRequest> group = manager->RequestLoadWithDependencies(c_TextureHandle);
			REQUIRE(group->IsValid());
			ReadbackImage image;
			std::optional<uint32_t> arrivedLevel;
			bool groupCompleteOnArrival = false;
			bool fallbackAfterArrival = false;
			for (int attempt = 0; attempt < 100; attempt++)
			{
				image = frame.Run(CameraAt(c_SmallDistance));
				const Ref<Texture> texture = PeekTexture(*manager);
				if (!texture || texture->GetBindlessSlot() == BindlessTextureTable::c_InvalidSlot)
					continue;
				if (!arrivedLevel)
				{
					arrivedLevel = texture->GetResidentLevel();
					groupCompleteOnArrival = group->IsComplete() && group->Progress() == 1.0f && !group->HasFailed();
					group.reset(); // Its pin goes: the rest of the test streams the texture as views alone ask for it
				}
				else
				{
					fallbackAfterArrival = fallbackAfterArrival || !ShowsTexture(image);
				}
				if (texture->GetResidentLevel() == 5 && streamingDone() && attempt > 10)
					break;
			}
			REQUIRE(arrivedLevel);
			CHECK(*arrivedLevel == 6);
			CHECK(groupCompleteOnArrival);
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
