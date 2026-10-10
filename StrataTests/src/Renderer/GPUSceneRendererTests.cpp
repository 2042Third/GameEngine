#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <map>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Size = c_SceneTestSize;

	// A sphere with three levels of detail (each simplified level has fewer triangles).
	Ref<Mesh> CreateLODSphere(std::vector<uint32_t>& outTriangleCounts)
	{
		Ref<Mesh> source = MeshFactory::CreateSphere(0.5f, 48, 24);
		REQUIRE(source);
		std::vector<uint32_t> indices = source->GetIndices();
		std::vector<std::vector<uint32_t>> levels = MeshUtils::GenerateLODs(source->GetPositions(), indices, 2);
		REQUIRE(levels.size() == 2);

		Submesh submesh = source->GetSubmeshes()[0];
		submesh.LODs = { MeshLOD { 0, static_cast<uint32_t>(indices.size()) } };
		outTriangleCounts = { static_cast<uint32_t>(indices.size() / 3) };
		for (const std::vector<uint32_t>& level : levels)
		{
			submesh.LODs.push_back(MeshLOD { static_cast<uint32_t>(indices.size()), static_cast<uint32_t>(level.size()) });
			outTriangleCounts.push_back(static_cast<uint32_t>(level.size() / 3));
			indices.insert(indices.end(), level.begin(), level.end());
		}
		REQUIRE(outTriangleCounts[1] < outTriangleCounts[0]);
		REQUIRE(outTriangleCounts[2] < outTriangleCounts[1]);
		std::string error;
		Ref<Mesh> mesh = Mesh::Create(source->GetPositions(), source->GetAttributes(), std::move(indices), { submesh }, &error);
		REQUIRE_MESSAGE(mesh, error);
		return mesh;
	}

	glm::vec3 SRGBToLinear(const glm::u8vec4& pixel)
	{
		auto decode = [](uint8_t value)
		{
			const float encoded = static_cast<float>(value) / 255.0f;
			return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
		};
		return glm::vec3(decode(pixel.r), decode(pixel.g), decode(pixel.b));
	}

}

