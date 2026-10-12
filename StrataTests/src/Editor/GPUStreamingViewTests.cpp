#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/StreamingView.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Renderer/TextureStreamingTestUtils.h"

#include <Strata/Asset/RuntimeAssetManager.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Math/Frustum.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Renderer/MeshFactory.h>
#include <Strata/Renderer/Texture.h>
#include <Strata/Reflection/PropertyJson.h>

#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = c_SceneTestSize;

	// Makes an asset manager the active one while it lives.
	struct ScopedActiveManager
	{
		explicit ScopedActiveManager(const Ref<AssetManagerBase>& manager)
		{
			AssetManager::SetActive(manager);
		}

		~ScopedActiveManager()
		{
			AssetManager::SetActive(nullptr);
		}

		ScopedActiveManager(const ScopedActiveManager&) = delete;
		ScopedActiveManager& operator=(const ScopedActiveManager&) = delete;
	};

	std::vector<uint8_t> ToBytes(const std::string& text)
	{
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	uint32_t EntityIDAt(const ReadbackImage& ids, const glm::uvec2& pixel)
	{
		REQUIRE(ids.BytesPerPixel == sizeof(uint32_t));
		uint32_t id = 0;
		std::memcpy(&id, ids.Pixels.data() + (static_cast<size_t>(pixel.y) * ids.Width + pixel.x) * sizeof(uint32_t), sizeof(uint32_t));
		return id;
	}

	uint32_t GetEntityID(Entity entity)
	{
		return static_cast<uint32_t>(entity.GetHandle()) + 1;
	}

	// The pixel a tint of `color` (8-bit, as the renderer packs it) makes of `before`.
	glm::ivec3 Blend(const glm::u8vec4& before, const glm::vec4& color)
	{
		const glm::vec4 packed = glm::round(glm::clamp(color, 0.0f, 1.0f) * 255.0f);
		const float alpha = packed.a / 255.0f;
		return glm::ivec3(glm::round(glm::vec3(packed) * alpha + glm::vec3(before) * (1.0f - alpha)));
	}

	bool IsNear(const glm::ivec3& a, const glm::ivec3& b, int tolerance = 1)
	{
		return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::ivec3(tolerance)));
	}

	// Counts the pixels of an entity, and those of them that look like `before` tinted with `color`.
	struct TintCheck
	{
		uint32_t Pixels = 0;
		uint32_t Tinted = 0;
		uint32_t Unchanged = 0;
	};

	TintCheck CheckTint(const ReadbackImage& ids, const ReadbackImage& before, const ReadbackImage& after, Entity entity, const glm::vec4& color)
	{
		TintCheck check;
		for (uint32_t y = 0; y < c_Size; y++)
		{
			for (uint32_t x = 0; x < c_Size; x++)
			{
				if (EntityIDAt(ids, glm::uvec2(x, y)) != GetEntityID(entity))
					continue;
				check.Pixels++;
				const glm::u8vec4 original = GetPixelRGBA8(before, x, y);
				const glm::u8vec4 now = GetPixelRGBA8(after, x, y);
				check.Tinted += IsNear(glm::ivec3(now), Blend(original, color)) ? 1u : 0u;
				check.Unchanged += now == original ? 1u : 0u;
			}
		}
		return check;
	}

}

