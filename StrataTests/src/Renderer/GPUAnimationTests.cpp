#include "Animation/AnimationTestUtils.h"
#include "Animation/SkinnedMeshTestUtils.h"
#include "Animation/TestSkinningPoses.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Animation/AnimationComponents.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/RendererComponents.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	using TestPoses = TestSkinningPoses;

	glm::mat4 Translation(const glm::vec3& offset)
	{
		return glm::translate(glm::mat4(1.0f), offset);
	}

	// The tube skinned on the CPU: every vertex blended by its influences' palette entries, normals and tangents with
	// them. Rendered unskinned, it is the reference the GPU's skinning must match.
	Ref<Mesh> SkinOnCPU(const std::vector<glm::vec3>& positions, const std::vector<MeshVertexAttributes>& attributes, const std::vector<MeshSkinVertex>& skin,
		const Mesh& mesh, const std::vector<glm::mat4>& palette)
	{
		std::vector<glm::vec3> skinnedPositions(positions.size());
		std::vector<MeshVertexAttributes> skinnedAttributes = attributes;
		for (size_t vertex = 0; vertex < positions.size(); vertex++)
		{
			skinnedPositions[vertex] = SkinPosition(positions[vertex], skin[vertex], palette);
			glm::mat4 blended(0.0f);
			for (int influence = 0; influence < 4; influence++)
				blended += palette[skin[vertex].Joints[influence]] * (static_cast<float>(skin[vertex].Weights[influence]) / 65535.0f);
			const glm::mat3 rotation(blended);
			skinnedAttributes[vertex].Normal = glm::transpose(glm::inverse(rotation)) * attributes[vertex].Normal;
			skinnedAttributes[vertex].Tangent = glm::vec4(rotation * glm::vec3(attributes[vertex].Tangent), attributes[vertex].Tangent.w);
		}
		std::string error;
		Ref<Mesh> reference = Mesh::Create(std::move(skinnedPositions), std::move(skinnedAttributes), mesh.GetIndices(), mesh.GetSubmeshes(), &error);
		REQUIRE_MESSAGE(reference, error);
		return reference;
	}

	uint32_t ReadEntityID(const ReadbackImage& image, uint32_t x, uint32_t y)
	{
		uint32_t id = 0;
		std::memcpy(&id, image.Pixels.data() + (static_cast<size_t>(y) * image.Width + x) * sizeof(uint32_t), sizeof(uint32_t));
		return id;
	}

	// The pixels covered by an entity in the renderer's entity-ID buffer.
	std::vector<uint8_t> EntityMask(SceneRenderer& renderer, Entity entity)
	{
		ReadbackImage ids;
		REQUIRE(Renderer::ReadTexture(renderer.GetEntityIDTexture(), ids));
		std::vector<uint8_t> mask(static_cast<size_t>(ids.Width) * ids.Height);
		for (uint32_t y = 0; y < ids.Height; y++)
		{
			for (uint32_t x = 0; x < ids.Width; x++)
				mask[static_cast<size_t>(y) * ids.Width + x] = ReadEntityID(ids, x, y) == static_cast<uint32_t>(entity.GetHandle()) + 1 ? 1 : 0;
		}
		return mask;
	}

	struct ArmScene
	{
		Scene SceneData;
		Entity Arm;
	};

	// A lit floor and the arm at `position`, posed through `poses` (Animator: the arm's own entity) or drawing `mesh`.
	void BuildArmScene(ArmScene& arm, AssetHandle mesh, AssetHandle material, const glm::vec3& position, bool skinned)
	{
		Scene& scene = arm.SceneData;
		AddNeutralPostProcess(scene);
		Entity floor = AddMesh(scene, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f), "Floor");
		floor.GetComponent<TransformComponent>().Scale = glm::vec3(12.0f);
		Entity sun = AddSun(scene, glm::normalize(glm::vec3(0.3f, -1.0f, -0.4f)), 3.0f);
		sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = 0.0f;
		arm.Arm = AddMesh(scene, mesh, material, position, "Arm");
		if (skinned)
			arm.Arm.AddComponent<SkinComponent>().Animator = arm.Arm.GetComponent<IDComponent>().ID;
	}

}

