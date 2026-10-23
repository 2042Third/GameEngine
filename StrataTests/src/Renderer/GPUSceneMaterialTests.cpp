#include "Renderer/TAATestUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

// Clear coats and glass (MaterialProperties::Clearcoat, Transmission; Scene/Forward.frag, SceneRendererGlass.cpp): the coat's
// second highlight and its energy, glass refracting the scene-color copy, and glass writing depth, entity ids and motion as an
// opaque surface does.

using namespace Strata;
using namespace Strata::Tests;
using namespace Strata::Tests::TAA;

namespace
{

	constexpr uint32_t c_ViewSize = 128;

	SceneRenderer CreateMaterialRenderer(const char* name, bool temporal, bool extendedStorageFormats = true, GPUDrivenMode mode = GPUDrivenMode::Off)
	{
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		specification.GPUDriven = mode;
		specification.TemporalAntiAliasing = temporal;
		specification.ExtendedStorageFormats = extendedStorageFormats;
		specification.DebugName = name;
		return SceneRenderer(specification);
	}

	// Renders until the sky's environment lights the scene (made over a few frames), or once without a sky.
	void RenderSettled(GPUContext& gpu, SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, bool environment)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		do
		{
			REQUIRE(renderer.Render(scene, camera));
			FinishFrame(gpu);
		} while (environment && !renderer.GetStats().EnvironmentLighting && std::chrono::steady_clock::now() < deadline);
		CHECK(renderer.GetStats().EnvironmentLighting == environment);
	}

	float Luminance(const glm::vec3& color)
	{
		return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
	}

	size_t Brightest(const std::vector<glm::vec3>& image, const std::optional<glm::ivec2>& awayFrom = std::nullopt, int distance = 0)
	{
		size_t best = 0;
		float bestLuminance = -1.0f;
		for (size_t index = 0; index < image.size(); index++)
		{
			const glm::ivec2 pixel(static_cast<int>(index % c_ViewSize), static_cast<int>(index / c_ViewSize));
			if (awayFrom && std::abs(pixel.x - awayFrom->x) < distance && std::abs(pixel.y - awayFrom->y) < distance)
				continue;
			if (Luminance(image[index]) > bestLuminance)
			{
				bestLuminance = Luminance(image[index]);
				best = index;
			}
		}
		return best;
	}

	glm::ivec2 PixelOf(size_t index)
	{
		return glm::ivec2(static_cast<int>(index % c_ViewSize), static_cast<int>(index / c_ViewSize));
	}

	// A plane under a sun whose mirror reflection the camera sees at the plane's center, drawn with `material`.
	std::vector<glm::vec3> RenderLitPlane(GPUContext& gpu, SceneTestAssets& assets, const MaterialProperties& material)
	{
		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(0.0f, -0.447f, 0.894f)), 3.0f, false);
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(material), glm::vec3(0.0f), "Plane").GetComponent<TransformComponent>().Scale = glm::vec3(10.0f);
		SceneRenderer renderer = CreateMaterialRenderer("Coat", false);
		renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		RenderSettled(gpu, renderer, scene, LookAt(glm::vec3(0.0f, 2.0f, 4.0f), glm::vec3(0.0f)), false);
		return ReadHDR(renderer.GetHDRTexture());
	}

	// Spheres in a uniform sky under a sun, drawn with `material`.
	std::vector<glm::vec3> RenderLitSpheres(GPUContext& gpu, SceneTestAssets& assets, const MaterialProperties& material, AssetHandle sky)
	{
		Scene scene;
		AddNeutralPostProcess(scene);
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().EnvironmentMap = sky;
		AddSun(scene, glm::normalize(glm::vec3(-0.3f, -1.0f, -0.6f)), 2.0f, false);
		const AssetHandle handle = assets.AddMaterial(material);
		for (int index = 0; index < 3; index++)
			AddMesh(scene, BuiltinAssets::SphereMesh, handle, glm::vec3(static_cast<float>(index) * 1.2f - 1.2f, 0.0f, 0.0f), "Sphere");
		SceneRenderer renderer = CreateMaterialRenderer("Energy", false);
		renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		RenderSettled(gpu, renderer, scene, LookAt(glm::vec3(0.0f, 0.5f, 3.5f), glm::vec3(0.0f)), true);
		return ReadHDR(renderer.GetHDRTexture());
	}

	// A wall red left of x = 0 and blue right of it, 3 units behind a glass pane turned 35 degrees about Y, seen from 3 units in
	// front of the pane. The pane is a quad scaled 2 across and 4 along its normal: its glass is 4 times its material's
	// Thickness thick (the scale along the normal, not the axes' mean).
	struct GlassScene
	{
		static constexpr float c_PaneAngle = glm::radians(35.0f);
		static constexpr float c_PaneScale = 2.0f;
		static constexpr float c_PaneDepthScale = 4.0f;

		Scene Contents;
		Entity Pane;
		SceneCamera Camera;

		GlassScene(SceneTestAssets& assets, const MaterialProperties& glass)
		{
			AddNeutralPostProcess(Contents);
			Entity red = AddMesh(Contents, BuiltinAssets::QuadMesh, assets.AddMaterial(UnlitColor({ 1.0f, 0.0f, 0.0f, 1.0f })), glm::vec3(-2.0f, 0.0f, -3.0f), "Red");
			red.GetComponent<TransformComponent>().Scale = glm::vec3(4.0f);
			Entity blue = AddMesh(Contents, BuiltinAssets::QuadMesh, assets.AddMaterial(UnlitColor({ 0.0f, 0.0f, 1.0f, 1.0f })), glm::vec3(2.0f, 0.0f, -3.0f), "Blue");
			blue.GetComponent<TransformComponent>().Scale = glm::vec3(4.0f);
			Pane = AddMesh(Contents, BuiltinAssets::QuadMesh, assets.AddMaterial(glass), glm::vec3(0.0f), "Pane");
			TransformComponent& transform = Pane.GetComponent<TransformComponent>();
			transform.Rotation = glm::quat(glm::vec3(0.0f, c_PaneAngle, 0.0f));
			transform.Scale = glm::vec3(c_PaneScale, c_PaneScale, c_PaneDepthScale);
			Camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
		}

		// The column of the center row where what the copy shows changes from red to blue, between pixel centers (the pixel
		// left of it plus the fraction where red's share passes one half), searched within the pane.
		static std::optional<float> FindEdge(const std::vector<glm::vec3>& image, uint32_t first, uint32_t last)
		{
			const uint32_t row = c_ViewSize / 2;
			auto redShare = [&](uint32_t x)
			{
				const glm::vec3 color = image[static_cast<size_t>(row) * c_ViewSize + x];
				return color.r + color.b > 1e-4f ? color.r / (color.r + color.b) : 0.5f;
			};
			for (uint32_t x = first; x < last; x++)
			{
				const float left = redShare(x);
				const float right = redShare(x + 1);
				if (left >= 0.5f && right < 0.5f)
					return static_cast<float>(x) + 0.5f + (left - 0.5f) / (left - right);
			}
			return std::nullopt;
		}

		// Where the renderer reads the copy for the pixel column `x` of the center row (Scene/ForwardGlass.frag's TransmitLight):
		// the column the refracted ray's exit from the pane's volume projects to, in pixels.
		float GetLookupColumn(float x, float ior, float thickness) const
		{
			const glm::vec2 ndc((x / c_ViewSize) * 2.0f - 1.0f, 0.0f);
			const glm::mat4 inverse = glm::inverse(Camera.Projection * Camera.View);
			const glm::vec4 farPoint = inverse * glm::vec4(ndc, 0.5f, 1.0f);
			const glm::vec3 direction = glm::normalize(glm::vec3(farPoint) / farPoint.w - Camera.Position);
			const glm::vec3 normal(std::sin(c_PaneAngle), 0.0f, std::cos(c_PaneAngle));
			const float distance = -glm::dot(Camera.Position, normal) / glm::dot(direction, normal);
			const glm::vec3 point = Camera.Position + direction * distance;
			const glm::vec3 refracted = glm::refract(direction, normal, 1.0f / ior);
			const glm::vec3 exit = point + refracted * thickness * c_PaneDepthScale;
			const glm::vec4 clip = Camera.Projection * Camera.View * glm::vec4(exit, 1.0f);
			return (clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(c_ViewSize);
		}
	};

	// What a frame wrote besides its color: entity ids, depth and motion (the velocity target's half-float bits, for exact
	// comparisons), and its HDR image with alpha.
	struct SurfaceFrame
	{
		std::vector<uint32_t> IDs;
		std::vector<float> Depth;
		std::vector<uint32_t> Velocity;
		std::vector<glm::vec4> HDR;
	};

	SurfaceFrame ReadSurfaceFrame(SceneRenderer& renderer)
	{
		SurfaceFrame frame;
		frame.IDs = ReadIDs(renderer);
		ReadbackImage depth;
		REQUIRE(Renderer::ReadTexture(renderer.GetDepthTexture(), depth));
		REQUIRE(depth.Pixels.size() == frame.IDs.size() * sizeof(float));
		frame.Depth.resize(frame.IDs.size());
		std::memcpy(frame.Depth.data(), depth.Pixels.data(), depth.Pixels.size());
		ReadbackImage velocity;
		REQUIRE(renderer.GetVelocityTexture());
		REQUIRE(Renderer::ReadTexture(renderer.GetVelocityTexture(), velocity));
		REQUIRE(velocity.Pixels.size() == frame.IDs.size() * sizeof(uint32_t));
		frame.Velocity.resize(frame.IDs.size());
		std::memcpy(frame.Velocity.data(), velocity.Pixels.data(), velocity.Pixels.size());
		frame.HDR = ReadHDRA(renderer.GetHDRTexture());
		return frame;
	}

	MaterialProperties Glass(float ior, float thickness)
	{
		MaterialProperties glass;
		glass.BaseColor = glm::vec4(1.0f);
		glass.Roughness = 0.0f;
		glass.Transmission = 1.0f;
		glass.IOR = ior;
		glass.Thickness = thickness;
		glass.DoubleSided = true;
		return glass;
	}

	// The center row's pixels the pane covers (its entity id), first and last.
	std::pair<uint32_t, uint32_t> GetPaneColumns(SceneRenderer& renderer, Entity pane)
	{
		const std::vector<uint32_t> ids = ReadIDs(renderer);
		uint32_t first = c_ViewSize;
		uint32_t last = 0;
		const uint32_t row = c_ViewSize / 2;
		for (uint32_t x = 0; x < c_ViewSize; x++)
		{
			if (ids[static_cast<size_t>(row) * c_ViewSize + x] == GetID(pane))
			{
				first = std::min(first, x);
				last = std::max(last, x);
			}
		}
		REQUIRE(first < last);
		return { first, last };
	}

}