TEST_SUITE("GPU.SceneRenderer")
{
	TEST_CASE("Lit meshes render, empty pixels show the clear color, and entities can be picked")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		Entity cube = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f), "Cube");
		Entity sun = scene.CreateEntity("Sun");
		sun.AddComponent<DirectionalLightComponent>().Intensity = 2.0f; // Shines along -Z, toward the cube's +Z face

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f));
		camera.ClearColor = { 0.2f, 0.4f, 0.6f, 1.0f };
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetStats().Rendered);

		const glm::u8vec4 center = GetPixelRGBA8(image, c_Size / 2, c_Size / 2);
		CHECK(center.r > 150); // White default material facing the light (the default ambient is slightly blue)
		CHECK(std::abs(center.r - center.g) <= 2);
		CHECK(std::abs(center.g - center.b) <= 2);
		const glm::u8vec4 corner = GetPixelRGBA8(image, 1, 1);
		CHECK(std::abs(corner.r - 51) <= 1); // Clear color passes through unchanged (sRGB) with neutral post-processing
		CHECK(std::abs(corner.g - 102) <= 1);
		CHECK(std::abs(corner.b - 153) <= 1);

		CHECK(renderer.GetEntityAt(scene, c_Size / 2, c_Size / 2) == cube);
		CHECK_FALSE(renderer.GetEntityAt(scene, 1, 1));
		CHECK_FALSE(renderer.GetEntityAt(scene, c_Size, 0)); // Outside the viewport

		const SceneRendererStats& stats = renderer.GetStats();
		CHECK(stats.Instances == 1);
		CHECK(stats.DrawCalls == 1);
		CHECK(stats.Triangles == 12);
		CHECK(stats.Lights == 1);
		CHECK(stats.InstancesPerLOD[0] == 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Front faces are counter-clockwise and back faces are culled unless double-sided")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle red = assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f }));
		MaterialProperties doubleSidedProperties = UnlitColor({ 0.0f, 1.0f, 0.0f, 1.0f });
		doubleSidedProperties.DoubleSided = true;
		const AssetHandle doubleSided = assets.AddMaterial(doubleSidedProperties);

		Scene scene;
		AddNeutralPostProcess(scene);
		Entity quad = AddMesh(scene, BuiltinAssets::QuadMesh, red, glm::vec3(0.0f), "Quad"); // Faces +Z

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);

		const ReadbackImage front = RenderToImage(renderer, scene, LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f)));
		CHECK(GetPixelRGBA8(front, c_Size / 2, c_Size / 2) == glm::u8vec4(255, 0, 0, 255));

		const ReadbackImage back = RenderToImage(renderer, scene, LookAt(glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.0f)));
		CHECK(GetPixelRGBA8(back, c_Size / 2, c_Size / 2) == glm::u8vec4(0, 0, 0, 255));

		quad.GetComponent<MeshRendererComponent>().Material = doubleSided;
		const ReadbackImage backDoubleSided = RenderToImage(renderer, scene, LookAt(glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.0f)));
		CHECK(GetPixelRGBA8(backDoubleSided, c_Size / 2, c_Size / 2) == glm::u8vec4(0, 255, 0, 255));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Mirrored transforms keep their outside facing out")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle red = assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f }));

		Scene scene;
		AddNeutralPostProcess(scene);
		Entity cube = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f), "Cube");
		AddSun(scene, glm::vec3(0.3f, -0.2f, -1.0f), 3.0f, false);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(1.5f, 1.2f, 3.0f), glm::vec3(0.0f));
		const ReadbackImage reference = RenderToImage(renderer, scene, camera);

		// A cube is symmetric: mirrored along any axis it must look exactly the same. Rendered inside out, the far
		// (unlit) faces would show instead.
		for (const glm::vec3& scale : { glm::vec3(-1.0f, 1.0f, 1.0f), glm::vec3(1.0f, -1.0f, 1.0f), glm::vec3(-1.0f, -1.0f, -1.0f) })
		{
			CAPTURE(scale);
			cube.GetComponent<TransformComponent>().Scale = scale;
			cube.MarkModified<TransformComponent>();
			const ReadbackImage mirrored = RenderToImage(renderer, scene, camera);
			CHECK(MaxDifference(mirrored, reference) <= 2);
			CHECK(renderer.GetEntityAt(scene, c_Size / 2, c_Size / 2) == cube);
		}

		// Mirrored and regular instances of one mesh are drawn in separate batches with opposite winding.
		cube.GetComponent<TransformComponent>().Scale = glm::vec3(-1.0f, 1.0f, 1.0f);
		cube.MarkModified<TransformComponent>();
		AddMesh(scene, BuiltinAssets::CubeMesh, red, glm::vec3(0.0f, 0.0f, -3.0f), "Regular");
		Entity mirroredQuad = AddMesh(scene, BuiltinAssets::QuadMesh, red, glm::vec3(0.0f, 0.0f, 1.5f), "MirroredQuad");
		mirroredQuad.GetComponent<TransformComponent>().Scale = glm::vec3(0.2f, -0.2f, 0.2f); // Still faces +Z
		CHECK(renderer.Render(scene, camera));
		CHECK(renderer.GetStats().DrawCalls == 3);
		const glm::uvec2 quadPixel = ProjectToPixel(camera, glm::vec3(0.0f, 0.0f, 1.5f), c_Size);
		CHECK(renderer.GetEntityAt(scene, quadPixel.x, quadPixel.y) == mirroredQuad);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Transparent surfaces blend over opaque ones and the nearest surface wins depth")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle red = assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f }));
		const AssetHandle blue = assets.AddMaterial(UnlitColor({ 0.0f, 0.0f, 1.0f, 1.0f }));
		MaterialProperties glassProperties = UnlitColor({ 0.0f, 1.0f, 0.0f, 0.5f });
		glassProperties.AlphaMode = MaterialAlphaMode::Blend;
		const AssetHandle glass = assets.AddMaterial(glassProperties);

		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::QuadMesh, red, glm::vec3(0.0f, 0.0f, 0.0f), "Back");
		Entity front = AddMesh(scene, BuiltinAssets::QuadMesh, blue, glm::vec3(0.0f, 0.0f, 0.5f), "Front");
		front.GetComponent<TransformComponent>().Scale = glm::vec3(0.5f); // Covers only the center

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));
		ReadbackImage image = RenderToImage(renderer, scene, camera);
		CHECK(GetPixelRGBA8(image, c_Size / 2, c_Size / 2) == glm::u8vec4(0, 0, 255, 255)); // Nearer quad
		CHECK(GetPixelRGBA8(image, c_Size / 2 + 12, c_Size / 2) == glm::u8vec4(255, 0, 0, 255));

		AddMesh(scene, BuiltinAssets::QuadMesh, glass, glm::vec3(0.0f, 0.0f, 1.0f), "Glass");
		image = RenderToImage(renderer, scene, camera);
		// 50% green over red in linear space, then sRGB-encoded: about 188 for both channels.
		const glm::u8vec4 blended = GetPixelRGBA8(image, c_Size / 2 + 12, c_Size / 2);
		CHECK(std::abs(blended.r - 188) <= 2);
		CHECK(std::abs(blended.g - 188) <= 2);
		CHECK(blended.b == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Overlapping transparent surfaces are drawn back to front")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		auto glass = [&](const glm::vec3& color)
		{
			MaterialProperties properties = UnlitColor(glm::vec4(color, 0.5f));
			properties.AlphaMode = MaterialAlphaMode::Blend;
			properties.DoubleSided = true;
			return assets.AddMaterial(properties);
		};

		Scene scene;
		AddNeutralPostProcess(scene);
		// Registered nearest first, so drawing in registry order would be wrong from +Z.
		AddMesh(scene, BuiltinAssets::QuadMesh, glass({ 1.0f, 0.0f, 0.0f }), glm::vec3(0.0f, 0.0f, 0.5f), "Red");
		AddMesh(scene, BuiltinAssets::QuadMesh, glass({ 0.0f, 1.0f, 0.0f }), glm::vec3(0.0f, 0.0f, 0.0f), "Green");

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		// Front surface at 50% over (back surface at 50% over black): front 0.5, back 0.25 (linear) = 188 and 137 (sRGB).
		const glm::u8vec4 fromFront = GetPixelRGBA8(RenderToImage(renderer, scene, LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f))), c_Size / 2, c_Size / 2);
		CHECK(std::abs(fromFront.r - 188) <= 2);
		CHECK(std::abs(fromFront.g - 137) <= 2);
		const glm::u8vec4 fromBehind = GetPixelRGBA8(RenderToImage(renderer, scene, LookAt(glm::vec3(0.0f, 0.0f, -3.0f), glm::vec3(0.0f))), c_Size / 2, c_Size / 2);
		CHECK(std::abs(fromBehind.r - 137) <= 2);
		CHECK(std::abs(fromBehind.g - 188) <= 2);
		CHECK(renderer.GetStats().DrawCalls == 2);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Identical meshes with the same material are drawn with one instanced draw call")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle red = assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f }));
		const AssetHandle green = assets.AddMaterial(UnlitColor({ 0.0f, 1.0f, 0.0f, 1.0f }));

		Scene scene;
		AddNeutralPostProcess(scene);
		std::vector<Entity> cubes;
		for (int index = 0; index < 5; index++)
		{
			Entity cube = AddMesh(scene, BuiltinAssets::CubeMesh, red, glm::vec3(static_cast<float>(index - 2) * 1.5f, 0.0f, 0.0f), "Cube");
			cube.GetComponent<TransformComponent>().Scale = glm::vec3(0.8f);
			cubes.push_back(cube);
		}

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 7.0f), glm::vec3(0.0f));
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetStats().DrawCalls == 1);
		CHECK(renderer.GetStats().Instances == 5);
		CHECK(renderer.GetStats().Triangles == 60);
		for (size_t index = 0; index < cubes.size(); index++)
		{
			const glm::uvec2 pixel = ProjectToPixel(camera, glm::vec3(static_cast<float>(index) * 1.5f - 3.0f, 0.0f, 0.4f), c_Size);
			CHECK(GetPixelRGBA8(image, pixel.x, pixel.y) == glm::u8vec4(255, 0, 0, 255));
			CHECK(renderer.GetEntityAt(scene, pixel.x, pixel.y) == cubes[index]);
		}

		// A different material splits the batch.
		cubes[4].GetComponent<MeshRendererComponent>().Material = green;
		renderer.Render(scene, camera);
		CHECK(renderer.GetStats().DrawCalls == 2);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The level of detail follows the projected size")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		std::vector<uint32_t> triangles;
		const AssetHandle sphere = assets.Add(CreateLODSphere(triangles), "LODSphere");

		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, sphere, assets.AddMaterial(UnlitColor({ 1.0f, 1.0f, 1.0f, 1.0f })), glm::vec3(0.0f), "Sphere");

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		// Bounding sphere radius 0.87: the screen fraction is 0.75 at 2 m (LOD 0), 0.3 at 5 m (LOD 1) and 0.125 at
		// 12 m (LOD 3, clamped to the last level, 2).
		const std::map<float, uint32_t> expected = { { 2.0f, 0u }, { 5.0f, 1u }, { 12.0f, 2u } };
		for (const auto& [distance, lod] : expected)
		{
			CAPTURE(distance);
			const ReadbackImage image = RenderToImage(renderer, scene, LookAt(glm::vec3(0.0f, 0.0f, distance), glm::vec3(0.0f)));
			const SceneRendererStats& stats = renderer.GetStats();
			CHECK(stats.InstancesPerLOD[lod] == 1);
			CHECK(stats.Instances == 1);
			CHECK(stats.Triangles == triangles[lod]);
			CHECK(GetPixelRGBA8(image, c_Size / 2, c_Size / 2) == glm::u8vec4(255, 255, 255, 255));
		}

		// A higher threshold switches earlier.
		SceneRendererSpecification specification;
		specification.LODThreshold = 2.0f;
		SceneRenderer eager(specification);
		eager.SetViewportSize(c_Size, c_Size);
		eager.Render(scene, LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f)));
		CHECK(eager.GetStats().InstancesPerLOD[2] == 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Invisible, culled and inactive entities are not drawn")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddMesh(scene, BuiltinAssets::SphereMesh, UUID::Null(), glm::vec3(0.0f), "Visible");
		AddMesh(scene, BuiltinAssets::SphereMesh, UUID::Null(), glm::vec3(0.0f, 0.0f, 10.0f), "Behind");
		Entity inactive = AddMesh(scene, BuiltinAssets::SphereMesh, UUID::Null(), glm::vec3(0.5f, 0.0f, 0.0f), "Inactive");
		inactive.SetActive(false);
		Entity zeroScale = AddMesh(scene, BuiltinAssets::SphereMesh, UUID::Null(), glm::vec3(-0.5f, 0.0f, 0.0f), "ZeroScale");
		zeroScale.GetComponent<TransformComponent>().Scale = glm::vec3(0.0f);
		Entity infinite = AddMesh(scene, BuiltinAssets::SphereMesh, UUID::Null(), glm::vec3(0.0f), "Infinite");
		infinite.GetComponent<TransformComponent>().Translation = glm::vec3(std::numeric_limits<float>::infinity(), 0.0f, 0.0f);
		AddMesh(scene, UUID(0x7777), UUID::Null(), glm::vec3(0.0f), "UnknownMesh");

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		CHECK(renderer.Render(scene, LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f))));
		const SceneRendererStats& stats = renderer.GetStats();
		CHECK(stats.Instances == 1);
		CHECK(stats.CulledInstances == 1);
		CHECK(stats.PendingAssets == 0); // Unknown handles are not "loading"
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Assets still loading are counted and drawn with fallbacks")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());

		// A pack with a mesh, a material and a texture. Loads finish on worker threads but are only published by the
		// asset manager's main-thread update, so the first frame sees all three still loading.
		TextureMip level0;
		level0.Width = 2;
		level0.Height = 2;
		for (int texel = 0; texel < 4; texel++)
			AppendPixel(level0.Data, 0, 255, 0, 255);
		TextureSpecification textureSpecification;
		textureSpecification.Format = TextureFormat::RGBA8SRGB;
		Ref<Texture> texture = Texture::Create(textureSpecification, { level0 });
		REQUIRE(texture);

		std::map<uint64_t, std::vector<uint8_t>> data;
		data[0xA000] = MeshFactory::CreateCube()->Serialize();
		const std::string materialJson = Material::Create(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f }))->Serialize().dump();
		data[0xA001] = std::vector<uint8_t>(materialJson.begin(), materialJson.end());
		data[0xA002] = texture->Serialize();
		std::vector<AssetMetadata> metadata(3);
		const std::pair<AssetType, const char*> entries[] = { { AssetType::Mesh, "Cube.mesh" }, { AssetType::Material, "Red.stmat" }, { AssetType::Texture, "Green.png" } };
		for (size_t index = 0; index < metadata.size(); index++)
		{
			metadata[index].Handle = UUID(0xA000 + index);
			metadata[index].Type = entries[index].first;
			metadata[index].Path = entries[index].second;
		}
		const std::filesystem::path path = CreateTemporaryDirectory("StreamingScene") / "Game.stpak";
		REQUIRE(AssetPack::Write(path, metadata, [&data](const AssetMetadata& asset, std::vector<uint8_t>& outData, std::string*)
		{
			outData = data.at(static_cast<uint64_t>(asset.Handle));
			return true;
		}));
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(path);
		REQUIRE(manager);
		AssetManager::SetActive(manager);
		AssetMetadata whiteMetadata;
		whiteMetadata.Name = "White";
		const AssetHandle white = manager->AddMemoryAsset(Material::Create(UnlitColor({ 1.0f, 1.0f, 1.0f, 1.0f })), whiteMetadata);
		MaterialProperties texturedProperties = UnlitColor({ 1.0f, 1.0f, 1.0f, 1.0f });
		texturedProperties.BaseColorMap = UUID(0xA002);
		AssetMetadata texturedMetadata;
		texturedMetadata.Name = "Textured";
		const AssetHandle textured = manager->AddMemoryAsset(Material::Create(texturedProperties), texturedMetadata);

		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, UUID(0xA000), white, glm::vec3(-1.5f, 0.0f, 0.0f), "StreamedMesh");
		AddMesh(scene, BuiltinAssets::CubeMesh, UUID(0xA001), glm::vec3(0.0f), "StreamedMaterial");
		AddMesh(scene, BuiltinAssets::CubeMesh, textured, glm::vec3(1.5f, 0.0f, 0.0f), "StreamedTexture");
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f));
		auto pixelAt = [&](const ReadbackImage& image, float x)
		{
			const glm::uvec2 pixel = ProjectToPixel(camera, glm::vec3(x, 0.0f, 0.5f), c_Size);
			return GetPixelRGBA8(image, pixel.x, pixel.y);
		};

		// A missing mesh skips its entity, a missing material falls back to the (lit) default material and a missing
		// texture to a neutral one.
		ReadbackImage image = RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetStats().PendingAssets == 3);
		CHECK(renderer.GetStats().Instances == 2);
		CHECK(pixelAt(image, -1.5f) == glm::u8vec4(0, 0, 0, 255));
		const glm::u8vec4 defaultMaterial = pixelAt(image, 0.0f);
		CHECK(defaultMaterial.r > 0);
		CHECK(defaultMaterial.r < 128);
		CHECK(pixelAt(image, 1.5f) == glm::u8vec4(255, 255, 255, 255));

		REQUIRE(manager->WaitForPendingLoads());
		image = RenderToImage(renderer, scene, camera);
		CHECK(renderer.GetStats().PendingAssets == 0);
		CHECK(renderer.GetStats().Instances == 3);
		CHECK(pixelAt(image, -1.5f) == glm::u8vec4(255, 255, 255, 255));
		CHECK(pixelAt(image, 0.0f) == glm::u8vec4(255, 0, 0, 255));
		CHECK(pixelAt(image, 1.5f) == glm::u8vec4(0, 255, 0, 255));
		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Overbright and invalid colors stay finite through the HDR pipeline")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle blinding = assets.AddMaterial(Emitting(1.0e6f)); // Beyond the half-float range
		MaterialProperties brokenProperties = Emitting(1.0f);
		brokenProperties.EmissiveIntensity = std::numeric_limits<float>::quiet_NaN();
		const AssetHandle broken = assets.AddMaterial(brokenProperties);

		Scene scene;
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		Entity wall = AddMesh(scene, BuiltinAssets::QuadMesh, blinding, glm::vec3(0.0f), "Wall");
		wall.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));
		for (TonemapOperator tonemapper : { TonemapOperator::None, TonemapOperator::Reinhard, TonemapOperator::ACES, TonemapOperator::AgX, TonemapOperator::KhronosNeutral })
		{
			CAPTURE(static_cast<int>(tonemapper));
			postProcess.Tonemapper = tonemapper;
			postProcess.Bloom = tonemapper == TonemapOperator::ACES; // The bloom chain must stay finite too
			const glm::u8vec4 center = GetPixelRGBA8(RenderToImage(renderer, scene, camera), c_Size / 2, c_Size / 2);
			CHECK(center.r >= 250);
			CHECK(center.g >= 250);
			CHECK(center.b >= 250);
		}
		ReadbackImage hdr;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), hdr));
		const glm::vec4 stored = ReadHalfTexel(hdr, c_Size / 2, c_Size / 2);
		CHECK(std::isfinite(stored.r));
		CHECK(stored.r == doctest::Approx(65000.0f).epsilon(0.01));

		// A NaN material renders black instead of poisoning its surroundings (bloom spreads light over the image).
		wall.GetComponent<TransformComponent>().Scale = glm::vec3(0.5f);
		wall.MarkModified<TransformComponent>();
		wall.GetComponent<MeshRendererComponent>().Material = broken;
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(0.5f)), glm::vec3(0.0f, 0.0f, -0.5f), "Background")
			.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f);
		postProcess.Tonemapper = TonemapOperator::ACES;
		postProcess.Bloom = true;
		postProcess.AutoExposure = true;
		renderer.ResetExposureAdaptation();
		const ReadbackImage image = RenderToImage(renderer, scene, camera);
		CHECK(GetPixelRGBA8(image, c_Size / 2, c_Size / 2) == glm::u8vec4(0, 0, 0, 255));
		const glm::u8vec4 surroundings = GetPixelRGBA8(image, 4, 4);
		CHECK(surroundings.r > 60);
		CHECK(surroundings.r == surroundings.g);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The renderer draws into matching framebuffers and rejects invalid ones")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const AssetHandle red = assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f }));
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::QuadMesh, red, glm::vec3(0.0f), "Quad");
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));

		SceneRenderer renderer;
		CHECK_FALSE(renderer.Render(scene, camera)); // Zero viewport: nothing happens
		CHECK_FALSE(renderer.GetStats().Rendered);
		CHECK(renderer.GetOutputTexture() == nullptr);

		auto createTarget = [&](uint32_t size, nvrhi::Format format)
		{
			nvrhi::TextureDesc desc;
			desc.width = size;
			desc.height = size;
			desc.format = format;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			nvrhi::TextureHandle texture = gpu.GetNvrhiDevice()->createTexture(desc);
			REQUIRE(texture);
			nvrhi::CommandListHandle commandList = gpu.GetNvrhiDevice()->createCommandList();
			commandList->open();
			commandList->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 1.0f, 1.0f));
			commandList->close();
			gpu.GetNvrhiDevice()->executeCommandList(commandList);
			return std::pair { texture, gpu.GetNvrhiDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture)) };
		};

		// Matching targets work after every resize; BGRA targets get the channels swapped.
		for (uint32_t size : { 16u, 48u, 32u })
		{
			CAPTURE(size);
			renderer.SetViewportSize(size, size);
			auto [texture, framebuffer] = createTarget(size, nvrhi::Format::BGRA8_UNORM);
			CHECK(renderer.Render(scene, camera, framebuffer));
			CHECK(renderer.GetStats().Rendered);
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(texture, image));
			CHECK(GetPixelRGBA8(image, size / 2, size / 2) == glm::u8vec4(0, 0, 255, 255)); // BGRA: red in the third byte
		}

		// Another size, an sRGB or a float format is rejected and the target is left untouched (cleared to blue).
		for (auto [size, format] : { std::pair { 64u, nvrhi::Format::RGBA8_UNORM }, std::pair { 32u, nvrhi::Format::SRGBA8_UNORM }, std::pair { 32u, nvrhi::Format::RGBA16_FLOAT } })
		{
			CAPTURE(size);
			CAPTURE(static_cast<int>(format));
			auto [texture, framebuffer] = createTarget(size, format);
			CHECK_FALSE(renderer.Render(scene, camera, framebuffer));
			CHECK_FALSE(renderer.GetStats().Rendered);
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(texture, image));
			if (format == nvrhi::Format::RGBA16_FLOAT)
				CHECK(ReadHalfTexel(image, size / 2, size / 2) == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
			else
				CHECK(GetPixelRGBA8(image, size / 2, size / 2) == glm::u8vec4(0, 0, 255, 255));
		}

		// A second color attachment would be left undefined: such framebuffers are rejected too.
		{
			auto [first, firstFramebuffer] = createTarget(32, nvrhi::Format::RGBA8_UNORM);
			auto [second, secondFramebuffer] = createTarget(32, nvrhi::Format::RGBA8_UNORM);
			nvrhi::FramebufferHandle both = gpu.GetNvrhiDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(first).addColorAttachment(second));
			REQUIRE(both);
			CHECK_FALSE(renderer.Render(scene, camera, both));
			CHECK_FALSE(renderer.GetStats().Rendered);
			for (nvrhi::ITexture* texture : { first.Get(), second.Get() })
			{
				ReadbackImage image;
				REQUIRE(Renderer::ReadTexture(texture, image));
				CHECK(GetPixelRGBA8(image, 16, 16) == glm::u8vec4(0, 0, 255, 255));
			}
		}
		CHECK(renderer.Render(scene, camera)); // Recovers without a target
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Viewports beyond the device limit fail gracefully and picking needs a rendered frame")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		Entity cube = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f), "Cube");
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f));

		SceneRenderer renderer;
		const uint32_t limit = gpu.GetDevice().GetInfo().MaxTextureDimension2D;
		renderer.SetViewportSize(limit + 1, 16);
		CHECK(renderer.GetOutputTexture() == nullptr);
		CHECK_FALSE(renderer.Render(scene, camera));
		CHECK_FALSE(renderer.GetStats().Rendered);
		CHECK_FALSE(renderer.GetEntityAt(scene, 0, 0));

		renderer.SetViewportSize(c_Size, c_Size);
		CHECK_FALSE(renderer.GetEntityAt(scene, c_Size / 2, c_Size / 2)); // Nothing rendered at this size yet
		CHECK(renderer.Render(scene, camera));
		CHECK(renderer.GetEntityAt(scene, c_Size / 2, c_Size / 2) == cube);
		renderer.SetViewportSize(c_Size * 2, c_Size);
		CHECK_FALSE(renderer.GetEntityAt(scene, c_Size / 2, c_Size / 2));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Metallic-roughness, emissive, occlusion and base color maps modulate their factors")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;

		Scene scene;
		AddNeutralPostProcess(scene);
		Entity quad = AddMesh(scene, BuiltinAssets::QuadMesh, UUID::Null(), glm::vec3(0.0f), "Quad");
		quad.GetComponent<TransformComponent>().Scale = glm::vec3(4.0f);
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.AmbientColor = glm::vec3(0.5f);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f));
		auto renderCenter = [&](const MaterialProperties& properties)
		{
			quad.GetComponent<MeshRendererComponent>().Material = assets.AddMaterial(properties);
			return GetPixelRGBA8(RenderToImage(renderer, scene, camera), c_Size / 2, c_Size / 2);
		};

		// Ambient light only reaches the diffuse part: the metallic map (blue channel) turns it off.
		MaterialProperties metal;
		metal.Metallic = 1.0f;
		metal.MetallicRoughnessMap = assets.AddSolidTexture({ 0, 255, 0, 255 }, TextureFormat::RGBA8);
		const glm::u8vec4 dielectric = renderCenter(metal);
		metal.MetallicRoughnessMap = assets.AddSolidTexture({ 0, 255, 255, 255 }, TextureFormat::RGBA8);
		CHECK(dielectric.r > 100);
		CHECK(renderCenter(metal) == glm::u8vec4(0, 0, 0, 255));

		// The occlusion map (red channel) scales the ambient light: 0.25 (linear) of it remains.
		MaterialProperties occluded;
		occluded.OcclusionMap = assets.AddSolidTexture({ 64, 0, 0, 255 }, TextureFormat::RGBA8);
		const float occludedRatio = SRGBToLinear(renderCenter(occluded)).r / SRGBToLinear(dielectric).r;
		CHECK(occludedRatio == doctest::Approx(64.0f / 255.0f).epsilon(0.05));
		occluded.OcclusionStrength = 0.0f;
		CHECK(std::abs(renderCenter(occluded).r - dielectric.r) <= 1);

		// The emissive map tints the emission; the base color map tints the surface (both sRGB).
		MaterialProperties emissive = UnlitColor({ 0.0f, 0.0f, 0.0f, 1.0f });
		emissive.EmissiveColor = glm::vec3(1.0f);
		emissive.EmissiveMap = assets.AddSolidTexture({ 255, 128, 0, 255 }, TextureFormat::RGBA8SRGB);
		const glm::ivec4 emitted = glm::ivec4(renderCenter(emissive));
		CHECK(glm::all(glm::lessThanEqual(glm::abs(emitted - glm::ivec4(255, 128, 0, 255)), glm::ivec4(1))));
		MaterialProperties textured = UnlitColor({ 1.0f, 1.0f, 1.0f, 1.0f });
		textured.BaseColorMap = assets.AddSolidTexture({ 30, 200, 90, 255 }, TextureFormat::RGBA8SRGB);
		const glm::ivec4 tinted = glm::ivec4(renderCenter(textured));
		CHECK(glm::all(glm::lessThanEqual(glm::abs(tinted - glm::ivec4(30, 200, 90, 255)), glm::ivec4(1))));

		// The roughness map (green channel) sharpens the highlight of a light reflected straight at the camera.
		AddSun(scene, glm::vec3(0.0f, 0.0f, -1.0f), 2.0f, false);
		MaterialProperties shiny;
		shiny.BaseColor = glm::vec4(0.05f, 0.05f, 0.05f, 1.0f);
		shiny.Roughness = 1.0f;
		shiny.MetallicRoughnessMap = assets.AddSolidTexture({ 0, 255, 0, 255 }, TextureFormat::RGBA8);
		const int rough = renderCenter(shiny).r;
		shiny.MetallicRoughnessMap = assets.AddSolidTexture({ 0, 40, 0, 255 }, TextureFormat::RGBA8);
		const int smooth = renderCenter(shiny).r;
		CHECK(smooth > rough + 50);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Normal maps follow mirrored transforms and the back faces of double-sided surfaces")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		// Tangent-space normal tilted toward +tangent and +bitangent: (0.5, 0.5, 0.71).
		MaterialProperties properties;
		properties.Roughness = 1.0f;
		properties.DoubleSided = true;
		properties.NormalMap = assets.AddSolidTexture({ 191, 191, 218, 255 }, TextureFormat::RGBA8);
		const AssetHandle bumpy = assets.AddMaterial(properties);

		Scene scene;
		AddNeutralPostProcess(scene);
		Entity quad = AddMesh(scene, BuiltinAssets::QuadMesh, bumpy, glm::vec3(0.0f), "Quad"); // Faces +Z
		Entity sun = AddSun(scene, glm::vec3(0.0f, 0.0f, -1.0f), 3.0f, false);

		SceneRenderer renderer;
		renderer.SetViewportSize(c_Size, c_Size);

		// The shading normal is found by lighting the surface from four directions around the side facing the camera:
		// the brightest is the one closest to the normal. Returns the signs of its x and y components.
		auto shadingNormalSigns = [&](bool mirrored, bool fromBehind)
		{
			quad.GetComponent<TransformComponent>().Scale = mirrored ? glm::vec3(-1.0f, 1.0f, 1.0f) : glm::vec3(1.0f);
			quad.MarkModified<TransformComponent>();
			const float side = fromBehind ? -1.0f : 1.0f;
			const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 2.0f * side), glm::vec3(0.0f));
			glm::ivec2 brightestSigns(0);
			int brightest = -1;
			int second = -1;
			for (int x : { -1, 1 })
			{
				for (int y : { -1, 1 })
				{
					const glm::vec3 towardLight = glm::normalize(glm::vec3(0.5f * static_cast<float>(x), 0.5f * static_cast<float>(y), 0.7071f * side));
					sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(-towardLight);
					sun.MarkModified<TransformComponent>();
					const int value = Red(RenderToImage(renderer, scene, camera), c_Size / 2, c_Size / 2);
					if (value > brightest)
					{
						second = brightest;
						brightest = value;
						brightestSigns = glm::ivec2(x, y);
					}
					else
					{
						second = std::max(second, value);
					}
				}
			}
			CHECK(brightest > second + 20); // One direction stands out
			return brightestSigns;
		};

		// The quad's tangent is +X and its bitangent (increasing v) -Y, so the front normal leans toward +X and -Y.
		const glm::ivec2 front = shadingNormalSigns(false, false);
		CHECK(front == glm::ivec2(1, -1));
		// Mirrored in X, the surface and its relief are mirrored: the normal leans toward -X, still toward -Y.
		CHECK(shadingNormalSigns(true, false) == glm::ivec2(-front.x, front.y));
		// The back face of a double-sided surface is shaded with the negated normal.
		CHECK(shadingNormalSigns(false, true) == glm::ivec2(-front.x, -front.y));
		CHECK(shadingNormalSigns(true, true) == glm::ivec2(front.x, -front.y));
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
