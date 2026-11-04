#include "Renderer/GIScenes.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/GlobalIlluminationLayout.h"

#include <glm/gtc/constants.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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
		// These cases measure the field on the surfaces drawn. GPU-driven occlusion culling hides the room's box below the window
		// from cameras within about 0.3 m of x = -10 (Docs/Roadmap.md's open items), and the views then show the sunlit ground
		// behind it: the field would be read on the wrong surface.
		specification.OcclusionCulling = false;
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
		// made the bound a thousandth of a nit.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SunRoomOptions room = DefaultRoom();
		room.PairedRoom = true;
		const glm::vec3 toB(6.2f, 0.0f, 0.0f);
		RoomRender wallsA(room, c_RoomSize, RoomSpecification());
		RoomRender floorA(room, c_FloorSize, RoomSpecification());
		const float meanA = 0.5f * (MeanLuminance(wallsA.Measure(RoomCamera(), GetWallPoints())) + MeanLuminance(floorA.Measure(FloorCamera(), GetFloorPoints())));
		std::vector<RoomPoint> wallPointsB = GetWallPoints(toB);
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