TEST_SUITE("GPU.Animation")
{
	TEST_CASE("A posed skinned mesh matches a CPU-skinned reference in the forward, entity-ID and shadow passes")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		// Every vertex follows one joint and the pose turns exact quarter turns, so the GPU's palette blend and the CPU's
		// skinning compute the same floats: the images must agree to the last bit (within 1/255 allows rounding).
		Ref<Mesh> tube = CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 4, .Sides = 12, .JointLength = 1.0f, .Radius = 0.25f, .BlendWeights = false });
		const std::vector<glm::vec3> positions = tube->GetPositions();
		const std::vector<MeshVertexAttributes> attributes = tube->GetAttributes();
		const std::vector<MeshSkinVertex> skin = tube->GetSkinVertices();
		const std::vector<glm::mat4> pose = BentChain(3, 1);
		std::vector<glm::mat4> palette(3);
		for (uint32_t joint = 0; joint < 3; joint++)
			palette[joint] = pose[joint] * tube->GetSkinBinding()->InverseBindMatrices[joint];
		Ref<Mesh> reference = SkinOnCPU(positions, attributes, skin, *tube, palette);
		MaterialProperties surface;
		surface.BaseColor = glm::vec4(0.8f, 0.5f, 0.2f, 1.0f);
		surface.Roughness = 0.7f;
		const AssetHandle material = assets.AddMaterial(surface);
		const AssetHandle skinnedMesh = assets.Add(tube, "Tube");
		const AssetHandle referenceMesh = assets.Add(reference, "Reference");

		constexpr uint32_t size = 160;
		const SceneCamera camera = LookAt(glm::vec3(1.5f, 3.0f, 7.0f), glm::vec3(-0.5f, 1.5f, 0.0f));
		const glm::vec3 position(0.5f, 0.25f, 0.0f);

		TestPoses poses;
		ArmScene skinnedScene;
		BuildArmScene(skinnedScene, skinnedMesh, material, position, true);
		poses.Poses[skinnedScene.Arm.GetHandle()] = pose;
		REQUIRE(skinnedScene.SceneData.RegisterService<ISkinningPoses>(poses));
		SceneRenderer skinnedRenderer;
		skinnedRenderer.SetViewportSize(size, size);
		const ReadbackImage skinnedImage = RenderToImage(skinnedRenderer, skinnedScene.SceneData, camera);
		CHECK(skinnedRenderer.GetStats().SkinnedInstances == 1);
		CHECK(skinnedRenderer.GetStats().PaletteBytesUploaded == 3 * sizeof(RenderData::AffineTransform));
		CHECK(RenderScene::Get(skinnedScene.SceneData).GetStats().PosedProxies == 1);
		const std::vector<uint8_t> skinnedMask = EntityMask(skinnedRenderer, skinnedScene.Arm);

		ArmScene referenceScene;
		BuildArmScene(referenceScene, referenceMesh, material, position, false);
		SceneRenderer referenceRenderer;
		referenceRenderer.SetViewportSize(size, size);
		const ReadbackImage referenceImage = RenderToImage(referenceRenderer, referenceScene.SceneData, camera);
		CHECK(referenceRenderer.GetStats().SkinnedInstances == 0);
		const std::vector<uint8_t> referenceMask = EntityMask(referenceRenderer, referenceScene.Arm);

		// Forward pass (and the floor's shadow, from the shadow pass): the same colors everywhere.
		CHECK(MaxDifference(skinnedImage, referenceImage) <= 1);
		// Entity-ID pass: the same pixels, and the pose put the bent part where the bind pose has none.
		CHECK(skinnedMask == referenceMask);
		const glm::uvec2 forearm = ProjectToPixel(camera, position + glm::vec3(-1.5f, 1.0f, 0.0f), size);
		CHECK(skinnedMask[static_cast<size_t>(forearm.y) * size + forearm.x] == 1);
		// The shadow of the bent forearm lies on the floor where the bind pose casts none.
		const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.3f, -1.0f, -0.4f));
		const glm::vec3 forearmPoint = position + glm::vec3(-1.5f, 1.0f, 0.0f);
		const glm::vec3 shadowPoint = forearmPoint + lightDirection * (forearmPoint.y / -lightDirection.y);
		const glm::vec3 litPoint = shadowPoint + glm::vec3(0.0f, 0.0f, 2.5f);
		CHECK(Red(skinnedImage, ProjectToPixel(camera, shadowPoint, size)) < Red(skinnedImage, ProjectToPixel(camera, litPoint, size)) / 2);
		CHECK(gpu.GetNewErrorCount() == 0);

		// Unchanged poses upload nothing; a new pose uploads only the palette.
		CHECK(skinnedRenderer.Render(skinnedScene.SceneData, camera));
		CHECK(skinnedRenderer.GetStats().PaletteBytesUploaded == 0);
		CHECK(skinnedRenderer.GetStats().InstanceBytesUploaded == 0);
		poses.Poses[skinnedScene.Arm.GetHandle()] = BentChain(3, 2);
		poses.Version++;
		CHECK(skinnedRenderer.Render(skinnedScene.SceneData, camera));
		CHECK(skinnedRenderer.GetStats().PaletteBytesUploaded == 3 * sizeof(RenderData::AffineTransform));
		CHECK(gpu.GetNewErrorCount() == 0);
		skinnedScene.SceneData.UnregisterService<ISkinningPoses>(poses);
	}

	TEST_CASE("A limb swung outside the bind-pose bounds is drawn and casts its shadow")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle material = assets.AddMaterial(UnlitColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)));
		MaterialProperties floorSurface;
		floorSurface.BaseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
		const AssetHandle floorMaterial = assets.AddMaterial(floorSurface);
		// A tall thin chain: its bind pose stands along +Y, the pose lays all of it along -X from the root.
		const AssetHandle tube = assets.Add(CreateSkinnedTube({ .Joints = 4, .RingsPerJoint = 2, .Sides = 8, .JointLength = 1.0f, .Radius = 0.2f }), "Tube");

		Scene scene;
		AddNeutralPostProcess(scene);
		Entity floor = AddMesh(scene, BuiltinAssets::PlaneMesh, floorMaterial, glm::vec3(-3.0f, 0.0f, 0.0f), "Floor");
		floor.GetComponent<TransformComponent>().Scale = glm::vec3(8.0f);
		Entity sun = AddSun(scene, glm::vec3(0.0f, -1.0f, 0.0f), 3.0f);
		sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = 0.0f;
		Entity arm = AddMesh(scene, tube, material, glm::vec3(0.0f, 1.0f, 0.0f), "Arm");
		arm.AddComponent<SkinComponent>().Animator = arm.GetComponent<IDComponent>().ID;
		TestPoses poses;
		std::vector<glm::mat4> pose = BentChain(4, 0);
		poses.Poses[arm.GetHandle()] = pose;
		REQUIRE(scene.RegisterService<ISkinningPoses>(poses));

		// The camera looks down at x = -3, where the swung limb lies: the bind pose's bounds (x within 0.2 of 0) are out of
		// view, so a renderer culling with them would draw nothing there.
		constexpr uint32_t size = 64;
		const SceneCamera camera = OrthographicLookAt(glm::vec3(-3.0f, 6.0f, 0.01f), glm::vec3(-3.0f, 0.0f, 0.0f), 2.0f);
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		ReadbackImage image = RenderToImage(renderer, scene, camera);
		const glm::uvec2 limb = ProjectToPixel(camera, glm::vec3(-3.0f, 1.0f, 0.0f), size);
		CHECK(renderer.GetStats().VisibleProxies == 2);
		CHECK(renderer.GetStats().SkinnedInstances == 1);
		CHECK(Red(image, limb) > 240);
		const std::vector<uint8_t> mask = EntityMask(renderer, arm);
		CHECK(mask[static_cast<size_t>(limb.y) * size + limb.x] == 1);
		const AABB& bounds = RenderScene::Get(scene).GetWorldBounds(RenderScene::Get(scene).FindProxy(arm.GetHandle()));
		CHECK(bounds.Min.x < -3.5f);
		CHECK(bounds.Max.y < 1.5f);
		// The renderer told the poses which animators it drew.
		CHECK(std::find(poses.Reported.begin(), poses.Reported.end(), arm.GetHandle()) != poses.Reported.end());

		// Seen at a slant, the floor straight under the swung limb (lit from straight above) lies in its shadow.
		const SceneCamera floorCamera = OrthographicLookAt(glm::vec3(-3.0f, 6.0f, 2.5f), glm::vec3(-3.0f, 0.0f, 0.5f), 3.0f);
		image = RenderToImage(renderer, scene, floorCamera);
		const glm::uvec2 shadowed = ProjectToPixel(floorCamera, glm::vec3(-3.0f, 0.0f, 0.0f), size);
		const glm::uvec2 lit = ProjectToPixel(floorCamera, glm::vec3(-3.0f, 0.0f, 0.9f), size);
		CHECK(renderer.GetStats().ShadowCasters > 0);
		CHECK(Red(image, shadowed) < Red(image, lit) / 3);
		CHECK(gpu.GetNewErrorCount() == 0);
		scene.UnregisterService<ISkinningPoses>(poses);
	}

	TEST_CASE("Blended skinning follows the CPU reference and skinned instances batch per mesh")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle material = assets.AddMaterial(UnlitColor(glm::vec4(0.9f, 0.9f, 0.9f, 1.0f)));
		Ref<Mesh> tube = CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 6, .Sides = 16, .JointLength = 1.0f, .Radius = 0.3f });
		const std::vector<glm::vec3> positions = tube->GetPositions();
		const std::vector<MeshVertexAttributes> attributes = tube->GetAttributes();
		const std::vector<MeshSkinVertex> skin = tube->GetSkinVertices();
		// A pose that is not exact: half turns of joints 1 and 2 about tilted axes.
		std::vector<glm::mat4> pose(3);
		pose[0] = glm::rotate(glm::mat4(1.0f), 0.3f, glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f)));
		pose[1] = pose[0] * Translation(glm::vec3(0.0f, 1.0f, 0.0f)) * glm::rotate(glm::mat4(1.0f), 0.8f, glm::vec3(0.0f, 0.0f, 1.0f));
		pose[2] = pose[1] * Translation(glm::vec3(0.0f, 1.0f, 0.0f)) * glm::rotate(glm::mat4(1.0f), 0.6f, glm::vec3(1.0f, 0.0f, 0.0f));
		std::vector<glm::mat4> palette(3);
		for (uint32_t joint = 0; joint < 3; joint++)
			palette[joint] = pose[joint] * tube->GetSkinBinding()->InverseBindMatrices[joint];
		const AssetHandle skinnedMesh = assets.Add(tube, "Tube");
		const AssetHandle referenceMesh = assets.Add(SkinOnCPU(positions, attributes, skin, *tube, palette), "Reference");

		constexpr uint32_t size = 128;
		const SceneCamera camera = LookAt(glm::vec3(3.0f, 2.0f, 6.0f), glm::vec3(-0.5f, 1.2f, 0.0f));
		TestPoses poses;
		Scene skinnedScene;
		AddNeutralPostProcess(skinnedScene);
		Entity arm = AddMesh(skinnedScene, skinnedMesh, material, glm::vec3(0.0f), "Arm");
		arm.AddComponent<SkinComponent>().Animator = arm.GetComponent<IDComponent>().ID;
		poses.Poses[arm.GetHandle()] = pose;
		REQUIRE(skinnedScene.RegisterService<ISkinningPoses>(poses));
		Scene referenceScene;
		AddNeutralPostProcess(referenceScene);
		Entity referenceArm = AddMesh(referenceScene, referenceMesh, material, glm::vec3(0.0f), "Arm");

		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		REQUIRE(renderer.Render(skinnedScene, camera));
		const std::vector<uint8_t> skinnedMask = EntityMask(renderer, arm);
		SceneRenderer referenceRenderer;
		referenceRenderer.SetViewportSize(size, size);
		REQUIRE(referenceRenderer.Render(referenceScene, camera));
		const std::vector<uint8_t> referenceMask = EntityMask(referenceRenderer, referenceArm);
		// Floating-point blending may move a silhouette pixel; every pixel inside both silhouettes agrees.
		size_t covered = 0;
		size_t differing = 0;
		for (size_t pixel = 0; pixel < skinnedMask.size(); pixel++)
		{
			covered += referenceMask[pixel];
			differing += skinnedMask[pixel] != referenceMask[pixel] ? 1u : 0u;
		}
		CHECK(covered > 300);
		CHECK(differing * 50 <= covered); // At most 2% of the silhouette, along its edge

		// Ten copies of the arm share one instanced draw per pass, each with its own palette.
		for (int copy = 1; copy < 10; copy++)
		{
			Entity other = AddMesh(skinnedScene, skinnedMesh, material, glm::vec3(static_cast<float>(copy) * 0.1f, 0.0f, -2.0f), "Copy");
			other.AddComponent<SkinComponent>().Animator = other.GetComponent<IDComponent>().ID;
			poses.Poses[other.GetHandle()] = pose;
		}
		REQUIRE(renderer.Render(skinnedScene, camera));
		CHECK(renderer.GetStats().SkinnedInstances == 10);
		CHECK(renderer.GetStats().OpaqueDrawCalls == 1);
		CHECK(RenderScene::Get(skinnedScene).GetStats().PaletteJoints == 30);

		// Without a pose the mesh shows its bind pose through the ordinary pipelines.
		poses.Poses.clear();
		poses.Version++;
		REQUIRE(renderer.Render(skinnedScene, camera));
		CHECK(renderer.GetStats().SkinnedInstances == 0);
		CHECK(RenderScene::Get(skinnedScene).GetStats().PosedProxies == 0);
		CHECK(RenderScene::Get(skinnedScene).GetStats().SkinProxies == 10);
		CHECK(gpu.GetNewErrorCount() == 0);
		skinnedScene.UnregisterService<ISkinningPoses>(poses);
	}

	TEST_CASE("Shadow casters out of view report their animators only when their shadows can fall into the view")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		MaterialProperties surface;
		surface.BaseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
		const AssetHandle material = assets.AddMaterial(surface);
		const AssetHandle tube = assets.Add(CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 2, .Sides = 8, .JointLength = 1.0f, .Radius = 0.25f }), "Tube");

		Scene scene;
		AddNeutralPostProcess(scene);
		Entity floor = AddMesh(scene, BuiltinAssets::PlaneMesh, material, glm::vec3(0.0f), "Floor");
		floor.GetComponent<TransformComponent>().Scale = glm::vec3(200.0f);
		// A low sun shining toward -Z: the shadow of a character 3 units tall reaches 5.2 along -Z.
		const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.0f, -std::sin(glm::radians(30.0f)), -std::cos(glm::radians(30.0f))));
		Entity sun = AddSun(scene, lightDirection, 3.0f);
		sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = 0.0f;
		TestPoses poses;
		auto addCharacter = [&](const glm::vec3& position, const char* name)
		{
			Entity character = AddMesh(scene, tube, material, position, name);
			character.AddComponent<SkinComponent>().Animator = character.GetComponent<IDComponent>().ID;
			poses.Poses[character.GetHandle()] = BentChain(3, 3);
			return character;
		};
		// Just behind the camera: out of view itself, its shadow falls on the floor in front of the camera.
		Entity shadowing = addCharacter(glm::vec3(0.0f, 0.0f, 1.5f), "Shadowing");
		// 30 units behind the camera, and far beside the view: neither they nor their shadows can be seen, although the
		// cascades (spheres around slices of the view, up to the shadow distance) take them in.
		Entity behind = addCharacter(glm::vec3(0.0f, 0.0f, 30.0f), "Behind");
		Entity beside = addCharacter(glm::vec3(40.0f, 0.0f, -20.0f), "Beside");
		REQUIRE(scene.RegisterService<ISkinningPoses>(poses));

		constexpr uint32_t size = 64;
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.6f, 0.0f), glm::vec3(0.0f, 0.0f, -3.0f));
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetStats().VisibleProxies == 1); // The floor
		const uint32_t castInstances = renderer.GetStats().ShadowCasters;
		auto reported = [&](Entity entity)
		{
			return std::find(poses.Reported.begin(), poses.Reported.end(), entity.GetHandle()) != poses.Reported.end();
		};
		CHECK(reported(shadowing));
		CHECK_FALSE(reported(behind));
		CHECK_FALSE(reported(beside));
		// The shadow the report is for: on the floor in front of the camera.
		const glm::uvec2 shaded = ProjectToPixel(camera, glm::vec3(0.0f, 0.0f, -3.0f), size);
		const glm::uvec2 lit = ProjectToPixel(camera, glm::vec3(1.0f, 0.0f, -3.0f), size);
		CHECK(Red(image, shaded) < Red(image, lit) / 2);
		// The characters behind and beside are shadow casters of the cascades (each one gone, fewer instances are drawn into
		// them): only the test of where their shadows can fall keeps their animators unreported.
		for (Entity unseen : { behind, beside })
		{
			unseen.SetActive(false);
			REQUIRE(renderer.Render(scene, camera));
			CHECK(renderer.GetStats().ShadowCasters < castInstances);
			unseen.SetActive(true);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
		scene.UnregisterService<ISkinningPoses>(poses);
	}

	TEST_CASE("A WhenVisible animator whose clip carries its mesh into a view that its first pose is not in is posed there")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle material = assets.AddMaterial(UnlitColor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)));
		const AssetHandle tube = assets.Add(CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 2, .Sides = 8, .JointLength = 1.0f, .Radius = 0.25f }), "Arm");
		const AssetHandle skeleton = assets.Add(CreateArmSkeleton(), "Skeleton");
		// The walk carries the arm 2 along +Z over its one-second loop (no root motion: the travel stays in the pose).
		const AssetHandle walk = assets.Add(CreateWaveClip(1.0f, 2.0f, "Walk"), "Walk");

		Scene scene;
		AddNeutralPostProcess(scene);
		Entity walker = scene.CreateEntity("Walker");
		AnimatorComponent& animator = walker.AddComponent<AnimatorComponent>();
		animator.Skeleton = skeleton;
		animator.Clip = walk;
		animator.CullingMode = AnimatorCullingMode::WhenVisible;
		Entity body = AddMesh(scene, tube, material, glm::vec3(0.0f), "Body");
		scene.SetParent(body, walker, false);
		body.AddComponent<SkinComponent>().Animator = walker.GetUUID();
		scene.OnRuntimeStart(SceneRuntimeMode::Play);

		// A narrow view of the place the arm reaches three quarters into the walk, far from where it starts.
		constexpr uint32_t size = 64;
		const SceneCamera camera = OrthographicLookAt(glm::vec3(6.0f, 0.5f, 1.5f), glm::vec3(0.0f, 0.5f, 1.5f), 0.6f);
		SceneRenderer renderer;
		renderer.SetViewportSize(size, size);
		for (int frame = 0; frame < 45; frame++)
		{
			scene.OnUpdateRuntime(1.0f / 60.0f);
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			REQUIRE(renderer.Render(scene, camera));
			gpu.GetDevice().EndFrame();
		}
		const std::vector<uint8_t> mask = EntityMask(renderer, body);
		const glm::uvec2 center = ProjectToPixel(camera, glm::vec3(0.0f, 0.5f, 1.5f), size);
		CHECK(mask[static_cast<size_t>(center.y) * size + center.x] == 1);
		CHECK(gpu.GetNewErrorCount() == 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A textured skinned mesh reports the texture maps it draws to the streamer as the mesh unskinned does")
	{
		// Mip streaming raises a texture's levels by what renderers report after each frame (GetVisibleTextures): a skinned
		// mesh reports the maps it draws with, by its posed bounds and its submesh's texel density, like any other mesh.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Ref<Mesh> tube = CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 4, .Sides = 12, .JointLength = 1.0f, .Radius = 0.25f, .BlendWeights = false });
		// The bind pose: joint j at j * JointLength, every palette entry the identity, so the reference is the tube itself.
		std::vector<glm::mat4> pose(3);
		std::vector<glm::mat4> palette(3);
		for (uint32_t joint = 0; joint < 3; joint++)
		{
			pose[joint] = Translation(glm::vec3(0.0f, static_cast<float>(joint), 0.0f));
			palette[joint] = pose[joint] * tube->GetSkinBinding()->InverseBindMatrices[joint];
		}
		Ref<Mesh> reference = SkinOnCPU(tube->GetPositions(), tube->GetAttributes(), tube->GetSkinVertices(), *tube, palette);
		const AssetHandle texture = assets.AddSolidTexture({ 200, 100, 50, 255 }, TextureFormat::RGBA8SRGB);
		MaterialProperties textured = UnlitColor({ 1.0f, 1.0f, 1.0f, 1.0f });
		textured.BaseColorMap = texture;
		const AssetHandle material = assets.AddMaterial(textured);
		const AssetHandle skinnedMesh = assets.Add(tube, "Tube");
		const AssetHandle referenceMesh = assets.Add(reference, "Reference");

		constexpr uint32_t size = 128;
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.5f, 5.0f), glm::vec3(0.0f, 1.5f, 0.0f));
		const auto report = [&](AssetHandle mesh, bool skinned)
		{
			Scene scene;
			AddNeutralPostProcess(scene);
			Entity arm = AddMesh(scene, mesh, material, glm::vec3(0.0f), "Arm");
			TestPoses poses;
			if (skinned)
			{
				arm.AddComponent<SkinComponent>().Animator = arm.GetUUID();
				poses.Poses[arm.GetHandle()] = pose;
				REQUIRE(scene.RegisterService<ISkinningPoses>(poses));
			}
			SceneRenderer renderer;
			renderer.SetViewportSize(size, size);
			REQUIRE(renderer.Render(scene, camera));
			CHECK(renderer.GetStats().SkinnedInstances == (skinned ? 1u : 0u));
			const std::span<const VisibleTextureUse> textures = renderer.GetVisibleTextures();
			REQUIRE(textures.size() == 1);
			const VisibleTextureUse use = textures[0];
			if (skinned)
				scene.UnregisterService<ISkinningPoses>(poses);
			return use;
		};
		const VisibleTextureUse skinnedUse = report(skinnedMesh, true);
		const VisibleTextureUse referenceUse = report(referenceMesh, false);
		CHECK(skinnedUse.Texture == texture);
		CHECK(referenceUse.Texture == texture);
		CHECK(skinnedUse.UVPixels > 0.0f);
		CHECK(skinnedUse.UVPixels == doctest::Approx(referenceUse.UVPixels).epsilon(0.1));
		CHECK(skinnedUse.PixelSize == doctest::Approx(referenceUse.PixelSize).epsilon(0.1));
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
