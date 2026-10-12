#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Renderer/TextureStreamingTestUtils.h"
#include "Renderer/UploadQueueTestUtils.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Renderer/TextureFormat.h"
#include "Strata/Renderer/TextureStreamer.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"
#include "TestHelpers.h"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

	// The peak signal-to-noise ratio of two RGBA8 images of the same size over their color channels from row `firstRow`
	// on, in dB (100 when they are equal there).
	double ComputePSNR(const ReadbackImage& a, const ReadbackImage& b, uint32_t firstRow = 0)
	{
		REQUIRE(a.Width == b.Width);
		REQUIRE(a.Height == b.Height);
		REQUIRE(firstRow < a.Height);
		double squaredError = 0.0;
		uint64_t samples = 0;
		for (uint32_t y = firstRow; y < a.Height; y++)
		{
			for (uint32_t x = 0; x < a.Width; x++)
			{
				const glm::ivec4 difference = glm::ivec4(GetPixelRGBA8(a, x, y)) - glm::ivec4(GetPixelRGBA8(b, x, y));
				squaredError += static_cast<double>(difference.r * difference.r + difference.g * difference.g + difference.b * difference.b);
				samples += 3;
			}
		}
		const double meanSquaredError = squaredError / static_cast<double>(samples);
		return meanSquaredError > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / meanSquaredError) : 100.0;
	}

	// A device frame in which the manager updates (running its streamer) and each renderer draws its scene, without reading
	// pixels back.
	void RunFrame(GPUContext& gpu, const Ref<RuntimeAssetManager>& manager, std::initializer_list<std::pair<SceneRenderer*, Scene*>> views, const SceneCamera& camera)
	{
		AssetManager::SetActive(manager);
		REQUIRE(gpu.GetDevice().BeginFrame());
		Strata::Renderer::BeginFrame();
		manager->Update();
		for (const auto& [renderer, scene] : views)
			REQUIRE(renderer->Render(*scene, camera));
		gpu.GetDevice().EndFrame();
	}

	// A second gradient texture and its material, for tests of two textures sharing a budget.
	constexpr AssetHandle c_OtherTextureHandle = AssetHandle(0xA1002);
	constexpr AssetHandle c_OtherMaterialHandle = AssetHandle(0xA1003);

	std::filesystem::path WriteTwoTexturePack()
	{
		MaterialProperties first = UnlitColor(glm::vec4(1.0f));
		first.BaseColorMap = c_TextureHandle;
		MaterialProperties second = UnlitColor(glm::vec4(1.0f));
		second.BaseColorMap = c_OtherTextureHandle;
		std::vector<uint8_t> gradient = CookGradientTexture();
		return WriteStreamingPack({ StreamingPackAsset { c_TextureHandle, AssetType::Texture, "Textures/Gradient.png", gradient },
			StreamingPackAsset { c_MaterialHandle, AssetType::Material, "Materials/Gradient.stmat", SerializeMaterial(first) },
			StreamingPackAsset { c_OtherTextureHandle, AssetType::Texture, "Textures/Other.png", std::move(gradient) },
			StreamingPackAsset { c_OtherMaterialHandle, AssetType::Material, "Materials/Other.stmat", SerializeMaterial(second) } });
	}

	Ref<Texture> PeekTexture(AssetManagerBase& manager, AssetHandle handle)
	{
		return std::dynamic_pointer_cast<Texture>(manager.PeekLoadedAsset(handle));
	}

	uint32_t GetResidentLevel(AssetManagerBase& manager, AssetHandle handle)
	{
		const Ref<Texture> texture = PeekTexture(manager, handle);
		return texture ? texture->GetResidentLevel() : std::numeric_limits<uint32_t>::max();
	}

	// The bytes of a gradient texture's levels [first, end).
	uint64_t GetGradientBytes(uint32_t first, uint32_t end)
	{
		uint64_t bytes = 0;
		for (uint32_t level = first; level < end; level++)
			bytes += GetMipByteSize(TextureFormat::BC7SRGB, std::max(1u, c_GradientTextureSize >> level), std::max(1u, c_GradientTextureSize >> level));
		return bytes;
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

			// A budget of 1 MiB of GPU textures drops its finest levels at once, only as many as the budget takes (level 2 on,
			// 341 KiB, is the most it holds), without the fallback, and it stays there while the budget cannot hold more.
			AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
			budgets.GpuTextures = 1ull << 20;
			manager->SetResidencyBudgets(budgets);
			for (uint32_t step = 0; step < 10; step++)
			{
				image = frame.Run(CameraAt(c_FillDistance));
				CHECK(ShowsTexture(image));
				CHECK(manager->GetAssetState(c_TextureHandle) == AssetState::Ready);
				CHECK(manager->GetStats().Resident.GpuTextures <= budgets.GpuTextures);
			}
			CHECK(PeekTexture(*manager)->GetResidentLevel() == 2);
			CHECK(manager->GetStats().Detail.LevelsDroppedForBudget == 2);

			// One that holds no more than its tail and the next level's half (4 KiB) drops it back to its tail, never further:
			// a texture in view keeps its tail.
			budgets.GpuTextures = 4096;
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
			CHECK(manager->GetStats().Detail.LevelsDroppedForBudget == 6);

			AssetManager::SetActive(nullptr);
			scene.reset();
			manager.reset();
			SettleDevice(gpu);
			CHECK(gpu.GetNewErrorCount() == 0);
		}
	}

	TEST_CASE("A large tiled floor seen at a grazing angle streams the levels its near part needs")
	{
		// A 100 x 100 floor (the unit plane scaled) with a checkerboard tiled 20 times, seen from 1.5 units above it: under
		// the camera 40960 texels cover 100 units, so the near floor needs a fine level although the floor's center lies 40
		// units away (from its bounding sphere there it would have looked like level 5). Its pixels are those of the fully
		// resident texture.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr AssetHandle c_CheckerHandle = AssetHandle(0xA2000);
		constexpr AssetHandle c_FloorMaterialHandle = AssetHandle(0xA2001);
		MaterialProperties floor = UnlitColor(glm::vec4(1.0f));
		floor.BaseColorMap = c_CheckerHandle;
		floor.UVTiling = glm::vec2(20.0f);
		const std::filesystem::path pack = WriteStreamingPack({ StreamingPackAsset { c_CheckerHandle, AssetType::Texture, "Textures/Checker.png", CookCheckerTexture() },
			StreamingPackAsset { c_FloorMaterialHandle, AssetType::Material, "Materials/Floor.stmat", SerializeMaterial(floor) } });
		Scope<Scene> scene = CreateScope<Scene>("Floor");
		AddNeutralPostProcess(*scene);
		Entity plane = AddMesh(*scene, BuiltinAssets::PlaneMesh, c_FloorMaterialHandle, glm::vec3(0.0f), "Floor");
		plane.GetComponent<TransformComponent>().Scale = glm::vec3(100.0f, 1.0f, 100.0f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.5f, 40.0f), glm::vec3(0.0f, 0.0f, 30.0f));

		for (const bool transfer : GetQueueModes(gpu))
		{
			CAPTURE(transfer);
			SettleDevice(gpu);
			const ScopedTransferQueue queueMode(transfer);
			REQUIRE(queueMode.IsEnabled() == transfer);

			// The reference: the whole texture, drawn without streaming.
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
				StreamingFrame frame { gpu, manager, renderer, *scene };
				for (int attempt = 0; attempt < 100; attempt++)
				{
					reference = frame.Run(camera);
					if (GetResidentLevel(*manager, c_CheckerHandle) == 0 && renderer.GetStats().PendingAssets == 0)
						break;
				}
				REQUIRE(renderer.GetStats().PendingAssets == 0);
				AssetManager::SetActive(nullptr);
			}

			std::string error;
			Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
			REQUIRE_MESSAGE(manager, error);
			SceneRenderer renderer;
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			StreamingFrame frame { gpu, manager, renderer, *scene };
			ReadbackImage image;
			for (int attempt = 0; attempt < 100; attempt++)
			{
				image = frame.Run(camera);
				const AssetManagerStats stats = manager->GetStats();
				if (attempt > 10 && stats.Detail.Reads == 0 && stats.Detail.Changes == 0 && GetResidentLevel(*manager, c_CheckerHandle) < 6)
					break;
			}
			const uint32_t resident = GetResidentLevel(*manager, c_CheckerHandle);
			CAPTURE(resident);
			CHECK(resident <= 1);
			CHECK(manager->GetStats().Detail.AssetsBelowWanted == 0);
			// The near floor (the lower half of the view) and the whole view look as with every level.
			const double nearPSNR = ComputePSNR(image, reference, c_ViewSize / 2);
			const double wholePSNR = ComputePSNR(image, reference);
			CAPTURE(nearPSNR);
			CAPTURE(wholePSNR);
			CHECK(nearPSNR >= 40.0);
			CHECK(wholePSNR >= 40.0);

			AssetManager::SetActive(nullptr);
			manager.reset();
			SettleDevice(gpu);
			CHECK(gpu.GetNewErrorCount() == 0);
		}
	}

	TEST_CASE("A texture needed more takes the finest levels of one needed less when the budget does not hold both")
	{
		// Two 2048x2048 textures: one on a quad filling the view (level 0), one on a quad 1.15 units away behind it (level 3).
		// The budget holds the first whole with the second's tail and less than its level 3.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path pack = WriteTwoTexturePack();
		const uint64_t budget = GetGradientBytes(0, c_TextureMips) + GetGradientBytes(6, c_TextureMips) + 40000;
		const SceneCamera camera = CameraAt(0.15f);

		for (const bool transfer : GetQueueModes(gpu))
		{
			CAPTURE(transfer);
			SettleDevice(gpu);
			const ScopedTransferQueue queueMode(transfer);
			REQUIRE(queueMode.IsEnabled() == transfer);
			std::string error;
			Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
			REQUIRE_MESSAGE(manager, error);
			AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
			budgets.GpuTextures = budget;
			manager->SetResidencyBudgets(budgets);
			SceneRenderer renderer;
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			Scope<Scene> scene = CreateScope<Scene>("Budget");
			AddNeutralPostProcess(*scene);
			AddMesh(*scene, BuiltinAssets::QuadMesh, c_OtherMaterialHandle, glm::vec3(0.0f, 0.0f, -1.0f), "Far");
			const auto settle = [&]()
			{
				for (int attempt = 0; attempt < 100; attempt++)
				{
					RunFrame(gpu, manager, { { &renderer, scene.get() } }, camera);
					const AssetManagerStats stats = manager->GetStats();
					if (attempt > 10 && stats.Detail.Reads == 0 && stats.Detail.Changes == 0)
						return;
				}
				FAIL("Streaming did not settle");
			};

			// Alone, the far texture gets the level it needs.
			settle();
			CHECK(GetResidentLevel(*manager, c_OtherTextureHandle) == 3);

			// The near one comes into view: it needs the far one's finest level, which goes (only that one); the far one keeps
			// what remains, and neither takes from the other again.
			AddMesh(*scene, BuiltinAssets::QuadMesh, c_MaterialHandle, glm::vec3(0.0f), "Near");
			settle();
			CHECK(GetResidentLevel(*manager, c_TextureHandle) == 0);
			CHECK(GetResidentLevel(*manager, c_OtherTextureHandle) == 4);
			CHECK(manager->GetStats().Detail.LevelsDroppedForBudget == 1);
			CHECK(manager->GetStats().Resident.GpuTextures <= budget);
			for (uint32_t step = 0; step < 30; step++)
				RunFrame(gpu, manager, { { &renderer, scene.get() } }, camera);
			CHECK(GetResidentLevel(*manager, c_TextureHandle) == 0);
			CHECK(GetResidentLevel(*manager, c_OtherTextureHandle) == 4);
			CHECK(manager->GetStats().Detail.LevelsDroppedForBudget == 1);
			CHECK(manager->GetStats().Detail.AssetsBelowWanted == 1);

			AssetManager::SetActive(nullptr);
			scene.reset();
			manager.reset();
			SettleDevice(gpu);
			CHECK(gpu.GetNewErrorCount() == 0);
		}
	}

	TEST_CASE("Both needing more than the budget holds, the texture larger on screen comes first and the other gets the finest level that fits")
	{
		// Both quads enter the view at once, the near one needing level 0 and the far one level 3: the near one is ranked
		// first, and the room left holds the far one's level 4 but not its level 3.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path pack = WriteTwoTexturePack();
		const uint64_t budget = GetGradientBytes(0, c_TextureMips) + GetGradientBytes(6, c_TextureMips) + 40000;
		const SceneCamera camera = CameraAt(0.15f);
		for (const bool transfer : GetQueueModes(gpu))
		{
			CAPTURE(transfer);
			SettleDevice(gpu);
			const ScopedTransferQueue queueMode(transfer);
			std::string error;
			Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
			REQUIRE_MESSAGE(manager, error);
			AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
			budgets.GpuTextures = budget;
			manager->SetResidencyBudgets(budgets);
			SceneRenderer renderer;
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			// Both materials and textures (their tails) are loaded before either is drawn, so the first frame that draws them
			// ranks them together.
			const std::vector<AssetHandle> handles = { c_MaterialHandle, c_TextureHandle, c_OtherMaterialHandle, c_OtherTextureHandle };
			for (int attempt = 0; attempt < 100; attempt++)
			{
				RunFrame(gpu, manager, {}, camera);
				bool loaded = true;
				for (AssetHandle handle : handles)
					loaded = manager->GetAsset(handle) != nullptr && loaded;
				if (loaded)
					break;
			}
			for (AssetHandle handle : handles)
				REQUIRE(manager->GetAssetState(handle) == AssetState::Ready);
			Scope<Scene> scene = CreateScope<Scene>("Budget");
			AddNeutralPostProcess(*scene);
			AddMesh(*scene, BuiltinAssets::QuadMesh, c_OtherMaterialHandle, glm::vec3(0.0f, 0.0f, -1.0f), "Far");
			AddMesh(*scene, BuiltinAssets::QuadMesh, c_MaterialHandle, glm::vec3(0.0f), "Near");
			for (int attempt = 0; attempt < 100; attempt++)
			{
				RunFrame(gpu, manager, { { &renderer, scene.get() } }, camera);
				const AssetManagerStats stats = manager->GetStats();
				if (attempt > 10 && stats.Detail.Reads == 0 && stats.Detail.Changes == 0)
					break;
			}
			CHECK(GetResidentLevel(*manager, c_TextureHandle) == 0);
			CHECK(GetResidentLevel(*manager, c_OtherTextureHandle) == 4);
			CHECK(manager->GetStats().Detail.LevelsDroppedForBudget == 0);
			CHECK(manager->GetStats().Resident.GpuTextures <= budget);

			AssetManager::SetActive(nullptr);
			scene.reset();
			manager.reset();
			SettleDevice(gpu);
			CHECK(gpu.GetNewErrorCount() == 0);
		}
	}

	TEST_CASE("A renderer without streaming waits for every level of what it draws, also from a manager that loads partly")
	{
		// Exact-pixel renderers set SceneRendererSpecification::TextureStreaming false and keep the manager's partial loads:
		// the texture counts as pending until every level is resident, then draws as fully resident, and a budget the
		// streaming of another view pushes against never takes its levels.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path pack = WriteTwoTexturePack();
		for (const bool transfer : GetQueueModes(gpu))
		{
			CAPTURE(transfer);
			SettleDevice(gpu);
			const ScopedTransferQueue queueMode(transfer);

			// The reference: every level loaded whole.
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
				for (int attempt = 0; attempt < 100 && (attempt == 0 || renderer.GetStats().PendingAssets > 0); attempt++)
					reference = frame.Run(CameraAt(5.6f));
				REQUIRE(renderer.GetStats().PendingAssets == 0);
				AssetManager::SetActive(nullptr);
			}

			std::string error;
			Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
			REQUIRE_MESSAGE(manager, error);
			REQUIRE(manager->ArePartialLoadsActive());
			SceneRendererSpecification specification;
			specification.TextureStreaming = false;
			SceneRenderer exact(specification);
			exact.SetViewportSize(c_ViewSize, c_ViewSize);
			Scope<Scene> scene = CreateQuadScene();
			StreamingFrame frame { gpu, manager, exact, *scene };
			ReadbackImage image;
			bool pendingWhilePartial = true;
			int frames = 0;
			for (; frames < 100; frames++)
			{
				image = frame.Run(CameraAt(5.6f));
				const uint32_t resident = GetResidentLevel(*manager, c_TextureHandle);
				if (resident > 0 && resident != std::numeric_limits<uint32_t>::max())
					pendingWhilePartial = pendingWhilePartial && exact.GetStats().PendingAssets > 0;
				if (resident == 0 && exact.GetStats().PendingAssets == 0)
					break;
			}
			CHECK(pendingWhilePartial);
			CHECK(manager->GetStats().PartialLoads >= 1);
			REQUIRE(GetResidentLevel(*manager, c_TextureHandle) == 0);
			CHECK(exact.GetStats().PendingAssets == 0);
			CHECK(MaxDifference(image, reference) <= 2);

			// A streaming view fills its screen with the other texture, under a budget that holds the first texture whole and
			// only part of the other: the first keeps every level, and the other gets what the room left holds (level 1).
			AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
			budgets.GpuTextures = 8ull << 20;
			manager->SetResidencyBudgets(budgets);
			SceneRenderer streaming;
			streaming.SetViewportSize(c_ViewSize, c_ViewSize);
			Scope<Scene> other = CreateScope<Scene>("Other");
			AddNeutralPostProcess(*other);
			AddMesh(*other, BuiltinAssets::QuadMesh, c_OtherMaterialHandle, glm::vec3(0.0f), "Quad");
			for (uint32_t step = 0; step < 60; step++)
			{
				AssetManager::SetActive(manager);
				REQUIRE(gpu.GetDevice().BeginFrame());
				Strata::Renderer::BeginFrame();
				manager->Update();
				image = RenderToImage(exact, *scene, CameraAt(5.6f));
				REQUIRE(streaming.Render(*other, CameraAt(0.15f)));
				gpu.GetDevice().EndFrame();
				CHECK(GetResidentLevel(*manager, c_TextureHandle) == 0);
				CHECK(exact.GetStats().PendingAssets == 0);
			}
			CHECK(MaxDifference(image, reference) <= 2);
			CHECK(GetResidentLevel(*manager, c_OtherTextureHandle) == 1);
			CHECK(manager->GetStats().Resident.GpuTextures <= budgets.GpuTextures);

			AssetManager::SetActive(nullptr);
			scene.reset();
			other.reset();
			manager.reset();
			SettleDevice(gpu);
			CHECK(gpu.GetNewErrorCount() == 0);
		}
	}

	TEST_CASE("A sky light's environment map is read whole once and then keeps only its tail")
	{
		// The environment is computed from every level of the map; afterwards the renderer samples its cube alone, so the
		// map's levels drop to its tail once and are never read again while the scene shows.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr AssetHandle c_SkyHandle = AssetHandle(0xA3000);
		const std::filesystem::path pack = WriteStreamingPack({ StreamingPackAsset { c_SkyHandle, AssetType::Texture, "Textures/Sky.hdr", CookEnvironmentMap(512, 256) } });
		std::string error;
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
		REQUIRE_MESSAGE(manager, error);
		Scope<Scene> scene = CreateScope<Scene>("Sky");
		AddNeutralPostProcess(*scene);
		SkyLightComponent& sky = scene->CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.EnvironmentMap = c_SkyHandle;
		SceneRenderer renderer;
		renderer.SetViewportSize(64, 64);
		const SceneCamera camera = CameraAt(3.0f);

		// The map arrives with its tail, the streamer raises it whole, and the renderer computes the environment from it.
		uint32_t frames = 0;
		uint32_t updates = 0;
		for (; frames < 200 && updates == 0; frames++)
		{
			RunFrame(gpu, manager, { { &renderer, scene.get() } }, camera);
			updates += renderer.GetStats().EnvironmentUpdates;
		}
		REQUIRE(updates == 1);
		const Ref<Texture> map = PeekTexture(*manager, c_SkyHandle);
		REQUIRE(map);
		CHECK(map->GetTailLevel() == 4);
		CHECK(map->GetResidentLevel() == 0);
		const AssetManagerStats raised = manager->GetStats();
		CHECK(raised.Detail.LevelsRaised == 4);

		// Nothing needs its levels any more: after the streamer's delay they drop to its tail, once; nothing is read or
		// pending again and the environment stays.
		for (uint32_t step = 0; step < 2 * TextureStreamingPolicy::c_DropDelayFrames + 20; step++)
		{
			RunFrame(gpu, manager, { { &renderer, scene.get() } }, camera);
			CHECK(renderer.GetStats().PendingAssets == 0);
			CHECK(renderer.GetStats().EnvironmentUpdates == 0);
			CHECK(renderer.GetStats().EnvironmentLighting);
		}
		const AssetManagerStats after = manager->GetStats();
		CHECK(after.Detail.LevelsRaised == raised.Detail.LevelsRaised);
		CHECK(after.Detail.BytesStreamed == raised.Detail.BytesStreamed);
		CHECK(after.RangeReadsCompleted == raised.RangeReadsCompleted);
		CHECK(after.Detail.LevelsDropped == raised.Detail.LevelsDropped + 4);
		CHECK(map->GetResidentLevel() == 4);

		AssetManager::SetActive(nullptr);
		scene.reset();
		manager.reset();
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A renderer that starts drawing a sky another renderer lights takes over its environment while the map keeps its tail")
	{
		// The environment computed from a map is shared by the renderers that light with it: a renderer created after the map
		// dropped to its tail (the editor's capture renderer, a second view) draws the sky and its lighting in its first
		// frame, without reading the map's levels again or computing the environment anew. Once no renderer lights with it,
		// the environment is released, and the next renderer computes it from the whole map again.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr AssetHandle c_SkyHandle = AssetHandle(0xA3001);
		constexpr uint32_t c_SkyViewSize = 64;
		const std::filesystem::path pack = WriteStreamingPack({ StreamingPackAsset { c_SkyHandle, AssetType::Texture, "Textures/Sky.hdr", CookEnvironmentMap(512, 256) } });
		for (const bool transfer : GetQueueModes(gpu))
		{
			CAPTURE(transfer);
			SettleDevice(gpu);
			const ScopedTransferQueue queueMode(transfer);
			std::string error;
			Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
			REQUIRE_MESSAGE(manager, error);
			Scope<Scene> scene = CreateScope<Scene>("Sky");
			AddNeutralPostProcess(*scene);
			scene->CreateEntity("Sky").AddComponent<SkyLightComponent>().EnvironmentMap = c_SkyHandle;
			// Lit by the environment alone: the ball shows its irradiance, the background the sky.
			AddMesh(*scene, BuiltinAssets::SphereMesh, BuiltinAssets::DefaultMaterial, glm::vec3(0.0f), "Ball");
			const SceneCamera camera = CameraAt(3.0f);

			Scope<SceneRenderer> first = CreateScope<SceneRenderer>();
			first->SetViewportSize(c_SkyViewSize, c_SkyViewSize);
			uint32_t updates = 0;
			for (uint32_t frame = 0; frame < 200 && updates == 0; frame++)
			{
				RunFrame(gpu, manager, { { first.get(), scene.get() } }, camera);
				updates += first->GetStats().EnvironmentUpdates;
			}
			REQUIRE(updates == 1);
			// Nothing samples the map any more: its levels drop to its tail.
			for (uint32_t step = 0; step < 2 * TextureStreamingPolicy::c_DropDelayFrames + 20; step++)
				RunFrame(gpu, manager, { { first.get(), scene.get() } }, camera);
			const Ref<Texture> map = PeekTexture(*manager, c_SkyHandle);
			REQUIRE(map);
			REQUIRE(map->GetResidentLevel() == map->GetTailLevel());
			REQUIRE(map->GetTailLevel() > 0);
			const AssetManagerStats before = manager->GetStats();

			// A new renderer's first frame matches the first renderer's, in the same device frame.
			Scope<SceneRenderer> second = CreateScope<SceneRenderer>();
			second->SetViewportSize(c_SkyViewSize, c_SkyViewSize);
			AssetManager::SetActive(manager);
			REQUIRE(gpu.GetDevice().BeginFrame());
			Strata::Renderer::BeginFrame();
			manager->Update();
			const ReadbackImage reference = RenderToImage(*first, *scene, camera);
			const ReadbackImage image = RenderToImage(*second, *scene, camera);
			gpu.GetDevice().EndFrame();
			CHECK(second->GetStats().PendingAssets == 0);
			CHECK(second->GetStats().EnvironmentLighting);
			CHECK(second->GetStats().EnvironmentUpdates == 0); // Taken over, not computed again
			CHECK(second->GetEnvironmentCube() == first->GetEnvironmentCube());
			CHECK(MaxDifference(image, reference) <= 2);
			// The sky's blue in a corner, and the ball lit in the middle.
			CHECK(GetPixelRGBA8(image, 1, 1).b > 100);
			const glm::u8vec4 ball = GetPixelRGBA8(image, c_SkyViewSize / 2, c_SkyViewSize / 2);
			CHECK(ball.r + ball.g + ball.b > 60);

			// Both keep drawing it without a read: the map stays at its tail.
			for (uint32_t step = 0; step < 10; step++)
			{
				RunFrame(gpu, manager, { { first.get(), scene.get() }, { second.get(), scene.get() } }, camera);
				CHECK(second->GetStats().PendingAssets == 0);
				CHECK(second->GetStats().EnvironmentLighting);
			}
			const AssetManagerStats after = manager->GetStats();
			CHECK(after.Detail.LevelsRaised == before.Detail.LevelsRaised);
			CHECK(after.RangeReads == before.RangeReads);
			CHECK(map->GetResidentLevel() == map->GetTailLevel());

			// No renderer lights with it any more: the next one computes it from the whole map again.
			first.reset();
			second.reset();
			Scope<SceneRenderer> third = CreateScope<SceneRenderer>();
			third->SetViewportSize(c_SkyViewSize, c_SkyViewSize);
			updates = 0;
			for (uint32_t frame = 0; frame < 200 && updates == 0; frame++)
			{
				RunFrame(gpu, manager, { { third.get(), scene.get() } }, camera);
				updates += third->GetStats().EnvironmentUpdates;
			}
			CHECK(updates == 1);
			CHECK(manager->GetStats().Detail.LevelsRaised > after.Detail.LevelsRaised);

			third.reset();
			AssetManager::SetActive(nullptr);
			scene.reset();
			manager.reset();
			SettleDevice(gpu);
			CHECK(gpu.GetNewErrorCount() == 0);
		}
	}

	TEST_CASE("A preview holds what a first load holds and never asks for finer levels")
	{
		// TextureStreamer::GetPreview, for thumbnails: the texture loads partly and stays at its tail while no view needs it.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path pack = WriteGradientStreamingPack();
		std::string error;
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
		REQUIRE_MESSAGE(manager, error);
		Ref<Texture> preview;
		for (uint32_t step = 0; step < 150; step++)
		{
			RunFrame(gpu, manager, {}, CameraAt(1.0f));
			preview = TextureStreamer::GetPreview(*manager, c_TextureHandle);
		}
		REQUIRE(preview);
		CHECK(manager->GetAssetState(c_TextureHandle) == AssetState::Ready);
		CHECK(preview->GetResidentLevel() == preview->GetTailLevel());
		CHECK(preview->GetBindlessSlot() != BindlessTextureTable::c_InvalidSlot);
		const AssetManagerStats stats = manager->GetStats();
		CHECK(stats.PartialLoads == 1);
		CHECK(stats.Detail.LevelsRaised == 0);
		CHECK(stats.RangeReads == 0);
		CHECK(stats.RangeReadsCompleted == 0);
		CHECK(TextureStreamer::GetPreview(*manager, c_GradientMaterialHandle) == nullptr); // Not a texture

		preview.reset();
		AssetManager::SetActive(nullptr);
		manager.reset();
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