TEST_SUITE("GPU.SceneRenderer.Materials")
{
	TEST_CASE("A clear coat adds a second highlight where its own normal map turns it")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		MaterialProperties base;
		base.BaseColor = glm::vec4(0.4f, 0.4f, 0.4f, 1.0f);
		base.Roughness = 0.35f;
		const std::vector<glm::vec3> plain = RenderLitPlane(gpu, assets, base);

		// The coat's normal turned 8 degrees about the plane's tangent frame: its highlight lies elsewhere than the base's.
		MaterialProperties coated = base;
		coated.Clearcoat = 1.0f;
		coated.ClearcoatRoughness = 0.08f;
		coated.ClearcoatNormalMap = assets.AddSolidTexture(glm::u8vec4(146, 128, 254, 255), TextureFormat::RGBA8);
		const std::vector<glm::vec3> withCoat = RenderLitPlane(gpu, assets, coated);

		const glm::ivec2 baseHighlight = PixelOf(Brightest(plain));
		const size_t second = Brightest(withCoat, baseHighlight, 12);
		CAPTURE(baseHighlight);
		CAPTURE(PixelOf(second));
		// A highlight of its own, far brighter than the base shows there...
		CHECK(Luminance(withCoat[second]) > 4.0f * Luminance(plain[second]));
		// ...and the base's still shows through the coat where it was (less what the coat reflects).
		const size_t first = static_cast<size_t>(baseHighlight.y) * c_ViewSize + baseHighlight.x;
		CHECK(Luminance(withCoat[first]) > 0.8f * Luminance(plain[first]));
		CHECK(Luminance(withCoat[first]) < Luminance(plain[first]) + Luminance(withCoat[second]));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A clear coat takes from the base the light it reflects")
	{
		// The coat reflects at least 4% of the light (its Fresnel at normal incidence), which no longer reaches the base: under a
		// sun and a uniform sky every pixel of a coated surface is at most 96.5% of the uncoated one plus what the coat alone
		// shows (over a black metal base, which reflects next to nothing), and never more than the two together.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::SphereMesh));
		const AssetHandle sky = assets.Add(CreateEnvironmentTexture(glm::vec3(0.6f), glm::vec3(0.6f)), "Sky");
		MaterialProperties base;
		base.BaseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
		base.Roughness = 0.5f;
		MaterialProperties coated = base;
		coated.Clearcoat = 1.0f;
		coated.ClearcoatRoughness = 0.2f;
		MaterialProperties coatOnly = coated;
		coatOnly.BaseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		coatOnly.Metallic = 1.0f;

		const std::vector<glm::vec3> plain = RenderLitSpheres(gpu, assets, base, sky);
		const std::vector<glm::vec3> coat = RenderLitSpheres(gpu, assets, coatOnly, sky);
		const std::vector<glm::vec3> both = RenderLitSpheres(gpu, assets, coated, sky);
		uint32_t surface = 0;
		uint32_t overBound = 0;
		uint32_t overSum = 0;
		uint32_t brighter = 0;
		float worst = 0.0f;
		for (size_t index = 0; index < plain.size(); index++)
		{
			const float a = Luminance(plain[index]);
			const float b = Luminance(coat[index]);
			const float c = Luminance(both[index]);
			if (a < 0.05f)
				continue; // The background
			surface++;
			const float bound = 0.965f * a + b + 1e-3f;
			worst = std::max(worst, (c - (0.96f * a + b)) / a);
			overBound += c > bound ? 1u : 0u;
			overSum += c > a + b + 1e-3f ? 1u : 0u;
			brighter += c > a * 1.02f ? 1u : 0u;
		}
		CAPTURE(worst);
		CHECK(surface > 1000u);
		CHECK(overBound == 0u);
		CHECK(overSum == 0u);
		CHECK(brighter > 0u); // The coat's own highlights
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Glass shows what lies behind it, displaced by its index of refraction and thickness")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::QuadMesh));
		constexpr float c_Thickness = 0.25f;

		// Without the pane: where the wall's edge is.
		std::optional<float> wallEdge;
		{
			GlassScene scene(assets, Glass(1.0f, c_Thickness));
			scene.Contents.DestroyEntity(scene.Pane);
			SceneRenderer renderer = CreateMaterialRenderer("Wall", false);
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		RenderSettled(gpu, renderer, scene.Contents, scene.Camera, false);
			CHECK(renderer.GetStats().SceneColorCopies == 0u);
			CHECK(renderer.GetSceneColorTexture() == nullptr);
			wallEdge = GlassScene::FindEdge(ReadHDR(renderer.GetHDRTexture()), 1, c_ViewSize - 2);
		}
		REQUIRE(wallEdge);

		for (const float ior : { 1.0f, 1.5f })
		{
			CAPTURE(ior);
			GlassScene scene(assets, Glass(ior, c_Thickness));
			SceneRenderer renderer = CreateMaterialRenderer("Glass", false);
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		RenderSettled(gpu, renderer, scene.Contents, scene.Camera, false);
			CHECK(renderer.GetStats().SceneColorCopies == 1u);
			const auto [first, last] = GetPaneColumns(renderer, scene.Pane);
			const std::vector<glm::vec3> image = ReadHDR(renderer.GetHDRTexture());
			const std::optional<float> edge = GlassScene::FindEdge(image, first + 1, last - 1);
			REQUIRE(edge);
			CAPTURE(*wallEdge);
			CAPTURE(*edge);
			// The renderer reads the copy where the wall's edge is at the edge's pixel (the copy is half the size: within 1.5).
			CHECK(std::abs(scene.GetLookupColumn(*edge, ior, c_Thickness) - *wallEdge) < 1.5f);
			if (ior == 1.0f)
				CHECK(std::abs(*edge - *wallEdge) < 1.5f); // Nothing bends: the edge stays where it is
			else
				CHECK(std::abs(*edge - *wallEdge) > 4.0f);
			// The pane passes the wall's light through, less what it reflects (no more than a third at this angle).
			const glm::vec3 behind = image[static_cast<size_t>(c_ViewSize / 2) * c_ViewSize + first + 2];
			CHECK(behind.r > 0.6f);
			CHECK(behind.r <= 1.0f);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Glass writes depth, entity ids and motion as an opaque surface does, and hides blended surfaces behind it")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::QuadMesh));
		MaterialProperties ghost = UnlitColor({ 1.0f, 0.0f, 0.0f, 0.5f });
		ghost.AlphaMode = MaterialAlphaMode::Blend;
		MaterialProperties stone;
		stone.BaseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
		const AssetHandle glass = assets.AddMaterial(Glass(1.5f, 0.2f));
		const AssetHandle opaque = assets.AddMaterial(stone);
		const AssetHandle blended = assets.AddMaterial(ghost);

		// The cube moves between two frames of a renderer with motion vectors; `ghosts` puts a blended quad between it and the
		// wall.
		auto render = [&](AssetHandle cubeMaterial, bool ghosts)
		{
			Scene scene;
			AddTemporalPostProcess(scene, TemporalAntiAliasingPreset::Sharp);
			AddSun(scene, glm::normalize(glm::vec3(-0.3f, -1.0f, -0.5f)), 3.0f, false);
			AddMesh(scene, BuiltinAssets::QuadMesh, opaque, glm::vec3(0.0f, 0.0f, -2.0f), "Wall").GetComponent<TransformComponent>().Scale = glm::vec3(8.0f);
			if (ghosts)
				AddMesh(scene, BuiltinAssets::QuadMesh, blended, glm::vec3(0.0f, 0.0f, -1.0f), "Ghost").GetComponent<TransformComponent>().Scale = glm::vec3(3.0f);
			Entity cube = AddMesh(scene, BuiltinAssets::CubeMesh, cubeMaterial, glm::vec3(0.0f), "Cube");
			cube.GetComponent<TransformComponent>().Rotation = glm::quat(glm::vec3(0.3f, 0.5f, 0.0f));
			const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f));
			SceneRenderer renderer = CreateMaterialRenderer("GlassMotion", true);
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			REQUIRE(renderer.Render(scene, camera));
			FinishFrame(gpu);
			cube.GetComponent<TransformComponent>().Translation.x = 0.15f;
			cube.MarkModified<TransformComponent>();
			REQUIRE(renderer.Render(scene, camera));
			FinishFrame(gpu);
			return std::make_pair(ReadSurfaceFrame(renderer), GetID(cube));
		};

		const auto [glassFrame, glassID] = render(glass, false);
		const auto [opaqueFrame, opaqueID] = render(opaque, false);
		const auto [ghostFrame, ghostID] = render(glass, true);
		REQUIRE(glassID == opaqueID);
		uint32_t cubePixels = 0;
		uint32_t moving = 0;
		for (size_t pixel = 0; pixel < glassFrame.IDs.size(); pixel++)
		{
			if (opaqueFrame.IDs[pixel] != opaqueID)
				continue;
			cubePixels++;
			CHECK(glassFrame.IDs[pixel] == glassID);
			CHECK(glassFrame.Depth[pixel] == opaqueFrame.Depth[pixel]);
			CHECK(glassFrame.Velocity[pixel] == opaqueFrame.Velocity[pixel]);
			moving += glassFrame.Velocity[pixel] != 0u ? 1u : 0u;
			// Not blended (HDR alpha 1: the TAA takes it for a surface of its own), and the blended quad behind it is hidden.
			CHECK(glassFrame.HDR[pixel].a == 1.0f);
			CHECK(ghostFrame.HDR[pixel] == glassFrame.HDR[pixel]);
		}
		CHECK(cubePixels > 500u);
		CHECK(moving == cubePixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Glass draws alike in renderers without entity ids (games, thumbnails, probe bakes), with and without motion")
	{
		// Scene/ForwardGlassNoEntityIDs.frag and ForwardGlassNoEntityIDsVelocity.frag write the color and the motion into a
		// framebuffer without the entity-ID attachment: what they write must match the variants with ids.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::QuadMesh));
		MaterialProperties stone;
		stone.BaseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
		const AssetHandle glass = assets.AddMaterial(Glass(1.5f, 0.2f));
		const AssetHandle opaque = assets.AddMaterial(stone);

		struct GlassFrame
		{
			std::vector<uint32_t> IDs; // Empty without entity ids
			std::vector<float> Depth;
			std::vector<uint32_t> Velocity; // The velocity target's half-float bits; empty without motion
			std::vector<glm::vec4> HDR;
			uint32_t CubeID = 0;
			uint32_t SceneColorCopies = 0;
		};
		// A glass cube in front of a wall, moved between two frames (with motion vectors when `temporal`).
		auto render = [&](bool entityIDs, bool temporal)
		{
			Scene scene;
			if (temporal)
				AddTemporalPostProcess(scene, TemporalAntiAliasingPreset::Sharp);
			else
				AddNeutralPostProcess(scene);
			AddSun(scene, glm::normalize(glm::vec3(-0.3f, -1.0f, -0.5f)), 3.0f, false);
			AddMesh(scene, BuiltinAssets::QuadMesh, opaque, glm::vec3(0.0f, 0.0f, -2.0f), "Wall").GetComponent<TransformComponent>().Scale = glm::vec3(8.0f);
			Entity cube = AddMesh(scene, BuiltinAssets::CubeMesh, glass, glm::vec3(0.0f), "Cube");
			cube.GetComponent<TransformComponent>().Rotation = glm::quat(glm::vec3(0.3f, 0.5f, 0.0f));
			const SceneCamera camera = LookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f));
			SceneRendererSpecification specification;
			specification.TextureStreaming = false;
			specification.TemporalAntiAliasing = temporal;
			specification.EntityIDs = entityIDs;
			specification.DebugName = entityIDs ? "GlassWithIDs" : "GlassWithoutIDs";
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			REQUIRE(renderer.Render(scene, camera));
			FinishFrame(gpu);
			cube.GetComponent<TransformComponent>().Translation.x = 0.15f;
			cube.MarkModified<TransformComponent>();
			REQUIRE(renderer.Render(scene, camera));
			FinishFrame(gpu);

			GlassFrame frame;
			frame.CubeID = GetID(cube);
			frame.SceneColorCopies = renderer.GetStats().SceneColorCopies;
			CHECK((renderer.GetEntityIDTexture() != nullptr) == entityIDs);
			if (entityIDs)
				frame.IDs = ReadIDs(renderer);
			ReadbackImage depth;
			REQUIRE(Renderer::ReadTexture(renderer.GetDepthTexture(), depth));
			frame.Depth.resize(static_cast<size_t>(c_ViewSize) * c_ViewSize);
			REQUIRE(depth.Pixels.size() == frame.Depth.size() * sizeof(float));
			std::memcpy(frame.Depth.data(), depth.Pixels.data(), depth.Pixels.size());
			CHECK((renderer.GetVelocityTexture() != nullptr) == temporal);
			if (temporal && renderer.GetVelocityTexture())
			{
				ReadbackImage velocity;
				REQUIRE(Renderer::ReadTexture(renderer.GetVelocityTexture(), velocity));
				frame.Velocity.resize(frame.Depth.size());
				REQUIRE(velocity.Pixels.size() == frame.Velocity.size() * sizeof(uint32_t));
				std::memcpy(frame.Velocity.data(), velocity.Pixels.data(), velocity.Pixels.size());
			}
			frame.HDR = ReadHDRA(renderer.GetHDRTexture());
			return frame;
		};

		for (const bool temporal : { false, true })
		{
			CAPTURE(temporal);
			const GlassFrame withIDs = render(true, temporal);
			const GlassFrame withoutIDs = render(false, temporal);
			CHECK(withIDs.SceneColorCopies == 1u);
			CHECK(withoutIDs.SceneColorCopies == 1u);
			uint32_t cubePixels = 0;
			uint32_t moving = 0;
			float worst = 0.0f;
			for (size_t pixel = 0; pixel < withIDs.IDs.size(); pixel++)
			{
				if (withIDs.IDs[pixel] != withIDs.CubeID)
					continue;
				cubePixels++;
				CHECK(withoutIDs.Depth[pixel] == withIDs.Depth[pixel]);
				CHECK(withoutIDs.HDR[pixel].a == 1.0f);
				// The same shading, compiled once more: equal up to the shader compiler's rounding.
				const glm::vec3 difference = glm::abs(glm::vec3(withoutIDs.HDR[pixel]) - glm::vec3(withIDs.HDR[pixel]));
				const float scale = std::max(0.05f, std::max(withIDs.HDR[pixel].r, std::max(withIDs.HDR[pixel].g, withIDs.HDR[pixel].b)));
				worst = std::max(worst, std::max(difference.r, std::max(difference.g, difference.b)) / scale);
				if (temporal)
				{
					CHECK(withoutIDs.Velocity[pixel] == withIDs.Velocity[pixel]);
					moving += withoutIDs.Velocity[pixel] != 0u ? 1u : 0u;
				}
			}
			CAPTURE(worst);
			CHECK(cubePixels > 500u);
			CHECK(worst < 0.01f);
			if (temporal)
				CHECK(moving == cubePixels);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The scene-color copy is made only for glass, as B10G11R11 where the device stores it and RGBA16F otherwise")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		REQUIRE(assets.Manager->LoadAssetSync(BuiltinAssets::QuadMesh));

		// Blended surfaces alone make no copy.
		{
			MaterialProperties ghost = UnlitColor({ 1.0f, 0.0f, 0.0f, 0.5f });
			ghost.AlphaMode = MaterialAlphaMode::Blend;
			GlassScene scene(assets, ghost);
			SceneRenderer renderer = CreateMaterialRenderer("Blended", false);
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		RenderSettled(gpu, renderer, scene.Contents, scene.Camera, false);
			CHECK(renderer.GetStats().SceneColorCopies == 0u);
			CHECK(renderer.GetSceneColorTexture() == nullptr);
		}

		std::vector<glm::vec3> images[2];
		for (const bool extended : { true, false })
		{
			CAPTURE(extended);
			GlassScene scene(assets, Glass(1.5f, 0.5f));
			SceneRenderer renderer = CreateMaterialRenderer("Copy", false, extended);
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		RenderSettled(gpu, renderer, scene.Contents, scene.Camera, false);
			CHECK(renderer.GetStats().SceneColorCopies == 1u);
			nvrhi::ITexture* copy = renderer.GetSceneColorTexture();
			REQUIRE(copy);
			const nvrhi::TextureDesc& desc = copy->getDesc();
			const bool stored = extended && gpu.GetDevice().GetInfo().SupportsExtendedStorageFormats;
			CHECK(desc.format == (stored ? nvrhi::Format::R11G11B10_FLOAT : nvrhi::Format::RGBA16_FLOAT));
			CHECK(desc.width == c_ViewSize / 2);
			CHECK(desc.height == c_ViewSize / 2);
			CHECK(desc.mipLevels == 6u);
			images[extended ? 0 : 1] = ReadHDR(renderer.GetHDRTexture());
			// Without glass in a later frame the copy is not made again (the target stays for the next glass).
			scene.Contents.DestroyEntity(scene.Pane);
			REQUIRE(renderer.Render(scene.Contents, scene.Camera));
			FinishFrame(gpu);
			CHECK(renderer.GetStats().SceneColorCopies == 0u);
		}
		// Both formats show the same picture, within B10G11R11's precision (5 or 6 bits of mantissa).
		float worst = 0.0f;
		for (size_t index = 0; index < images[0].size(); index++)
		{
			const glm::vec3 difference = glm::abs(images[0][index] - images[1][index]);
			worst = std::max(worst, std::max(difference.r, std::max(difference.g, difference.b)));
		}
		CHECK(worst < 0.04f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
