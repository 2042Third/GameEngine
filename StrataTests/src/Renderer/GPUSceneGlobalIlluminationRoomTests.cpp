#include "Renderer/FlickerMetrics.h"
#include "Renderer/GIScenes.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/DeferredReleaseQueue.h"
#include "Strata/Renderer/GPUProfiler.h"
#include "Strata/Renderer/GlobalIlluminationLayout.h"
#include "Strata/Renderer/RendererQuality.h"

#include <glm/gtc/constants.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
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
	// Frames the sun room's cases render with every bounce: its bounces settle in 12 (GISettleFrames of its plaster's 0.58;
	// "Bounces settle after a cut" checks it), and four more.
	constexpr uint32_t c_BounceFrames = 16;

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

	// One bounce unless asked (`bounceGain` 1): the one-bounce reference (SunRoomReference) and the cases tuned against it.
	SceneRendererSpecification RoomSpecification(GlobalIlluminationLevel level = GlobalIlluminationLevel::High, float bounceGain = 0.0f)
	{
		SceneRendererSpecification specification = ExactSpecification();
		specification.Quality.GlobalIllumination = level;
		specification.GlobalIllumination.VoxelizeTrianglesOnCut = 0; // The first frame is complete
		specification.GlobalIllumination.BounceGain = bounceGain;
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

	// A sun room seen by a camera: renders until the GI settles (at most 8 frames) and reads the points. The renderer resolves
	// assets through the active asset manager: each render activates its own room's, so cases that hold several rooms at once read
	// each with its own materials (the last one made is active, and the others' materials would resolve to the default white).
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
			AssetManager::SetActive(Assets.Manager);
			for (uint32_t frame = 0; frame < frames; frame++)
				REQUIRE(RenderGIFrame(*Renderer, SceneData, camera, GIViewOptions()));
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

	// The voxels of levels 0-2 lying wholly in a box (world) that hold light: injection's leaks, before any probe.
	uint32_t CountLitVoxels(const SceneRenderer& renderer, const glm::vec3& cameraPosition, const glm::vec3& minimum, const glm::vec3& maximum)
	{
		const RenderData::GIFieldConstants& field = renderer.GetGlobalIlluminationFieldConstants();
		const glm::ivec3 dimensions(field.VoxelDimensions);
		uint32_t lit = 0;
		for (uint32_t level = 0; level < 3; level++)
		{
			std::vector<uint8_t> radiance;
			REQUIRE(renderer.ReadGlobalIlluminationVoxels(level, SceneRenderer::GlobalIlluminationTexture::Radiance, radiance));
			const float voxel = field.VoxelOrigin[level].w;
			for (int z = field.VoxelValidMinimum[level].z; z < field.VoxelValidMaximum[level].z; z++)
			{
				for (int y = field.VoxelValidMinimum[level].y; y < field.VoxelValidMaximum[level].y; y++)
				{
					for (int x = field.VoxelValidMinimum[level].x; x < field.VoxelValidMaximum[level].x; x++)
					{
						const glm::vec3 center = cameraPosition + glm::vec3(field.VoxelOrigin[level]) + (glm::vec3(x, y, z) + 0.5f) * voxel;
						if (glm::any(glm::lessThan(center, minimum + 0.5f * voxel)) || glm::any(glm::greaterThan(center, maximum - 0.5f * voxel)))
							continue;
						const glm::ivec3 texel = ((glm::ivec3(x, y, z) + glm::ivec3(field.VoxelTexel[level])) % dimensions + dimensions) % dimensions;
						uint16_t value[4];
						std::memcpy(value, radiance.data() + (texel.x + dimensions.x * (texel.y + dimensions.y * texel.z)) * 8, sizeof(value));
						lit += glm::unpackHalf1x16(value[0]) + glm::unpackHalf1x16(value[1]) + glm::unpackHalf1x16(value[2]) > 0.0f ? 1u : 0u;
					}
				}
			}
		}
		return lit;
	}

	SunRoomOptions DefaultRoom()
	{
		SunRoomOptions room;
		room.SunDirection = SunRoomDirection(-40.0f, 30.0f);
		return room;
	}

	// The index of a cascade's probe at a world position (the field's probes sit at its minimum corner plus (index + 0.5)
	// spacings), for a renderer whose last frame was seen from `camera`.
	glm::ivec3 FindProbe(const SceneRenderer& renderer, const SceneCamera& camera, uint32_t cascade, const glm::vec3& position)
	{
		const GlobalIlluminationLayout* layout = renderer.GetGlobalIlluminationLayout();
		REQUIRE(layout);
		REQUIRE(cascade < layout->Cascades.size());
		const ProbeCascadeLayout& probes = layout->Cascades[cascade];
		const glm::vec3 origin = camera.Position + glm::vec3(renderer.GetGlobalIlluminationFieldConstants().FieldOrigin);
		const glm::ivec3 index = glm::ivec3(glm::round((position - origin) / probes.Spacing - 0.5f));
		INFO("probe at " << position.x << ", " << position.y << ", " << position.z);
		REQUIRE(glm::all(glm::greaterThanEqual(index, glm::ivec3(0))));
		REQUIRE(glm::all(glm::lessThan(index, glm::ivec3(probes.Grid))));
		return index;
	}

	// A cascade-0 probe's ambient cube face (+X, -X, +Y, -Y, +Z, -Z) as the field holds it: the luminance of a white Lambertian
	// surface facing the face's axis there (nits).
	float ReadFieldFace(const SceneRenderer& renderer, const SceneCamera& camera, const std::vector<uint8_t>& field, const glm::vec3& position, uint32_t face)
	{
		const glm::ivec3 probe = FindProbe(renderer, camera, 0, position);
		const glm::ivec3 grid(renderer.GetGlobalIlluminationLayout()->Cascades.front().Grid);
		const size_t x = static_cast<size_t>(probe.x) * 3 + face % 3;
		const size_t y = static_cast<size_t>(probe.y) * 2 + face / 3;
		const size_t texel = x + static_cast<size_t>(3 * grid.x) * (y + static_cast<size_t>(2 * grid.y) * static_cast<size_t>(probe.z));
		REQUIRE((texel + 1) * 8 <= field.size());
		uint16_t value[4];
		std::memcpy(value, field.data() + texel * 8, sizeof(value));
		return Luminance(glm::vec3(glm::unpackHalf1x16(value[0]), glm::unpackHalf1x16(value[1]), glm::unpackHalf1x16(value[2]))) / RenderData::c_GIRadianceScale;
	}

	// The ceiling's junctions with the walls the room camera sees (0.1 m from each).
	std::vector<RoomPoint> GetCeilingJunctions(const glm::vec3& offset = glm::vec3(0.0f))
	{
		std::vector<RoomPoint> points = {
			{ "ceiling by the south wall", { -10.5f, 3.0f, 0.9f }, c_Down },
			{ "ceiling by the west wall", { -12.9f, 3.0f, -0.5f }, c_Down },
		};
		for (RoomPoint& point : points)
			point.Point += offset;
		return points;
	}

	// A table of the field's luminance at the points against the reference's, for MESSAGE; adds the points' absolute errors of
	// the ratio (field over reference, minus 1) to `error`.
	std::string FormatTable(const std::string& title, const std::vector<RoomPoint>& points, const std::vector<glm::vec3>& measured,
		const std::vector<glm::vec3>& references, float& error)
	{
		std::string table = title;
		for (size_t index = 0; index < points.size(); index++)
		{
			const float value = Luminance(measured[index]);
			const float reference = Luminance(references[index]);
			const float ratio = value / std::max(reference, 1e-6f);
			error += std::abs(ratio - 1.0f);
			table += fmt::format("\n  {:<34} field {:10.3f}  reference {:10.3f}  ratio {:7.3f}", points[index].Name, value, reference, ratio);
		}
		return table;
	}

	// The cascades' first spacing at a level.
	float FirstSpacing(GlobalIlluminationLevel level)
	{
		return GlobalIllumination::ComputeLayout(level)->Cascades.front().Spacing;
	}

	// Frees what released renderers left in the deferred release queue (nothing pumps it in a test): the GI's resources are
	// 116 MiB a renderer at High.
	void CollectReleases()
	{
		Renderer::GetDeferredReleases().Collect();
	}

	// The 16 m hall's camera, toward its window from 5 m inside, and the points it reads: the south wall around the window, the
	// floor before it and the ceiling.
	SceneCamera HallCamera(const glm::vec3& offset)
	{
		return MakeCamera(glm::vec3(-10.0f, 1.6f, -4.0f) + offset, glm::vec3(-10.0f, 1.2f, 1.0f) + offset, 70.0f, c_RoomSize);
	}

	std::vector<RoomPoint> GetHallPoints(const glm::vec3& offset)
	{
		std::vector<RoomPoint> points = {
			{ "south wall far west", { -13.5f, 1.0f, 1.0f }, c_North },
			{ "south wall far east", { -6.5f, 1.0f, 1.0f }, c_North },
			{ "south wall west", { -12.0f, 2.0f, 1.0f }, c_North },
			{ "south wall east", { -8.0f, 2.0f, 1.0f }, c_North },
			{ "below the window", { -10.0f, 0.5f, 1.0f }, c_North },
			{ "floor west", { -12.0f, 0.1f, -1.0f }, c_Up },
			{ "floor middle", { -10.0f, 0.1f, -1.0f }, c_Up },
			{ "floor east", { -8.0f, 0.1f, -1.0f }, c_Up },
			{ "floor by the window west", { -11.0f, 0.1f, 0.4f }, c_Up },
			{ "floor by the window east", { -9.0f, 0.1f, 0.4f }, c_Up },
			{ "ceiling west", { -11.0f, 3.0f, -1.0f }, c_Down },
			{ "ceiling east", { -9.0f, 3.0f, -1.0f }, c_Down },
		};
		for (RoomPoint& point : points)
			point.Point += offset;
		return points;
	}

	// The hall's floor and ceiling where they meet its south wall (inside face z 1) on both sides of the window (correction 4's
	// junctions: 0.05 and 0.15 m from the wall on the floor, 0.1 m on the ceiling).
	std::vector<RoomPoint> GetHallJunctions(const glm::vec3& offset)
	{
		std::vector<RoomPoint> points;
		for (const float x : { -12.0f, -8.0f })
		{
			const std::string side = x < -10.0f ? "west" : "east";
			for (const float inset : { 0.05f, 0.15f })
				points.push_back({ fmt::format("floor {} m from the south wall, {}", inset, side), { x, 0.1f, 1.0f - inset }, c_Up });
			points.push_back({ fmt::format("ceiling by the south wall, {}", side), { x, 3.0f, 0.9f }, c_Down });
		}
		for (RoomPoint& point : points)
			point.Point += offset;
		return points;
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

	TEST_CASE("Paired rooms: the sealed one stays dark")
	{
		// Room B is room A moved 6.2 m east (sharing A's east wall), sealed. Room A's light is the nineteen points' mean, as the
		// sealed room's case takes it: one bounce leaves A's floor nearly dark (its reference is 0-60 nits), so a floor mean alone
		// made the bound a thousandth of a nit. Every point of B is checked, its floor's and ceiling's junctions with the walls
		// too (correction 4).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SunRoomOptions room = DefaultRoom();
		room.PairedRoom = true;
		const glm::vec3 toB(6.2f, 0.0f, 0.0f);
		RoomRender wallsA(room, c_RoomSize, RoomSpecification());
		RoomRender floorA(room, c_FloorSize, RoomSpecification());
		const float meanA = 0.5f * (MeanLuminance(wallsA.Measure(RoomCamera(), GetWallPoints())) + MeanLuminance(floorA.Measure(FloorCamera(), GetFloorPoints())));
		std::vector<RoomPoint> wallPointsB = GetWallPoints(toB);
		const std::vector<RoomPoint> ceilingJunctionsB = GetCeilingJunctions(toB);
		wallPointsB.insert(wallPointsB.end(), ceilingJunctionsB.begin(), ceilingJunctionsB.end());
		std::vector<RoomPoint> floorPointsB = GetFloorPoints(toB, true);
		RoomRender wallsB(room, c_RoomSize, RoomSpecification());
		RoomRender floorB(room, c_FloorSize, RoomSpecification());
		const std::vector<glm::vec3> valuesB = wallsB.Measure(RoomCamera(toB), wallPointsB);
		const std::vector<glm::vec3> floorValuesB = floorB.Measure(FloorCamera(toB), floorPointsB);
		MESSAGE("room A " << meanA << ", room B walls " << MeanLuminance(valuesB) << ", floor " << MeanLuminance(floorValuesB));
		CHECK(meanA > 0.0f);
		for (size_t index = 0; index < wallPointsB.size(); index++)
		{
			INFO("room B " << wallPointsB[index].Name);
			CHECK(Luminance(valuesB[index]) <= c_SealedBound * meanA);
		}
		for (size_t index = 0; index < floorPointsB.size(); index++)
		{
			INFO("room B " << floorPointsB[index].Name << " " << floorPointsB[index].Point.x << ", " << floorPointsB[index].Point.z);
			CHECK(Luminance(floorValuesB[index]) <= c_SealedBound * meanA);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The sunlit patch's voxels hold its power")
	{
		// Correction 8's split of a miss into geometry and probes: the floor's lit voxel layer (the layer above its top face, which
		// lies on the 0.1 m grid) at High, its radiance times its area against the reference's sunlit floor sampled at the same
		// columns. Marching the sun's shadow through filtered occupancy, and marking voxels solid that a box's edge only touches
		// from outside, closed the window by a voxel or more at each edge: 48% of the power.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const SunRoomOptions room = DefaultRoom();
		RoomRender render(room, c_RoomSize, RoomSpecification());
		render.Measure(RoomCamera(), GetWallPoints());
		const RenderData::GIFieldConstants& field = render.Renderer->GetGlobalIlluminationFieldConstants();
		std::vector<uint8_t> radiance;
		REQUIRE(render.Renderer->ReadGlobalIlluminationVoxels(0, SceneRenderer::GlobalIlluminationTexture::Radiance, radiance));
		const glm::ivec3 dimensions(field.VoxelDimensions);
		const float voxel = field.VoxelOrigin[0].w;
		REQUIRE(voxel > 0.0f);
		// The renderer's origin is the camera's position.
		const glm::vec3 minimum = glm::vec3(-9.0f, 1.5f, -4.5f) + glm::vec3(field.VoxelOrigin[0]);
		const int layer = static_cast<int>(std::floor((0.15f - minimum.y) / voxel));
		REQUIRE(layer >= field.VoxelValidMinimum[0].y);
		REQUIRE(layer < field.VoxelValidMaximum[0].y);
		double power = 0.0;
		double reference = 0.0;
		glm::dvec2 powerCenter(0.0);
		glm::dvec2 referenceCenter(0.0);
		uint32_t columns = 0;
		for (int z = field.VoxelValidMinimum[0].z; z < field.VoxelValidMaximum[0].z; z++)
		{
			for (int x = field.VoxelValidMinimum[0].x; x < field.VoxelValidMaximum[0].x; x++)
			{
				const glm::vec3 center = minimum + (glm::vec3(x, layer, z) + 0.5f) * voxel;
				if (center.x < -13.0f || center.x > -7.0f || center.z < -5.0f || center.z > 1.0f)
					continue;
				columns++;
				const glm::ivec3 texel = ((glm::ivec3(x, layer, z) + glm::ivec3(field.VoxelTexel[0])) % dimensions + dimensions) % dimensions;
				uint16_t value[4];
				std::memcpy(value, radiance.data() + (texel.x + dimensions.x * (texel.y + dimensions.y * texel.z)) * 8, sizeof(value));
				const float alpha = glm::unpackHalf1x16(value[3]);
				if (alpha > 0.0f)
				{
					const glm::vec3 color(glm::unpackHalf1x16(value[0]), glm::unpackHalf1x16(value[1]), glm::unpackHalf1x16(value[2]));
					const double voxelPower = Luminance(color / alpha) / RenderData::c_GIRadianceScale * voxel * voxel;
					power += voxelPower;
					powerCenter += voxelPower * glm::dvec2(center.x, center.z);
				}
				const double referencePower = Luminance(SunRoomRadiance(room, glm::vec3(center.x, 0.1f, center.z), c_Up)) * voxel * voxel;
				reference += referencePower;
				referenceCenter += referencePower * glm::dvec2(center.x, center.z);
			}
		}
		MESSAGE("the floor's sunlit power: voxels " << power << ", reference " << reference << " (" << power / reference << ") over " << columns << " columns");
		// Where the power lies too: a patch of the right power in the wrong place would light the wrong walls.
		powerCenter /= std::max(power, 1e-9);
		referenceCenter /= std::max(reference, 1e-9);
		MESSAGE("the patch's center (x, z): voxels " << powerCenter.x << ", " << powerCenter.y << ", reference " << referenceCenter.x << ", "
			<< referenceCenter.y);
		CHECK(glm::length(powerCenter - referenceCenter) <= 0.05);
		CHECK(columns == 3600u);
		CHECK(power >= 0.9 * reference);
		CHECK(power <= 1.1 * reference);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The ceiling's voxels face up")
	{
		// The walls' inner faces end at the ceiling's underside, a voxel boundary: touching counts as overlap, so their top edges
		// marked the ceiling's layer above them, and their normals turned the ceiling's sunlit top toward the room (normals (0,
		// 0.71, 0.71) along the north wall, with the room moved off the grid at Low). A face's normal goes only to voxels it
		// covers part of or lies on, never to one it touches along an edge: the layer holds no surface, or the walls' hidden tops (up).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const glm::vec3 offset(0.25f * FirstSpacing(GlobalIlluminationLevel::Low), 0.0f, 0.25f * FirstSpacing(GlobalIlluminationLevel::Low));
		SunRoomOptions room = DefaultRoom();
		room.Offset = offset;
		RoomRender render(room, c_RoomSize, RoomSpecification(GlobalIlluminationLevel::Low));
		const SceneCamera camera = RoomCamera(offset);
		render.Measure(camera, {});
		const RenderData::GIFieldConstants& field = render.Renderer->GetGlobalIlluminationFieldConstants();
		const glm::ivec3 dimensions(field.VoxelDimensions);
		std::vector<uint8_t> normals;
		REQUIRE(render.Renderer->ReadGlobalIlluminationVoxels(0, SceneRenderer::GlobalIlluminationTexture::Normal, normals));
		const float voxel = field.VoxelOrigin[0].w;
		uint32_t checked = 0;
		for (int z = field.VoxelValidMinimum[0].z; z < field.VoxelValidMaximum[0].z; z++)
		{
			for (int y = field.VoxelValidMinimum[0].y; y < field.VoxelValidMaximum[0].y; y++)
			{
				for (int x = field.VoxelValidMinimum[0].x; x < field.VoxelValidMaximum[0].x; x++)
				{
					// The ceiling's layer (3.0-3.2 m) over the room and its walls' inner faces, short of the ceiling's own end faces.
					const glm::vec3 center = camera.Position + glm::vec3(field.VoxelOrigin[0]) + (glm::vec3(x, y, z) + 0.5f) * voxel;
					if (std::abs(center.y - 3.1f) > 0.01f || center.x < -13.0f + offset.x || center.x > -6.9f + offset.x || center.z < -5.0f + offset.z
						|| center.z > 1.1f + offset.z)
						continue;
					const glm::ivec3 texel = ((glm::ivec3(x, y, z) + glm::ivec3(field.VoxelTexel[0])) % dimensions + dimensions) % dimensions;
					const int8_t* normal = reinterpret_cast<const int8_t*>(normals.data() + (texel.x + dimensions.x * (texel.y + dimensions.y * texel.z)) * 4);
					INFO("voxel at " << center.x << ", " << center.y << ", " << center.z << ": normal " << normal[0] / 127.0f << ", " << normal[1] / 127.0f << ", "
						<< normal[2] / 127.0f << ", w " << normal[3] / 127.0f);
					CHECK(normal[3] >= 0); // No surface (under the ceiling's top) or the walls' tops under it, never two-sided
					CHECK(std::abs(normal[0]) <= 6);
					CHECK(std::abs(normal[2]) <= 6);
					checked++;
				}
			}
		}
		CHECK(checked == 31u * 31u);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Still frames are identical")
	{
		// 6.2: the GI is a function of the scene and the camera alone (fixed directions, no history, nothing jitters): the gi view of
		// a still camera's frames 8 and 9 agree to the bit, at High and Low.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		for (GlobalIlluminationLevel level : { GlobalIlluminationLevel::High, GlobalIlluminationLevel::Low })
		{
			CAPTURE(level == GlobalIlluminationLevel::High ? "High" : "Low");
			RoomRender render(DefaultRoom(), c_RoomSize, RoomSpecification(level));
			AssetManager::SetActive(render.Assets.Manager);
			const SceneCamera camera = RoomCamera();
			ReadbackImage frames[2];
			for (uint32_t frame = 1; frame <= 9; frame++)
			{
				REQUIRE(RenderGIFrame(*render.Renderer, render.SceneData, camera, GIViewOptions()));
				if (frame >= 8)
					REQUIRE(Renderer::ReadTexture(render.Renderer->GetHDRTexture(), frames[frame - 8]));
			}
			REQUIRE(render.Renderer->GetStats().GlobalIllumination);
			REQUIRE(frames[0].Pixels.size() == frames[1].Pixels.size());
			REQUIRE_FALSE(frames[0].Pixels.empty());
			size_t differing = 0;
			for (size_t index = 0; index < frames[0].Pixels.size(); index++)
				differing += frames[0].Pixels[index] != frames[1].Pixels[index] ? 1 : 0;
			CHECK(differing == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Dead probes")
	{
		// 6.2: cascade 0's probes inside a 2 m closed block are dead (validity 0: no march, no gather takes them), those 0.05 m from
		// its faces partly valid (validity fades as a surface comes near), and GIDeadProbes counts exactly the dead probes (nothing
		// else is in the scene). The block's faces lie 0.05 m past the probe planes (0.25 m + 0.5 m k), so the probes 0.05 m inside
		// its faces at 2.3 m find open space within their cells and move out of it, live (relocation, D18.7: a probe inside an
		// object over a contact or beside a gap serves the gather from the open side); those deeper than half a spacing stay dead.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties properties;
		properties.BaseColor = glm::vec4(glm::vec3(0.58f), 1.0f);
		const glm::vec3 minimum(0.3f);
		const glm::vec3 maximum(2.3f);
		AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(properties), 0.5f * (minimum + maximum), "Block").GetComponent<TransformComponent>().Scale =
			maximum - minimum;
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(RoomSpecification());
		renderer->SetViewportSize(c_FloorSize.x, c_FloorSize.y);
		AssetManager::SetActive(assets.Manager);
		const SceneCamera camera = MakeCamera(glm::vec3(1.3f, 1.3f, 4.5f), glm::vec3(1.3f), 70.0f, c_FloorSize);
		// The block's mesh resident first, then the frame that makes the GI and counts its dead probes.
		for (uint32_t frame = 0; frame < 100 && (frame < 2 || renderer->GetStats().PendingAssets > 0 || renderer->GetStats().DeferredProxies > 0); frame++)
			REQUIRE(RenderGIFrame(*renderer, scene, camera, SceneRenderOptions {}));
		// Without HDR output: the frame runs to its end, where the GI's counters are copied back.
		SceneRenderOptions options;
		options.LightingView = SceneLightingView::GlobalIllumination;
		REQUIRE(RenderGIFrame(*renderer, scene, camera, options));
		REQUIRE(renderer->GetStats().GlobalIllumination);
		REQUIRE_FALSE(renderer->GetStats().GlobalIlluminationPending);
		renderer->ResolveGPUStats(true);
		const SceneRendererGPUStats& counts = renderer->GetGPUStats();
		REQUIRE(counts.HasGICounters);
		REQUIRE(counts.GICountersRender == renderer->GetRenderCount());

		std::vector<uint8_t> validity;
		REQUIRE(renderer->ReadGlobalIlluminationValidity(0, validity));
		const ProbeCascadeLayout& probes = renderer->GetGlobalIlluminationLayout()->Cascades.front();
		const glm::ivec3 grid(probes.Grid);
		REQUIRE(validity.size() == static_cast<size_t>(grid.x) * grid.y * grid.z * 4);
		const glm::vec3 origin = camera.Position + glm::vec3(renderer->GetGlobalIlluminationFieldConstants().FieldOrigin);
		uint32_t inside = 0;
		uint32_t dead = 0;
		uint32_t relocated = 0;
		uint32_t beside = 0;
		for (int z = 0; z < grid.z; z++)
		{
			for (int y = 0; y < grid.y; y++)
			{
				for (int x = 0; x < grid.x; x++)
				{
					const glm::vec3 position = origin + (glm::vec3(x, y, z) + 0.5f) * probes.Spacing;
					const uint8_t value = validity[(x + grid.x * (y + grid.y * static_cast<size_t>(z))) * 4];
					INFO("probe at " << position.x << ", " << position.y << ", " << position.z << ": validity " << value / 255.0f);
					if (glm::all(glm::greaterThan(position, minimum)) && glm::all(glm::lessThan(position, maximum)))
					{
						inside++;
						const glm::vec3 depth = glm::min(position - minimum, maximum - position);
						if (std::min({ depth.x, depth.y, depth.z }) < 0.5f * probes.Spacing - 0.1f)
						{
							// Moved out through the nearest face (GlobalIllumination.slang's DecodeProbeMove): live, outside the block.
							const size_t texel = (x + grid.x * (y + grid.y * static_cast<size_t>(z))) * 4;
							const glm::vec3 move = (glm::vec3(validity[texel + 1], validity[texel + 2], validity[texel + 3]) - 128.0f) / 127.0f * (0.5f * probes.Spacing);
							const glm::vec3 moved = position + move;
							CHECK(value > 0);
							CHECK_FALSE((glm::all(glm::greaterThan(moved, minimum)) && glm::all(glm::lessThan(moved, maximum))));
							relocated++;
						}
						else
						{
							CHECK(value == 0);
							dead++;
						}
						continue;
					}
					// 0.05 m outside one of the block's faces, level with its middle on the other axes.
					const glm::vec3 outside = glm::max(minimum - position, position - maximum);
					const int axes = (outside.x > 0.0f ? 1 : 0) + (outside.y > 0.0f ? 1 : 0) + (outside.z > 0.0f ? 1 : 0);
					if (axes == 1 && std::max({ outside.x, outside.y, outside.z }) < 0.06f)
					{
						CHECK(value > 0);
						CHECK(value < 255);
						beside++;
					}
				}
			}
		}
		CHECK(inside == 64u);
		CHECK(relocated == 64u - 27u); // Those with a coordinate at 2.25 m
		CHECK(dead == 27u);
		CHECK(beside == 3u * 16u); // The probes 0.05 m outside the three faces at 0.3 m
		CHECK(counts.GIDeadProbes == dead);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The sun lights no voxel inside a sealed room")
	{
		// The sun's walk from a voxel whose surface runs on under a wall (the floor under the walls' feet) went through the wall
		// when it skipped solid voxels holding its own face, or when it started a voxel and more along the ray: 22 floor voxels
		// along the sealed room's south wall were sunlit at Low.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		for (GlobalIlluminationLevel level : { GlobalIlluminationLevel::High, GlobalIlluminationLevel::Low })
		{
			const float spacing = GlobalIllumination::ComputeLayout(level)->Cascades.front().Spacing;
			for (const glm::vec3& offset : { glm::vec3(0.0f), glm::vec3(0.25f * spacing, 0.0f, 0.25f * spacing), glm::vec3(0.5f * spacing, 0.0f, 0.5f * spacing) })
			{
				SunRoomOptions room = DefaultRoom();
				room.SealWindow = true;
				room.Offset = offset;
				RoomRender render(room, c_RoomSize, RoomSpecification(level));
				render.Measure(RoomCamera(offset), GetWallPoints(offset));
				const uint32_t lit = CountLitVoxels(*render.Renderer, glm::vec3(-9.0f, 1.5f, -4.5f) + offset, glm::vec3(-13.0f, 0.1f, -5.0f) + offset,
					glm::vec3(-7.0f, 3.0f, 1.0f) + offset);
				INFO(std::string(level == GlobalIlluminationLevel::High ? "High" : "Low") << ", offset " << offset.x);
				CHECK(lit == 0u);
			}
		}
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

	TEST_CASE("Probes on a wall's face move off it")
	{
		// The room's inner faces lie on cascade 2's probe planes (odd meters): those probes would die, half inside the walls, and
		// their children took one surviving parent's view from metres away (the north wall's probes read three times the patch's
		// light on the west and a fifth of it on the east). They move along the surface's normal by c_GIProbeMoveVoxels voxels of
		// level 1 and live; a probe in open air stays where it is.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		RoomRender render(DefaultRoom(), c_RoomSize, RoomSpecification());
		const SceneCamera camera = RoomCamera();
		render.Measure(camera, {});
		const GlobalIlluminationLayout* layout = render.Renderer->GetGlobalIlluminationLayout();
		REQUIRE(layout);
		const float spacing = layout->Cascades[2].Spacing;
		const glm::ivec3 grid(layout->Cascades[2].Grid);
		const float move = RenderData::c_GIProbeMoveVoxels * layout->Levels[1].VoxelSize;
		std::vector<uint8_t> validity;
		REQUIRE(render.Renderer->ReadGlobalIlluminationValidity(2, validity));
		struct Expected
		{
			const char* Name;
			glm::vec3 Position;
			glm::vec3 Move;
		};
		const Expected expectations[] = {
			{ "on the west wall", { -13.0f, 1.0f, -3.0f }, { move, 0.0f, 0.0f } },
			{ "on the east wall", { -7.0f, 1.0f, -3.0f }, { -move, 0.0f, 0.0f } },
			{ "on the north wall", { -11.0f, 1.0f, -5.0f }, { 0.0f, 0.0f, move } },
			{ "on the ceiling", { -11.0f, 3.0f, -3.0f }, { 0.0f, -move, 0.0f } },
			{ "in the open", { -11.0f, 1.0f, -3.0f }, { 0.0f, 0.0f, 0.0f } },
		};
		for (const Expected& expected : expectations)
		{
			INFO(expected.Name);
			const glm::ivec3 probe = FindProbe(*render.Renderer, camera, 2, expected.Position);
			const uint8_t* texel = validity.data() + (static_cast<size_t>(probe.x) + grid.x * (static_cast<size_t>(probe.y) + static_cast<size_t>(grid.y) * probe.z)) * 4;
			const glm::vec3 decoded = (glm::vec3(texel[1], texel[2], texel[3]) - 128.0f) / 127.0f * (0.5f * spacing);
			MESSAGE(expected.Name << ": validity " << texel[0] / 255.0f << ", moved " << decoded.x << ", " << decoded.y << ", " << decoded.z);
			CHECK(texel[0] == 255);
			CHECK(glm::length(decoded - expected.Move) <= 0.02f);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Orthographic cameras")
	{
		// An orthographic camera 5 m above the yard runs the GI, and its gi view equals a perspective camera's at the same point
		// within 2% at the image center (the field follows the camera's position, never its projection).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		AssetManager::SetActive(assets.Manager);
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(RoomSpecification());
		renderer->SetViewportSize(256, 256);
		const glm::vec3 position(0.5f, 5.0f, -3.0f);
		const glm::vec3 target(0.5f, 0.0f, -2.9f);
		const SceneCamera cameras[] = { LookAt(position, target), OrthographicLookAt(position, target, 6.0f) };
		float centers[2] = {};
		for (int index = 0; index < 2; index++)
		{
			CAPTURE(index);
			for (uint32_t frame = 0; frame < 4; frame++)
				REQUIRE(RenderGIFrame(*renderer, scene, cameras[index], GIViewOptions()));
			REQUIRE(renderer->GetStats().GlobalIllumination);
			REQUIRE_FALSE(renderer->GetStats().GlobalIlluminationPending);
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(renderer->GetHDRTexture(), image));
			glm::vec3 sum(0.0f);
			for (uint32_t dy = 0; dy < 3; dy++)
			{
				for (uint32_t dx = 0; dx < 3; dx++)
					sum += glm::vec3(ReadHalfTexel(image, image.Width / 2 - 1 + dx, image.Height / 2 - 1 + dy));
			}
			centers[index] = Luminance(sum / 9.0f / c_RoomPreExposure);
		}
		MESSAGE("the image center: perspective " << centers[0] << ", orthographic " << centers[1]);
		CHECK(centers[0] > 0.0f);
		CHECK(centers[1] == doctest::Approx(centers[0]).epsilon(0.02));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A probe's move off a wall does not depend on the camera's distance")
	{
		// The move was c_GIProbeMoveVoxels voxels of the finest level holding the probe: a camera risen far enough above the room
		// that level 1 no longer holds the west wall's probe doubled it, from 0.3 m to 0.6 m (the probe's light and its
		// children's stepped in one frame). It is that many voxels of the cascade's minimum level from every camera.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		RoomRender render(DefaultRoom(), c_RoomSize, RoomSpecification());
		render.Measure(RoomCamera(), {});
		const GlobalIlluminationLayout* layout = render.Renderer->GetGlobalIlluminationLayout();
		REQUIRE(layout);
		REQUIRE(layout->Cascades[2].MinimumLevel == 1);
		const float spacing = layout->Cascades[2].Spacing;
		const glm::ivec3 grid(layout->Cascades[2].Grid);
		const float move = RenderData::c_GIProbeMoveVoxels * layout->Levels[1].VoxelSize;
		const glm::vec3 probePosition(-13.0f, 1.0f, -3.0f);
		// The room camera, and one 6 m above the probe: beyond level 1's weight (its window reaches 6.4 m up, less its scroll and
		// guard voxels), well within level 2's.
		const SceneCamera cameras[] = { RoomCamera(), LookAt(glm::vec3(-10.0f, 7.0f, -2.0f), glm::vec3(-10.5f, 0.0f, -2.5f), 16.0f / 9.0f) };
		for (const SceneCamera& camera : cameras)
		{
			CAPTURE(camera.Position.y);
			render.Measure(camera, {});
			const glm::vec3 relative = probePosition - camera.Position;
			MESSAGE("level 1's weight at the probe: " << GlobalIllumination::GetLevelWeight(layout->Levels[1], relative));
			std::vector<uint8_t> validity;
			REQUIRE(render.Renderer->ReadGlobalIlluminationValidity(2, validity));
			const glm::ivec3 probe = FindProbe(*render.Renderer, camera, 2, probePosition);
			const uint8_t* texel = validity.data() + (static_cast<size_t>(probe.x) + grid.x * (static_cast<size_t>(probe.y) + static_cast<size_t>(grid.y) * probe.z)) * 4;
			const glm::vec3 decoded = (glm::vec3(texel[1], texel[2], texel[3]) - 128.0f) / 127.0f * (0.5f * spacing);
			MESSAGE("validity " << texel[0] / 255.0f << ", moved " << decoded.x << ", " << decoded.y << ", " << decoded.z);
			CHECK(glm::length(decoded - glm::vec3(move, 0.0f, 0.0f)) <= 0.02f);
		}
		CHECK(GlobalIllumination::GetLevelWeight(layout->Levels[1], probePosition - cameras[1].Position) == 0.0f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Relocated probes move continuously as the camera walks through the voxel levels' blend")
	{
		// Relocation (D18.7) searched the occupancy blended over the voxel levels by the camera's distance, with a hard threshold
		// and a hard choice of the best candidate, so a probe crossing the blend of level 0 into level 1 (whose 0.2 m voxels make
		// surfaces fatter) flipped its choice and jumped 0.1-0.4 m in one frame: a ring of popping probes about 6 m around a
		// moving camera at High. Each level searches its own voxels (world data) and the moves blend by the levels' weights, so a
		// 2 cm step of the camera moves a probe and changes its validity by little. The block of "Dead probes": the probes 0.05 m
		// inside its faces relocate out of it; the camera walks along x so that those inside its +x face cross level 0's edge
		// (5.2 to 6 m away).
		constexpr uint32_t c_Steps = 40;
		constexpr float c_Step = 0.02f;
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties properties;
		properties.BaseColor = glm::vec4(glm::vec3(0.58f), 1.0f);
		const glm::vec3 minimum(0.3f);
		const glm::vec3 maximum(2.3f);
		AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(properties), 0.5f * (minimum + maximum), "Block").GetComponent<TransformComponent>().Scale =
			maximum - minimum;
		constexpr glm::uvec2 c_Size { 256, 256 };
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(RoomSpecification());
		renderer->SetViewportSize(c_Size.x, c_Size.y);
		AssetManager::SetActive(assets.Manager);
		// The 64 probes inside the block (0.75 to 2.25 m on each axis); the 37 with a coordinate at 2.25 m relocate.
		std::vector<glm::vec3> probePositions;
		for (int z = 0; z < 4; z++)
		{
			for (int y = 0; y < 4; y++)
			{
				for (int x = 0; x < 4; x++)
					probePositions.push_back(glm::vec3(0.75f) + 0.5f * glm::vec3(x, y, z));
			}
		}
		std::vector<glm::vec3> previousPositions;
		std::vector<float> previousValidity;
		float largestMove = 0.0f;
		float largestValidityChange = 0.0f;
		float largestJump = 0.0f;
		for (uint32_t step = 0; step <= c_Steps; step++)
		{
			const float distance = 5.2f + c_Step * static_cast<float>(step);
			const glm::vec3 eye(2.25f - distance, 1.3f, 1.3f);
			const SceneCamera camera = MakeCamera(eye, eye + glm::vec3(1.0f, 0.0f, 0.0f), 70.0f, c_Size);
			for (uint32_t frame = 0; frame < 100 && (frame < 2 || renderer->GetStats().PendingAssets > 0 || renderer->GetStats().DeferredProxies > 0); frame++)
				REQUIRE(RenderGIFrame(*renderer, scene, camera, GIViewOptions()));
			REQUIRE(renderer->GetStats().GlobalIllumination);
			REQUIRE_FALSE(renderer->GetStats().GlobalIlluminationPending);
			std::vector<uint8_t> validity;
			REQUIRE(renderer->ReadGlobalIlluminationValidity(0, validity));
			const ProbeCascadeLayout& probes = renderer->GetGlobalIlluminationLayout()->Cascades.front();
			const glm::ivec3 grid(probes.Grid);
			std::vector<glm::vec3> positions;
			std::vector<float> values;
			for (const glm::vec3& position : probePositions)
			{
				const glm::ivec3 probe = FindProbe(*renderer, camera, 0, position);
				const uint8_t* texel = validity.data() + (static_cast<size_t>(probe.x) + grid.x * (static_cast<size_t>(probe.y) + static_cast<size_t>(grid.y) * probe.z)) * 4;
				positions.push_back(position + (glm::vec3(texel[1], texel[2], texel[3]) - 128.0f) / 127.0f * (0.5f * probes.Spacing));
				values.push_back(texel[0] / 255.0f);
			}
			if (!previousPositions.empty())
			{
				for (size_t index = 0; index < positions.size(); index++)
				{
					const float moved = glm::length(positions[index] - previousPositions[index]);
					const float changed = std::abs(values[index] - previousValidity[index]);
					if (moved > largestMove || changed > largestValidityChange)
					{
						const glm::vec3& probe = probePositions[index];
						MESSAGE("step " << step << " (" << distance << " m), probe at " << probe.x << ", " << probe.y << ", " << probe.z << ": moved " << moved
							<< " m, validity " << previousValidity[index] << " -> " << values[index]);
					}
					largestMove = std::max(largestMove, moved);
					largestValidityChange = std::max(largestValidityChange, changed);
				}
			}
			for (size_t index = 0; index < positions.size(); index++)
				largestJump = std::max(largestJump, glm::length(positions[index] - probePositions[index]));
			previousPositions = std::move(positions);
			previousValidity = std::move(values);
		}
		MESSAGE("per 2 cm step: a probe moved at most " << largestMove << " m, its validity changed at most " << largestValidityChange);
		CHECK(largestJump > 0.05f); // Probes relocated
		CHECK(largestMove <= 0.035f);
		CHECK(largestValidityChange <= 0.18f);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A strafe's gi view is a fresh renderer's at its camera")
	{
		// The field is a function of world data and of where the camera stands, never of where it stood: after a strafe along the
		// yard's fence (13 frames, 0.02 m each, one bounce: every bounce takes the frame before's field by design), the gi view, the
		// field, cascade 0's validity and its depth moments equal a fresh renderer's at the last camera, bit for bit. What made the
		// fence crawl was a camera input of the gather itself (DeveloperGuide, "Global illumination"), not a history.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const float aspect = static_cast<float>(c_RoomSize.x) / static_cast<float>(c_RoomSize.y);
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		struct Reading
		{
			ReadbackImage Image;
			std::vector<uint8_t> Field;
			std::vector<uint8_t> Validity;
			std::vector<uint8_t> Depth;
		};
		const auto read = [&](SceneRenderer& renderer)
		{
			Reading reading;
			REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), reading.Image));
			REQUIRE(renderer.ReadGlobalIlluminationField(reading.Field));
			REQUIRE(renderer.ReadGlobalIlluminationValidity(0, reading.Validity));
			REQUIRE(renderer.ReadGlobalIlluminationFieldDepth(reading.Depth));
			return reading;
		};
		Scope<SceneRenderer> strafe = CreateScope<SceneRenderer>(RoomSpecification());
		strafe->SetViewportSize(c_RoomSize.x, c_RoomSize.y);
		for (uint32_t frame = 0; frame <= 13; frame++)
			REQUIRE(RenderGIFrame(*strafe, scene, YardStrafe(frame, 1.0f, aspect), GIViewOptions()));
		REQUIRE_FALSE(strafe->GetStats().GlobalIlluminationPending);
		const Reading moved = read(*strafe);
		strafe.reset();
		Scope<SceneRenderer> fresh = CreateScope<SceneRenderer>(RoomSpecification());
		fresh->SetViewportSize(c_RoomSize.x, c_RoomSize.y);
		REQUIRE(RenderGIFrame(*fresh, scene, YardStrafe(13, 1.0f, aspect), GIViewOptions()));
		REQUIRE_FALSE(fresh->GetStats().GlobalIlluminationPending);
		const Reading still = read(*fresh);
		fresh.reset();
		CollectReleases();
		CHECK(moved.Image.Pixels == still.Image.Pixels);
		CHECK(moved.Field == still.Field);
		CHECK(moved.Validity == still.Validity);
		CHECK(moved.Depth == still.Depth);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A sunlit floor's light is continuous across the voxel levels' blends")
	{
		// The sun's walk from a voxel where its level blends into the next (weight below one half) read the coarser level from
		// its start, inside that level's voxel holding the floor: the floor's lit layer went dark in a ring at each level's edge
		// (about 5.8 m and 11.6 m from the camera at High), and wholly under a camera 2.6 m above it. Every voxel a read takes
		// (weight above 0) holds the sunlit floor's light.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties white;
		const AssetHandle material = assets.AddMaterial(white);
		// A 60 m floor whose top (y 0.1) lies on level 0's grid, in the middle of level 1's voxels.
		AddMesh(scene, BuiltinAssets::CubeMesh, material, glm::vec3(0.0f, -0.4f, 0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(60.0f, 1.0f, 60.0f);
		AddSun(scene, glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f)), 10.0f, false);
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(RoomSpecification());
		renderer->SetViewportSize(256, 256);
		AssetManager::SetActive(assets.Manager);
		for (const float height : { 1.7f, 2.7f })
		{
			CAPTURE(height);
			const SceneCamera camera = LookAt(glm::vec3(0.3f, height, 0.2f), glm::vec3(0.3f, 0.0f, 8.0f));
			bool settled = false;
			for (uint32_t frame = 0; frame < 100 && !settled; frame++)
			{
				REQUIRE(RenderGIFrame(*renderer, scene, camera, GIViewOptions()));
				settled = renderer->GetStats().GlobalIlluminationSettled;
			}
			REQUIRE(settled);
			const GlobalIlluminationLayout* layout = renderer->GetGlobalIlluminationLayout();
			REQUIRE(layout);
			const RenderData::GIFieldConstants& field = renderer->GetGlobalIlluminationFieldConstants();
			const glm::ivec3 dimensions(field.VoxelDimensions);
			for (uint32_t level = 0; level < 2; level++)
			{
				CAPTURE(level);
				std::vector<uint8_t> radiance;
				REQUIRE(renderer->ReadGlobalIlluminationVoxels(level, SceneRenderer::GlobalIlluminationTexture::Radiance, radiance));
				const float voxel = field.VoxelOrigin[level].w;
				REQUIRE(voxel > 0.0f);
				const glm::vec3 minimum = camera.Position + glm::vec3(field.VoxelOrigin[level]);
				// The layer holding the floor's light: level 0's lit layer above the top (y 0.1-0.2), level 1's voxels holding it.
				const int layer = static_cast<int>(std::floor((0.15f - minimum.y) / voxel));
				REQUIRE(layer >= field.VoxelValidMinimum[level].y);
				REQUIRE(layer < field.VoxelValidMaximum[level].y);
				float lowest = std::numeric_limits<float>::max();
				float highest = 0.0f;
				uint32_t taken = 0;
				uint32_t blended = 0;
				for (int z = field.VoxelValidMinimum[level].z; z < field.VoxelValidMaximum[level].z; z++)
				{
					for (int x = field.VoxelValidMinimum[level].x; x < field.VoxelValidMaximum[level].x; x++)
					{
						const glm::vec3 center = minimum + (glm::vec3(x, layer, z) + 0.5f) * voxel;
						const float weight = GlobalIllumination::GetLevelWeight(layout->Levels[level], center - camera.Position);
						if (weight <= 0.0f)
							continue;
						taken++;
						blended += weight < 0.5f ? 1u : 0u;
						const glm::ivec3 texel = ((glm::ivec3(x, layer, z) + glm::ivec3(field.VoxelTexel[level])) % dimensions + dimensions) % dimensions;
						uint16_t value[4];
						std::memcpy(value, radiance.data() + (texel.x + dimensions.x * (texel.y + dimensions.y * texel.z)) * 8, sizeof(value));
						const float alpha = glm::unpackHalf1x16(value[3]);
						const float light = alpha > 0.0f
							? Luminance(glm::vec3(glm::unpackHalf1x16(value[0]), glm::unpackHalf1x16(value[1]), glm::unpackHalf1x16(value[2])) / alpha) : 0.0f;
						lowest = std::min(lowest, light);
						highest = std::max(highest, light);
					}
				}
				MESSAGE("height " << height << ", level " << level << ": " << taken << " voxels taken (" << blended << " below half weight), light "
					<< lowest << " to " << highest);
				CHECK(blended > 0u);
				CHECK(highest > 0.0f);
				CHECK(lowest >= 0.99f * highest);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The probes beside the north wall see the patch's light")
	{
		// The twelve cascade-0 probes 0.25 m from the north wall, 1.25 m up: their +Z faces (toward the sunlit floor patch 4-5 m
		// away and the window) against the reference. The patch is half a texel of cascade 2 seen from there: one ray a texel caught
		// it whole for some probes and missed it for others (2-3 times the reference on the west, 0.14-0.4 on the east), and the
		// probes beside a wall took the view of one parent metres away. A guard against that defect, within half the reference
		// either way (measured 0.60-0.88: the voxels' floor layer, read up to a tenth full, lies above the floor and makes the patch
		// look smaller from this low; Gate W's bound is on the walls' light, 6.2 case 1).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const SunRoomOptions room = DefaultRoom();
		RoomRender render(room, c_RoomSize, RoomSpecification());
		const SceneCamera camera = RoomCamera();
		render.Measure(camera, {});
		std::vector<uint8_t> field;
		REQUIRE(render.Renderer->ReadGlobalIlluminationField(field));
		std::string table = "+Z faces 0.25 m from the north wall (field / reference):";
		for (float x = -12.75f; x < -7.0f; x += 0.5f)
		{
			const glm::vec3 position(x, 1.25f, -4.75f);
			const float value = ReadFieldFace(*render.Renderer, camera, field, position, 4);
			const float reference = Luminance(SunRoomReference(room, position, glm::vec3(0.0f, 0.0f, 1.0f)));
			table += fmt::format("\n  x {:6.2f}: {:6.1f} / {:6.1f} ({:.2f})", x, value, reference, value / reference);
			INFO("x " << x << ": " << value << " against " << reference);
			CHECK(value >= 0.5f * reference);
			CHECK(value <= 1.5f * reference);
		}
		MESSAGE(table);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The north wall's light does not depend on the camera")
	{
		// Correction 14a: camera A sees the floor's sunlit patch and the north wall, camera B (2 m from that wall) has the patch behind
		// it; the wall's light comes from world data either way, within 10% between the two (they read the clipmap's levels with
		// other weights). Both rooms are held at once: each render activates its own room's assets (RoomRender::Measure), else the
		// first read the second's and took the default white for its plaster (1.75 times the light).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const SunRoomOptions room = DefaultRoom();
		std::vector<RoomPoint> points;
		for (float x : { -12.0f, -10.0f, -8.0f })
		{
			for (float y : { 1.0f, 2.0f })
				points.push_back({ fmt::format("north wall x {} y {}", x, y), { x, y, -5.0f }, -c_North });
		}
		const glm::vec3 target(-10.0f, 1.6f, -5.1f);
		RoomRender renderA(room, c_RoomSize, RoomSpecification());
		RoomRender renderB(room, c_RoomSize, RoomSpecification());
		const std::vector<glm::vec3> valuesA = renderA.Measure(MakeCamera(glm::vec3(-10.0f, 1.6f, 0.8f), target, 70.0f, c_RoomSize), points);
		const std::vector<glm::vec3> valuesB = renderB.Measure(MakeCamera(glm::vec3(-10.0f, 1.6f, -3.0f), target, 70.0f, c_RoomSize), points);
		for (size_t index = 0; index < points.size(); index++)
		{
			const float a = Luminance(valuesA[index]);
			const float b = Luminance(valuesB[index]);
			INFO(points[index].Name << ": camera A " << a << ", camera B " << b);
			CHECK(a > 0.0f);
			CHECK(std::abs(b - a) <= 0.1f * a);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A sealed room stays dark")
	{
		// The window filled with plaster: the nineteen points' mean at most erhe's 0.0017 of the open room's, and every junction of
		// the floor and the ceiling with the walls (correction 4) too, at High and Low, with the room moved by a quarter and a half
		// of the first cascade's spacing along x and z (the probe planes cut its walls at three places); with one bounce, and with
		// the bounce's gain 1 too (6.4: re-injection must not carry a leak further), settled. Low bounces once whatever the gain
		// (GlobalIlluminationLayout::Bounces): with every bounce, at the quarter offset the walls' pixels in its north-west
		// three-wall corner took light through the walls in the gather (73 nits; 0.019 with one bounce, whose outside there is
		// dark), so its pass with the gain 1 guards that Low's default stays dark.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		for (const float bounceGain : { 0.0f, 1.0f })
		{
			for (GlobalIlluminationLevel level : { GlobalIlluminationLevel::High, GlobalIlluminationLevel::Low })
			{
				const float spacing = FirstSpacing(level);
				const uint32_t frames = bounceGain > 0.0f ? c_BounceFrames : 3;
				for (float share : { 0.0f, 0.25f, 0.5f })
				{
					const glm::vec3 offset(share * spacing, 0.0f, share * spacing);
					const std::string name = fmt::format("{}, offset {} m, bounce gain {}", level == GlobalIlluminationLevel::High ? "High" : "Low", offset.x,
						bounceGain);
					CAPTURE(name);
					SunRoomOptions open = DefaultRoom();
					open.Offset = offset;
					SunRoomOptions sealed = open;
					sealed.SealWindow = true;
					std::vector<RoomPoint> walls = GetWallPoints(offset);
					const std::vector<RoomPoint> ceilingJunctions = GetCeilingJunctions(offset);
					walls.insert(walls.end(), ceilingJunctions.begin(), ceilingJunctions.end());
					const std::vector<RoomPoint> floors = GetFloorPoints(offset, true);
					const auto measure = [&](const SunRoomOptions& room, std::vector<glm::vec3>& wallValues, std::vector<glm::vec3>& floorValues)
					{
						RoomRender wallRender(room, c_RoomSize, RoomSpecification(level, bounceGain));
						wallValues = wallRender.Measure(RoomCamera(offset), walls, frames);
						RoomRender floorRender(room, c_FloorSize, RoomSpecification(level, bounceGain));
						floorValues = floorRender.Measure(FloorCamera(offset), floors, frames);
					};
					// The nineteen points: the ten on the walls and ceiling, the floor's nine.
					const auto nineteenMean = [](const std::vector<glm::vec3>& wallValues, const std::vector<glm::vec3>& floorValues)
					{
						float sum = 0.0f;
						for (size_t index = 0; index < 10; index++)
							sum += Luminance(wallValues[index]);
						for (size_t index = 0; index < 9; index++)
							sum += Luminance(floorValues[index]);
						return sum / 19.0f;
					};
					std::vector<glm::vec3> openWalls, openFloors, sealedWalls, sealedFloors;
					measure(open, openWalls, openFloors);
					measure(sealed, sealedWalls, sealedFloors);
					CollectReleases();
					const float openMean = nineteenMean(openWalls, openFloors);
					const float sealedMean = nineteenMean(sealedWalls, sealedFloors);
					float largest = 0.0f;
					std::string largestName;
					for (size_t index = 0; index < walls.size() + floors.size(); index++)
					{
						const bool wall = index < walls.size();
						const float value = Luminance(wall ? sealedWalls[index] : sealedFloors[index - walls.size()]);
						if (value > largest)
						{
							largest = value;
							largestName = wall ? walls[index].Name : floors[index - walls.size()].Name;
						}
					}
					MESSAGE(name << ": open room " << openMean << ", sealed " << sealedMean << " (" << sealedMean / openMean << "), its brightest point "
						<< largestName << " " << largest << " (" << largest / openMean << ")");
					CHECK(openMean > 0.0f);
					CHECK(sealedMean <= c_SealedBound * openMean);
					for (size_t index = 10; index < walls.size(); index++)
					{
						INFO(walls[index].Name);
						CHECK(Luminance(sealedWalls[index]) <= c_SealedBound * openMean);
					}
					for (size_t index = 9; index < floors.size(); index++)
					{
						INFO(floors[index].Name << " " << floors[index].Point.x << ", " << floors[index].Point.z);
						CHECK(Luminance(sealedFloors[index]) <= c_SealedBound * openMean);
					}
				}
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A sealed hall stays dark")
	{
		// Correction 2: a 16 m hall, whose cascade-2 probes' 8 m rays end inside it, merges its parents' sunlit outside through the
		// wall unless the lead-in reaches back to the probe; and the top cascade's rays must cross its 0.2 m walls in voxels that
		// hold them (level 2's 0.4 m voxels let 0.0031 to 0.056 of the open hall's light in). The sealed hall's twelve points at
		// most 0.0017 of the same hall's with its 2 x 1.3 m window, at High, with the hall moved as the sealed room is, and each of
		// its junctions with the south wall too (correction 4).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const float spacing = FirstSpacing(GlobalIlluminationLevel::High);
		for (float share : { 0.0f, 0.25f, 0.5f })
		{
			const glm::vec3 offset(share * spacing, 0.0f, share * spacing);
			CAPTURE(offset.x);
			SunRoomOptions open = DefaultRoom();
			open.Hall = true;
			open.Offset = offset;
			SunRoomOptions sealed = open;
			sealed.SealWindow = true;
			const std::vector<RoomPoint> points = GetHallPoints(offset);
			const std::vector<RoomPoint> junctions = GetHallJunctions(offset);
			std::vector<glm::vec3> openValues;
			std::vector<glm::vec3> sealedValues;
			std::vector<glm::vec3> sealedJunctions;
			{
				RoomRender openRender(open, c_RoomSize, RoomSpecification());
				openValues = openRender.Measure(HallCamera(offset), points);
				RoomRender sealedRender(sealed, c_RoomSize, RoomSpecification());
				sealedValues = sealedRender.Measure(HallCamera(offset), points);
				sealedJunctions = ReadPoints(*sealedRender.Renderer, HallCamera(offset), junctions);
			}
			CollectReleases();
			float error = 0.0f;
			const std::string table = FormatTable(fmt::format("The open hall, offset {} m, against the reference:", offset.x), points, openValues,
				References(open, points), error);
			MESSAGE(table << "\n  open mean " << MeanLuminance(openValues) << ", sealed mean " << MeanLuminance(sealedValues) << " ("
				<< MeanLuminance(sealedValues) / MeanLuminance(openValues) << ")");
			CHECK(MeanLuminance(openValues) > 0.0f);
			CHECK(MeanLuminance(sealedValues) <= c_SealedBound * MeanLuminance(openValues));
			for (size_t index = 0; index < junctions.size(); index++)
			{
				INFO(junctions[index].Name);
				CHECK(Luminance(sealedJunctions[index]) <= c_SealedBound * MeanLuminance(openValues));
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}

TEST_SUITE("GPU.SceneRenderer.GlobalIllumination")
{
	TEST_CASE("An emissive wall lights the floor")
	{
		// A 2 x 2 m wall of luminance 4 standing on a gray floor in the dark: the floor 0.5, 1 and 2 m in front of its foot within 20%
		// of L F, F the rectangle's form factor there.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		AddNeutralPostProcess(scene);
		MaterialProperties gray;
		gray.BaseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
		gray.Roughness = 1.0f;
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(gray), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(20.0f, 1.0f, 20.0f);
		constexpr float c_WallLuminance = 4.0f;
		AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(Emitting(c_WallLuminance)), glm::vec3(0.0f, 1.0f, -0.05f), "Wall")
			.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f, 2.0f, 0.1f);
		const SceneCamera camera = MakeCamera(glm::vec3(0.0f, 1.6f, 4.5f), glm::vec3(0.0f, 0.4f, 0.0f), 60.0f, c_RoomSize);
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(RoomSpecification());
		renderer->SetViewportSize(c_RoomSize.x, c_RoomSize.y);
		SceneRenderOptions options = GIViewOptions();
		options.PreExposure = 1.0f;
		for (uint32_t frame = 0; frame < 3; frame++)
			REQUIRE(RenderGIFrame(*renderer, scene, camera, options));
		REQUIRE_FALSE(renderer->GetStats().GlobalIlluminationPending);
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer->GetHDRTexture(), image));
		REQUIRE(image.Format == nvrhi::Format::RGBA16_FLOAT);
		for (float distance : { 0.5f, 1.0f, 2.0f })
		{
			CAPTURE(distance);
			// The form factor of the rectangle at the floor point on its axis (cosines at both ends over r^2, over pi).
			constexpr int c_Steps = 400;
			double sum = 0.0;
			for (int i = 0; i < c_Steps; i++)
			{
				const double x = 2.0 * ((i + 0.5) / c_Steps - 0.5);
				for (int j = 0; j < c_Steps; j++)
				{
					const double y = 2.0 * (j + 0.5) / c_Steps;
					const double r2 = x * x + y * y + static_cast<double>(distance) * distance;
					sum += y * distance / (r2 * r2);
				}
			}
			const double expected = c_WallLuminance * sum * (2.0 / c_Steps) * (2.0 / c_Steps) / glm::pi<double>();
			const glm::uvec2 pixel = ProjectToPixel(camera, glm::vec3(0.0f, 0.01f, distance), image.Width, image.Height);
			glm::vec3 value(0.0f);
			for (int dy = -1; dy <= 1; dy++)
			{
				for (int dx = -1; dx <= 1; dx++)
					value += glm::vec3(ReadHalfTexel(image, pixel.x + dx, pixel.y + dy)) / 9.0f;
			}
			MESSAGE("the floor " << distance << " m from the wall: " << Luminance(value) << " against L F = " << expected << " ("
				<< Luminance(value) / expected << ")");
			CHECK(std::abs(static_cast<double>(Luminance(value)) / expected - 1.0) <= 0.2);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}

namespace
{
	// Asks for global illumination in the scene's Post Process (AddNeutralPostProcess's), at an intensity.
	void SetGlobalIllumination(Scene& scene, bool asked, float intensity = 1.0f)
	{
		auto view = scene.GetRegistry().view<PostProcessComponent>();
		REQUIRE(view.size() == 1);
		scene.GetRegistry().patch<PostProcessComponent>(view.front(), [&](PostProcessComponent& postProcess)
		{
			postProcess.GlobalIllumination = asked;
			postProcess.GlobalIlluminationIntensity = intensity;
		});
	}

	ReadbackImage ReadImage(nvrhi::ITexture* texture)
	{
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(texture, image));
		return image;
	}

	// What a renderer made of global illumination: nothing at all, or a layout's resources.
	void CheckNoGlobalIllumination(SceneRenderer& renderer)
	{
		CHECK_FALSE(renderer.GetStats().GlobalIllumination);
		CHECK(renderer.GetStats().GlobalIlluminationSettled);
		CHECK(renderer.GetGlobalIlluminationMemoryBytes() == 0);
		CHECK(renderer.GetGlobalIlluminationLayout() == nullptr);
		renderer.ResolveGPUStats(true);
		for (const GPUStageTime& stage : renderer.GetGPUStats().StageMs)
			CHECK(stage.Stage != RendererGPUStages::Get().RadianceCascades);
	}

	// What a renderer holds of global illumination at a level (0 at Off): its layout's resources (the quality row's estimate) and
	// the scene's material table (its first 256 entries, all these scenes use).
	uint64_t LayoutBytes(GlobalIlluminationLevel level)
	{
		const std::optional<GlobalIlluminationLayout> layout = GlobalIllumination::ComputeLayout(level);
		return layout ? layout->GetMemoryBytes() + 256 * sizeof(RenderData::GIMaterial) : 0;
	}

	// The sun room's lit image, only its indirect light (the Indirect view), read at the points after three frames.
	std::vector<glm::vec3> MeasureIndirect(RoomRender& render, const std::vector<RoomPoint>& points, bool joined)
	{
		AssetManager::SetActive(render.Assets.Manager);
		for (uint32_t frame = 0; frame < 3; frame++)
			REQUIRE(RenderGIFrame(*render.Renderer, render.SceneData, RoomCamera(), GIViewOptions(SceneLightingView::Indirect)));
		CHECK(render.Renderer->GetStats().GlobalIllumination == joined);
		CHECK_FALSE(render.Renderer->GetStats().GlobalIlluminationPending);
		return ReadPoints(*render.Renderer, RoomCamera(), points);
	}

	float RedOverGreen(const glm::vec3& color)
	{
		return color.r / std::max(color.g, 1e-6f);
	}

	// Ground 60 x 60 m under the yard's procedural sky, with no sun: on open ground the field's light and today's (the sky's)
	// differ by a few percent, which the field's fade blends over 2 m.
	void BuildOpenGround(Scene& scene, SceneTestAssets& assets)
	{
		AddNeutralPostProcess(scene);
		MaterialProperties properties;
		properties.BaseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
		properties.Roughness = 0.9f;
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(properties), glm::vec3(0.0f), "Ground").GetComponent<TransformComponent>().Scale =
			glm::vec3(60.0f, 1.0f, 60.0f);
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.Source = SkyLightSource::Procedural;
		sky.ZenithColor = glm::vec3(0.30f, 0.50f, 1.0f);
		sky.HorizonColor = glm::vec3(0.95f, 1.0f, 1.08f);
		sky.GroundColor = glm::vec3(0.20f, 0.18f, 0.15f);
		sky.SunIntensity = 0.0f;
		sky.Intensity = 1.0f;
	}

	// Camera-relative positions of an image's pixels from its reversed-Z depth (no jitter: no temporal anti-aliasing); the sky
	// (depth 0) gets no position.
	std::vector<std::optional<glm::vec3>> ReadRelativePositions(SceneRenderer& renderer, const SceneCamera& camera)
	{
		ReadbackImage depth;
		REQUIRE(Renderer::ReadTexture(renderer.GetDepthTexture(), depth));
		REQUIRE(depth.Pixels.size() == static_cast<size_t>(depth.Width) * depth.Height * sizeof(float));
		const glm::mat4 inverseViewProjection = glm::inverse(camera.Projection * glm::mat4(glm::mat3(camera.View)));
		std::vector<std::optional<glm::vec3>> positions(static_cast<size_t>(depth.Width) * depth.Height);
		for (uint32_t y = 0; y < depth.Height; y++)
		{
			for (uint32_t x = 0; x < depth.Width; x++)
			{
				float z = 0.0f; // Plain reads: Debug builds check no condition per pixel
				std::memcpy(&z, depth.Pixels.data() + (static_cast<size_t>(y) * depth.Width + x) * sizeof(float), sizeof(float));
				if (z <= 0.0f)
					continue;
				const glm::vec2 ndc((static_cast<float>(x) + 0.5f) / static_cast<float>(depth.Width) * 2.0f - 1.0f,
					1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(depth.Height) * 2.0f);
				const glm::vec4 position = inverseViewProjection * glm::vec4(ndc, z, 1.0f);
				positions[static_cast<size_t>(y) * depth.Width + x] = glm::vec3(position) / position.w;
			}
		}
		return positions;
	}
}

TEST_SUITE("GPU.SceneRenderer.GlobalIlluminationJoin")
{
	TEST_CASE("Without GI asked, or at the row Off, nothing is made and the image is the same")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		const SceneCamera camera = YardStrafe(0, 0.0f, static_cast<float>(c_RoomSize.x) / static_cast<float>(c_RoomSize.y));
		const auto render = [&](const SceneRendererSpecification& specification, bool asked, uint64_t expectedBytes)
		{
			SetGlobalIllumination(scene, asked);
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_RoomSize.x, c_RoomSize.y);
			for (uint32_t frame = 0; frame < 4; frame++)
				REQUIRE(RenderGIFrame(renderer, scene, camera, SceneRenderOptions {}));
			if (expectedBytes == 0)
				CheckNoGlobalIllumination(renderer);
			else
				CHECK(renderer.GetGlobalIlluminationMemoryBytes() == expectedBytes);
			return ReadImage(renderer.GetOutputTexture());
		};
		// Not asked at High; asked at Off; neither (Medium's row is Off): the same image, and nothing of the GI.
		const ReadbackImage notAsked = render(RoomSpecification(GlobalIlluminationLevel::High), false, 0);
		const ReadbackImage rowOff = render(RoomSpecification(GlobalIlluminationLevel::Off), true, 0);
		const ReadbackImage plain = render(ExactSpecification(), false, 0);
		CHECK(rowOff.Pixels == notAsked.Pixels);
		CHECK(plain.Pixels == notAsked.Pixels);
		// Asked at High: the field lights the yard.
		const ReadbackImage joined = render(RoomSpecification(GlobalIlluminationLevel::High), true, LayoutBytes(GlobalIlluminationLevel::High));
		CHECK(joined.Pixels != notAsked.Pixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A pending frame is today's light")
	{
		// The first frame of a rebuild voxelizes one region (VoxelizeTrianglesOnCut 1): the field is not used, and the image is
		// the one without GI to the bit (the join is skipped, not weighted by 0).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		const SceneCamera camera = YardStrafe(0, 0.0f, static_cast<float>(c_RoomSize.x) / static_cast<float>(c_RoomSize.y));
		SceneRendererSpecification specification = RoomSpecification(GlobalIlluminationLevel::High);
		specification.GlobalIllumination.VoxelizeTrianglesOnCut = 1;
		SceneRenderer off(specification);
		off.SetViewportSize(c_RoomSize.x, c_RoomSize.y);
		REQUIRE(RenderGIFrame(off, scene, camera, SceneRenderOptions {}));
		const ReadbackImage offImage = ReadImage(off.GetOutputTexture());

		SetGlobalIllumination(scene, true);
		SceneRenderer pending(specification);
		pending.SetViewportSize(c_RoomSize.x, c_RoomSize.y);
		REQUIRE(RenderGIFrame(pending, scene, camera, SceneRenderOptions {}));
		CHECK(pending.GetStats().GlobalIllumination);
		CHECK(pending.GetStats().GlobalIlluminationPending);
		CHECK(ReadImage(pending.GetOutputTexture()).Pixels == offImage.Pixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The owner's experiment in the lit image: a red floor tints the walls")
	{
		// The sun room's walls in the lit image's indirect light, with a red floor against a white one: redder by at least 0.7 of
		// the one-bounce reference's change of R/G with GI asked at High, and not at all without it (the room has no sky: its
		// walls' indirect light is then 0 with either floor, to the bit).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const SunRoomOptions white = DefaultRoom();
		SunRoomOptions red = white;
		red.FloorAlbedo = glm::vec3(0.62f, 0.09f, 0.07f);
		std::vector<RoomPoint> walls = GetWallPoints();
		walls.resize(7);
		const std::vector<glm::vec3> whiteReferences = References(white, walls);
		const std::vector<glm::vec3> redReferences = References(red, walls);
		float referenceChange = 0.0f;
		for (size_t index = 0; index < walls.size(); index++)
			referenceChange += RedOverGreen(redReferences[index]) - RedOverGreen(whiteReferences[index]);
		referenceChange /= static_cast<float>(walls.size());
		CHECK(referenceChange > 0.0f);

		for (const bool asked : { true, false })
		{
			CAPTURE(asked);
			RoomRender whiteRender(white, c_RoomSize, RoomSpecification());
			RoomRender redRender(red, c_RoomSize, RoomSpecification());
			SetGlobalIllumination(whiteRender.SceneData, asked);
			SetGlobalIllumination(redRender.SceneData, asked);
			const std::vector<glm::vec3> whiteMeasured = MeasureIndirect(whiteRender, walls, asked);
			const std::vector<glm::vec3> redMeasured = MeasureIndirect(redRender, walls, asked);
			if (!asked)
			{
				for (size_t index = 0; index < walls.size(); index++)
				{
					CAPTURE(walls[index].Name);
					CHECK(redMeasured[index] == whiteMeasured[index]);
				}
				continue;
			}
			float change = 0.0f;
			for (size_t index = 0; index < walls.size(); index++)
			{
				CAPTURE(walls[index].Name);
				CHECK(Luminance(whiteMeasured[index]) > 0.0f);
				change += RedOverGreen(redMeasured[index]) - RedOverGreen(whiteMeasured[index]);
			}
			change /= static_cast<float>(walls.size());
			MESSAGE("The lit image's walls, mean R/G change with the red floor: " << change << " (reference " << referenceChange << "); R/G at the "
				<< walls[0].Name << ": white floor " << RedOverGreen(whiteMeasured[0]) << ", red floor " << RedOverGreen(redMeasured[0]));
			CHECK(change >= 0.7f * referenceChange);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Intensity scales the field")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		std::vector<RoomPoint> walls = GetWallPoints();
		walls.resize(7);
		RoomRender render(DefaultRoom(), c_RoomSize, RoomSpecification());
		SetGlobalIllumination(render.SceneData, true, 1.0f);
		const std::vector<glm::vec3> single = MeasureIndirect(render, walls, true);
		SetGlobalIllumination(render.SceneData, true, 2.0f);
		const std::vector<glm::vec3> twice = MeasureIndirect(render, walls, true);
		SetGlobalIllumination(render.SceneData, true, 0.0f);
		const std::vector<glm::vec3> none = MeasureIndirect(render, walls, true);
		for (size_t index = 0; index < walls.size(); index++)
		{
			CAPTURE(walls[index].Name);
			REQUIRE(Luminance(single[index]) > 0.0f);
			CHECK(std::abs(Luminance(twice[index]) / (2.0f * Luminance(single[index])) - 1.0f) <= 0.01f);
			CHECK(none[index] == glm::vec3(0.0f));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The field's share takes no multi-bounce")
	{
		// Real bounce replaces the ambient occlusion's multi-bounce fit inside the field: with ambient occlusion on, the sun
		// room's indirect light inside the full field (all the field's there) is the same to the bit with the fit on and off, at
		// open walls and in the corners the occlusion darkens.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		RoomRender render(DefaultRoom(), c_RoomSize, RoomSpecification());
		SetGlobalIllumination(render.SceneData, true);
		GetPostProcess(render.SceneData).AmbientOcclusion = true;
		std::vector<RoomPoint> points = GetWallPoints();
		points.push_back({ "south wall by the floor", { -11.0f, 0.12f, 1.0f }, c_North });
		points.push_back({ "east wall by the south wall", { -7.0f, 1.5f, 0.9f }, -c_East });
		const std::vector<glm::vec3> with = MeasureIndirect(render, points, true);
		GetPostProcess(render.SceneData).AmbientOcclusionMultiBounce = false;
		const std::vector<glm::vec3> without = MeasureIndirect(render, points, true);
		for (size_t index = 0; index < points.size(); index++)
		{
			CAPTURE(points[index].Name);
			CHECK(Luminance(with[index]) > 0.0f);
			CHECK(with[index] == without[index]);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The row follows SetQuality")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		RoomRender render(DefaultRoom(), c_RoomSize, RoomSpecification(GlobalIlluminationLevel::Off));
		SetGlobalIllumination(render.SceneData, true);
		AssetManager::SetActive(render.Assets.Manager);
		RendererQualitySettings quality = render.Renderer->GetSpecification().Quality;
		for (const GlobalIlluminationLevel level : { GlobalIlluminationLevel::Off, GlobalIlluminationLevel::High, GlobalIlluminationLevel::Low,
			GlobalIlluminationLevel::Off })
		{
			CAPTURE(RendererQuality::GlobalIlluminationLevelToString(level));
			quality.GlobalIllumination = level;
			render.Renderer->SetQuality(quality);
			// Released at once when the row changes, before any frame.
			CHECK(render.Renderer->GetGlobalIlluminationMemoryBytes() == 0);
			REQUIRE(RenderGIFrame(*render.Renderer, render.SceneData, RoomCamera(), SceneRenderOptions {}));
			CHECK(render.Renderer->GetStats().GlobalIllumination == (level != GlobalIlluminationLevel::Off));
			CHECK(render.Renderer->GetGlobalIlluminationMemoryBytes() == LayoutBytes(level));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Paths are equal in the lit image")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!gpu.GetDevice().GetInfo().SupportsDrawIndirectFirstInstance)
			return;
		const auto render = [](GPUDrivenMode mode)
		{
			RoomRender room(DefaultRoom(), c_RoomSize, [mode]()
			{
				SceneRendererSpecification specification = RoomSpecification();
				specification.GPUDriven = mode;
				return specification;
			}());
			SetGlobalIllumination(room.SceneData, true);
			AssetManager::SetActive(room.Assets.Manager);
			for (uint32_t frame = 0; frame < 3; frame++)
				REQUIRE(RenderGIFrame(*room.Renderer, room.SceneData, RoomCamera(), GIViewOptions(SceneLightingView::None)));
			CHECK(room.Renderer->GetStats().GlobalIllumination);
			CHECK(room.Renderer->GetStats().GPUDriven == (mode == GPUDrivenMode::Auto));
			return ReadImage(room.Renderer->GetHDRTexture());
		};
		CHECK(render(GPUDrivenMode::Auto).Pixels == render(GPUDrivenMode::Off).Pixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Inside the field the diffuse is the field's")
	{
		// The sun room's walls and ceiling lie inside the full field, and the room has no sky: their indirect light is the field's
		// alone, the gi view's (the field's light on a white surface) times the plaster's albedo.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const SunRoomOptions room = DefaultRoom();
		const std::vector<RoomPoint> points = GetWallPoints();
		RoomRender render(room, c_RoomSize, RoomSpecification());
		SetGlobalIllumination(render.SceneData, true);
		const std::vector<glm::vec3> field = render.Measure(RoomCamera(), points);
		const std::vector<glm::vec3> indirect = MeasureIndirect(render, points, true);
		for (size_t index = 0; index < points.size(); index++)
		{
			CAPTURE(points[index].Name);
			REQUIRE(Luminance(field[index]) > 0.0f);
			for (int channel = 0; channel < 3; channel++)
				CHECK(indirect[index][channel] == doctest::Approx(field[index][channel] * room.FloorAlbedo[channel]).epsilon(0.01));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Transparent surfaces take the field")
	{
		// A blended quad and a glass pane that scatters half its light lie flat under the sun room's ceiling, below a black box, so
		// the camera sees their undersides, which face the sunlit patch of the floor, and their pixels hold their own light alone.
		// In the open room the field lights them; in the sealed one they stay as dark as its walls (erhe's leak at most); without
		// GI they keep today's light, of which the room has next to none (no sky).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::vector<RoomPoint> panes = {
			{ "blended quad", { -10.8f, 2.5f, -0.6f }, c_Down },
			{ "glass pane", { -9.4f, 2.5f, -0.6f }, c_Down },
		};
		const auto measure = [&](bool sealed, bool asked)
		{
			SunRoomOptions options = DefaultRoom();
			options.SealWindow = sealed;
			RoomRender render(options, c_RoomSize, RoomSpecification());
			REQUIRE(render.Assets.Manager->LoadAssetSync(BuiltinAssets::QuadMesh));
			MaterialProperties black;
			black.BaseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
			black.Roughness = 1.0f;
			AddMesh(render.SceneData, BuiltinAssets::CubeMesh, render.Assets.AddMaterial(black), { -10.1f, 2.7f, -0.6f }, "Backdrop")
				.GetComponent<TransformComponent>().Scale = glm::vec3(2.6f, 0.02f, 1.4f);
			MaterialProperties blended;
			blended.BaseColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.75f);
			blended.AlphaMode = MaterialAlphaMode::Blend;
			blended.Roughness = 1.0f;
			MaterialProperties glass;
			glass.BaseColor = glm::vec4(1.0f);
			glass.Roughness = 1.0f;
			glass.Transmission = 0.5f;
			for (size_t index = 0; index < panes.size(); index++)
			{
				const AssetHandle material = render.Assets.AddMaterial(index == 0 ? blended : glass);
				TransformComponent& transform = AddMesh(render.SceneData, BuiltinAssets::QuadMesh, material, panes[index].Point, panes[index].Name)
					.GetComponent<TransformComponent>();
				transform.Rotation = glm::angleAxis(glm::half_pi<float>(), c_East); // The quad faces +z: turned to face down
				transform.Scale = glm::vec3(0.6f, 1.0f, 1.0f);
			}
			SetGlobalIllumination(render.SceneData, asked);
			return MeasureIndirect(render, panes, asked);
		};
		const std::vector<glm::vec3> open = measure(false, true);
		const std::vector<glm::vec3> sealed = measure(true, true);
		const std::vector<glm::vec3> off = measure(false, false);
		for (size_t index = 0; index < panes.size(); index++)
		{
			CAPTURE(panes[index].Name);
			MESSAGE(panes[index].Name << ": open room " << Luminance(open[index]) << ", sealed " << Luminance(sealed[index]) << ", without GI "
				<< Luminance(off[index]));
			CHECK(Luminance(open[index]) > 0.0f);
			CHECK(Luminance(sealed[index]) <= c_SealedBound * Luminance(open[index]));
			CHECK(Luminance(off[index]) <= c_SealedBound * Luminance(open[index]));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The field meets today's light at its edge")
	{
		// Open ground under the sky, seen along +x: from 5 to 12 m out (across the fade, 7.5-9.5 m at High) neighboring pixels of the
		// lit image differ by at most 2%. Cameras every 0.5 m across one 4 m step of the field's snap, on x and on y (correction
		// 1): a fade without probes, or a field that ends before its fade does, shows as a dark band.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildOpenGround(scene, assets);
		SetGlobalIllumination(scene, true);
		SceneRenderer renderer(RoomSpecification());
		renderer.SetViewportSize(c_RoomSize.x, c_RoomSize.y);
		SceneRenderOptions options;
		options.HDROutput = true;
		options.PreExposure = 1.0f;
		for (const int axis : { 0, 1 })
		{
			for (int step = 0; step < 8; step++)
			{
				glm::vec3 position(0.1f, 1.7f, 0.3f);
				position[axis] += 0.5f * static_cast<float>(step);
				CAPTURE(position);
				const SceneCamera camera = MakeCamera(position, glm::vec3(position.x + 10.0f, 0.0f, position.z), 55.0f, c_RoomSize);
				for (uint32_t frame = 0; frame < 2; frame++)
					REQUIRE(RenderGIFrame(renderer, scene, camera, options));
				REQUIRE(renderer.GetStats().GlobalIllumination);
				REQUIRE_FALSE(renderer.GetStats().GlobalIlluminationPending);
				ReadbackImage image;
				REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), image));
				REQUIRE(image.Format == nvrhi::Format::RGBA16_FLOAT);
				const glm::uvec2 closest = ProjectToPixel(camera, glm::vec3(position.x + 5.0f, 0.0f, position.z), image.Width, image.Height);
				const glm::uvec2 farthest = ProjectToPixel(camera, glm::vec3(position.x + 12.0f, 0.0f, position.z), image.Width, image.Height);
				REQUIRE(closest.y < image.Height);
				REQUIRE(farthest.y < closest.y);
				float largest = 0.0f;
				float previous = Luminance(glm::vec3(ReadHalfTexel(image, farthest.x, farthest.y)));
				for (uint32_t y = farthest.y + 1; y <= closest.y; y++)
				{
					const float value = Luminance(glm::vec3(ReadHalfTexel(image, farthest.x, y)));
					REQUIRE(value > 0.0f);
					largest = std::max(largest, std::abs(value - previous) / std::max(value, previous));
					previous = value;
				}
				CHECK(largest <= 0.02f);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Beyond the field the light is today's, bit for bit")
	{
		// The yard's ground and props: every pixel whose point lies beyond the field's reach on some axis (its weight 0), and the
		// sky, is the same 8-bit value with GI asked as without (x * 1 and + 0 are exact); inside, the field changes the light.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		Scene scene;
		BuildYard(scene, assets);
		const SceneCamera camera = YardStrafe(0, 0.0f, static_cast<float>(c_RoomSize.x) / static_cast<float>(c_RoomSize.y));
		SceneRenderer off(RoomSpecification());
		off.SetViewportSize(c_RoomSize.x, c_RoomSize.y);
		SceneRenderer joined(RoomSpecification());
		joined.SetViewportSize(c_RoomSize.x, c_RoomSize.y);
		for (uint32_t frame = 0; frame < 3; frame++)
			REQUIRE(RenderGIFrame(off, scene, camera, SceneRenderOptions {}));
		SetGlobalIllumination(scene, true);
		for (uint32_t frame = 0; frame < 3; frame++)
			REQUIRE(RenderGIFrame(joined, scene, camera, SceneRenderOptions {}));
		REQUIRE(joined.GetStats().GlobalIllumination);
		REQUIRE_FALSE(joined.GetStats().GlobalIlluminationPending);
		const glm::vec3 reach(joined.GetGlobalIlluminationFieldConstants().FieldFade);
		REQUIRE(reach.x > 0.0f);
		const ReadbackImage offImage = ReadImage(off.GetOutputTexture());
		const ReadbackImage joinedImage = ReadImage(joined.GetOutputTexture());
		REQUIRE(offImage.Pixels.size() == joinedImage.Pixels.size());
		const std::vector<std::optional<glm::vec3>> positions = ReadRelativePositions(joined, camera);
		REQUIRE(positions.size() * 4 == joinedImage.Pixels.size());
		uint32_t beyond = 0;
		uint32_t beyondChanged = 0;
		uint32_t insideChanged = 0;
		for (size_t pixel = 0; pixel < positions.size(); pixel++)
		{
			const bool changed = std::memcmp(offImage.Pixels.data() + pixel * 4, joinedImage.Pixels.data() + pixel * 4, 4) != 0;
			// A 5 cm margin keeps the depth's reconstruction off the reach's boundary.
			const std::optional<glm::vec3>& position = positions[pixel];
			if (!position || glm::any(glm::greaterThan(glm::abs(*position), reach + 0.05f)))
			{
				beyond++;
				beyondChanged += changed ? 1 : 0;
			}
			else
				insideChanged += changed ? 1 : 0;
		}
		MESSAGE("The yard: " << beyond << " of " << positions.size() << " pixels beyond the field, " << beyondChanged << " of them changed; "
			<< insideChanged << " changed inside");
		CHECK(beyond > positions.size() / 10);
		CHECK(beyondChanged == 0);
		CHECK(insideChanged > 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}

namespace
{
	// Renders a still camera's view until the GI says it settled with nothing left to load (at least three frames, at most
	// `maxFrames`); the frames rendered.
	uint32_t RenderUntilSettled(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, const SceneRenderOptions& options, uint32_t maxFrames)
	{
		for (uint32_t frame = 1; frame <= maxFrames; frame++)
		{
			REQUIRE(RenderGIFrame(renderer, scene, camera, options));
			const SceneRendererStats& stats = renderer.GetStats();
			REQUIRE(stats.GlobalIllumination);
			if (frame >= 3 && stats.GlobalIlluminationSettled && stats.GIWaitingProxies == 0 && stats.PendingAssets == 0)
				return frame;
		}
		FAIL_CHECK("the GI did not settle within " << maxFrames << " frames");
		return maxFrames;
	}

	// A color's share of its sum: how red (x), green (y) and blue (z) it is whatever its brightness.
	glm::vec3 Chromaticity(const glm::vec3& color)
	{
		return color / std::max(color.r + color.g + color.b, 1e-6f);
	}

	// The irradiance over pi a point (normal `normal`) receives straight from the rectangle y = `height`, x and z within
	// `minimum` to `maximum`, of luminance `luminance`, facing it (no occluders): numeric integration over `steps` x `steps` cells.
	double RectangleLight(const glm::dvec3& point, const glm::dvec3& normal, double height, const glm::dvec2& minimum, const glm::dvec2& maximum,
		double luminance, int steps)
	{
		const glm::dvec2 cell = (maximum - minimum) / static_cast<double>(steps);
		const double facing = height > point.y ? -1.0 : 1.0; // The rectangle's normal: along y, toward the point
		double sum = 0.0;
		for (int i = 0; i < steps; i++)
		{
			for (int j = 0; j < steps; j++)
			{
				const glm::dvec3 sample(minimum.x + (i + 0.5) * cell.x, height, minimum.y + (j + 0.5) * cell.y);
				const glm::dvec3 toSample = sample - point;
				const double distance2 = glm::dot(toSample, toSample);
				const double distance = std::sqrt(distance2);
				const double cosine = glm::dot(normal, toSample) / distance;
				const double cosineThere = -facing * toSample.y / distance;
				if (cosine > 0.0 && cosineThere > 0.0)
					sum += cosine * cosineThere / distance2;
			}
		}
		return luminance * sum * cell.x * cell.y / glm::pi<double>();
	}

	// The 32 x 32 pixel blocks' mean luminance of an RGBA16F image (the blocks wholly inside it).
	std::vector<float> BlockMeans(const ReadbackImage& image)
	{
		std::vector<float> means;
		for (uint32_t y = 0; y + 32 <= image.Height; y += 32)
		{
			for (uint32_t x = 0; x + 32 <= image.Width; x += 32)
			{
				double sum = 0.0;
				for (uint32_t dy = 0; dy < 32; dy++)
				{
					for (uint32_t dx = 0; dx < 32; dx++)
						sum += Luminance(glm::vec3(ReadHalfTexel(image, x + dx, y + dy)));
				}
				means.push_back(static_cast<float>(sum / 1024.0));
			}
		}
		return means;
	}

	// The largest change of a block's mean from one frame to the next, relative to it; blocks darker than a hundredth of the
	// image's mean are left out (their relative changes are noise around zero).
	float LargestBlockChange(const std::vector<float>& before, const std::vector<float>& after)
	{
		double total = 0.0;
		for (float mean : after)
			total += mean;
		const float darkest = 0.01f * static_cast<float>(total / static_cast<double>(std::max<size_t>(after.size(), 1)));
		float largest = 0.0f;
		for (size_t index = 0; index < before.size() && index < after.size(); index++)
		{
			if (after[index] < darkest)
				continue;
			largest = std::max(largest, std::abs(after[index] - before[index]) / after[index]);
		}
		return largest;
	}

	// The points the Cornell box's camera reads: the ceiling away from the panel, the floor beside each colored wall, the sphere's
	// sides turned toward each wall (45 degrees toward the camera), the floor's middle and the back wall.
	std::vector<RoomPoint> GetCornellPoints()
	{
		const glm::vec3 sphere(1.15f, 0.9f, -0.3f);
		const glm::vec3 towardRed = glm::normalize(glm::vec3(-1.0f, 0.0f, 1.0f));
		const glm::vec3 towardGreen = glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f));
		return {
			{ "ceiling left", { -1.8f, 5.5f, -1.8f }, c_Down },
			{ "ceiling right", { 1.8f, 5.5f, -1.8f }, c_Down },
			{ "ceiling back", { 0.0f, 5.5f, -2.0f }, c_Down },
			{ "floor by the red wall", { -2.45f, 0.0f, 0.8f }, c_Up },
			{ "floor by the green wall", { 2.45f, 0.0f, 0.8f }, c_Up },
			{ "sphere toward the red wall", sphere + 0.9f * towardRed, towardRed },
			{ "sphere toward the green wall", sphere + 0.9f * towardGreen, towardGreen },
			{ "floor middle", { 0.0f, 0.0f, 0.8f }, c_Up },
			{ "back wall", { 0.0f, 2.75f, -2.75f }, -c_North },
		};
	}

	// The field (the gi view) at the Cornell box's points, with one bounce or every bounce (settled); `outFrames` receives the
	// frames it took to settle.
	std::vector<glm::vec3> MeasureCornellBox(float bounceGain, GlobalIlluminationLevel level, uint32_t& outFrames)
	{
		SceneTestAssets assets;
		Scene scene;
		BuildCornellBox(scene, assets);
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(RoomSpecification(level, bounceGain));
		renderer->SetViewportSize(c_RoomSize.x, c_RoomSize.y);
		const SceneCamera camera = CornellCamera(static_cast<float>(c_RoomSize.x) / static_cast<float>(c_RoomSize.y));
		outFrames = RenderUntilSettled(*renderer, scene, camera, GIViewOptions(), 40);
		REQUIRE_FALSE(renderer->GetStats().GlobalIlluminationPending);
		std::vector<glm::vec3> values = ReadPoints(*renderer, camera, GetCornellPoints());
		renderer.reset();
		CollectReleases();
		return values;
	}
}

TEST_SUITE("GPU.SceneRenderer.GlobalIlluminationBounce")
{
	TEST_CASE("The Cornell box: bounces light the ceiling and carry the walls' colors")
	{
		// The owner's box (6.4), lit by its ceiling panel alone, at High (Low bounces once: its gain 1 gives one bounce's light).
		// One bounce leaves the ceiling dark (only the panel's sides reach it,
		// at a grazing angle) and the floor and sphere white. What every bounce adds to each ceiling point is at least half of what
		// the floor alone sends it (the floor's light straight from the panel's underside, integrated, times its albedo, seen from
		// the point: the box and sphere hide less than a quarter of it and the walls add about as much again, so half bounds a
		// field within Gate W's errors), and the floor and sphere beside each
		// colored wall take its color: redder by the red wall than by the green one, greener by the green one. The floor 0.3 m
		// from a wall sees it over nearly half its hemisphere, but the panel's white light dominates there: the walls' light
		// estimated from the panel and the floor puts its red share 9% above the other side's (0.356 against 0.327), so the case
		// asks 4%; the sphere's sides, lit far less by the panel, 10%. One bounce leaves both sides the panel's white.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::vector<RoomPoint> points = GetCornellPoints();
		const double whiteLuminance = Luminance(c_CornellWhite);
		std::vector<double> bounds(points.size(), 0.0);
		for (size_t index = 0; index < 3; index++)
		{
			constexpr int c_Cells = 22;
			const glm::dvec3 point(points[index].Point);
			double sum = 0.0;
			for (int i = 0; i < c_Cells; i++)
			{
				for (int j = 0; j < c_Cells; j++)
				{
					const glm::dvec3 cell(-2.75 + (i + 0.5) * 5.5 / c_Cells, 0.0, -2.75 + (j + 0.5) * 5.5 / c_Cells);
					const double floorLight = RectangleLight(cell, glm::dvec3(0.0, 1.0, 0.0), 5.2, { -0.65, -0.525 }, { 0.65, 0.525 }, c_CornellPanelLuminance, 12);
					const glm::dvec3 toCell = cell - point;
					const double distance2 = glm::dot(toCell, toCell);
					const double cosine = -toCell.y / std::sqrt(distance2); // At the ceiling point and at the floor cell alike
					sum += whiteLuminance * floorLight * cosine * cosine / distance2;
				}
			}
			bounds[index] = 0.5 * sum * (5.5 / c_Cells) * (5.5 / c_Cells) / glm::pi<double>();
		}
		for (GlobalIlluminationLevel level : { GlobalIlluminationLevel::High, GlobalIlluminationLevel::Low })
		{
			const std::string levelName = level == GlobalIlluminationLevel::High ? "High" : "Low";
			CAPTURE(levelName);
			uint32_t oneFrames = 0;
			uint32_t everyFrames = 0;
			const std::vector<glm::vec3> one = MeasureCornellBox(0.0f, level, oneFrames);
			const std::vector<glm::vec3> every = MeasureCornellBox(1.0f, level, everyFrames);
			if (!GlobalIllumination::ComputeLayout(level)->Bounces)
			{
				// Low bounces once whatever the gain (GlobalIlluminationLayout::Bounces): the same light, settled with the voxels.
				for (size_t index = 0; index < points.size(); index++)
				{
					INFO(points[index].Name);
					CHECK(every[index] == one[index]);
				}
				CHECK(everyFrames == oneFrames);
				continue;
			}
			std::string table = fmt::format("the Cornell box at {} (every bounce settled in {} frames), gi view (nits per albedo):", levelName, everyFrames);
			for (size_t index = 0; index < points.size(); index++)
			{
				const glm::vec3 chroma = Chromaticity(every[index]);
				table += fmt::format("\n  {:<30} one bounce {:9.3f}  every bounce {:9.3f} (r {:.3f} g {:.3f} b {:.3f})", points[index].Name,
					Luminance(one[index]), Luminance(every[index]), chroma.r, chroma.g, chroma.b);
				if (bounds[index] > 0.0)
					table += fmt::format("  bound {:.3f}", bounds[index]);
			}
			MESSAGE(table);
			for (size_t index = 0; index < 3; index++)
			{
				INFO(points[index].Name);
				CHECK(Luminance(every[index]) - Luminance(one[index]) >= bounds[index]);
			}
			// Beside each wall the floor and the sphere take its color; with one bounce both sides are the panel's white.
			const glm::vec3 redFloor = Chromaticity(every[3]);
			const glm::vec3 greenFloor = Chromaticity(every[4]);
			const glm::vec3 redSphere = Chromaticity(every[5]);
			const glm::vec3 greenSphere = Chromaticity(every[6]);
			CHECK(redFloor.r > 1.04f * greenFloor.r);
			CHECK(greenFloor.g > 1.04f * redFloor.g);
			CHECK(redSphere.r > 1.1f * greenSphere.r);
			CHECK(greenSphere.g > 1.1f * redSphere.g);
			CHECK(Chromaticity(one[3]).r <= 1.04f * Chromaticity(one[4]).r);
			for (size_t index = 0; index < points.size(); index++)
			{
				INFO(points[index].Name);
				CHECK(Luminance(every[index]) >= 0.999f * Luminance(one[index]));
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A glowing closed box converges to the series of bounces")
	{
		// A closed 4 x 3 x 4 m box whose every face emits Le and reflects rho of its light: inside, the radiance is uniform, Le with
		// one bounce and Le (1 + rho + rho^2 + ...) = Le / (1 - rho) with every bounce, rho the albedo as the voxels hold it (8 bits).
		// The field reads Le within 10% with one bounce (the cascades' own error in a closed room: 4-9% low at High). Each bounce
		// gathers through the same field, so with every bounce a point whose one-bounce field reads f_i Le reads
		// f_i Le / (1 - rho f), f the room's mean ratio (the six points' mean): re-injection adds no error of its own when every
		// point is within 3% of that series (and the series is within 20% of Le / (1 - rho)).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr float c_Luminance = 100.0f;
		constexpr float c_Albedo = 0.5f;
		constexpr float c_PreExposure = 1.0f / 256.0f;
		const float storedAlbedo = std::round(c_Albedo * 255.0f) / 255.0f;
		const std::vector<RoomPoint> points = {
			{ "north wall", { 0.0f, 1.5f, -2.0f }, -c_North },
			{ "north wall high", { 0.8f, 2.2f, -2.0f }, -c_North },
			{ "west wall", { -2.0f, 1.2f, -1.0f }, c_East },
			{ "east wall", { 2.0f, 1.8f, -1.0f }, -c_East },
			{ "floor", { 0.0f, 0.0f, -1.0f }, c_Up },
			{ "ceiling", { 0.0f, 3.0f, -1.2f }, c_Down },
		};
		const SceneCamera camera = MakeCamera(glm::vec3(0.0f, 1.5f, 1.6f), glm::vec3(0.0f, 1.5f, -2.0f), 75.0f, c_RoomSize);
		for (GlobalIlluminationLevel level : { GlobalIlluminationLevel::High, GlobalIlluminationLevel::Low })
		{
			CAPTURE(level == GlobalIlluminationLevel::High ? "High" : "Low");
			// Low bounces once whatever the gain (GlobalIlluminationLayout::Bounces): its second pass reads the first's values.
			const bool bounces = GlobalIllumination::ComputeLayout(level)->Bounces;
			std::vector<float> oneRatios(points.size(), 1.0f); // One bounce's field over Le at each point (the first pass)
			for (const float bounceGain : { 0.0f, 1.0f })
			{
				CAPTURE(bounceGain);
				SceneTestAssets assets;
				Scene scene;
				BuildGlowingBox(scene, assets, glm::vec3(-2.0f, 0.0f, -2.0f), glm::vec3(2.0f, 3.0f, 2.0f), c_Albedo, c_Luminance);
				Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(RoomSpecification(level, bounceGain));
				renderer->SetViewportSize(c_RoomSize.x, c_RoomSize.y);
				SceneRenderOptions options = GIViewOptions();
				options.PreExposure = c_PreExposure;
				// Settled, then on until each further bounce adds less than a thousandth of Le (rho^12).
				const uint32_t settled = RenderUntilSettled(*renderer, scene, camera, options, 40);
				for (uint32_t frame = settled; frame < 14; frame++)
					REQUIRE(RenderGIFrame(*renderer, scene, camera, options));
				ReadbackImage image;
				REQUIRE(Renderer::ReadTexture(renderer->GetHDRTexture(), image));
				const bool everyBounce = bounces && bounceGain > 0.0f;
				const float expected = everyBounce ? c_Luminance / (1.0f - storedAlbedo) : c_Luminance;
				std::string table = fmt::format("{}: expected {:.3f}", everyBounce ? "every bounce" : "one bounce", expected);
				float meanRatio = 0.0f;
				for (const float ratio : oneRatios)
					meanRatio += ratio / static_cast<float>(oneRatios.size());
				for (size_t index = 0; index < points.size(); index++)
				{
					const RoomPoint& point = points[index];
					const glm::uvec2 pixel = ProjectToPixel(camera, point.Point + point.Normal * 0.01f, image.Width, image.Height);
					REQUIRE(pixel.x > 2);
					REQUIRE(pixel.y > 2);
					REQUIRE(pixel.x + 3 < image.Width);
					REQUIRE(pixel.y + 3 < image.Height);
					const float value = Luminance(glm::vec3(ReadHalfTexel(image, pixel.x, pixel.y))) / c_PreExposure;
					INFO(point.Name << ": " << value << " against " << expected);
					if (!everyBounce)
					{
						if (bounceGain > 0.0f)
							CHECK(value == doctest::Approx(oneRatios[index] * expected).epsilon(1e-6));
						oneRatios[index] = value / expected;
						table += fmt::format("\n  {:<16} {:9.3f} ({:.3f})", point.Name, value, value / expected);
						CHECK(std::abs(value / expected - 1.0f) <= 0.1f);
						continue;
					}
					const float series = oneRatios[index] * c_Luminance / (1.0f - storedAlbedo * meanRatio);
					table += fmt::format("\n  {:<16} {:9.3f} ({:.3f}; the one-bounce series {:.3f}: {:.3f})", point.Name, value, value / expected, series,
						value / series);
					CHECK(std::abs(value / series - 1.0f) <= 0.03f);
					CHECK(std::abs(value / expected - 1.0f) <= 0.2f);
				}
				MESSAGE(table);
				renderer.reset();
				CollectReleases();
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Bounces fill the sun room")
	{
		// 6.4: with every bounce each point takes at least its one-bounce light and the ceiling over the patch at least 10% more.
		// What the bounces add is at most the series rho / (1 - rho) times the brightest one-bounce light the points read (each
		// bounce's field is at most rho times the brightest of the one before; the ceiling over the patch is the room's brightest
		// field). The spec's guess, at most 2.5 times the one-bounce reference, does not hold where one bounce is dim: what the
		// bounces add is about even across the room, so the east and south walls rise about threefold, and the floor, which one
		// bounce leaves dark (it sees no sunlit surface), far more.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const SunRoomOptions room = DefaultRoom();
		const std::vector<RoomPoint> walls = GetWallPoints();
		const std::vector<RoomPoint> floors = GetFloorPoints();
		const auto measure = [&](float bounceGain, const SceneCamera& camera, glm::uvec2 size, const std::vector<RoomPoint>& points)
		{
			RoomRender render(room, size, RoomSpecification(GlobalIlluminationLevel::High, bounceGain));
			return render.Measure(camera, points, bounceGain > 0.0f ? c_BounceFrames : 3);
		};
		std::vector<RoomPoint> points = walls;
		points.insert(points.end(), floors.begin(), floors.end());
		std::vector<glm::vec3> one = measure(0.0f, RoomCamera(), c_RoomSize, walls);
		std::vector<glm::vec3> every = measure(1.0f, RoomCamera(), c_RoomSize, walls);
		const std::vector<glm::vec3> oneFloors = measure(0.0f, FloorCamera(), c_FloorSize, floors);
		const std::vector<glm::vec3> everyFloors = measure(1.0f, FloorCamera(), c_FloorSize, floors);
		CollectReleases();
		one.insert(one.end(), oneFloors.begin(), oneFloors.end());
		every.insert(every.end(), everyFloors.begin(), everyFloors.end());
		const std::vector<glm::vec3> references = References(room, points);
		std::string table = "the sun room at High, one bounce and every bounce against the one-bounce reference:";
		for (size_t index = 0; index < points.size(); index++)
		{
			const float oneValue = Luminance(one[index]);
			const float everyValue = Luminance(every[index]);
			table += fmt::format("\n  {:<30} one {:9.3f}  every {:9.3f} ({:.3f} of one, {:.3f} of the reference)", points[index].Name, oneValue, everyValue,
				everyValue / std::max(oneValue, 1e-6f), everyValue / std::max(Luminance(references[index]), 1e-6f));
		}
		float brightest = 0.0f;
		for (const glm::vec3& value : one)
			brightest = std::max(brightest, Luminance(value));
		const float albedo = std::min(room.FloorAlbedo.x, RenderData::c_GIMaxBounceAlbedo); // The plaster's, walls and floor alike
		const float addedBound = albedo / (1.0f - albedo) * brightest;
		table += fmt::format("\n  what the bounces add: at most {:.3f}", addedBound);
		MESSAGE(table);
		for (size_t index = 0; index < points.size(); index++)
		{
			INFO(points[index].Name);
			CHECK(Luminance(every[index]) >= 0.999f * Luminance(one[index]));
			CHECK(Luminance(every[index]) - Luminance(one[index]) <= addedBound);
		}
		// The ceiling over the patch: GetWallPoints' "ceiling over the window side" and "ceiling middle".
		for (const size_t index : { size_t { 7 }, size_t { 8 } })
		{
			INFO(points[index].Name);
			CHECK(Luminance(every[index]) >= 1.1f * Luminance(one[index]));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Bounces settle after a cut")
	{
		// 6.4: a still camera's gi view from its first frame (a cut). The GI says it settled GISettleFrames frames after the cut
		// (GlobalIllumination::GetSettleFrames of the scene's brightest albedo) and stays settled; the first frame K from which
		// every 32 px block's mean changes by less than 0.25% for four frames comes at least c_GISettleMarginFrames before that.
		// The sun room, the yard, the Cornell box and a closed box glowing at the albedo clamp (0.95, kept as 0.9: the slowest
		// series, c_GIMaxSettleFrames), at High.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr uint32_t c_Frames = RenderData::c_GIMaxSettleFrames + 8;
		constexpr float c_Change = RenderData::c_GISettleChange;
		const float aspect = static_cast<float>(c_RoomSize.x) / static_cast<float>(c_RoomSize.y);
		for (uint32_t sceneIndex = 0; sceneIndex < 4; sceneIndex++)
		{
			const std::string sceneName = sceneIndex == 0 ? "the sun room" : sceneIndex == 1 ? "the yard" : sceneIndex == 2 ? "the Cornell box"
				: "a glowing closed box at the clamp";
			CAPTURE(sceneName);
			SceneTestAssets assets;
			Scene scene;
			SceneCamera camera;
			if (sceneIndex == 0)
			{
				BuildSunRoom(scene, assets, DefaultRoom());
				camera = RoomCamera();
			}
			else if (sceneIndex == 1)
			{
				BuildYard(scene, assets);
				camera = YardStrafe(0, 0.0f, aspect);
			}
			else if (sceneIndex == 2)
			{
				BuildCornellBox(scene, assets);
				camera = CornellCamera(aspect);
			}
			else
			{
				BuildGlowingBox(scene, assets, glm::vec3(-2.0f, 0.0f, -2.0f), glm::vec3(2.0f, 3.0f, 2.0f), 0.95f, 100.0f);
				camera = MakeCamera(glm::vec3(0.0f, 1.5f, 1.6f), glm::vec3(0.0f, 1.5f, -2.0f), 75.0f, c_RoomSize);
			}
			Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(RoomSpecification(GlobalIlluminationLevel::High, 1.0f));
			renderer->SetViewportSize(c_RoomSize.x, c_RoomSize.y);
			std::vector<std::vector<float>> means;
			uint32_t firstSettled = 0;
			uint32_t settleFrames = 0;
			for (uint32_t frame = 1; frame <= c_Frames; frame++)
			{
				REQUIRE(RenderGIFrame(*renderer, scene, camera, GIViewOptions()));
				REQUIRE(renderer->GetStats().GlobalIllumination);
				REQUIRE_FALSE(renderer->GetStats().GlobalIlluminationPending);
				settleFrames = renderer->GetStats().GISettleFrames;
				if (renderer->GetStats().GlobalIlluminationSettled && firstSettled == 0)
					firstSettled = frame;
				if (firstSettled != 0)
					CHECK(renderer->GetStats().GlobalIlluminationSettled);
				ReadbackImage image;
				REQUIRE(Renderer::ReadTexture(renderer->GetHDRTexture(), image));
				means.push_back(BlockMeans(image));
			}
			// changes[k] is the change from frame k + 1 to frame k + 2 (frames counted from 1).
			std::vector<float> changes;
			for (size_t index = 1; index < means.size(); index++)
				changes.push_back(LargestBlockChange(means[index - 1], means[index]));
			uint32_t settledImage = 0;
			for (uint32_t frame = 1; frame + 4 <= c_Frames && settledImage == 0; frame++)
			{
				bool still = true;
				for (uint32_t step = 0; step < 4; step++)
					still = still && changes[frame - 1 + step] < c_Change;
				if (still)
					settledImage = frame;
			}
			std::string list;
			for (size_t index = 0; index < changes.size(); index++)
				list += fmt::format("{}{:.3f}%", index == 0 ? "" : " ", 100.0f * changes[index]);
			MESSAGE(sceneName << ": the image settles (K) at frame " << settledImage << ", the GI says settled at frame " << firstSettled
				<< " (GISettleFrames " << settleFrames << "); the largest block change frame to frame: " << list);
			CHECK(settledImage > 0);
			CHECK(firstSettled == settleFrames + 1);
			CHECK(settledImage + RenderData::c_GISettleMarginFrames <= firstSettled);
			if (sceneIndex == 0)
				CHECK(settleFrames + 4 <= c_BounceFrames);
			if (sceneIndex == 3)
				CHECK(settleFrames == RenderData::c_GIMaxSettleFrames);
			renderer.reset();
			CollectReleases();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A move takes each voxel's bounce from its own place")
	{
		// Re-injection reads the field the frame before left relative to that frame's camera (Bounce.yzw, this camera minus that
		// one, and PreviousField's voxel origins): the sun room settled under the floor camera 1 m west of its middle, which then
		// moves 2 m east in one frame (less than a window: no cut, the bounces go on). In the frame after the move the floor,
		// lit almost only by the walls' and the ceiling's re-injected light, reads as a renderer settled at the new camera does,
		// within 1% at each of nine points (measured: within 0.02%; with Bounce.yzw 0, which takes each voxel's bounce from 2 m
		// west, 11% to four times as bright).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr float c_Tolerance = 0.01f;
		const glm::vec3 before(-1.0f, 0.0f, 0.0f);
		const glm::vec3 after(1.0f, 0.0f, 0.0f);
		std::vector<RoomPoint> points;
		for (float x : { -11.6f, -10.0f, -8.4f })
		{
			for (float z : { -4.4f, -2.0f, 0.4f })
				points.push_back({ fmt::format("floor at {}, {}", x, z), { x, 0.1f, z }, c_Up });
		}
		std::vector<glm::vec3> moved;
		{
			RoomRender render(DefaultRoom(), c_FloorSize, RoomSpecification(GlobalIlluminationLevel::High, 1.0f));
			render.Measure(FloorCamera(before), points, c_BounceFrames);
			REQUIRE(render.Renderer->GetStats().GlobalIlluminationSettled);
			moved = render.Measure(FloorCamera(after), points, 1);
		}
		CollectReleases();
		std::vector<glm::vec3> settled;
		{
			RoomRender render(DefaultRoom(), c_FloorSize, RoomSpecification(GlobalIlluminationLevel::High, 1.0f));
			settled = render.Measure(FloorCamera(after), points, c_BounceFrames);
		}
		CollectReleases();
		std::string table = "the floor in the frame after a 2 m move against settled there (gi view, nits per albedo):";
		for (size_t index = 0; index < points.size(); index++)
		{
			const float ratio = Luminance(moved[index]) / std::max(Luminance(settled[index]), 1e-6f);
			table += fmt::format("\n  {:<22} {:9.3f} against {:9.3f} ({:.4f})", points[index].Name, Luminance(moved[index]), Luminance(settled[index]), ratio);
			INFO(points[index].Name);
			CHECK(Luminance(settled[index]) > 1.0f);
			CHECK(std::abs(ratio - 1.0f) <= c_Tolerance);
		}
		MESSAGE(table);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Still frames are identical with bounces")
	{
		// 6.4: re-injection feeds the field back through the voxels, yet a still camera's frames 30 and 31 agree to the bit (no
		// rounding cycle), in the sun room and the Cornell box at High (Low bounces once: "Still frames are identical").
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		REQUIRE(GlobalIllumination::ComputeLayout(GlobalIlluminationLevel::High)->Bounces);
		for (uint32_t run = 0; run < 2; run++)
		{
			const GlobalIlluminationLevel level = GlobalIlluminationLevel::High;
			CAPTURE(run);
			SceneTestAssets assets;
			Scene scene;
			SceneCamera camera;
			if (run == 0)
			{
				BuildSunRoom(scene, assets, DefaultRoom());
				camera = RoomCamera();
			}
			else
			{
				BuildCornellBox(scene, assets);
				camera = CornellCamera(static_cast<float>(c_RoomSize.x) / static_cast<float>(c_RoomSize.y));
			}
			Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(RoomSpecification(level, 1.0f));
			renderer->SetViewportSize(c_RoomSize.x, c_RoomSize.y);
			ReadbackImage frames[2];
			for (uint32_t frame = 1; frame <= 31; frame++)
			{
				REQUIRE(RenderGIFrame(*renderer, scene, camera, GIViewOptions()));
				if (frame >= 30)
					REQUIRE(Renderer::ReadTexture(renderer->GetHDRTexture(), frames[frame - 30]));
			}
			REQUIRE(frames[0].Pixels.size() == frames[1].Pixels.size());
			REQUIRE_FALSE(frames[0].Pixels.empty());
			size_t differing = 0;
			for (size_t index = 0; index < frames[0].Pixels.size(); index++)
				differing += frames[0].Pixels[index] != frames[1].Pixels[index] ? 1 : 0;
			CHECK(differing == 0);
			renderer.reset();
			CollectReleases();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("An emissive panel lights a dark room by bounces")
	{
		// 6.4: the sealed room without a sun, lit only by a 1 x 0.6 m panel of 2,000 nits on its west wall: the far (east) wall is
		// brighter with every bounce than with one, and the room sees no sky (its open share 0).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SunRoomOptions dark;
		dark.SunIlluminance = 0.0f;
		dark.SealWindow = true;
		const std::vector<RoomPoint> points = {
			{ "east wall low", { -7.0f, 1.0f, -0.5f }, -c_East },
			{ "east wall high", { -7.0f, 2.0f, -1.5f }, -c_East },
		};
		float values[2] = {};
		for (uint32_t pass = 0; pass < 2; pass++)
		{
			const float bounceGain = pass == 0 ? 0.0f : 1.0f;
			RoomRender render(dark, c_RoomSize, RoomSpecification(GlobalIlluminationLevel::High, bounceGain));
			AddMesh(render.SceneData, BuiltinAssets::CubeMesh, render.Assets.AddMaterial(Emitting(2000.0f)), glm::vec3(-12.95f, 1.5f, -2.0f), "Panel")
				.GetComponent<TransformComponent>().Scale = glm::vec3(0.1f, 0.6f, 1.0f);
			values[pass] = MeanLuminance(render.Measure(RoomCamera(), points, bounceGain > 0.0f ? c_BounceFrames : 3));
			if (pass == 1)
			{
				REQUIRE(RenderGIFrame(*render.Renderer, render.SceneData, RoomCamera(), GIViewOptions(SceneLightingView::OpenShare)));
				const std::vector<glm::vec3> open = ReadPoints(*render.Renderer, RoomCamera(), points);
				for (size_t index = 0; index < points.size(); index++)
				{
					INFO(points[index].Name);
					CHECK(Luminance(open[index]) == 0.0f);
				}
			}
		}
		CollectReleases();
		MESSAGE("the east wall's light from the panel: one bounce " << values[0] << ", every bounce " << values[1] << " (" << values[1] / values[0] << ")");
		CHECK(values[0] > 0.0f);
		CHECK(values[1] > 1.05f * values[0]);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Bounces add no flicker in motion")
	{
		// 6.4: the strafes again with every bounce. Moving the camera scrolls the voxel levels and moves the field, and each frame's
		// bounce takes last frame's field from the same world place (Bounce.yzw), so a strafe's gi view flickers about as much with
		// every bounce as with one: the yard's fence strafe (0.02 m a frame) and a strafe across the sun room (0.06 m a frame: probe
		// cells and voxel scrolls crossed). Each renderer first settles at the strafe's first camera, so the series' approach is not
		// measured as change. Each statistic with every bounce is at most half again one bounce's, or 0.01% above it where one
		// bounce barely flickers (measured: the yard 0.0064% mean against 0.0055%, the sun room 0.031% against 0.048%).
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr uint32_t c_StrafeFrames = 12;
		const float aspect = static_cast<float>(c_RoomSize.x) / static_cast<float>(c_RoomSize.y);
		for (uint32_t sceneIndex = 0; sceneIndex < 2; sceneIndex++)
		{
			const std::string sceneName = sceneIndex == 0 ? "the yard's fence strafe" : "the sun room strafe";
			CAPTURE(sceneName);
			const auto path = [&](uint32_t frame)
			{
				return sceneIndex == 0 ? YardStrafe(frame, 1.0f, aspect) : RoomCamera(glm::vec3(0.06f * static_cast<float>(frame), 0.0f, 0.0f));
			};
			FlickerResult results[2];
			for (uint32_t pass = 0; pass < 2; pass++)
			{
				SceneTestAssets assets;
				Scene scene;
				if (sceneIndex == 0)
					BuildYard(scene, assets);
				else
					BuildSunRoom(scene, assets, DefaultRoom());
				Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(RoomSpecification(GlobalIlluminationLevel::High, pass == 0 ? 0.0f : 1.0f));
				renderer->SetViewportSize(c_RoomSize.x, c_RoomSize.y);
				RenderUntilSettled(*renderer, scene, path(0), GIViewOptions(), RenderData::c_GIMaxSettleFrames + 4);
				REQUIRE(renderer->GetStats().GlobalIlluminationSettled);
				FlickerSequence sequence(3);
				for (uint32_t frame = 0; frame < c_StrafeFrames; frame++)
				{
					const SceneCamera camera = path(frame);
					REQUIRE(RenderGIFrame(*renderer, scene, camera, GIViewOptions()));
					REQUIRE_FALSE(renderer->GetStats().GlobalIlluminationPending);
					sequence.Add(ReadFlickerFrame(*renderer, camera));
				}
				results[pass] = sequence.GetResult();
				renderer.reset();
				CollectReleases();
			}
			for (uint32_t pass = 0; pass < 2; pass++)
			{
				const FlickerResult& result = results[pass];
				MESSAGE(sceneName << ", " << std::string(pass == 0 ? "one bounce" : "every bounce") << ": mean " << result.Mean * 100.0 << "%, p99 " << result.P99 * 100.0
					<< "%, blocks " << result.Blocks * 100.0 << "%, blocks p99 " << result.BlocksP99 * 100.0 << "%, measured "
					<< result.MeasuredShare * 100.0 << "% over " << result.Frames << " triples");
				CHECK(std::isfinite(result.Mean));
				CHECK(std::isfinite(result.Blocks));
				CHECK(result.MeasuredShare > 0.3);
				CHECK(result.Frames == c_StrafeFrames - 3);
			}
			const auto bound = [](double one) { return std::max(1.5 * one, one + 0.0001); };
			CHECK(results[1].Mean <= bound(results[0].Mean));
			CHECK(results[1].P99 <= bound(results[0].P99));
			CHECK(results[1].Blocks <= bound(results[0].Blocks));
			CHECK(results[1].BlocksP99 <= bound(results[0].BlocksP99));
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}

namespace
{
	// The Cornell box's gi view with every bounce, settled, each pixel's world position (none where nothing is drawn) and the field's
	// probe planes: cascade 0's probes lie at Origin + (i + 0.5) Spacing.
	struct CornellView
	{
		ReadbackImage Image;
		std::vector<std::optional<glm::vec3>> Positions;
		SceneCamera Camera;
		glm::vec3 Origin { 0.0f };
		float Spacing = 0.0f;

		float GetLuminance(uint32_t x, uint32_t y) const
		{
			return Luminance(glm::vec3(ReadHalfTexel(Image, x, y))) / c_RoomPreExposure;
		}

		// The gi view's luminance at a world point: the pixel it projects to, when that pixel shows the point (its position within
		// 1.5 cm of it).
		std::optional<float> Read(const glm::vec3& point) const
		{
			const glm::vec4 clip = Camera.Projection * Camera.View * glm::vec4(point, 1.0f);
			if (clip.w <= 0.0f || std::abs(clip.x) >= clip.w || std::abs(clip.y) >= clip.w)
				return std::nullopt;
			const glm::uvec2 pixel = ProjectToPixel(Camera, point, Image.Width, Image.Height);
			if (pixel.x >= Image.Width || pixel.y >= Image.Height)
				return std::nullopt;
			const std::optional<glm::vec3>& position = Positions[static_cast<size_t>(pixel.y) * Image.Width + pixel.x];
			if (!position || glm::distance(*position, point) > 0.015f)
				return std::nullopt;
			return GetLuminance(pixel.x, pixel.y);
		}
	};

	CornellView RenderCornellView(GlobalIlluminationLevel level)
	{
		SceneTestAssets assets;
		Scene scene;
		BuildCornellBox(scene, assets);
		Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(RoomSpecification(level, 1.0f));
		renderer->SetViewportSize(c_RoomSize.x, c_RoomSize.y);
		CornellView view;
		view.Camera = CornellCamera(static_cast<float>(c_RoomSize.x) / static_cast<float>(c_RoomSize.y));
		RenderUntilSettled(*renderer, scene, view.Camera, GIViewOptions(), 40);
		REQUIRE_FALSE(renderer->GetStats().GlobalIlluminationPending);
		REQUIRE(Renderer::ReadTexture(renderer->GetHDRTexture(), view.Image));
		REQUIRE(view.Image.Format == nvrhi::Format::RGBA16_FLOAT);
		view.Positions = ReadRelativePositions(*renderer, view.Camera);
		for (std::optional<glm::vec3>& position : view.Positions)
		{
			if (position)
				*position += view.Camera.Position;
		}
		view.Origin = view.Camera.Position + glm::vec3(renderer->GetGlobalIlluminationFieldConstants().FieldOrigin);
		view.Spacing = FirstSpacing(level);
		renderer.reset();
		CollectReleases();
		return view;
	}

	// One of the Cornell box's inside faces: the plane where coordinate `Axis` is `Value`, within `Minimum` to `Maximum` along
	// the other two axes (in x, y, z order).
	struct CornellFace
	{
		const char* Name = "";
		int Axis = 0;
		float Value = 0.0f;
		glm::vec2 Minimum { 0.0f };
		glm::vec2 Maximum { 0.0f };
	};

	// How far outside the Cornell box's sphere and tall box a floor point (x, z) lies (negative under them): the sphere's footprint is
	// its radius around its foot, the box's its 1.6 m square turned 18 degrees, measured along its own axes.
	float GetCornellClearance(const glm::vec2& point)
	{
		const glm::vec2 sphere(1.15f, -0.3f);
		const glm::vec2 d = point - glm::vec2(-1.05f, -1.0f);
		const float angle = glm::radians(18.0f);
		const glm::vec2 local(std::cos(angle) * d.x - std::sin(angle) * d.y, std::sin(angle) * d.x + std::cos(angle) * d.y);
		return std::min(glm::distance(point, sphere) - 0.9f, std::max(std::abs(local.x), std::abs(local.y)) - 0.8f);
	}

	// The median of some values (0 for none).
	float Median(std::vector<float> values)
	{
		if (values.empty())
			return 0.0f;
		std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2), values.end());
		return values[values.size() / 2];
	}

	// A cascade-0 probe's depth moments toward a direction, bilinear between the four texels of its tile around the direction's
	// octahedral point (the texels Include/GIField.slang's LiesBeforeProbeHits reads), as their mixture: the mean distance to the
	// first hit and its variance.
	glm::vec2 ReadProbeDepth(const SceneRenderer& renderer, const std::vector<uint8_t>& depth, const glm::ivec3& probe, const glm::vec3& direction)
	{
		const glm::ivec3 grid(renderer.GetGlobalIlluminationLayout()->Cascades.front().Grid);
		constexpr int c_Texels = static_cast<int>(RenderData::c_GIFieldDepthTexels);
		const glm::vec3 n = direction / (std::abs(direction.x) + std::abs(direction.y) + std::abs(direction.z));
		const glm::vec2 encoded = n.z >= 0.0f ? glm::vec2(n.x, n.y)
			: glm::vec2((1.0f - std::abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f), (1.0f - std::abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f));
		const glm::vec2 coordinate = (encoded * 0.5f + 0.5f) * static_cast<float>(c_Texels - 2) + 0.5f;
		const glm::ivec2 first = glm::clamp(glm::ivec2(glm::floor(coordinate)), glm::ivec2(0), glm::ivec2(c_Texels - 2));
		const glm::vec2 fraction = glm::clamp(coordinate - glm::vec2(first), 0.0f, 1.0f);
		glm::vec2 texels[4];
		for (int corner = 0; corner < 4; corner++)
		{
			const size_t x = static_cast<size_t>(probe.x * c_Texels + first.x + (corner & 1));
			const size_t y = static_cast<size_t>(probe.y * c_Texels + first.y + (corner >> 1));
			const size_t index = x + static_cast<size_t>(grid.x * c_Texels) * (y + static_cast<size_t>(grid.y * c_Texels) * static_cast<size_t>(probe.z));
			REQUIRE((index + 1) * 4 <= depth.size());
			uint16_t value[2];
			std::memcpy(value, depth.data() + index * 4, sizeof(value));
			texels[corner] = glm::vec2(glm::unpackHalf1x16(value[0]), glm::unpackHalf1x16(value[1]));
		}
		const float weights[4] = { (1.0f - fraction.x) * (1.0f - fraction.y), fraction.x * (1.0f - fraction.y), (1.0f - fraction.x) * fraction.y,
			fraction.x * fraction.y };
		float mean = 0.0f;
		for (int corner = 0; corner < 4; corner++)
			mean += weights[corner] * texels[corner].x;
		float variance = 0.0f;
		for (int corner = 0; corner < 4; corner++)
			variance += weights[corner] * (texels[corner].y + (texels[corner].x - mean) * (texels[corner].x - mean));
		return glm::vec2(mean, variance);
	}
}

TEST_SUITE("GPU.SceneRenderer.GlobalIlluminationStitching")
{
	TEST_CASE("The Cornell box's GI has no steps at probe-cell borders")
	{
		// The owner's Cornell box (every bounce, settled): where a face crosses a probe plane, the gather's eight probes change, and
		// a gather that switches probes on and off (a visibility march's verdict, a renormalized weight that reaches 0) changes its
		// light there in a step: the bands on the walls and the blocks on the floor. Across every probe plane crossing the floor,
		// the ceiling and the three walls (lines 5 cm apart, 15 cm inside the face's edges), the change between the points c_Near
		// on either side of the plane exceeds what the light's slope just beside it (the changes from c_Near to 3 c_Near on each
		// side) predicts by at most c_MaxStep of the light there (or of a twentieth of the faces' median where darker). A smooth
		// gradient passes however steep; a step of a probe's weight does not. The floor within 0.3 m of the sphere and the box is
		// the contact case's (below). At High (the owner's look); Low's 1 m probes are measured and reported: next to the box and
		// the sphere whole cells of them lie in the coarse voxels' solids, and the floor and walls there still step (DeveloperGuide,
		// "Global illumination").
		constexpr float c_Near = 0.02f;
		constexpr float c_MaxStep = 0.1f;
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::vector<CornellFace> faces = {
			{ "floor", 1, 0.0f, { -2.75f, -2.75f }, { 2.75f, 2.75f } },
			{ "ceiling", 1, 5.5f, { -2.75f, -2.75f }, { 2.75f, 2.75f } },
			{ "back wall", 2, -2.75f, { -2.75f, 0.0f }, { 2.75f, 5.5f } },
			{ "red wall", 0, -2.75f, { 0.0f, -2.75f }, { 5.5f, 2.75f } },
			{ "green wall", 0, 2.75f, { 0.0f, -2.75f }, { 5.5f, 2.75f } },
		};
		for (GlobalIlluminationLevel level : { GlobalIlluminationLevel::High, GlobalIlluminationLevel::Low })
		{
			const std::string levelName = level == GlobalIlluminationLevel::High ? "High" : "Low";
			CAPTURE(levelName);
			const CornellView view = RenderCornellView(level);
			for (const CornellFace& face : faces)
			{
				INFO(face.Name);
				// The face's two tangent axes, in x, y, z order (CornellFace's Minimum and Maximum).
				const int axes[2] = { face.Axis == 0 ? 1 : 0, face.Axis == 2 ? 1 : 2 };
				const auto pointOn = [&](float a, float b)
				{
					glm::vec3 point(0.0f);
					point[face.Axis] = face.Value;
					point[axes[0]] = a;
					point[axes[1]] = b;
					return point;
				};
				std::vector<float> values;
				for (float a = face.Minimum.x + 0.15f; a <= face.Maximum.x - 0.15f; a += 0.1f)
				{
					for (float b = face.Minimum.y + 0.15f; b <= face.Maximum.y - 0.15f; b += 0.1f)
					{
						if (const std::optional<float> value = view.Read(pointOn(a, b)))
							values.push_back(*value);
					}
				}
				const float floorLevel = 0.05f * Median(values);
				float largest = 0.0f;
				glm::vec3 where(0.0f);
				uint32_t crossings = 0;
				for (int across = 0; across < 2; across++)
				{
					const int axis = axes[across];
					const float low = across == 0 ? face.Minimum.x + 0.15f : face.Minimum.y + 0.15f;
					const float high = across == 0 ? face.Maximum.x - 0.15f : face.Maximum.y - 0.15f;
					const float otherLow = across == 0 ? face.Minimum.y + 0.15f : face.Minimum.x + 0.15f;
					const float otherHigh = across == 0 ? face.Maximum.y - 0.15f : face.Maximum.x - 0.15f;
					const float first = std::ceil((low - view.Origin[axis]) / view.Spacing - 0.5f);
					for (float k = first; view.Origin[axis] + (k + 0.5f) * view.Spacing <= high; k += 1.0f)
					{
						const float plane = view.Origin[axis] + (k + 0.5f) * view.Spacing;
						for (float other = otherLow; other <= otherHigh; other += 0.05f)
						{
							const auto at = [&](float offset)
							{
								return across == 0 ? view.Read(pointOn(plane + offset, other)) : view.Read(pointOn(other, plane + offset));
							};
							const glm::vec3 center = across == 0 ? pointOn(plane, other) : pointOn(other, plane);
							if (face.Axis == 1 && face.Value == 0.0f && GetCornellClearance(glm::vec2(center.x, center.z)) < 0.3f)
								continue;
							const std::optional<float> farBefore = at(-3.0f * c_Near);
							const std::optional<float> before = at(-c_Near);
							const std::optional<float> after = at(c_Near);
							const std::optional<float> farAfter = at(3.0f * c_Near);
							if (!farBefore || !before || !after || !farAfter)
								continue;
							crossings++;
							const float slope = 0.5f * (std::abs(*before - *farBefore) + std::abs(*farAfter - *after));
							const float step = (std::abs(*after - *before) - slope) / std::max(0.5f * (*before + *after), floorLevel);
							if (step > largest)
							{
								largest = step;
								where = across == 0 ? pointOn(plane, other) : pointOn(other, plane);
							}
						}
					}
				}
				MESSAGE(levelName << ", " << std::string(face.Name) << ": " << crossings << " crossings of probe planes, the largest step "
					<< largest * 100.0f << "% at (" << where.x << ", " << where.y << ", " << where.z << ")");
				CHECK(crossings > 100);
				if (level == GlobalIlluminationLevel::High)
					CHECK(largest <= c_MaxStep);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The Cornell box's floor darkens smoothly toward the sphere's and the box's feet")
	{
		// Around the sphere's and the tall box's feet (every bounce, settled), outside what they cover, the floor's GI may fall
		// toward them, but no floor point is darker than c_MinRatio of the brightest of its neighbours a voxel (0.1 m) away along x
		// and z: a gather whose probes each pass or fail a march through the voxels left a staircase of black cells there. The floor
		// the sphere overhangs counts where the camera sees it, down to c_ContactGap from its contact point (a gap of 0.23 m under
		// it, two voxels: closer in, no visibility point fits between the floor's and the sphere's voxels). Points the camera does
		// not see and neighbours under the objects are left out, and so are points whose brightest neighbour is darker than a
		// fiftieth of the floor's median. At High around the sphere's footprint and under its overhang (0.083 while the floor, 2.75 m
		// below the camera, lay beyond voxel level 0's weights: level 1's 0.2 m voxels closed the gap and the probe relocated there
		// read a fortieth of its open neighbour's light; the finest level's window now lies c_GIFinestLevelDrop below the camera).
		// The tall box's foot (the gap between its back-left corner and the red wall) still falls faster than c_MinRatio a voxel:
		// there the darkest point keeps at least c_MinBoxFootRatio of its brightest neighbour (0.10 before probe relocation and the
		// gather's guard visibility). That gap is in the box's full shadow from the panel, but the probes in front of it read the
		// panel through the cascades' merge (parents beside the box see past it), 17 times their neighbours in the shadow, which
		// the trilinear weights blend in linearly: c_MinRatio there is a goal not met yet (DeveloperGuide, "Global
		// illumination"). Low's 1 m probes are measured and reported: whole cells of them lie in the coarse voxels' solids beside
		// the box and the sphere.
		constexpr float c_MinRatio = 0.5f;
		constexpr float c_MinBoxFootRatio = 0.2f;
		constexpr float c_ContactGap = 0.6f;
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const glm::vec2 sphere(1.15f, -0.3f);
		constexpr float c_SphereRadius = 0.9f;
		const glm::vec2 box(-1.05f, -1.0f);
		constexpr float c_BoxHalf = 0.8f;
		const float boxAngle = glm::radians(18.0f);
		// The distance outside the tall box's footprint along its own axes (negative inside): (x, z) turned back by its 18 degrees.
		const auto outsideBox = [&](const glm::vec2& point)
		{
			const glm::vec2 d = point - box;
			const glm::vec2 local(std::cos(boxAngle) * d.x - std::sin(boxAngle) * d.y, std::sin(boxAngle) * d.x + std::cos(boxAngle) * d.y);
			return std::max(std::abs(local.x), std::abs(local.y)) - c_BoxHalf;
		};
		const auto covered = [&](const glm::vec2& point) { return glm::distance(point, sphere) < c_ContactGap || outsideBox(point) < 0.0f; };
		for (GlobalIlluminationLevel level : { GlobalIlluminationLevel::High, GlobalIlluminationLevel::Low })
		{
			const std::string levelName = level == GlobalIlluminationLevel::High ? "High" : "Low";
			CAPTURE(levelName);
			const CornellView view = RenderCornellView(level);
			std::vector<float> floorValues;
			for (int i = 0; i <= 52; i++)
			{
				for (int j = 0; j <= 52; j++)
				{
					if (const std::optional<float> value = view.Read(glm::vec3(-2.6f + 0.1f * static_cast<float>(i), 0.0f, -2.6f + 0.1f * static_cast<float>(j))))
						floorValues.push_back(*value);
				}
			}
			const float floorMedian = Median(floorValues);
			REQUIRE(floorMedian > 0.0f);
			// 0: the floor around the sphere's footprint, 1: the rim it overhangs, 2: around the tall box's footprint.
			for (uint32_t area = 0; area < 3; area++)
			{
				const std::string name = area == 0 ? "the sphere's foot" : area == 1 ? "the floor the sphere overhangs" : "the tall box's foot";
				INFO(name);
				float lowest = std::numeric_limits<float>::max();
				glm::vec2 lowestAt(0.0f);
				uint32_t measured = 0;
				for (int i = 0; i <= 208; i++)
				{
					for (int j = 0; j <= 208; j++)
					{
						const glm::vec2 point(-2.6f + 0.025f * static_cast<float>(i), -2.6f + 0.025f * static_cast<float>(j));
						const float fromSphere = glm::distance(point, sphere);
						const bool near = area == 0 ? fromSphere >= c_SphereRadius && fromSphere <= c_SphereRadius + 0.6f
							: area == 1 ? fromSphere < c_SphereRadius : fromSphere >= c_SphereRadius && outsideBox(point) <= 0.8f;
						if (!near || covered(point))
							continue;
						const std::optional<float> value = view.Read(glm::vec3(point.x, 0.0f, point.y));
						if (!value)
							continue;
						float brightest = 0.0f;
						for (const glm::vec2 offset : { glm::vec2(0.1f, 0.0f), glm::vec2(-0.1f, 0.0f), glm::vec2(0.0f, 0.1f), glm::vec2(0.0f, -0.1f) })
						{
							const glm::vec2 neighbour = point + offset;
							if (covered(neighbour) || std::abs(neighbour.x) > 2.7f || std::abs(neighbour.y) > 2.7f)
								continue;
							if (const std::optional<float> other = view.Read(glm::vec3(neighbour.x, 0.0f, neighbour.y)))
								brightest = std::max(brightest, *other);
						}
						if (brightest < 0.02f * floorMedian)
							continue;
						measured++;
						const float ratio = *value / brightest;
						if (ratio < lowest)
						{
							lowest = ratio;
							lowestAt = point;
						}
					}
				}
				MESSAGE(levelName << ", " << name << ": " << measured << " floor points, the darkest against its neighbours " << lowest
					<< " of the brightest, at (" << lowestAt.x << ", 0, " << lowestAt.y << "); the floor's median " << floorMedian);
				CHECK(measured > (area == 1 ? 50u : 200u));
				if (level == GlobalIlluminationLevel::High)
					CHECK(lowest >= (area == 2 ? c_MinBoxFootRatio : c_MinRatio));
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The Cornell box's far corners darken smoothly")
	{
		// Where the back wall meets the side walls and the ceiling, and the ceiling the side walls (every bounce, settled), the light
		// on either face may fall toward the junction, but no point within c_Reach of it is darker than c_MinRatio of the point a
		// voxel (0.1 m) farther from it: a gather whose visibility point lay in the perpendicular face's voxels drew a black line
		// a voxel wide along every concave corner, beaded at voxel spacing. Points the camera does not see are left out. At High;
		// Low is measured and reported.
		constexpr float c_MinRatio = 0.5f;
		constexpr float c_Reach = 0.2f;
		constexpr float c_Edge = 2.75f;
		constexpr float c_Height = 5.5f;
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		// A junction: the face a point lies on is where coordinate FaceAxis is FaceValue; it moves away from the junction along
		// AwayAxis in direction AwaySign from AwayStart, and along it along AlongAxis from AlongMinimum to AlongMaximum.
		struct Junction
		{
			const char* Name = "";
			int FaceAxis = 0;
			float FaceValue = 0.0f;
			int AwayAxis = 0;
			float AwayStart = 0.0f;
			float AwaySign = 1.0f;
			int AlongAxis = 0;
			float AlongMinimum = 0.0f;
			float AlongMaximum = 0.0f;
		};
		const std::vector<Junction> junctions = {
			{ "back wall by the red wall", 2, -c_Edge, 0, -c_Edge, 1.0f, 1, 0.3f, c_Height - 0.3f },
			{ "red wall by the back wall", 0, -c_Edge, 2, -c_Edge, 1.0f, 1, 0.3f, c_Height - 0.3f },
			{ "back wall by the green wall", 2, -c_Edge, 0, c_Edge, -1.0f, 1, 0.3f, c_Height - 0.3f },
			{ "green wall by the back wall", 0, c_Edge, 2, -c_Edge, 1.0f, 1, 0.3f, c_Height - 0.3f },
			{ "back wall by the ceiling", 2, -c_Edge, 1, c_Height, -1.0f, 0, -c_Edge + 0.3f, c_Edge - 0.3f },
			{ "ceiling by the back wall", 1, c_Height, 2, -c_Edge, 1.0f, 0, -c_Edge + 0.3f, c_Edge - 0.3f },
			{ "ceiling by the red wall", 1, c_Height, 0, -c_Edge, 1.0f, 2, -c_Edge + 0.3f, c_Edge - 0.3f },
			{ "red wall by the ceiling", 0, -c_Edge, 1, c_Height, -1.0f, 2, -c_Edge + 0.3f, c_Edge - 0.3f },
			{ "ceiling by the green wall", 1, c_Height, 0, c_Edge, -1.0f, 2, -c_Edge + 0.3f, c_Edge - 0.3f },
			{ "green wall by the ceiling", 0, c_Edge, 1, c_Height, -1.0f, 2, -c_Edge + 0.3f, c_Edge - 0.3f },
		};
		for (GlobalIlluminationLevel level : { GlobalIlluminationLevel::High, GlobalIlluminationLevel::Low })
		{
			const std::string levelName = level == GlobalIlluminationLevel::High ? "High" : "Low";
			CAPTURE(levelName);
			const CornellView view = RenderCornellView(level);
			for (const Junction& junction : junctions)
			{
				INFO(junction.Name);
				const auto pointAt = [&](float away, float along)
				{
					glm::vec3 point(0.0f);
					point[junction.FaceAxis] = junction.FaceValue;
					point[junction.AwayAxis] = junction.AwayStart + junction.AwaySign * away;
					point[junction.AlongAxis] = along;
					return point;
				};
				float lowest = std::numeric_limits<float>::max();
				glm::vec3 lowestAt(0.0f);
				uint32_t measured = 0;
				for (float along = junction.AlongMinimum; along <= junction.AlongMaximum; along += 0.025f)
				{
					for (float away = 0.01f; away <= c_Reach; away += 0.01f)
					{
						const std::optional<float> value = view.Read(pointAt(away, along));
						const std::optional<float> farther = view.Read(pointAt(away + 0.1f, along));
						if (!value || !farther || *farther <= 0.0f)
							continue;
						measured++;
						const float ratio = *value / *farther;
						if (ratio < lowest)
						{
							lowest = ratio;
							lowestAt = pointAt(away, along);
						}
					}
				}
				MESSAGE(levelName << ", " << std::string(junction.Name) << ": " << measured << " points, the darkest against the point 0.1 m "
					<< "farther from the corner " << lowest << " of it, at (" << lowestAt.x << ", " << lowestAt.y << ", " << lowestAt.z << ")");
				CHECK(measured > 200);
				if (level == GlobalIlluminationLevel::High)
					CHECK(lowest >= c_MinRatio);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A probe's depth moments hold its distances to the surfaces around it")
	{
		// Cascade 0's probes keep each direction's distance to the first hit (FieldReduce.comp); the gather reads them only to
		// skip its guard where a point lies before every ray around it hit anything (LiesBeforeProbeHits).
		// In the Cornell box, a probe a known height over the open floor finds the floor straight down within a voxel and a half
		// (the voxels' soft surface and the filter over neighbouring rays), its spread within two voxels, and nothing up to the
		// march's end straight up (the ceiling lies farther): the distances are the march's, in meters.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		for (GlobalIlluminationLevel level : { GlobalIlluminationLevel::High, GlobalIlluminationLevel::Low })
		{
			const std::string levelName = level == GlobalIlluminationLevel::High ? "High" : "Low";
			CAPTURE(levelName);
			SceneTestAssets assets;
			Scene scene;
			BuildCornellBox(scene, assets);
			Scope<SceneRenderer> renderer = CreateScope<SceneRenderer>(RoomSpecification(level));
			renderer->SetViewportSize(c_RoomSize.x, c_RoomSize.y);
			const SceneCamera camera = CornellCamera(static_cast<float>(c_RoomSize.x) / static_cast<float>(c_RoomSize.y));
			for (uint32_t frame = 0; frame < 3; frame++)
				REQUIRE(RenderGIFrame(*renderer, scene, camera, GIViewOptions()));
			REQUIRE_FALSE(renderer->GetStats().GlobalIlluminationPending);
			std::vector<uint8_t> depth;
			REQUIRE(renderer->ReadGlobalIlluminationFieldDepth(depth));
			const float spacing = FirstSpacing(level);
			const float voxel = renderer->GetGlobalIlluminationLayout()->Levels.front().VoxelSize;
			// Probes lie at (i + 0.5) spacings from a corner snapped to whole top-cascade spacings: half a spacing over the floor, and
			// this one toward the open front, beside the middle, far from every other surface.
			const glm::vec3 position(0.5f * spacing, 0.5f * spacing, 1.5f);
			const glm::ivec3 probe = FindProbe(*renderer, camera, 0, position);
			const glm::vec3 center = camera.Position + glm::vec3(renderer->GetGlobalIlluminationFieldConstants().FieldOrigin)
				+ (glm::vec3(probe) + 0.5f) * spacing;
			const float missDistance = 2.0f * spacing;
			const glm::vec2 down = ReadProbeDepth(*renderer, depth, probe, glm::vec3(0.0f, -1.0f, 0.0f));
			const glm::vec2 up = ReadProbeDepth(*renderer, depth, probe, glm::vec3(0.0f, 1.0f, 0.0f));
			MESSAGE(levelName << ": the probe at (" << center.x << ", " << center.y << ", " << center.z << ") finds the floor at " << down.x
				<< " m (spread " << std::sqrt(down.y) << "), up " << up.x << " m (spread " << std::sqrt(up.y) << ")");
			CHECK(std::abs(down.x - center.y) <= 1.5f * voxel);
			CHECK(std::sqrt(down.y) <= 2.0f * voxel);
			CHECK(up.x >= 0.95f * missDistance);
			renderer.reset();
			CollectReleases();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
