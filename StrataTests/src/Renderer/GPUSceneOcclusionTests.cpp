#include "Renderer/SceneRendererTestUtils.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Occlusion culling on the GPU-driven path (SceneRendererSpecification::OcclusionCulling): what the view saw in the previous
// frame is drawn first, a depth pyramid of that tests the rest, and what is found visible is drawn in the same frame.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = 128;

	bool DeviceDrawsGPUDriven()
	{
		return Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance;
	}

	// The pixels of the entity in the renderer's entity-ID buffer (32-bit entity identifiers + 1).
	uint32_t CountEntityPixels(SceneRenderer& renderer, Entity entity)
	{
		ReadbackImage ids;
		REQUIRE(Renderer::ReadTexture(renderer.GetEntityIDTexture(), ids));
		const uint32_t expected = static_cast<uint32_t>(entity.GetHandle()) + 1;
		uint32_t pixels = 0;
		for (size_t offset = 0; offset + sizeof(uint32_t) <= ids.Pixels.size(); offset += sizeof(uint32_t))
		{
			uint32_t id = 0;
			std::memcpy(&id, ids.Pixels.data() + offset, sizeof(id));
			pixels += id == expected ? 1u : 0u;
		}
		return pixels;
	}

	// A wall with a box behind it, seen from in front of the wall.
	struct WallScene
	{
		Scene World;
		Entity Wall;
		Entity Box;

		explicit WallScene(SceneTestAssets& assets)
		{
			AddNeutralPostProcess(World);
			const AssetHandle material = assets.AddMaterial(UnlitColor(glm::vec4(0.8f, 0.8f, 0.8f, 1.0f)));
			Wall = AddMesh(World, BuiltinAssets::CubeMesh, material, glm::vec3(0.0f, 0.0f, -5.0f), "Wall");
			Wall.GetComponent<TransformComponent>().Scale = glm::vec3(8.0f, 8.0f, 0.5f);
			Box = AddMesh(World, BuiltinAssets::CubeMesh, material, glm::vec3(0.0f, 0.0f, -10.0f), "Box");
		}
	};

	// A frame, its counters read back.
	void Frame(GPUContext& gpu, SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		REQUIRE(gpu.GetDevice().BeginFrame());
		Renderer::BeginFrame();
		REQUIRE(renderer.Render(scene, camera));
		gpu.GetDevice().EndFrame();
		renderer.ResolveGPUStats(true);
		REQUIRE(renderer.GetGPUStats().CountersRender == renderer.GetRenderCount());
	}

	void CheckOcclusion(GPUContext& gpu, const SceneCamera& camera, const SceneCamera& aside)
	{
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		WallScene scene(assets);
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);

		// The first frame has no history: both are drawn (the second phase tests against an empty pyramid). Then the box,
		// drawn once more in the first phase, is found hidden and left out.
		Frame(gpu, renderer, scene.World, camera);
		CHECK(renderer.GetGPUStats().OccludedProxies == 0);
		CHECK(renderer.GetGPUStats().Instances == 2);
		Frame(gpu, renderer, scene.World, camera);
		Frame(gpu, renderer, scene.World, camera);
		CHECK(renderer.GetGPUStats().OccludedProxies == 1);
		CHECK(renderer.GetGPUStats().Instances == 1); // The wall alone
		CHECK(renderer.GetGPUStats().VisibleProxies == 2);
		CHECK(CountEntityPixels(renderer, scene.Box) == 0);
		CHECK(CountEntityPixels(renderer, scene.Wall) > 0);
		CHECK(AddGPUCounts(renderer.GetStats(), renderer.GetGPUStats()).OccludedProxies == 1);

		// The wall goes: the box shows in that same frame (the second phase finds it against the first's depth).
		scene.Wall.SetActive(false);
		Frame(gpu, renderer, scene.World, camera);
		CHECK(renderer.GetGPUStats().OccludedProxies == 0);
		CHECK(CountEntityPixels(renderer, scene.Box) > 0);

		// Back, the box is hidden again; the camera steps aside (no cut) and sees it beside the wall at once.
		scene.Wall.SetActive(true);
		Frame(gpu, renderer, scene.World, camera);
		Frame(gpu, renderer, scene.World, camera);
		CHECK(renderer.GetGPUStats().OccludedProxies == 1);
		CHECK(CountEntityPixels(renderer, scene.Box) == 0);
		Frame(gpu, renderer, scene.World, aside);
		CHECK(renderer.GetGPUStats().OccludedProxies == 0);
		CHECK(CountEntityPixels(renderer, scene.Box) > 0);

		// Without occlusion culling the box is drawn behind the wall.
		SceneRendererSpecification everything;
		everything.OcclusionCulling = false;
		SceneRenderer unculled(everything);
		unculled.SetViewportSize(c_Size, c_Size);
		for (int frame = 0; frame < 3; frame++)
			Frame(gpu, unculled, scene.World, camera);
		CHECK(unculled.GetGPUStats().OccludedProxies == 0);
		CHECK(unculled.GetGPUStats().Instances == 2);
		CHECK(CountEntityPixels(unculled, scene.Box) == 0);
		gpu.GetDevice().WaitForIdle();
	}

}

TEST_SUITE("GPU.SceneRenderer.Occlusion")
{
	TEST_CASE("A box behind a wall is left out, and drawn in the frame the wall goes or the camera steps aside")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -10.0f));
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(50.0f), 1.0f, 0.1f, 100.0f);
		camera.VerticalFOV = glm::radians(50.0f);
		// 9 units aside (a cut is more than 10), turned 39 degrees (less than 45): the box shows past the wall's edge.
		SceneCamera aside = LookAt(glm::vec3(9.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, -10.0f));
		aside.Projection = camera.Projection;
		aside.VerticalFOV = camera.VerticalFOV;
		CheckOcclusion(gpu, camera, aside);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Orthographic views are occlusion culled alike")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		const SceneCamera camera = OrthographicLookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -10.0f), 6.0f);
		// 8 units aside, turned 40 degrees (no cut): along the new direction the box shows past the wall's edge.
		const glm::vec3 position(8.0f, 0.0f, 0.0f);
		const glm::vec3 direction(-std::sin(glm::radians(40.0f)), 0.0f, -std::cos(glm::radians(40.0f)));
		const SceneCamera aside = OrthographicLookAt(position, position + direction * 10.0f, 6.0f);
		CheckOcclusion(gpu, camera, aside);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
