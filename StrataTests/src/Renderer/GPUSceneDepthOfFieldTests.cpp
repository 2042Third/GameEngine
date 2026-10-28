#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/PostProcessMath.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/RendererQuality.h"
#include "Strata/Scene/Components.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// Depth of field: the half-resolution circles of confusion, the near field's tiles, the gather and the composite
// (PostProcess/DepthOfField*.comp), and the lens programs pass (CameraLens).
namespace
{

	constexpr uint32_t c_Width = 1920;
	constexpr uint32_t c_Height = 1080;

	// A full-frame lens of LookAt's 60 degree view, focused `focus` meters away at the f-number `fNumber`.
	CameraLens Lens(float focus, float fNumber)
	{
		CameraLens lens;
		lens.SensorHeight = 24.0f;
		lens.FocalLength = 0.5f * lens.SensorHeight / std::tan(glm::radians(30.0f));
		lens.FNumber = fNumber;
		lens.FocusDistance = focus;
		return lens;
	}

	// A wall at z = 0, black left of x = 0 and white (1) right of it, seen square-on from `distance` meters; manual exposure,
	// no tone curve, bloom, vignette or anti-aliasing.
	struct EdgeWall
	{
		Scene SceneData;
		PostProcessComponent* PostProcess = nullptr;

		explicit EdgeWall(SceneTestAssets& assets)
		{
			PostProcess = &AddNeutralPostProcess(SceneData);
			AddMesh(SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(0.0f)), glm::vec3(-20.0f, 0.0f, 0.0f), "Black")
				.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f, 40.0f, 1.0f);
			AddMesh(SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(1.0f)), glm::vec3(20.0f, 0.0f, 0.0f), "White")
				.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f, 40.0f, 1.0f);
		}

		static SceneCamera CameraAt(float distance)
		{
			return LookAt(glm::vec3(0.0f, 0.0f, distance), glm::vec3(0.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height));
		}
	};

	ReadbackImage RenderImage(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, const SceneRenderOptions& options = {})
	{
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		return image;
	}

	SceneRenderOptions WithLens(const CameraLens& lens)
	{
		SceneRenderOptions options;
		options.Lens = lens;
		return options;
	}

	// The red channel's linear value, averaged over the rows [top, bottom), per column.
	std::vector<float> LinearProfile(const ReadbackImage& image, uint32_t top, uint32_t bottom)
	{
		std::vector<float> profile(image.Width, 0.0f);
		for (uint32_t y = top; y < bottom; y++)
		{
			for (uint32_t x = 0; x < image.Width; x++)
				profile[x] += PostProcessMath::SRGBToLinear(static_cast<float>(Red(image, x, y)) / 255.0f) / static_cast<float>(bottom - top);
		}
		return profile;
	}

	// The distance between a rising profile's crossings of `low` and `high` (linear interpolation between columns).
	float CrossingWidth(const std::vector<float>& profile, float low, float high)
	{
		auto crossing = [&](float level)
		{
			for (size_t x = 1; x < profile.size(); x++)
			{
				if (profile[x - 1] < level && profile[x] >= level)
					return static_cast<float>(x - 1) + (level - profile[x - 1]) / (profile[x] - profile[x - 1]);
			}
			FAIL("the profile never reaches ", level);
			return 0.0f;
		};
		return crossing(high) - crossing(low);
	}

	// The 10-90% width of a uniform disk's edge spread, in diameters: the chord at h radii from the center leaves a tenth of the
	// disk beyond it where (acos(h) - h sqrt(1 - h^2)) / pi = 0.1.
	float DiskEdgeWidth1090()
	{
		float low = 0.0f;
		float high = 1.0f;
		for (int step = 0; step < 40; step++)
		{
			const float h = 0.5f * (low + high);
			const float beyond = (std::acos(h) - h * std::sqrt(1.0f - h * h)) / glm::pi<float>();
			(beyond > 0.1f ? low : high) = h;
		}
		return low; // 2 h r over 2 r
	}

}