TEST_SUITE("GPU.Editor.StreamingView")
{
	TEST_CASE("The streaming view tints entities whose assets are still loading until they are ready")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		// Loads are read on an I/O thread, where the fake manager holds them back at its gate.
		ScopedJobSystem jobs(1, 1);
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		ScopedActiveManager active(manager);
		const AssetHandle material = manager->AddStored(0x5E0001, AssetType::Material,
			ToBytes(JsonUtils::Dump(Material::Create(UnlitColor({ 0.2f, 0.9f, 0.3f, 1.0f }))->Serialize())), "Materials/Held.stmat");
		const AssetHandle mesh = manager->AddStored(0x5E0002, AssetType::Mesh, MeshFactory::CreateCube()->Serialize(), "Meshes/Held.mesh");
		manager->CloseGate();

		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::vec3(-0.3f, -1.0f, -0.4f), 2.0f, false);
		Entity pendingMaterial = AddMesh(scene, BuiltinAssets::CubeMesh, material, glm::vec3(-1.2f, 0.0f, 0.0f), "Pending Material");
		Entity pendingMesh = AddMesh(scene, mesh, BuiltinAssets::DefaultMaterial, glm::vec3(1.2f, 0.0f, 0.0f), "Pending Mesh");
		Entity ready = AddMesh(scene, BuiltinAssets::CubeMesh, BuiltinAssets::DefaultMaterial, glm::vec3(0.0f, 1.2f, 0.0f), "Ready");

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.5f, 5.0f), glm::vec3(0.0f, 0.5f, 0.0f));
		ViewportDebugColors colors;
		colors.Pending = glm::vec4(1.0f, 0.85f, 0.2f, 0.6f);
		StreamingView view;
		DebugDraw placeholders;
		const auto render = [&](ViewportDebugView debugView, ReadbackImage& outImage, ReadbackImage& outIDs)
		{
			placeholders.Clear();
			view.Build(scene, debugView, camera, renderer.GetSpecification().LODThreshold, colors, placeholders);
			SceneRenderOptions options;
			options.EntityTints = view.GetTints();
			options.DebugShapes = &placeholders;
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), outImage));
			REQUIRE(Renderer::ReadTexture(renderer.GetEntityIDTexture(), outIDs));
		};

		// The first render requests both loads; they wait at the gate.
		ReadbackImage plain;
		ReadbackImage ids;
		render(ViewportDebugView::None, plain, ids);
		CHECK(renderer.GetStats().PendingAssets == 2);
		CHECK(manager->GetAssetState(material) == AssetState::Loading);
		CHECK(manager->GetAssetState(mesh) == AssetState::Loading);
		CHECK(view.GetTints().empty());

		ReadbackImage streaming;
		ReadbackImage streamingIDs;
		render(ViewportDebugView::Streaming, streaming, streamingIDs);
		CHECK(view.GetStats().PendingEntities == 2);
		CHECK(view.GetStats().FailedEntities == 0);
		CHECK(view.GetStats().PlaceholderBoxes == 1);
		REQUIRE(view.GetTints().size() == 1);
		CHECK(view.GetTints()[0].Target == pendingMaterial);
		CHECK(renderer.GetStats().TintedEntities == 1);
		// The entity with the pending material (drawn with the default one meanwhile) is tinted in the pending color...
		const TintCheck tinted = CheckTint(ids, plain, streaming, pendingMaterial, colors.Pending);
		CHECK(tinted.Pixels > 50);
		CHECK(tinted.Tinted == tinted.Pixels);
		// ... the ready one is not ...
		const TintCheck untouched = CheckTint(ids, plain, streaming, ready, colors.Pending);
		CHECK(untouched.Pixels > 50);
		CHECK(untouched.Unchanged == untouched.Pixels);
		// ... and the entity whose mesh is missing (nothing is drawn for it) is a box in the pending color.
		CHECK(placeholders.GetLineCount() == 12);
		const glm::uvec2 boxCorner = ProjectToPixel(camera, glm::vec3(1.2f + 0.5f, 0.5f, 0.5f), c_Size);
		bool boxDrawn = false;
		const glm::ivec3 pendingLine = glm::ivec3(glm::round(glm::vec3(colors.Pending) * 255.0f));
		for (int y = -2; y <= 2; y++)
		{
			for (int x = -2; x <= 2; x++)
			{
				const glm::uvec2 pixel(static_cast<uint32_t>(static_cast<int>(boxCorner.x) + x), static_cast<uint32_t>(static_cast<int>(boxCorner.y) + y));
				boxDrawn = boxDrawn || IsNear(glm::ivec3(GetPixelRGBA8(streaming, pixel.x, pixel.y)), pendingLine, 2);
			}
		}
		CHECK(boxDrawn);
		CHECK(EntityIDAt(ids, ProjectToPixel(camera, glm::vec3(1.2f, 0.0f, 0.5f), c_Size)) != GetEntityID(pendingMesh));

		// Once the loads are through and finalized, nothing is tinted or boxed: the streaming view shows the plain image.
		manager->OpenGate();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while ((manager->GetAssetState(material) != AssetState::Ready || manager->GetAssetState(mesh) != AssetState::Ready) && std::chrono::steady_clock::now() < deadline)
		{
			manager->Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		REQUIRE(manager->GetAssetState(material) == AssetState::Ready);
		REQUIRE(manager->GetAssetState(mesh) == AssetState::Ready);
		ReadbackImage loaded;
		render(ViewportDebugView::None, loaded, ids);
		CHECK(renderer.GetStats().PendingAssets == 0);
		render(ViewportDebugView::Streaming, streaming, streamingIDs);
		CHECK(view.GetTints().empty());
		CHECK(view.GetStats().PendingEntities == 0);
		CHECK(view.GetStats().PlaceholderBoxes == 0);
		CHECK(placeholders.IsEmpty());
		CHECK(MaxDifference(streaming, loaded) == 0);
		CHECK(EntityIDAt(ids, ProjectToPixel(camera, glm::vec3(1.2f, 0.0f, 0.5f), c_Size)) == GetEntityID(pendingMesh));

		// An asset that fails is tinted in the failed color, also through a material's texture.
		const AssetHandle broken = manager->AddStored(0x5E0003, AssetType::Texture, ToBytes("not a texture"), "Textures/Broken.png");
		MaterialProperties properties = UnlitColor({ 1.0f, 1.0f, 1.0f, 1.0f });
		properties.BaseColorMap = broken;
		const AssetHandle brokenMaterial = manager->AddStored(0x5E0004, AssetType::Material, ToBytes(JsonUtils::Dump(Material::Create(properties)->Serialize())),
			"Materials/Broken.stmat");
		ready.GetComponent<MeshRendererComponent>().Material = brokenMaterial;
		ready.MarkModified<MeshRendererComponent>(); // The render proxies follow the component's changes
		const auto failDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (manager->GetAssetState(broken) != AssetState::Failed && std::chrono::steady_clock::now() < failDeadline)
		{
			render(ViewportDebugView::Streaming, streaming, streamingIDs);
			manager->Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		REQUIRE(manager->GetAssetState(broken) == AssetState::Failed);
		render(ViewportDebugView::Streaming, streaming, streamingIDs);
		CHECK(view.GetStats().FailedEntities == 1);
		REQUIRE(view.GetTints().size() == 1);
		CHECK(view.GetTints()[0].Target == ready);
		CHECK(view.GetTints()[0].Color == colors.Failed);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The streaming view culls and boxes meshes that have not loaded with the bounds the asset pipeline recorded")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ScopedJobSystem jobs(1, 1);
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		ScopedActiveManager active(manager);
		// Two long meshes that never arrive (the gate stays closed), with recorded bounds 20 units long along x.
		const AABB recorded(glm::vec3(-10.0f, -0.5f, -0.5f), glm::vec3(10.0f, 0.5f, 0.5f));
		const AssetHandle reaching = manager->AddStored(0x5E0011, AssetType::Mesh, MeshFactory::CreateCube()->Serialize(), "Meshes/Reaching.mesh");
		const AssetHandle away = manager->AddStored(0x5E0012, AssetType::Mesh, MeshFactory::CreateCube()->Serialize(), "Meshes/Away.mesh");
		manager->SetDependencies(reaching, {}, recorded);
		manager->SetDependencies(away, {}, recorded);
		manager->CloseGate();

		Scene scene;
		AddNeutralPostProcess(scene);
		// Looking down -z from the origin: the first entity's origin is 9 units to the right, outside the view, but its mesh
		// reaches across it; the second lies wholly behind the camera.
		AddMesh(scene, reaching, BuiltinAssets::DefaultMaterial, glm::vec3(9.0f, 0.0f, -6.0f), "Reaching");
		AddMesh(scene, away, BuiltinAssets::DefaultMaterial, glm::vec3(0.0f, 0.0f, 30.0f), "Away");
		const SceneCamera camera = LookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
		REQUIRE_FALSE(Frustum(camera.Projection * camera.View).IsAABBVisible(AABB(glm::vec3(8.5f, -0.5f, -6.5f), glm::vec3(9.5f, 0.5f, -5.5f))));

		ViewportDebugColors colors;
		StreamingView view;
		DebugDraw placeholders;
		view.Build(scene, ViewportDebugView::Streaming, camera, 1.0f, colors, placeholders);
		CHECK(view.GetStats().PendingEntities == 1);
		CHECK(view.GetStats().PlaceholderBoxes == 1);
		// The box is the recorded one: its 12 edges span 20 units along x around the entity's origin.
		REQUIRE(placeholders.GetLineCount() == 12);
		float minX = std::numeric_limits<float>::max();
		float maxX = std::numeric_limits<float>::lowest();
		for (const DebugLineVertex& vertex : placeholders.GetLines(DebugDrawDepth::Tested))
		{
			minX = std::min(minX, vertex.Position.x);
			maxX = std::max(maxX, vertex.Position.x);
		}
		CHECK(minX == doctest::Approx(-1.0f));
		CHECK(maxX == doctest::Approx(19.0f));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The debug views look at assets without requesting them, so what only they see can be evicted")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const Ref<Mesh> cube = MeshFactory::CreateCube();
		const AssetHandle mesh = assets.Add(cube, "Cube");
		const AssetHandle material = assets.AddMaterial(MaterialProperties());

		Scene scene;
		AddNeutralPostProcess(scene);
		Entity inView = AddMesh(scene, mesh, material, glm::vec3(0.0f, 0.0f, -3.0f), "In View");
		AddMesh(scene, mesh, material, glm::vec3(0.0f, 0.0f, 30.0f), "Behind");
		const SceneCamera camera = LookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		RenderToImage(renderer, scene, camera);
		REQUIRE(AssetManager::GetAssetState(mesh) == AssetState::Ready);
		REQUIRE(AssetManager::GetAssetState(material) == AssetState::Ready);

		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		const auto lastRequested = [&manager](AssetHandle handle)
		{
			for (const AssetResidencyInfo& info : manager->GetResidencyInfo())
			{
				if (info.Handle == handle)
					return info.LastRequestedFrame;
			}
			return uint64_t(0);
		};
		manager->Update();
		manager->Update();
		const uint64_t meshRequested = lastRequested(mesh);
		const uint64_t materialRequested = lastRequested(material);
		const uint64_t calls = manager->GetStats().GetAssetCalls;
		REQUIRE(manager->GetStats().Frame > meshRequested);

		ViewportDebugColors colors;
		StreamingView view;
		DebugDraw placeholders;
		for (const ViewportDebugView debugView : { ViewportDebugView::Streaming, ViewportDebugView::LOD })
		{
			view.Build(scene, debugView, camera, renderer.GetSpecification().LODThreshold, colors, placeholders);
			manager->Update();
		}
		CHECK(view.GetStats().EntitiesPerLOD[0] == 1); // The LOD view saw the loaded mesh in view, and only that entity
		CHECK(manager->GetStats().GetAssetCalls == calls);
		CHECK(lastRequested(mesh) == meshRequested);
		CHECK(lastRequested(material) == materialRequested);

		// The view brings the render proxies up to date before the frame renders: the renderer still reports what changed
		// since its previous frame.
		inView.GetComponent<TransformComponent>().Translation.x = 0.5f;
		inView.MarkModified<TransformComponent>();
		view.Build(scene, ViewportDebugView::Streaming, camera, renderer.GetSpecification().LODThreshold, colors, placeholders);
		RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetStats().ProxyUpdates == 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The LOD view colors entities by the level of detail the renderer draws")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		// A cube with four levels of detail (all the same triangles: only the choice matters here).
		const Ref<Mesh> cube = MeshFactory::CreateCube();
		std::vector<Submesh> submeshes = cube->GetSubmeshes();
		REQUIRE(submeshes.size() == 1);
		const MeshLOD full = submeshes[0].LODs[0];
		submeshes[0].LODs = { full, full, full, full };
		std::string error;
		const Ref<Mesh> lodCube = Mesh::Create(cube->GetPositions(), cube->GetAttributes(), cube->GetIndices(), submeshes, &error);
		REQUIRE_MESSAGE(lodCube, error);
		const AssetHandle mesh = assets.Add(lodCube, "LODCube");

		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::vec3(-0.3f, -1.0f, -0.4f), 2.0f, false);
		const AssetHandle material = assets.AddMaterial(MaterialProperties());
		// Seen from the origin: a cube two units away fills a large part of the view (level 0), a ten-unit cube 60 units
		// away a quarter of its height (level 2).
		Entity nearCube = AddMesh(scene, mesh, material, glm::vec3(-0.6f, 0.0f, -2.0f), "Near");
		Entity farCube = AddMesh(scene, mesh, material, glm::vec3(20.0f, 0.0f, -60.0f), "Far");
		farCube.GetComponent<TransformComponent>().Scale = glm::vec3(10.0f);
		// Above the others, so it hides neither.
		Entity plainCube = AddMesh(scene, BuiltinAssets::CubeMesh, material, glm::vec3(0.0f, 1.0f, -3.0f), "Single Level");

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
		const ReadbackImage plain = RenderToImage(renderer, scene, camera);
		const std::array<uint32_t, SceneRendererStats::c_LODStatCount> rendererLODs = renderer.GetStats().InstancesPerLOD;
		ReadbackImage ids;
		REQUIRE(Renderer::ReadTexture(renderer.GetEntityIDTexture(), ids));

		ViewportDebugColors colors;
		StreamingView view;
		DebugDraw placeholders;
		view.Build(scene, ViewportDebugView::LOD, camera, renderer.GetSpecification().LODThreshold, colors, placeholders);
		CHECK(placeholders.IsEmpty());
		// The view's levels are the renderer's: level 0 twice (the near cube and the single-level one), level 2 once.
		const std::array<uint32_t, 4>& perLOD = view.GetStats().EntitiesPerLOD;
		CHECK(perLOD[0] == 2);
		CHECK(perLOD[1] == 0);
		CHECK(perLOD[2] + perLOD[3] == 1);
		CHECK(perLOD[2] == 1);
		CHECK(rendererLODs[0] == 2);
		CHECK(rendererLODs[2] == 1);
		for (const SceneEntityTint& tint : view.GetTints())
		{
			if (tint.Target == nearCube || tint.Target == plainCube)
				CHECK(tint.Color == colors.LOD[0]);
			else
				CHECK(tint.Color == colors.LOD[2]);
		}

		SceneRenderOptions options;
		options.EntityTints = view.GetTints();
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		ReadbackImage tinted;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), tinted));
		const TintCheck nearCheck = CheckTint(ids, plain, tinted, nearCube, colors.LOD[0]);
		const TintCheck farCheck = CheckTint(ids, plain, tinted, farCube, colors.LOD[2]);
		CHECK(nearCheck.Pixels > 100);
		CHECK(nearCheck.Tinted == nearCheck.Pixels);
		CHECK(farCheck.Pixels > 20);
		CHECK(farCheck.Tinted == farCheck.Pixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The mip view tints entities by the mip levels their textures lack, which asset.stats lists")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		// asset.stats reads the active manager; the context made its own active, which the streaming manager replaces.
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		std::string error;
		const Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(WriteGradientStreamingPack(), &error);
		REQUIRE_MESSAGE(manager, error);
		ScopedActiveManager active(manager);

		// A quad with the 2048x2048 gradient at 56 pixels, where it needs mip level 5 (its first load holds level 6 and the
		// smaller ones), and a cube with the default material, which has no texture to stream: the mip view leaves it alone.
		Scene scene;
		AddNeutralPostProcess(scene);
		const Entity textured = AddMesh(scene, BuiltinAssets::QuadMesh, c_GradientMaterialHandle, glm::vec3(0.0f), "Textured");
		AddMesh(scene, BuiltinAssets::CubeMesh, BuiltinAssets::DefaultMaterial, glm::vec3(1.2f, 0.0f, -1.0f), "Plain");
		SceneRenderer renderer;
		renderer.SetViewportSize(256, 256); // The view the 56 pixels are of
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 5.6f), glm::vec3(0.0f));
		ViewportDebugColors colors;
		StreamingView view;
		DebugDraw placeholders;

		// Frames as the editor runs them: the device's frame, the manager's update (its streamer), the view, the render.
		std::array<bool, 4> seen = {};
		bool settled = false;
		for (int frame = 0; frame < 300 && !settled; frame++)
		{
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			manager->Update();
			placeholders.Clear();
			view.Build(scene, ViewportDebugView::Mip, camera, renderer.GetSpecification().LODThreshold, colors, placeholders);
			SceneRenderOptions options;
			options.EntityTints = view.GetTints();
			REQUIRE(renderer.Render(scene, camera, nullptr, options));
			gpu.GetDevice().EndFrame();

			// Only the textured entity is ever tinted, in the color of its count.
			const std::array<uint32_t, 4>& counts = view.GetStats().EntitiesPerMipShortfall;
			REQUIRE(view.GetTints().size() <= 1);
			CHECK(counts[0] + counts[1] + counts[2] + counts[3] == view.GetTints().size());
			for (size_t bucket = 0; bucket < counts.size(); bucket++)
			{
				if (counts[bucket] == 0)
					continue;
				seen[bucket] = true;
				CHECK(view.GetTints()[0].Target == textured);
				CHECK(view.GetTints()[0].Color == colors.Mip[bucket]);
			}
			const Ref<Texture> texture = std::dynamic_pointer_cast<Texture>(manager->PeekLoadedAsset(c_GradientTextureHandle));
			const AssetManagerStats stats = manager->GetStats();
			settled = frame > 10 && texture && texture->GetResidentLevel() == 5 && stats.Detail.Reads == 0 && stats.Detail.Changes == 0;
		}
		REQUIRE(settled);
		// Loading first (the material, then the texture), then one level short while level 5 streams in, then as needed.
		CHECK(seen[3]);
		CHECK(seen[1]);
		CHECK(view.GetStats().EntitiesPerMipShortfall == std::array<uint32_t, 4> { 1, 0, 0, 0 });
		CHECK(renderer.GetStats().TintedEntities == 1);

		// asset.stats: the texture's row has its levels, and the totals what the streamer did.
		const EditorCommandResult result = commands.Execute(context, "asset.stats", { { "assets", true } });
		REQUIRE_MESSAGE(result.Success, result.Error);
		const nlohmann::json& totals = result.Value["mipStreaming"];
		const AssetManagerStats stats = manager->GetStats();
		CHECK(totals["partialLoads"] == 1);
		CHECK(totals["levelsRaised"] == stats.Detail.LevelsRaised);
		CHECK(totals["levelsRaised"].get<uint64_t>() >= 1);
		CHECK(totals["levelsDropped"] == stats.Detail.LevelsDropped);
		CHECK(totals["levelsDroppedForBudget"] == stats.Detail.LevelsDroppedForBudget);
		CHECK(totals["bytesStreamed"] == stats.Detail.BytesStreamed);
		CHECK(totals["bytesStreamed"].get<uint64_t>() > 0);
		CHECK(totals["assetsBelowWanted"] == 0);
		CHECK(totals["rangeReadsCompleted"] == stats.RangeReadsCompleted);
		const Ref<Texture> texture = std::dynamic_pointer_cast<Texture>(manager->PeekLoadedAsset(c_GradientTextureHandle));
		REQUIRE(texture);
		bool listed = false;
		for (const nlohmann::json& row : result.Value["assets"])
		{
			if (row["handle"] == UUIDToJson(c_GradientMaterialHandle))
				CHECK_FALSE(row.contains("residentMip")); // Materials do not stream levels
			if (row["handle"] != UUIDToJson(c_GradientTextureHandle))
				continue;
			listed = true;
			CHECK(row["residentMip"] == 5);
			CHECK(row["wantedMip"] == 5);
			CHECK(row["mipCount"] == c_GradientTextureMips);
			CHECK(row["residentBytes"] == texture->GetLevelBytes(5, c_GradientTextureMips));
		}
		CHECK(listed);

		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
