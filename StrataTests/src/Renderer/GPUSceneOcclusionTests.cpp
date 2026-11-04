#include "Renderer/SceneRendererTestUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
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

	// The global illumination's sun room (LookDevScene.json's room, as the radiance cascades' GIScenes.cpp builds it): a 6.4 m
	// room of plaster boxes on a 60 x 60 m ground under the look-dev sun, its south wall open in a 2 x 1.3 m window (or sealed
	// by a box) above a 2 x 0.9 x 0.2 m box. From inside, looking at the window, the box stands in front of the ground outside
	// (or the seal): nothing hides it.
	struct SunRoomScene
	{
		Scene World;
		Entity Below; // The box below the window

		SunRoomScene(SceneTestAssets& assets, bool sealed)
		{
			AddNeutralPostProcess(World);
			MaterialProperties groundProperties;
			groundProperties.BaseColor = glm::vec4(0.35f, 0.35f, 0.35f, 1.0f);
			groundProperties.Roughness = 0.85f;
			const AssetHandle ground = assets.AddMaterial(groundProperties);
			MaterialProperties plasterProperties;
			plasterProperties.BaseColor = glm::vec4(0.58f, 0.58f, 0.58f, 1.0f);
			plasterProperties.Roughness = 0.9f;
			const AssetHandle plaster = assets.AddMaterial(plasterProperties);
			AddMesh(World, BuiltinAssets::PlaneMesh, ground, glm::vec3(0.0f), "Ground").GetComponent<TransformComponent>().Scale = glm::vec3(60.0f, 1.0f, 60.0f);
			struct Part
			{
				glm::vec3 Center;
				glm::vec3 Size;
				const char* Name;
			};
			const Part parts[] = {
				{ { -10.0f, 0.05f, -2.0f }, { 6.4f, 0.1f, 6.4f }, "Floor" },
				{ { -10.0f, 3.1f, -2.0f }, { 6.4f, 0.2f, 6.4f }, "Ceiling" },
				{ { -13.1f, 1.5f, -2.0f }, { 0.2f, 3.0f, 6.4f }, "West Wall" },
				{ { -6.9f, 1.5f, -2.0f }, { 0.2f, 3.0f, 6.4f }, "East Wall" },
				{ { -10.0f, 1.5f, -5.1f }, { 6.4f, 3.0f, 0.2f }, "North Wall" },
				{ { -12.1f, 1.5f, 1.1f }, { 2.2f, 3.0f, 0.2f }, "Left Of Window" },
				{ { -7.9f, 1.5f, 1.1f }, { 2.2f, 3.0f, 0.2f }, "Right Of Window" },
				{ { -10.0f, 0.45f, 1.1f }, { 2.0f, 0.9f, 0.2f }, "Below Window" },
				{ { -10.0f, 2.6f, 1.1f }, { 2.0f, 0.8f, 0.2f }, "Above Window" },
				{ { -10.0f, 1.55f, 1.1f }, { 2.0f, 1.3f, 0.2f }, "Window Seal" }
			};
			for (const Part& part : parts)
			{
				const bool seal = std::strcmp(part.Name, "Window Seal") == 0;
				if (seal && !sealed)
					continue;
				Entity entity = AddMesh(World, BuiltinAssets::CubeMesh, plaster, part.Center, part.Name);
				entity.GetComponent<TransformComponent>().Scale = part.Size;
				if (std::strcmp(part.Name, "Below Window") == 0)
					Below = entity;
			}
			// The look-dev sun, rotated by (-40, 30, 0) degrees, at 100,000 lux.
			AddSun(World, glm::normalize(Math::EulerDegreesToQuat(glm::vec3(-40.0f, 30.0f, 0.0f)) * glm::vec3(0.0f, 0.0f, -1.0f)), 100000.0f);
		}
	};

	// The GI's room camera: from `position` toward the window at (-10, 1.2, 1), 70 degrees.
	SceneCamera SunRoomCamera(const glm::vec3& position, glm::uvec2 size)
	{
		const float aspectRatio = static_cast<float>(size.x) / static_cast<float>(size.y);
		SceneCamera camera = LookAt(position, glm::vec3(-10.0f, 1.2f, 1.0f), aspectRatio);
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(70.0f), aspectRatio, 0.05f, 1000.0f);
		camera.Near = 0.05f;
		camera.Far = 1000.0f;
		camera.VerticalFOV = glm::radians(70.0f);
		return camera;
	}

	// The pixels of each entity in the renderer's entity-ID buffer, by identifier (entity + 1; 0 where nothing was drawn).
	std::map<uint32_t, uint32_t> CountEntityIDs(SceneRenderer& renderer)
	{
		ReadbackImage ids;
		REQUIRE(Renderer::ReadTexture(renderer.GetEntityIDTexture(), ids));
		std::map<uint32_t, uint32_t> counts;
		for (size_t offset = 0; offset + sizeof(uint32_t) <= ids.Pixels.size(); offset += sizeof(uint32_t))
		{
			uint32_t id = 0;
			std::memcpy(&id, ids.Pixels.data() + offset, sizeof(id));
			counts[id]++;
		}
		return counts;
	}

	// The R32 values of a readback (the depth buffer's D32, or a level of the depth pyramid), row by row.
	std::vector<float> ReadFloats(const ReadbackImage& image)
	{
		REQUIRE(image.Pixels.size() == static_cast<size_t>(image.Width) * image.Height * sizeof(float));
		std::vector<float> values(static_cast<size_t>(image.Width) * image.Height);
		std::memcpy(values.data(), image.Pixels.data(), image.Pixels.size());
		return values;
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

	TEST_CASE("A box behind a wall whose screen rectangle reaches the view's last row and column is left out, at odd and even sizes")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		WallScene scene(assets);
		// The wall covers the whole view; the box behind it reaches past the view's right and bottom edges, from about the
		// middle: its footprint is chosen at a coarse level (7: 3 x 2 texels at both sizes) where the pixels of the view's last
		// row and column, shifted, lie past the level's last texel. Read clamped, that texel holds the wall's depth; read past
		// the mip, the result is undefined (on common drivers 0, the farthest depth, which keeps the box).
		scene.Wall.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f, 40.0f, 0.5f);
		TransformComponent& box = scene.Box.GetComponent<TransformComponent>();
		box.Translation = glm::vec3(8.5f, -4.5f, -10.0f);
		box.Scale = glm::vec3(11.0f, 6.0f, 1.0f);
		scene.Wall.MarkModified<TransformComponent>();
		scene.Box.MarkModified<TransformComponent>();
		for (const glm::uvec2 size : { glm::uvec2(960, 540), glm::uvec2(961, 541) })
		{
			INFO(size.x << " x " << size.y);
			scene.Wall.SetActive(true);
			const SceneCamera camera = LookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -10.0f), static_cast<float>(size.x) / static_cast<float>(size.y));
			SceneRenderer renderer(ExactSpecification());
			renderer.SetViewportSize(size.x, size.y);
			for (int frame = 0; frame < 3; frame++)
				Frame(gpu, renderer, scene.World, camera);
			CHECK(renderer.GetGPUStats().VisibleProxies == 2);
			CHECK(renderer.GetGPUStats().OccludedProxies == 1);
			CHECK(renderer.GetGPUStats().Instances == 1); // The wall alone
			CHECK(CountEntityPixels(renderer, scene.Box) == 0);

			// The box is in view: without the wall it shows.
			scene.Wall.SetActive(false);
			Frame(gpu, renderer, scene.World, camera);
			CHECK(renderer.GetGPUStats().OccludedProxies == 0);
			CHECK(CountEntityPixels(renderer, scene.Box) > 0);
		}
		gpu.GetDevice().WaitForIdle();
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
		// Every frame draws the cascades (no layer kept from the previous frame), so each frame's culling is measured.
		SceneRendererSpecification redrawn;
		redrawn.CacheShadowCascades = false;
		SceneRenderer culled(redrawn);
		culled.SetViewportSize(c_Size, c_Size);
		SceneRendererSpecification everything = redrawn;
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
		// Every caster the cascades draw now they drew the frame before: the first phase draws them all.
		CHECK(culled.GetGPUStats().ShadowDraws > 0);
		CHECK(culled.GetGPUStats().SecondPhaseShadowDraws == 0);
		// The view's occlusion culling hides nothing here: the camera sees every mesh.
		CHECK(culled.GetGPUStats().OccludedProxies == 0);

		// The slab goes: the box's shadow is drawn in that same frame (the cascades' second phase finds it).
		scene.Slab.SetActive(false);
		CheckSameShadows(gpu, culled, unculled, scene.World, camera);
		CHECK(culled.GetStats().ShadowOcclusionCulled);
		CHECK(culled.GetGPUStats().OccludedCasters == 0);
		CHECK(culled.GetGPUStats().SecondPhaseShadowDraws >= 1);
		CHECK(culled.GetGPUStats().SecondPhaseShadowDraws < culled.GetGPUStats().ShadowDraws);
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
		SceneRendererSpecification redrawn;
		redrawn.CacheShadowCascades = false;
		SceneRenderer culled(redrawn);
		culled.SetViewportSize(c_Size, c_Size);
		SceneRendererSpecification everything = redrawn;
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

	TEST_CASE("A box in plain view below a window is drawn from cameras about the window's axis, at odd and even sizes")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		SceneRendererSpecification everything = ExactSpecification();
		everything.OcclusionCulling = false;
		// The window open, and sealed: then everything around the box (the seal, the walls, the floor) is as near as its
		// nearest point or nearer, and only the box's own pixels show what is farther (the ground behind it, in the depth of a
		// phase that did not draw it). A pyramid read off those pixels by a texel hides it.
		for (const bool sealed : { false, true })
		{
			SunRoomScene scene(assets, sealed);
			const uint32_t below = static_cast<uint32_t>(scene.Below.GetHandle()) + 1;
			// 960 x 540 (the GI stream's) halves to 135 rows at the pyramid's level 1, and the others to odd rows or columns at
			// other levels; 1280 x 720 halves evenly down to level 3.
			for (const glm::uvec2 size : { glm::uvec2(960, 540), glm::uvec2(961, 541), glm::uvec2(1280, 720), glm::uvec2(1001, 563) })
			{
				SceneRenderer culled(ExactSpecification());
				culled.SetViewportSize(size.x, size.y);
				SceneRenderer unculled(everything);
				unculled.SetViewportSize(size.x, size.y);
				// The GI stream's camera at (-10, 1.6, -4), those within 0.3 m of it where the box went missing, and beyond.
				// The camera moves on without a cut, so each position starts from the previous one's visibility.
				for (const float x : { -10.4f, -10.3f, -10.15f, -10.001f, -10.0f, -9.99f, -9.85f, -9.7f, -9.0f })
				{
					INFO(std::string(sealed ? "sealed" : "open") << ", " << size.x << " x " << size.y << ", the camera at x " << x);
					const SceneCamera camera = SunRoomCamera(glm::vec3(x, 1.6f, -4.0f), size);
					for (int frame = 0; frame < 3; frame++)
						Frame(gpu, culled, scene.World, camera);
					Frame(gpu, unculled, scene.World, camera);
					// Nothing in the room is hidden from inside it, and everything the unculled renderer shows the culled one
					// shows.
					CHECK(culled.GetGPUStats().OccludedProxies == 0);
					const std::map<uint32_t, uint32_t> expected = CountEntityIDs(unculled);
					const std::map<uint32_t, uint32_t> actual = CountEntityIDs(culled);
					REQUIRE(expected.count(below) == 1);
					CHECK(actual.count(below) == 1);
					for (const std::pair<const uint32_t, uint32_t>& entry : expected)
					{
						INFO("entity " << entry.first - 1 << " (" << entry.second << " pixels unculled)");
						CHECK((entry.first == 0 || actual.count(entry.first) == 1));
					}
				}
			}
		}
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Each depth pyramid texel holds the farthest depth of the pixels it covers, a level's last row and column those past it too")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!DeviceDrawsGPUDriven())
			return;
		SceneTestAssets assets;
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::CubeMesh));
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::PlaneMesh));
		SunRoomScene scene(assets, false);
		// Even and odd rows and columns at every level: 960 x 540 has 135 rows at level 1, 961 x 541 an odd level 0, and 37 x 5
		// a level of 3 rows that halves to 1 and levels of one row that stay one. Built with the minimum sampler (where the
		// device has one) and with four loads per 2x2 (as devices without one build it).
		for (const bool minimumSampler : { true, false })
		{
			for (const glm::uvec2 size : { glm::uvec2(960, 540), glm::uvec2(961, 541), glm::uvec2(37, 5) })
			{
				INFO(size.x << " x " << size.y << (minimumSampler ? ", minimum sampler" : ", four loads"));
				SceneRendererSpecification specification = ExactSpecification();
				specification.MinimumSamplerPyramids = minimumSampler;
				SceneRenderer renderer(specification);
				renderer.SetViewportSize(size.x, size.y);
				const SceneCamera camera = SunRoomCamera(glm::vec3(-10.0f, 1.6f, -4.0f), size);
				// Settled frames: the first phase draws everything in view, so the final depth is the one the pyramid reduces.
				for (int frame = 0; frame < 3; frame++)
					Frame(gpu, renderer, scene.World, camera);
				nvrhi::ITexture* pyramid = renderer.GetDepthPyramid();
				REQUIRE(pyramid != nullptr);
				ReadbackImage depthImage;
				REQUIRE(Renderer::ReadTexture(renderer.GetDepthTexture(), depthImage));
				REQUIRE(depthImage.Width == size.x);
				REQUIRE(depthImage.Height == size.y);
				const std::vector<float> depth = ReadFloats(depthImage);

				// Level 0 is half the depth buffer, rounded up; each level below it half the one above, rounded down (the
				// texture's own mip sizes), down to one texel.
				const glm::uvec2 levelZero = (size + 1u) / 2u;
				const uint32_t levels = pyramid->getDesc().mipLevels;
				CHECK((std::max(levelZero.x, levelZero.y) >> (levels - 1)) == 1u);
				for (uint32_t level = 0; level < levels; level++)
				{
					INFO("level " << level);
					const glm::uvec2 levelSize = glm::max(levelZero >> level, glm::uvec2(1));
					ReadbackImage image;
					REQUIRE(Renderer::ReadTexture(pyramid, image, level));
					REQUIRE(image.Width == levelSize.x);
					REQUIRE(image.Height == levelSize.y);
					const std::vector<float> values = ReadFloats(image);
					// Pixel p belongs to texel p >> (level + 1), the last row and column also to those past them: Cull.comp reads
					// that texel for it.
					std::vector<float> expected(values.size(), std::numeric_limits<float>::infinity());
					for (uint32_t y = 0; y < size.y; y++)
					{
						const uint32_t row = std::min(y >> (level + 1), levelSize.y - 1);
						for (uint32_t x = 0; x < size.x; x++)
						{
							const uint32_t column = std::min(x >> (level + 1), levelSize.x - 1);
							float& farthest = expected[static_cast<size_t>(row) * levelSize.x + column];
							farthest = std::min(farthest, depth[static_cast<size_t>(y) * size.x + x]);
						}
					}
					uint32_t wrong = 0;
					uint32_t nearer = 0; // Not conservative: nearer than a pixel it covers, which can hide what that pixel shows
					for (size_t texel = 0; texel < values.size(); texel++)
					{
						wrong += values[texel] != expected[texel] ? 1u : 0u;
						nearer += values[texel] > expected[texel] ? 1u : 0u;
					}
					CHECK(nearer == 0u);
					CHECK(wrong == 0u);
				}
			}
		}
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