TEST_SUITE("Renderer.CameraLens")
{
	TEST_CASE("A camera's lens follows its component")
	{
		Scene scene("Lens");
		Entity camera = scene.CreateEntity("Camera");
		camera.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 5.0f);
		camera.MarkModified<TransformComponent>();
		CameraComponent& component = camera.AddComponent<CameraComponent>();
		CHECK_FALSE(CameraLens::FromEntity(scene, camera).has_value()); // Depth of field is off by default

		component.DepthOfField = true;
		component.FocusDistance = 7.0f;
		component.Aperture = 4.0f;
		component.SensorHeight = 36.0f;
		component.PerspectiveFOV = 40.0f;
		std::optional<CameraLens> lens = CameraLens::FromEntity(scene, camera);
		REQUIRE(lens.has_value());
		CHECK(lens->FocusDistance == doctest::Approx(7.0f));
		CHECK(lens->FNumber == doctest::Approx(4.0f));
		CHECK(lens->SensorHeight == doctest::Approx(36.0f));
		CHECK(lens->FocalLength == doctest::Approx(18.0f / std::tan(glm::radians(20.0f))));

		// A focus entity: its distance along the camera's view, wherever it lies across it.
		Entity subject = scene.CreateEntity("Subject");
		subject.GetComponent<TransformComponent>().Translation = glm::vec3(2.0f, 1.0f, -3.0f);
		subject.MarkModified<TransformComponent>();
		component.FocusEntity = subject.GetUUID();
		lens = CameraLens::FromEntity(scene, camera);
		REQUIRE(lens.has_value());
		CHECK(lens->FocusDistance == doctest::Approx(8.0f));
		// Behind the camera: the near plane.
		subject.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 9.0f);
		subject.MarkModified<TransformComponent>();
		CHECK(CameraLens::FromEntity(scene, camera)->FocusDistance == doctest::Approx(component.PerspectiveNear));
		// An entity that is gone: the focus distance again.
		scene.DestroyEntity(subject);
		CHECK(CameraLens::FromEntity(scene, camera)->FocusDistance == doctest::Approx(7.0f));

		component.Projection = ProjectionType::Orthographic;
		CHECK_FALSE(CameraLens::FromEntity(scene, camera).has_value());
	}

	TEST_CASE("The circle of confusion is a thin lens's")
	{
		CameraLens lens;
		lens.FocalLength = 50.0f;
		lens.FNumber = 2.0f;
		lens.SensorHeight = 24.0f;
		lens.FocusDistance = 2.0f;
		// f^2 / (N (zf - f)) = 2500 / 3900 mm at infinity; at 4 m half of it; on a 1000-row image, / 24 * 1000 pixels.
		const float infinity = 2500.0f / 3900.0f / 24.0f * 1000.0f;
		CHECK(lens.GetCircleOfConfusionScale(1000.0f) == doctest::Approx(infinity));
		CHECK(lens.GetCircleOfConfusion(4.0f, 1000.0f) == doctest::Approx(0.5f * infinity));
		CHECK(lens.GetCircleOfConfusion(2.0f, 1000.0f) == doctest::Approx(0.0f));
		CHECK(lens.GetCircleOfConfusion(1.0f, 1000.0f) == doctest::Approx(-infinity));
		// Nearer than a lens can focus: held at 1.1 focal lengths.
		lens.FocusDistance = 0.01f;
		CHECK(std::isfinite(lens.GetCircleOfConfusionScale(1000.0f)));
		CHECK(lens.GetCircleOfConfusionScale(1000.0f) > 0.0f);
	}
}

