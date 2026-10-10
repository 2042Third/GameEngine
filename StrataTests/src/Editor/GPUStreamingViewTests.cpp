#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/StreamingView.h"
#include "Renderer/SceneRendererTestUtils.h"

#include <Strata/Core/JsonUtils.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Renderer/MeshFactory.h>

#include <chrono>
#include <cstring>
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
}
