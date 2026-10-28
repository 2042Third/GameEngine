#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/RendererComponents.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// The sun's contact shadows (Effects/ScreenShadows.frag: a march toward the sun over the depth buffer): they ground what the
// cascades are too coarse for, never shadow an open plane or a convex lit surface, have no grain without anti-aliasing,
// fade with distance and at the screen's edges, and Medium has none.

namespace
{

	constexpr uint32_t c_Size = 256;
	constexpr float c_PlateHalfWidth = 0.15f;
	constexpr float c_PlateHeight = 0.05f;

	SceneRendererSpecification ContactSpecification(uint32_t steps, GPUDrivenMode mode = GPUDrivenMode::Off)
	{
		SceneRendererSpecification specification;
		specification.GPUDriven = mode;
		specification.TextureStreaming = false;
		specification.Quality.ShadowCascades = 1;
		specification.Quality.ShadowResolution = ShadowMapResolution::Size1024;
		specification.Quality.ContactShadows = steps;
		return specification;
	}

	// A 0.3 x 0.05 x 0.3 m plate resting on a floor under a sun from +X, 30 degrees high, so it shades 0.05 / tan(30 degrees) =
	// 8.7 cm of the floor beyond its -X edge. One 1024-texel cascade over 400 m has texels of about 0.75 m: the plate is below a
	// texel, a caster the cascades leave out.
	struct PlateScene
	{
		Scene SceneData;
		Entity Sun;
		Entity Plate;

		explicit PlateScene(float shadowDistance = 400.0f)
		{
			AddNeutralPostProcess(SceneData);
			AddMesh(SceneData, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(400.0f);
			Plate = AddMesh(SceneData, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 0.5f * c_PlateHeight, 0.0f), "Plate");
			Plate.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f * c_PlateHalfWidth, c_PlateHeight, 2.0f * c_PlateHalfWidth);
			Sun = AddSun(SceneData, SunDirection(30.0f, 0.0f), 3.0f);
			Sun.GetComponent<DirectionalLightComponent>().ShadowDistance = shadowDistance;
		}

		void SetLength(float meters)
		{
			Sun.GetComponent<DirectionalLightComponent>().ContactShadowLength = meters;
			Sun.MarkModified<DirectionalLightComponent>();
		}

		// The plate out of the cascades whatever their texels: only contact shadows can shade the floor beside it.
		void CastNoCascadeShadow()
		{
			Plate.GetComponent<MeshRendererComponent>().CastShadows = false;
			Plate.MarkModified<MeshRendererComponent>();
		}
	};

	SceneCamera WithFieldOfView(SceneCamera camera, float degrees, float farPlane = 500.0f)
	{
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(degrees), 1.0f, 0.1f, farPlane);
		camera.VerticalFOV = glm::radians(degrees);
		camera.Far = farPlane;
		return camera;
	}

	// The look-dev contact shot's framing: 2.6 m from the plate with a 40 degree field of view, from -Z and above.
	SceneCamera PlateCamera()
	{
		return WithFieldOfView(LookAt(glm::vec3(0.0f, 1.3f, -2.6f), glm::vec3(0.0f)), 40.0f);
	}

	// Straight down from `height` over (x, 0, 0): image columns follow +X, rows +Z.
	SceneCamera TopCamera(float x, float height, float degrees)
	{
		return WithFieldOfView(LookAt(glm::vec3(x, height, 0.0f), glm::vec3(x, 0.0f, 0.0f)), degrees);
	}

	float ShareAt(const SunlightImage& image, const SceneCamera& camera, const glm::vec3& point)
	{
		const glm::uvec2 pixel = ProjectToPixel(camera, point, image.Size);
		REQUIRE(pixel.x < image.Size);
		REQUIRE(pixel.y < image.Size);
		return image.At(pixel.x, pixel.y);
	}

	// The mean of the sun's share lost over the floor beside the plate's -X edge (x from the edge minus `from` to minus `to`, |z|
	// below 0.1 m), seen by `camera`.
	float MeanShade(const SunlightImage& image, const SceneCamera& camera, float from, float to)
	{
		double sum = 0.0;
		uint32_t count = 0;
		for (float x = -c_PlateHalfWidth - from; x >= -c_PlateHalfWidth - to; x -= 0.0025f)
		{
			for (float z = -0.1f; z <= 0.1f; z += 0.0025f)
			{
				const glm::uvec2 pixel = ProjectToPixel(camera, glm::vec3(x, 0.0f, z), image.Size);
				if (pixel.x >= image.Size || pixel.y >= image.Size || image.At(pixel.x, pixel.y) < 0.0f)
					continue;
				sum += 1.0 - image.At(pixel.x, pixel.y);
				count++;
			}
		}
		REQUIRE(count > 0);
		return static_cast<float>(sum / count);
	}

	int LargestRedDifference(const ReadbackImage& a, const ReadbackImage& b, uint32_t size)
	{
		int largest = 0;
		for (uint32_t y = 0; y < size; y++)
		{
			for (uint32_t x = 0; x < size; x++)
				largest = std::max(largest, std::abs(Red(a, x, y) - Red(b, x, y)));
		}
		return largest;
	}

}

