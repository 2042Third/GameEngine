#include "Renderer/GIScenes.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/GlobalIlluminationLayout.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// The owner's room experiments on global illumination's gi view (Docs/DeveloperGuide.md, "Global illumination"): the look-dev
// room lit by the sun through its window (BuildSunRoom), one bounce, against an independent CPU reference (SunRoomReference).
// The gi view shows the field's diffuse light on white surfaces: inside the field it is the field alone (neutral post-processing:
// no ambient occlusion), so its luminance at a surface point is the bounce the field gives there.

namespace
{

	constexpr float c_RoomPreExposure = 1.0f / 8192.0f;
	constexpr glm::uvec2 c_RoomSize { 960, 540 };
	constexpr glm::uvec2 c_FloorSize { 512, 512 };
	constexpr float c_SealedBound = 0.0017f; // erhe's leak between rooms

	struct RoomPoint
	{
		std::string Name;
		glm::vec3 Point;
		glm::vec3 Normal;
	};

	const glm::vec3 c_Down(0.0f, -1.0f, 0.0f);
	const glm::vec3 c_Up(0.0f, 1.0f, 0.0f);
	const glm::vec3 c_East(1.0f, 0.0f, 0.0f);
	const glm::vec3 c_North(0.0f, 0.0f, -1.0f);

	// The ceiling and walls the room camera sees (the first seven are on walls: the red floor's case reads them).
	std::vector<RoomPoint> GetWallPoints(const glm::vec3& offset = glm::vec3(0.0f))
	{
		std::vector<RoomPoint> points = {
			{ "west wall low", { -13.0f, 1.0f, -0.5f }, c_East },
			{ "west wall high", { -13.0f, 2.0f, -1.5f }, c_East },
			{ "east wall low", { -7.0f, 1.0f, -0.5f }, -c_East },
			{ "east wall high", { -7.0f, 2.0f, -1.5f }, -c_East },
			{ "south wall west", { -12.0f, 1.5f, 1.0f }, c_North },
			{ "south wall east", { -8.0f, 1.5f, 1.0f }, c_North },
			{ "below the window", { -10.0f, 0.5f, 1.0f }, c_North },
			{ "ceiling over the window side", { -10.5f, 3.0f, -0.5f }, c_Down },
			{ "ceiling middle", { -10.0f, 3.0f, -1.5f }, c_Down }, // The stash's z -2.5 lies above the camera's view
			{ "ceiling east", { -8.0f, 3.0f, -1.0f }, c_Down },
		};
		for (RoomPoint& point : points)
			point.Point += offset;
		return points;
	}

	// The floor on a 3 x 3 grid inset 0.6 m, and its junctions with the walls (0.05 and 0.15 m from each, a three-wall corner).
	std::vector<RoomPoint> GetFloorPoints(const glm::vec3& offset = glm::vec3(0.0f), bool junctions = false)
	{
		std::vector<RoomPoint> points;
		for (float x : { -12.4f, -10.0f, -7.6f })
		{
			for (float z : { -4.4f, -2.0f, 0.4f })
				points.push_back({ "floor", { x, 0.1f, z }, c_Up });
		}
		if (junctions)
		{
			for (float inset : { 0.05f, 0.15f })
			{
				points.push_back({ "floor by the west wall", { -13.0f + inset, 0.1f, -2.0f }, c_Up });
				points.push_back({ "floor by the east wall", { -7.0f - inset, 0.1f, -2.0f }, c_Up });
				points.push_back({ "floor by the north wall", { -10.0f, 0.1f, -5.0f + inset }, c_Up });
				points.push_back({ "floor by the south wall", { -12.0f, 0.1f, 1.0f - inset }, c_Up });
			}
			points.push_back({ "floor in the north-west corner", { -12.95f, 0.1f, -4.95f }, c_Up });
		}
		for (RoomPoint& point : points)
			point.Point += offset;
		return points;
	}