TEST_SUITE("GPU.SceneRenderer.DepthOfField")
{
	TEST_CASE("The focus plane renders bit-identically to depth of field off")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const ReadbackImage sharp = RenderImage(renderer, wall.SceneData, EdgeWall::CameraAt(2.0f));
		CHECK_FALSE(renderer.GetStats().DepthOfField);
		const ReadbackImage focused = RenderImage(renderer, wall.SceneData, EdgeWall::CameraAt(2.0f), WithLens(Lens(2.0f, 1.4f)));
		CHECK(renderer.GetStats().DepthOfField);
		CHECK(MaxDifference(sharp, focused) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("An edge out of focus blurs over its circle of confusion")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const SceneCamera camera = EdgeWall::CameraAt(4.0f);
		const CameraLens lens = Lens(1.0f, 1.0f);
		const float diameter = lens.GetCircleOfConfusion(4.0f, static_cast<float>(c_Height));
		REQUIRE(diameter > 10.0f);
		REQUIRE(diameter < RenderData::c_DepthOfFieldMaxDiameter * static_cast<float>(c_Height));
		const std::vector<float> sharp = LinearProfile(RenderImage(renderer, wall.SceneData, camera), c_Height / 4, 3 * c_Height / 4);
		const std::vector<float> blurred = LinearProfile(RenderImage(renderer, wall.SceneData, camera, WithLens(lens)), c_Height / 4, 3 * c_Height / 4);
		CHECK(renderer.GetStats().DepthOfField);
		CHECK(renderer.GetStats().DepthOfFieldInfinityCoC == doctest::Approx(lens.GetCircleOfConfusionScale(static_cast<float>(c_Height))));
		CHECK(CrossingWidth(sharp, 0.1f, 0.9f) < 1.5f);
		const float width = CrossingWidth(blurred, 0.1f, 0.9f);
		const float expected = DiskEdgeWidth1090() * diameter;
		MESSAGE("10-90% width ", width, " pixels against a ", diameter, " pixel disk's ", expected);
		CHECK(std::abs(width - expected) <= 0.15f * expected);
		// The blur is a disk's: nothing beyond its radius changes.
		const float edge = static_cast<float>(c_Width) * 0.5f;
		const int reach = static_cast<int>(std::ceil(0.5f * diameter)) + 6;
		CHECK(blurred[static_cast<size_t>(edge) - static_cast<size_t>(reach)] == sharp[static_cast<size_t>(edge) - static_cast<size_t>(reach)]);
		CHECK(blurred[static_cast<size_t>(edge) + static_cast<size_t>(reach)] == sharp[static_cast<size_t>(edge) + static_cast<size_t>(reach)]);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A near object's blur spreads over the background in focus")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene("NearCard");
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(1.0f)), glm::vec3(0.0f), "Wall")
			.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f, 40.0f, 1.0f);
		// A black card half a meter in front of the camera, 4 m from the wall in focus.
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(0.0f)), glm::vec3(0.0f, 0.0f, 3.5f), "Card")
			.GetComponent<TransformComponent>().Scale = glm::vec3(0.1f, 0.1f, 1.0f);
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const SceneCamera camera = EdgeWall::CameraAt(4.0f);
		const CameraLens lens = Lens(4.0f, 2.0f);
		const float radius = -0.5f * lens.GetCircleOfConfusion(0.5f, static_cast<float>(c_Height));
		REQUIRE(radius > 5.0f);
		const ReadbackImage sharp = RenderImage(renderer, scene, camera);
		const ReadbackImage defocused = RenderImage(renderer, scene, camera, WithLens(lens));
		CHECK(renderer.GetStats().DepthOfField);
		const uint32_t row = c_Height / 2;
		// The card's right edge in the sharp image.
		uint32_t edge = c_Width / 2;
		while (edge < c_Width && Red(sharp, edge, row) < 128)
			edge++;
		REQUIRE(edge < c_Width - 100);
		REQUIRE(edge > c_Width / 2 + 20);
		// Outside the card, within the blur's radius: the card's blur darkens the wall in focus.
		const uint32_t inside = edge + static_cast<uint32_t>(0.3f * radius);
		CHECK(Red(sharp, inside, row) == 255);
		CHECK(Red(defocused, inside, row) < 230);
		// Beyond the blur: the wall exactly.
		const uint32_t beyond = edge + static_cast<uint32_t>(2.0f * radius) + 4;
		CHECK(Red(defocused, beyond, row) == Red(sharp, beyond, row));
		CHECK(Red(defocused, edge + 200, row) == Red(sharp, edge + 200, row));
		// The card's middle stays the card's.
		CHECK(Red(defocused, c_Width / 2, row) < 10);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Under TAA an in-focus silhouette over a defocused background flickers no more than without the lens")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene("Silhouette");
		PostProcessComponent& postProcess = AddNeutralPostProcess(scene);
		postProcess.AntiAliasing = true;
		postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
		// A white wall 20 m from the camera, far out of focus, and a black card 1 m from it in focus, tilted a little so its edges
		// cross the pixels at every offset: its silhouette lies on in-focus depth in some of TAA's jitter phases and on the wall's
		// in others.
		AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(1.0f)), glm::vec3(0.0f, 0.0f, -16.0f), "Wall")
			.GetComponent<TransformComponent>().Scale = glm::vec3(100.0f, 100.0f, 1.0f);
		TransformComponent& card = AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(0.0f)), glm::vec3(-0.25f, 0.0f, 3.0f), "Card")
			.GetComponent<TransformComponent>();
		card.Scale = glm::vec3(0.5f, 0.5f, 1.0f);
		card.Rotation = glm::angleAxis(glm::radians(3.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		const SceneCamera camera = EdgeWall::CameraAt(4.0f);
		const CameraLens lens = Lens(1.0f, 1.0f);
		REQUIRE(lens.GetCircleOfConfusion(20.0f, static_cast<float>(c_Height)) > 15.0f);
		// The largest change of any pixel between consecutive frames, over 16 frames (two cycles of the jitter) after the history
		// converged.
		auto flicker = [&](std::optional<CameraLens> withLens)
		{
			SceneRenderer renderer(ExactSpecification());
			renderer.SetViewportSize(c_Width, c_Height);
			SceneRenderOptions options;
			options.Lens = withLens;
			for (int frame = 0; frame < 24; frame++)
				REQUIRE(renderer.Render(scene, camera, nullptr, options));
			CHECK(renderer.GetStats().TemporalAntiAliasing);
			CHECK(renderer.GetStats().DepthOfField == withLens.has_value());
			ReadbackImage previous = RenderImage(renderer, scene, camera, options);
			int largest = 0;
			for (int frame = 0; frame < 16; frame++)
			{
				ReadbackImage image = RenderImage(renderer, scene, camera, options);
				largest = std::max(largest, MaxDifference(previous, image));
				previous = std::move(image);
			}
			return largest;
		};
		const int sharp = flicker(std::nullopt);
		const int defocused = flicker(lens);
		MESSAGE("largest change between frames: ", sharp, " codes without the lens, ", defocused, " with it");
		CHECK(defocused <= sharp + 6);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Both draw paths defocus equally")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		AddMesh(wall.SceneData, BuiltinAssets::CubeMesh, assets.AddMaterial(Emitting(0.5f)), glm::vec3(0.3f, 0.2f, 2.5f), "Box")
			.GetComponent<TransformComponent>().Scale = glm::vec3(0.2f);
		SceneRendererSpecification cpuSpecification = ExactSpecification();
		cpuSpecification.GPUDriven = GPUDrivenMode::Off;
		SceneRendererSpecification gpuSpecification = ExactSpecification();
		gpuSpecification.GPUDriven = GPUDrivenMode::Auto;
		SceneRenderer cpu(cpuSpecification);
		SceneRenderer gpuDriven(gpuSpecification);
		ReadbackImage images[2];
		SceneRenderer* renderers[2] = { &cpu, &gpuDriven };
		for (int index = 0; index < 2; index++)
		{
			renderers[index]->SetViewportSize(c_Width, c_Height);
			for (int frame = 0; frame < 3; frame++)
				images[index] = RenderImage(*renderers[index], wall.SceneData, EdgeWall::CameraAt(4.0f), WithLens(Lens(2.0f, 1.4f)));
			CHECK(renderers[index]->GetStats().DepthOfField);
		}
		CHECK(gpuDriven.GetStats().GPUDriven);
		CHECK(MaxDifference(images[0], images[1]) <= 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Off costs nothing")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		struct Case
		{
			const char* Name;
			bool Lens;
			SceneEffectChoice Player;
			bool Orthographic;
		};
		const Case cases[] = {
			{ "no lens", false, SceneEffectChoice::Scene, false },
			{ "player off", true, SceneEffectChoice::Off, false },
			{ "orthographic", true, SceneEffectChoice::Scene, true },
		};
		for (const Case& testCase : cases)
		{
			SceneRendererSpecification specification = ExactSpecification();
			specification.Quality.DepthOfField = testCase.Player;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_Width, c_Height);
			SceneRenderOptions options;
			if (testCase.Lens)
				options.Lens = Lens(1.0f, 1.4f);
			const SceneCamera camera = testCase.Orthographic ? OrthographicLookAt(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f), 4.0f) : EdgeWall::CameraAt(4.0f);
			RenderImage(renderer, wall.SceneData, camera, options);
			CAPTURE(testCase.Name);
			CHECK_FALSE(renderer.GetStats().DepthOfField);
			CHECK(renderer.GetDepthOfFieldTexture() == nullptr);
		}
		CHECK(RendererQuality::GetPreset(RendererQualityPreset::Low).DepthOfField == SceneEffectChoice::Scene);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Defocused pixels are not sharpened, and the motion blur keeps their mark")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		wall.PostProcess->AntiAliasing = true;
		wall.PostProcess->AntiAliasingMethod = AntiAliasingMode::TAA;
		wall.PostProcess->TAAPreset = TemporalAntiAliasingPreset::Custom;
		SetTemporalAntiAliasingSettings(*wall.PostProcess, c_TAASharpSettings);
		// A still camera with motion blur on: its gather copies every pixel, keeping the depth of field's mark.
		wall.PostProcess->MotionBlur = true;
		auto render = [&](float sharpening, ReadbackImage& outMask, ReadbackImage& outMotionMask)
		{
			wall.PostProcess->TAASharpening = sharpening;
			SceneRenderer renderer(ExactSpecification());
			renderer.SetViewportSize(c_Width, c_Height);
			ReadbackImage image;
			for (int frame = 0; frame < 3; frame++)
				image = RenderImage(renderer, wall.SceneData, EdgeWall::CameraAt(4.0f), WithLens(Lens(1.0f, 1.4f)));
			CHECK(renderer.GetStats().DepthOfField);
			CHECK(renderer.GetStats().MotionBlurred);
			CHECK(renderer.GetStats().TemporalAntiAliasing);
			REQUIRE(Renderer::ReadTexture(renderer.GetDepthOfFieldTexture(), outMask));
			REQUIRE(Renderer::ReadTexture(renderer.GetMotionBlurTexture(), outMotionMask));
			return image;
		};
		ReadbackImage mask;
		ReadbackImage motionMask;
		const ReadbackImage sharpened = render(0.5f, mask, motionMask);
		ReadbackImage unusedMask;
		ReadbackImage unusedMotionMask;
		const ReadbackImage unsharpened = render(0.0f, unusedMask, unusedMotionMask);
		uint32_t defocused = 0;
		uint32_t differences = 0;
		uint32_t lostMarks = 0;
		for (uint32_t y = 0; y < c_Height; y += 3)
		{
			for (uint32_t x = 0; x < c_Width; x += 3)
			{
				const float mark = ReadHalfTexel(mask, x, y).a;
				lostMarks += ReadHalfTexel(motionMask, x, y).a != mark ? 1u : 0u;
				if (mark < 0.5f)
					continue;
				defocused++;
				differences += GetPixelRGBA8(sharpened, x, y) != GetPixelRGBA8(unsharpened, x, y) ? 1u : 0u;
			}
		}
		CHECK(defocused > 1000);
		CHECK(differences == 0);
		CHECK(lostMarks == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