TEST_SUITE("GPU.SceneRenderer.ContactShadows")
{
	TEST_CASE("A 5 cm plate on the floor casts a contact shadow the cascades are too coarse for")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlateScene plate;
		const SceneCamera camera = PlateCamera();
		SceneRenderer renderer(ContactSpecification(12));
		renderer.SetViewportSize(c_Size, c_Size);
		// Inside the 8.7 cm the plate shades, and well beyond it.
		const glm::vec3 beside(-c_PlateHalfWidth - 0.04f, 0.0f, 0.0f);
		const glm::vec3 open(-c_PlateHalfWidth - 0.40f, 0.0f, 0.0f);
		SunlightImage sunlight = RenderSunlight(renderer, plate.SceneData, camera, plate.Sun);
		CHECK(sunlight.Stats.SubTexelCasters >= 1); // The plate: the cascades have no shadow of it
		CHECK(ShareAt(sunlight, camera, beside) < 0.5f);
		CHECK(ShareAt(sunlight, camera, open) > 0.95f);
		// The light's length 0 turns them off: the floor beside the plate is lit.
		plate.SetLength(0.0f);
		sunlight = RenderSunlight(renderer, plate.SceneData, camera, plate.Sun);
		CHECK(ShareAt(sunlight, camera, beside) > 0.95f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Contact shadows never shadow an open plane")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(80.0f);
		Entity sun = AddSun(scene, glm::vec3(0.0f, -1.0f, 0.0f), 1.0f);
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 3.0f, 6.0f), glm::vec3(0.0f, 0.0f, -6.0f));
		for (const uint32_t steps : { 12u, 16u })
		{
			SceneRenderer renderer(ContactSpecification(steps));
			renderer.SetViewportSize(128, 128);
			// Grazing suns too: a ray rising off the floor never reads the floor's own pixels as occluders.
			for (const float elevation : { 3.0f, 8.0f, 20.0f, 60.0f })
			{
				CAPTURE(steps);
				CAPTURE(elevation);
				DirectionalLightComponent& light = sun.GetComponent<DirectionalLightComponent>();
				sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(SunDirection(elevation, 35.0f));
				sun.MarkModified<TransformComponent>();
				light.CastShadows = true;
				sun.MarkModified<DirectionalLightComponent>();
				const ReadbackImage shadowed = RenderToImage(renderer, scene, camera);
				CHECK(renderer.GetStats().ScreenShadows);
				light.CastShadows = false;
				sun.MarkModified<DirectionalLightComponent>();
				const ReadbackImage reference = RenderToImage(renderer, scene, camera);
				CHECK(LargestRedDifference(shadowed, reference, 128) <= 2);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Contact shadows never darken a convex lit surface")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		// Coarse smooth-shaded meshes, casting nothing into the cascades: only contact shadows could darken them. A sphere of 16 x 8
		// segments (vertex normals up to about 11 degrees off its faces) and a cylinder of 8 (22.5 degrees, and faces 2 m long):
		// near their terminators some faces turn from the sun under normals turned toward it, and a ray from such a face that
		// runs along it, as the sun's rays run up the cylinder's sides, enters the mesh.
		struct Shape
		{
			const char* Name;
			Ref<Mesh> Mesh;
			glm::vec3 Center;
		};
		const Shape shapes[] = { { "Sphere", MeshFactory::CreateSphere(0.5f, 16, 8), glm::vec3(0.0f) },
			{ "Cylinder", MeshFactory::CreateCylinder(0.5f, 2.0f, 8), glm::vec3(0.0f) } };
		for (const Shape& shape : shapes)
		{
			const std::string name = shape.Name;
			CAPTURE(name);
			Scene scene;
			AddNeutralPostProcess(scene);
			Entity mesh = AddMesh(scene, assets.Add(shape.Mesh, shape.Name), UUID::Null(), shape.Center, shape.Name);
			mesh.GetComponent<MeshRendererComponent>().CastShadows = false;
			// The sun's share of each pixel (HDR), not the 8-bit image: the faces in question are lit at grazing angles, where
			// a whole shadow is a step or two of 8 bits.
			Entity sun = AddSun(scene, SunDirection(45.0f, 0.0f), 50.0f);
			const SceneCamera camera = LookAt(shape.Center + glm::vec3(0.0f, 0.6f, -2.5f), shape.Center);
			for (const uint32_t steps : { 12u, 16u })
			{
				SceneRenderer renderer(ContactSpecification(steps));
				renderer.SetViewportSize(c_Size, c_Size);
				// Suns from the side (their terminators face the camera) and from the front.
				for (const float elevation : { 20.0f, 45.0f, 70.0f })
				{
					for (const float azimuth : { 0.0f, 10.0f, 20.0f, 340.0f, 350.0f, 300.0f })
					{
						CAPTURE(steps);
						CAPTURE(elevation);
						CAPTURE(azimuth);
						sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(SunDirection(elevation, azimuth));
						sun.MarkModified<TransformComponent>();
						const SunlightImage sunlight = RenderSunlight(renderer, scene, camera, sun);
						uint32_t lit = 0;
						uint32_t darkened = 0;
						float least = 1.0f;
						for (const float share : sunlight.Fraction)
						{
							if (share < 0.0f)
								continue; // No sunlight reaches the pixel
							lit++;
							least = std::min(least, share);
							if (share < 0.98f)
								darkened++;
						}
						CAPTURE(least);
						CHECK(lit > 1000);
						CHECK(darkened == 0);
					}
				}
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Contact shadows have no grain without anti-aliasing")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlateScene plate;
		// Straight down 1.5 m over the shaded strip, 1024 pixels: a march of 12 steps over 51 pixels (5% of the height), about 4
		// pixels a step, so the per-pixel offsets dither its far edge over about 4 pixels unless the blur follows the step.
		constexpr uint32_t size = 1024;
		const SceneCamera camera = TopCamera(-c_PlateHalfWidth - 0.05f, 1.5f, 40.0f);
		SceneRenderer renderer(ContactSpecification(12));
		renderer.SetViewportSize(size, size);
		const SunlightImage sunlight = RenderSunlight(renderer, plate.SceneData, camera, plate.Sun);
		// The shadow is the same all along the plate's edge (z): per column of the floor beside the edge, the RMS of each pixel's
		// difference from its column's mean, against the shadow's contrast. The rows from z = -0.1 to 0.1 m.
		const glm::uvec2 nearCorner = ProjectToPixel(camera, glm::vec3(-c_PlateHalfWidth - 0.005f, 0.0f, -0.1f), size);
		const glm::uvec2 farCorner = ProjectToPixel(camera, glm::vec3(-c_PlateHalfWidth - 0.12f, 0.0f, 0.1f), size);
		REQUIRE(nearCorner.x < size);
		REQUIRE(farCorner.y < size);
		const uint32_t firstColumn = std::min(nearCorner.x, farCorner.x);
		const uint32_t lastColumn = std::max(nearCorner.x, farCorner.x);
		const uint32_t firstRow = std::min(nearCorner.y, farCorner.y);
		const uint32_t lastRow = std::max(nearCorner.y, farCorner.y);
		REQUIRE(lastColumn > firstColumn + 50);
		REQUIRE(lastRow > firstRow + 50);
		double squares = 0.0;
		size_t count = 0;
		float darkest = 1.0f;
		for (uint32_t x = firstColumn; x <= lastColumn; x++)
		{
			double mean = 0.0;
			for (uint32_t y = firstRow; y <= lastRow; y++)
				mean += sunlight.At(x, y);
			mean /= static_cast<double>(lastRow - firstRow + 1);
			darkest = std::min(darkest, static_cast<float>(mean));
			for (uint32_t y = firstRow; y <= lastRow; y++)
			{
				squares += (sunlight.At(x, y) - mean) * (sunlight.At(x, y) - mean);
				count++;
			}
		}
		const double contrast = 1.0 - darkest;
		REQUIRE(contrast > 0.5); // The contact shadow is there
		const double noise = std::sqrt(squares / static_cast<double>(count)) / contrast;
		MESSAGE("Contact shadow grain along the edge: " << noise * 100.0 << "% of the contrast");
		CHECK(noise <= 0.02);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Contact shadows fade with distance and at the screen's edges")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		// A shadow distance of 10 m (fading from 5 m), and a plate the cascades never see: whatever shades the floor beside it is a
		// contact shadow.
		constexpr float shadowDistance = 10.0f;
		PlateScene plate(shadowDistance);
		plate.CastNoCascadeShadow();
		SceneRenderer renderer(ContactSpecification(12));
		renderer.SetViewportSize(c_Size, c_Size);

		// Near (2 m) and at 0.95 of the shadow distance, the far camera's field of view narrowed so the plate fills as many pixels.
		const glm::vec3 target(-c_PlateHalfWidth - 0.04f, 0.0f, 0.0f);
		const glm::vec3 direction = glm::normalize(glm::vec3(0.0f, 0.5f, -1.0f));
		const SceneCamera nearCamera = WithFieldOfView(LookAt(target + direction * 2.0f, target), 40.0f);
		const float farDistance = 0.95f * shadowDistance;
		const float farDegrees = glm::degrees(2.0f * std::atan(std::tan(glm::radians(20.0f)) * 2.0f / farDistance));
		const SceneCamera farCamera = WithFieldOfView(LookAt(target + direction * farDistance, target), farDegrees);
		const float nearShade = MeanShade(RenderSunlight(renderer, plate.SceneData, nearCamera, plate.Sun), nearCamera, 0.01f, 0.05f);
		const float farShade = MeanShade(RenderSunlight(renderer, plate.SceneData, farCamera, plate.Sun), farCamera, 0.01f, 0.05f);
		CAPTURE(nearShade);
		CAPTURE(farShade);
		CHECK(nearShade > 0.5f);
		CHECK(farShade <= 0.2f * nearShade);

		// Straight down, the plate's -X face at the image's center and then 1% of the width from its right edge: what the march
		// finds there fades out, so an occluder leaving the screen leaves no band.
		const float halfWidth = 1.5f * std::tan(glm::radians(20.0f)); // Of the floor in view
		const SceneCamera centered = TopCamera(-c_PlateHalfWidth, 1.5f, 40.0f);
		const SceneCamera atEdge = TopCamera(-c_PlateHalfWidth - 0.98f * halfWidth, 1.5f, 40.0f);
		const float centeredShade = MeanShade(RenderSunlight(renderer, plate.SceneData, centered, plate.Sun), centered, 0.005f, 0.03f);
		const float edgeShade = MeanShade(RenderSunlight(renderer, plate.SceneData, atEdge, plate.Sun), atEdge, 0.005f, 0.03f);
		CAPTURE(centeredShade);
		CAPTURE(edgeShade);
		CHECK(centeredShade > 0.5f);
		CHECK(edgeShade <= 0.25f * centeredShade);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Medium draws no contact shadows")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlateScene plate;
		const SceneCamera camera = PlateCamera();
		SceneRendererSpecification medium;
		medium.TextureStreaming = false;
		CHECK(medium.Quality.ContactShadows == 0);
		SceneRenderer renderer(medium);
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage withLength = RenderToImage(renderer, plate.SceneData, camera);
		plate.SetLength(0.0f);
		const ReadbackImage without = RenderToImage(renderer, plate.SceneData, camera);
		CHECK(withLength.Pixels == without.Pixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