	SceneCamera MakeCamera(const glm::vec3& position, const glm::vec3& target, float verticalDegrees, glm::uvec2 size)
	{
		const float aspectRatio = static_cast<float>(size.x) / static_cast<float>(size.y);
		SceneCamera camera = LookAt(position, target, aspectRatio);
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(verticalDegrees), aspectRatio, 0.05f, 1000.0f);
		camera.Far = 1000.0f;
		camera.VerticalFOV = glm::radians(verticalDegrees);
		return camera;
	}

	// The stash's room camera: toward the window, 70 degrees.
	SceneCamera RoomCamera(const glm::vec3& offset = glm::vec3(0.0f))
	{
		return MakeCamera(glm::vec3(-9.0f, 1.5f, -4.5f) + offset, glm::vec3(-10.0f, 1.2f, 1.1f) + offset, 70.0f, c_RoomSize);
	}

	// Straight down at the floor from under the ceiling, 100 degrees.
	SceneCamera FloorCamera(const glm::vec3& offset = glm::vec3(0.0f))
	{
		return MakeCamera(glm::vec3(-10.0f, 2.9f, -2.0f) + offset, glm::vec3(-10.0f, 0.0f, -1.999f) + offset, 100.0f, c_FloorSize);
	}

	float Luminance(const glm::vec3& color)
	{
		return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
	}

	SceneRenderOptions GIViewOptions(SceneLightingView view = SceneLightingView::GlobalIllumination)
	{
		SceneRenderOptions options;
		options.LightingView = view;
		options.HDROutput = true;
		options.PreExposure = c_RoomPreExposure;
		return options;
	}

	SceneRendererSpecification RoomSpecification(GlobalIlluminationLevel level = GlobalIlluminationLevel::High)
	{
		SceneRendererSpecification specification = ExactSpecification();
		specification.GlobalIllumination.Level = level;
		specification.GlobalIllumination.VoxelizeTrianglesOnCut = 0; // The first frame is complete
		return specification;
	}

	// The gi view's luminance (rgb) at the points, a 3 x 3 pixel mean around each one's projection.
	std::vector<glm::vec3> ReadPoints(SceneRenderer& renderer, const SceneCamera& camera, const std::vector<RoomPoint>& points)
	{
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), image));
		REQUIRE(image.Format == nvrhi::Format::RGBA16_FLOAT);
		std::vector<glm::vec3> values;
		for (const RoomPoint& point : points)
		{
			// Read just off the surface toward the camera, so the projection lands on the surface's own pixels.
			const glm::uvec2 pixel = ProjectToPixel(camera, point.Point + point.Normal * 0.01f, image.Width, image.Height);
			INFO(point.Name << " at " << pixel.x << ", " << pixel.y);
			REQUIRE(pixel.x > 2);
			REQUIRE(pixel.y > 2);
			REQUIRE(pixel.x + 3 < image.Width);
			REQUIRE(pixel.y + 3 < image.Height);
			glm::vec3 sum(0.0f);
			for (int dy = -1; dy <= 1; dy++)
			{
				for (int dx = -1; dx <= 1; dx++)
					sum += glm::vec3(ReadHalfTexel(image, pixel.x + dx, pixel.y + dy));
			}
			values.push_back(sum / 9.0f / c_RoomPreExposure);
		}
		return values;
	}

	// A sun room seen by a camera: renders until the GI settles (at most 8 frames) and reads the points.
	struct RoomRender
	{
		SceneTestAssets Assets;
		Scene SceneData;
		Scope<SceneRenderer> Renderer;
		glm::uvec2 Size { 0 };

		RoomRender(const SunRoomOptions& room, glm::uvec2 size, const SceneRendererSpecification& specification)
			: Size(size)
		{
			BuildSunRoom(SceneData, Assets, room);
			Renderer = CreateScope<SceneRenderer>(specification);
			Renderer->SetViewportSize(size.x, size.y);
		}

		std::vector<glm::vec3> Measure(const SceneCamera& camera, const std::vector<RoomPoint>& points, uint32_t frames = 3)
		{
			for (uint32_t frame = 0; frame < frames; frame++)
				REQUIRE(Renderer->Render(SceneData, camera, nullptr, GIViewOptions()));
			REQUIRE(Renderer->GetStats().GlobalIllumination);
			REQUIRE_FALSE(Renderer->GetStats().GlobalIlluminationPending);
			return ReadPoints(*Renderer, camera, points);
		}

		void SetSun(const glm::vec3& direction)
		{
			for (auto [entity, light, transform] : SceneData.GetRegistry().view<DirectionalLightComponent, TransformComponent>().each())
			{
				transform.Rotation = Math::LookRotation(direction);
				SceneData.GetRegistry().patch<TransformComponent>(entity);
			}
		}
	};

	std::vector<glm::vec3> References(const SunRoomOptions& room, const std::vector<RoomPoint>& points)
	{
		std::vector<glm::vec3> references;
		for (const RoomPoint& point : points)
			references.push_back(SunRoomReference(room, point.Point, point.Normal));
		return references;
	}

	float MeanLuminance(const std::vector<glm::vec3>& values)
	{
		float sum = 0.0f;
		for (const glm::vec3& value : values)
			sum += Luminance(value);
		return values.empty() ? 0.0f : sum / static_cast<float>(values.size());
	}

	SunRoomOptions DefaultRoom()
	{
		SunRoomOptions room;
		room.SunDirection = SunRoomDirection(-40.0f, 30.0f);
		return room;
	}

}

