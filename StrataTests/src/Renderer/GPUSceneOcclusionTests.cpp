#include "Renderer/SceneRendererTestUtils.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Occlusion culling on the GPU-driven path (SceneRendererSpecification::OcclusionCulling): what the view saw in the previous
// frame is drawn first, a depth pyramid of that tests the rest, and what is found visible is drawn in the same frame. The
// shadow cascades are culled alike, each against a pyramid of its own shadow map.

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

	// A slab high above a box, under a sun shining almost straight down: the slab hides the box from the light (in every
	// cascade both fall in), while the camera, looking from the side, sees both.
	struct SlabScene
	{
		Scene World;
		Entity Slab;
		Entity Box;

		explicit SlabScene(SceneTestAssets& assets)
		{
			AddNeutralPostProcess(World);
			AddSun(World, glm::normalize(glm::vec3(0.1f, -1.0f, 0.05f)), 3.0f);
			MaterialProperties lit;
			lit.BaseColor = glm::vec4(0.7f, 0.7f, 0.75f, 1.0f);
			const AssetHandle material = assets.AddMaterial(lit);
			AddMesh(World, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
			Slab = AddMesh(World, BuiltinAssets::CubeMesh, material, glm::vec3(0.0f, 6.0f, 0.0f), "Slab");
			Slab.GetComponent<TransformComponent>().Scale = glm::vec3(6.0f, 0.5f, 6.0f);
			Box = AddMesh(World, BuiltinAssets::CubeMesh, material, glm::vec3(0.0f, 1.0f, 0.0f), "Box");
		}
	};

	SceneCamera SlabCamera()
	{
		SceneCamera camera = LookAt(glm::vec3(0.0f, 2.5f, 14.0f), glm::vec3(0.0f, 2.0f, 0.0f));
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(50.0f), 1.0f, 0.1f, 200.0f);
		camera.VerticalFOV = glm::radians(50.0f);
		camera.Far = 200.0f;
		return camera;
	}

	// Renders a frame with each renderer and checks that the culled one drew the same image, the casters it left out counted
	// as occluded (each caster in each cascade is drawn, found occluded, or both: drawn by the first phase, which draws what was
	// visible in the previous frame, and then found hidden).
	void CheckSameShadows(GPUContext& gpu, SceneRenderer& culled, SceneRenderer& unculled, Scene& scene, const SceneCamera& camera)
	{
		REQUIRE(gpu.GetDevice().BeginFrame());
		Renderer::BeginFrame();
		const ReadbackImage expected = RenderToImage(unculled, scene, camera);
		const ReadbackImage actual = RenderToImage(culled, scene, camera);
		gpu.GetDevice().EndFrame();
		culled.ResolveGPUStats(true);
		unculled.ResolveGPUStats(true);
		REQUIRE(culled.GetGPUStats().CountersRender == culled.GetRenderCount());
		REQUIRE(unculled.GetGPUStats().CountersRender == unculled.GetRenderCount());
		CHECK(MaxDifference(expected, actual) <= 1);
		CHECK(unculled.GetGPUStats().OccludedCasters == 0);
		CHECK(culled.GetGPUStats().ShadowInstances <= unculled.GetGPUStats().ShadowInstances);
		CHECK(culled.GetGPUStats().ShadowInstances + culled.GetGPUStats().OccludedCasters >= unculled.GetGPUStats().ShadowInstances);
		CHECK(culled.GetGPUStats().Overflow == 0);
	}

	// Renders frames as CheckSameShadows does until three in a row culled the cascades (at most 40): whether they did.
	bool RenderUntilCascadesCulled(GPUContext& gpu, SceneRenderer& culled, SceneRenderer& unculled, Scene& scene, const SceneCamera& camera)
	{
		uint32_t consecutive = 0;
		for (int frame = 0; frame < 40 && consecutive < 3; frame++)
		{
			CheckSameShadows(gpu, culled, unculled, scene, camera);
			consecutive = culled.GetStats().ShadowOcclusionCulled ? consecutive + 1 : 0u;
		}
		return consecutive == 3;
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

	TEST_CASE("A caster hidden from the light behind another is left out of its cascades, and drawn in the frame it shows")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		SlabScene scene(assets);
		SceneRenderer culled;
		culled.SetViewportSize(c_Size, c_Size);
		SceneRendererSpecification everything;
		everything.OcclusionCulling = false;
		SceneRenderer unculled(everything);
		unculled.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = SlabCamera();

		// The first frame has no history: the cascades are drawn whole, which records it. The second draws them all first and
		// finds the box hidden; from the third on it is left out, with the same shadows.
		CheckSameShadows(gpu, culled, unculled, scene.World, camera);
		CHECK_FALSE(culled.GetStats().ShadowOcclusionCulled);
		CHECK(culled.GetGPUStats().OccludedCasters == 0);
		CHECK(culled.GetGPUStats().ShadowInstances == unculled.GetGPUStats().ShadowInstances);
		CheckSameShadows(gpu, culled, unculled, scene.World, camera);
		CHECK(culled.GetStats().ShadowOcclusionCulled);
		CheckSameShadows(gpu, culled, unculled, scene.World, camera);
		CHECK(culled.GetStats().ShadowOcclusionCulled);
		const uint32_t occluded = culled.GetGPUStats().OccludedCasters;
		CHECK(occluded >= 1);
		CHECK(culled.GetGPUStats().ShadowInstances + occluded == unculled.GetGPUStats().ShadowInstances);
		CHECK(AddGPUCounts(culled.GetStats(), culled.GetGPUStats()).OccludedCasters == occluded);
		// The view's occlusion culling hides nothing here: the camera sees every mesh.
		CHECK(culled.GetGPUStats().OccludedProxies == 0);

		// The slab goes: the box's shadow is drawn in that same frame (the cascades' second phase finds it).
		scene.Slab.SetActive(false);
		CheckSameShadows(gpu, culled, unculled, scene.World, camera);
		CHECK(culled.GetStats().ShadowOcclusionCulled);
		CHECK(culled.GetGPUStats().OccludedCasters == 0);
		// Back, the box is hidden again once the culling looks again (it paused, finding nothing hidden).
		scene.Slab.SetActive(true);
		CHECK(RenderUntilCascadesCulled(gpu, culled, unculled, scene.World, camera));
		CHECK(culled.GetGPUStats().OccludedCasters == occluded);
		CHECK(culled.GetGPUStats().ShadowInstances + occluded == unculled.GetGPUStats().ShadowInstances);
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The cascades' occlusion culling pauses while it hides nothing, and resumes once it does")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		SlabScene scene(assets);
		scene.Slab.SetActive(false);
		SceneRenderer culled;
		culled.SetViewportSize(c_Size, c_Size);
		SceneRendererSpecification everything;
		everything.OcclusionCulling = false;
		SceneRenderer unculled(everything);
		unculled.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = SlabCamera();

		// The second frame culls the cascades (the first has no history, and nothing says yet that it does not pay); once a
		// frame's counters show nothing hidden, only every 16th frame looks again.
		CheckSameShadows(gpu, culled, unculled, scene.World, camera);
		CheckSameShadows(gpu, culled, unculled, scene.World, camera);
		CHECK(culled.GetStats().ShadowOcclusionCulled);
		CHECK(culled.GetGPUStats().OccludedCasters == 0);
		uint32_t culledFrames = 0;
		for (int frame = 0; frame < 48; frame++)
		{
			CheckSameShadows(gpu, culled, unculled, scene.World, camera);
			culledFrames += culled.GetStats().ShadowOcclusionCulled ? 1u : 0u;
		}
		CHECK(culledFrames == 3);

		// The slab comes: the next look finds the box hidden behind it (the frames that draw the cascades whole record their
		// history), and from then on every frame culls the cascades.
		scene.Slab.SetActive(true);
		CHECK(RenderUntilCascadesCulled(gpu, culled, unculled, scene.World, camera));
		CHECK(culled.GetGPUStats().OccludedCasters >= 1);
		CHECK(culled.GetGPUStats().ShadowInstances + culled.GetGPUStats().OccludedCasters == unculled.GetGPUStats().ShadowInstances);
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