TEST_SUITE("GPU.SceneRenderer.GlobalIlluminationRoom")
{
	TEST_CASE("A red floor tints the walls")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const SunRoomOptions white = DefaultRoom();
		SunRoomOptions red = white;
		red.FloorAlbedo = glm::vec3(0.62f, 0.09f, 0.07f);
		std::vector<RoomPoint> walls = GetWallPoints();
		walls.resize(7);
		const auto ratio = [](const glm::vec3& color) { return color.r / std::max(color.g, 1e-6f); };
		RoomRender whiteRender(white, c_RoomSize, RoomSpecification());
		RoomRender redRender(red, c_RoomSize, RoomSpecification());
		const std::vector<glm::vec3> whiteMeasured = whiteRender.Measure(RoomCamera(), walls);
		const std::vector<glm::vec3> redMeasured = redRender.Measure(RoomCamera(), walls);
		const std::vector<glm::vec3> whiteReferences = References(white, walls);
		const std::vector<glm::vec3> redReferences = References(red, walls);
		float measuredChange = 0.0f;
		float referenceChange = 0.0f;
		for (size_t index = 0; index < walls.size(); index++)
		{
			measuredChange += ratio(redMeasured[index]) - ratio(whiteMeasured[index]);
			referenceChange += ratio(redReferences[index]) - ratio(whiteReferences[index]);
		}
		MESSAGE("R/G change over the seven wall points: field " << measuredChange / 7.0f << ", reference " << referenceChange / 7.0f);
		CHECK(referenceChange > 0.0f);
		CHECK(measuredChange >= 0.7f * referenceChange);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Moving the sun moves the bounce in the same frame")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const SunRoomOptions first = DefaultRoom();
		SunRoomOptions second = first;
		second.SunDirection = SunRoomDirection(-40.0f, 60.0f);
		const std::vector<RoomPoint> points = GetWallPoints();
		RoomRender moving(first, c_RoomSize, RoomSpecification());
		const std::vector<glm::vec3> before = moving.Measure(RoomCamera(), points, 4);
		moving.SetSun(second.SunDirection);
		const std::vector<glm::vec3> after = moving.Measure(RoomCamera(), points, 1);
		RoomRender fresh(second, c_RoomSize, RoomSpecification());
		const std::vector<glm::vec3> expected = fresh.Measure(RoomCamera(), points);
		uint32_t changed = 0;
		for (size_t index = 0; index < points.size(); index++)
		{
			INFO(points[index].Name);
			const float value = Luminance(after[index]);
			CHECK(std::abs(value - Luminance(expected[index])) <= 0.01f * std::max(Luminance(expected[index]), 1e-3f));
			if (std::abs(value - Luminance(before[index])) > 0.1f * std::max(Luminance(before[index]), 1e-6f))
				changed++;
		}
		MESSAGE("points changed by more than 10%: " << changed);
		CHECK(changed >= 3);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("No sky through misses")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SunRoomOptions room;
		room.SunIlluminance = 0.0f;
		room.SealWindow = true;
		room.AmbientColor = glm::vec3(1000.0f);
		const std::vector<RoomPoint> inside = GetFloorPoints(glm::vec3(0.0f), true);
		RoomRender interior(room, c_FloorSize, RoomSpecification());
		const std::vector<glm::vec3> values = interior.Measure(FloorCamera(), inside);
		// The open ground outside, south of the room, seen from above.
		const std::vector<RoomPoint> ground = { { "ground", { -10.0f, 0.0f, 6.0f }, c_Up } };
		RoomRender outside(room, c_FloorSize, RoomSpecification());
		const float groundValue = Luminance(outside.Measure(MakeCamera(glm::vec3(-10.0f, 2.0f, 6.0f), glm::vec3(-10.0f, 0.0f, 6.001f), 90.0f, c_FloorSize), ground)[0]);
		MESSAGE("open ground " << groundValue << ", sealed interior " << MeanLuminance(values));
		CHECK(groundValue > 0.0f);
		for (size_t index = 0; index < inside.size(); index++)
		{
			INFO(inside[index].Name << " " << inside[index].Point.x << ", " << inside[index].Point.z);
			CHECK(Luminance(values[index]) <= c_SealedBound * groundValue);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}

