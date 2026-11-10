#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/RendererQuality.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// Many shadowed point lights (SceneRendererLocalShadows.cpp): more than the 16 of 0.7.0 within the atlas, shadows that fade in
// and out instead of popping while the camera walks past the lights, a redraw budget that starves no view, and resized tiles
// that keep their shadows until the new ones are drawn.

namespace
{

	SceneRendererSpecification ManySpecification(GPUDrivenMode mode, bool spread)
	{
		SceneRendererSpecification specification;
		specification.GPUDriven = mode;
		specification.TextureStreaming = false;
		specification.SpreadLightingUpdates = spread;
		return specification;
	}

	float ReadRed(const ReadbackImage& image, const glm::uvec2& pixel)
	{
		REQUIRE(pixel.x < image.Width);
		REQUIRE(pixel.y < image.Height);
		uint16_t value = 0;
		std::memcpy(&value, image.Pixels.data() + (static_cast<size_t>(pixel.y) * image.Width + pixel.x) * image.BytesPerPixel, sizeof(value));
		return glm::unpackHalf1x16(value);
	}

	ReadbackImage RenderHDR(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera)
	{
		REQUIRE(renderer.Render(scene, camera));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), image));
		REQUIRE(image.Format == nvrhi::Format::RGBA16_FLOAT);
		return image;
	}

	Entity AddBox(Scene& scene, AssetHandle material, const glm::vec3& min, const glm::vec3& max, const std::string& name)
	{
		Entity box = AddMesh(scene, BuiltinAssets::CubeMesh, material, (min + max) * 0.5f, name);
		box.GetComponent<TransformComponent>().Scale = max - min;
		return box;
	}

	void Move(Entity entity, const glm::vec3& translation)
	{
		entity.GetComponent<TransformComponent>().Translation = translation;
		entity.MarkModified<TransformComponent>();
	}

	void SetCastShadows(Entity lamp, bool castShadows)
	{
		lamp.GetComponent<PointLightComponent>().CastShadows = castShadows;
		lamp.MarkModified<PointLightComponent>();
	}

	// Lamps over a matte floor, each with a small cube halfway down offset along X by a sixth of its height, whose shadow centers
	// on the floor twice as far along X. A lamp's range reaches no other lamp's shadow, so the light at a shadow's center is its
	// lamp's alone.
	struct LampField
	{
		SceneTestAssets Assets;
		Scene SceneData;
		std::vector<Entity> Lamps;
		std::vector<Entity> Casters;
		std::vector<glm::vec3> ShadowCenters;

		LampField(const std::vector<glm::vec3>& positions, float range, const glm::vec3& floorMin, const glm::vec3& floorMax)
		{
			AddNeutralPostProcess(SceneData);
			MaterialProperties matte;
			matte.BaseColor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
			matte.Roughness = 1.0f;
			const AssetHandle material = Assets.AddMaterial(matte);
			AddBox(SceneData, material, floorMin, floorMax, "Floor");
			for (const glm::vec3& position : positions)
			{
				Entity lamp = AddPointLight(SceneData, position, glm::vec3(1.0f), 30.0f, range);
				lamp.GetComponent<PointLightComponent>().CastShadows = true;
				Lamps.push_back(lamp);
				const float height = position.y;
				const glm::vec3 caster = position + glm::vec3(height / 6.0f, -height * 0.5f, 0.0f);
				Casters.push_back(AddBox(SceneData, material, caster - glm::vec3(0.25f), caster + glm::vec3(0.25f), "Caster"));
				ShadowCenters.push_back(position + glm::vec3(height / 3.0f, -height, 0.0f));
			}
		}

		// Every lamp's CastShadows off for one image (the lit reference), then on again.
		ReadbackImage RenderUnshadowed(SceneRenderer& renderer, const SceneCamera& camera)
		{
			for (Entity lamp : Lamps)
				SetCastShadows(lamp, false);
			ReadbackImage image = RenderHDR(renderer, SceneData, camera);
			for (Entity lamp : Lamps)
				SetCastShadows(lamp, true);
			return image;
		}
	};

	// A grid of `count` lamps 6 m apart.
	std::vector<glm::vec3> GridPositions(uint32_t count, uint32_t columns)
	{
		std::vector<glm::vec3> positions;
		for (uint32_t index = 0; index < count; index++)
			positions.emplace_back(static_cast<float>(index % columns) * 6.0f, 3.0f, static_cast<float>(index / columns) * 6.0f);
		return positions;
	}

	// A street: two rows of twelve lamps 5 m apart along X, 6 m across it.
	std::vector<glm::vec3> StreetPositions()
	{
		std::vector<glm::vec3> positions;
		for (uint32_t lamp = 0; lamp < 24; lamp++)
			positions.emplace_back(static_cast<float>(lamp / 2) * 5.0f, 3.0f, lamp % 2 == 0 ? 3.0f : -3.0f);
		return positions;
	}

	// The camera walking down the street between the rows, 0.3 m a frame.
	SceneCamera StreetWalk(uint32_t frame)
	{
		const glm::vec3 position(-6.0f + static_cast<float>(frame) * 0.3f, 2.5f, 0.0f);
		return LookAt(position, position + glm::vec3(10.0f, -2.5f, 0.0f));
	}

	// The pixel of a world point in an image of `size`, if the point lies in front of the camera and inside the image.
	std::optional<glm::uvec2> FindPixel(const SceneCamera& camera, const glm::vec3& point, uint32_t size)
	{
		const glm::vec4 clip = camera.Projection * camera.View * glm::vec4(point, 1.0f);
		if (!(clip.w > 0.0f))
			return std::nullopt;
		const glm::vec2 ndc = glm::vec2(clip) / clip.w;
		if (std::abs(ndc.x) > 0.98f || std::abs(ndc.y) > 0.98f)
			return std::nullopt;
		return ProjectToPixel(camera, point, size);
	}

	// One frame of a walk (SceneRendererStats): the shadowed lights in view, those given a shadow, those fading out the one they
	// lost, and how many of those given one have tiles of each size (entry i: 128 << i texels).
	struct WalkFrame
	{
		uint32_t Requested = 0;
		uint32_t Shadowed = 0;
		uint32_t Fading = 0;
		std::array<uint32_t, SceneRendererStats::c_LocalShadowSizeClasses> SizeCounts = {};
	};

	// What a walk past lamps saw of their shadows.
	struct WalkResult
	{
		uint32_t Gained = 0;      // Times a lamp in view went from no shadow to a full one
		uint32_t Lost = 0;        // And from a full shadow to none
		float LargestStep = 0.0f; // The largest change of a lamp's shadow from one frame to the next
		float WeakestKept = 1.0f; // The weakest a lamp's shadow got after it was full, while the lamp stayed in view
		std::vector<WalkFrame> Frames;
		std::vector<std::optional<float>> Last; // Each lamp's shadow in the last frame, where it could be measured
	};

	// Walks the camera along a path with a renderer of `quality` that spreads its lighting updates, measuring each lamp's shadow
	// every frame (1 full, 0 none) at its center against the lit and the fully shadowed floor. A shadow may change by a step of
	// the fade per frame (SceneRenderer::c_LocalShadowFadeFrames), and the measurement by a little more (the filter's edge moves
	// under the pixel as the camera does). Before the walk, the renderer draws the path's first view `settleFrames` times.
	WalkResult WalkPastLamps(GPUContext& gpu, LampField& field, GPUDrivenMode mode, const RendererQualitySettings& quality, uint32_t imageSize, uint32_t settleFrames,
		uint32_t frames, const std::function<SceneCamera(uint32_t)>& path)
	{
		const float bound = 1.0f / static_cast<float>(SceneRenderer::c_LocalShadowFadeFrames) + 0.08f;
		SceneRendererSpecification walkSpecification = ManySpecification(mode, true);
		walkSpecification.Quality = quality;
		SceneRenderer walker(walkSpecification);
		walker.SetViewportSize(imageSize, imageSize);
		SceneRendererSpecification fullSpecification = ManySpecification(mode, false);
		fullSpecification.Quality.MaxShadowedLocalLights = static_cast<uint32_t>(field.Lamps.size());
		SceneRenderer full(fullSpecification);
		full.SetViewportSize(imageSize, imageSize);
		SceneRenderer unshadowed(ManySpecification(mode, false));
		unshadowed.SetViewportSize(imageSize, imageSize);
		for (uint32_t frame = 0; frame < settleFrames; frame++)
			REQUIRE(walker.Render(field.SceneData, path(0)));

		WalkResult result;
		result.Last.resize(field.Lamps.size());
		std::vector<std::vector<float>> strengths(field.Lamps.size());
		std::vector<uint8_t> wasFull(field.Lamps.size(), 0);
		for (uint32_t frame = 0; frame < frames; frame++)
		{
			const SceneCamera camera = path(frame);
			const ReadbackImage walked = RenderHDR(walker, field.SceneData, camera);
			const SceneRendererStats& stats = walker.GetStats();
			result.Frames.push_back(WalkFrame { stats.LocalShadowRequested, stats.LocalShadowLights, stats.LocalShadowFading, stats.LocalShadowSizeCounts });
			const ReadbackImage dark = RenderHDR(full, field.SceneData, camera);
			const ReadbackImage lit = field.RenderUnshadowed(unshadowed, camera);
			for (size_t lamp = 0; lamp < field.Lamps.size(); lamp++)
			{
				std::vector<float>& history = strengths[lamp];
				result.Last[lamp].reset();
				const std::optional<glm::uvec2> pixel = FindPixel(camera, field.ShadowCenters[lamp], imageSize);
				const float contrast = pixel ? ReadRed(lit, *pixel) - ReadRed(dark, *pixel) : 0.0f;
				if (!pixel || !(contrast > ReadRed(lit, *pixel) * 0.5f))
				{
					history.clear(); // Out of view, or too small to measure: a new run of frames starts
					wasFull[lamp] = 0;
					continue;
				}
				const float strength = (ReadRed(lit, *pixel) - ReadRed(walked, *pixel)) / contrast;
				result.Last[lamp] = strength;
				if (strength > 0.9f)
					wasFull[lamp] = 1;
				if (wasFull[lamp] != 0)
					result.WeakestKept = std::min(result.WeakestKept, strength);
				if (!history.empty())
				{
					CAPTURE(frame);
					CAPTURE(lamp);
					CAPTURE(history.back());
					CAPTURE(strength);
					const float step = std::abs(strength - history.back());
					result.LargestStep = std::max(result.LargestStep, step);
					CHECK(step <= bound);
					const float lowest = *std::min_element(history.begin(), history.end());
					const float highest = *std::max_element(history.begin(), history.end());
					const bool gained = strength > 0.9f && lowest < 0.1f;
					const bool lost = strength < 0.1f && highest > 0.9f;
					result.Gained += gained ? 1u : 0u;
					result.Lost += lost ? 1u : 0u;
					if (gained || lost)
						history.clear();
				}
				history.push_back(strength);
			}
		}
		MESSAGE("largest change of a shadow in a frame: ", result.LargestStep, "; shadows gained in view: ", result.Gained, ", lost: ", result.Lost);
		gpu.GetDevice().WaitForIdle();
		return result;
	}

}

TEST_SUITE("GPU.SceneRenderer.ManyLocalShadows")
{
	TEST_CASE("Every one of 64 and of 200 shadowed point lights casts its shadow within the atlas, alike on both paths")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr uint32_t c_ImageSize = 1024;
		for (const uint32_t count : { 64u, 200u })
		{
			CAPTURE(count);
			const uint32_t columns = count == 64 ? 8 : 20;
			const uint32_t rows = (count + columns - 1) / columns;
			LampField field(GridPositions(count, columns), 4.5f, glm::vec3(-6.0f, -1.0f, -6.0f),
				glm::vec3(static_cast<float>(columns) * 6.0f, 0.0f, static_cast<float>(rows) * 6.0f));
			const glm::vec3 center(static_cast<float>(columns - 1) * 3.0f + 1.0f, 0.0f, static_cast<float>(rows - 1) * 3.0f);
			// Straight down without perspective: no caster hides a shadow's center from the camera.
			const SceneCamera camera = OrthographicLookAt(center + glm::vec3(0.0f, 50.0f, 0.01f), center, static_cast<float>(std::max(columns, rows)) * 6.0f + 6.0f);
			// High shadows 64 lights in a 4096 atlas, Ultra 256 in an 8192 one.
			RendererQualitySettings quality = RendererQuality::GetPreset(count == 64 ? RendererQualityPreset::High : RendererQualityPreset::Ultra);
			quality.MaxShadowedLocalLights = count;

			std::vector<ReadbackImage> images;
			for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
			{
				CAPTURE(static_cast<int>(mode));
				SceneRendererSpecification specification = ManySpecification(mode, false);
				specification.Quality = quality;
				SceneRenderer renderer(specification);
				renderer.SetViewportSize(c_ImageSize, c_ImageSize);
				// Every view in one frame: more than 255, so the GPU's culling batches start past the 8 bits 0.7.0 gave them.
				const ReadbackImage shadowed = RenderHDR(renderer, field.SceneData, camera);
				const SceneRendererStats& stats = renderer.GetStats();
				CHECK(stats.LocalShadowRequested == count);
				CHECK(stats.LocalShadowLights == count);
				CHECK(stats.LocalShadowFading == 0);
				CHECK(stats.LocalShadowViewsDrawn == count * 6);
				CHECK(stats.LocalShadowViewsPending == 0);
				uint32_t sized = 0;
				for (const uint32_t lights : stats.LocalShadowSizeCounts)
					sized += lights;
				CHECK(sized == count);
				const ReadbackImage lit = field.RenderUnshadowed(renderer, camera);
				const auto countCasting = [&](const ReadbackImage& image)
				{
					uint32_t casting = 0;
					for (size_t lamp = 0; lamp < field.Lamps.size(); lamp++)
					{
						const glm::uvec2 pixel = ProjectToPixel(camera, field.ShadowCenters[lamp], c_ImageSize);
						casting += ReadRed(image, pixel) < ReadRed(lit, pixel) * 0.5f ? 1u : 0u;
					}
					return casting;
				};
				for (size_t lamp = 0; lamp < field.Lamps.size(); lamp++)
				{
					CAPTURE(lamp);
					const glm::uvec2 pixel = ProjectToPixel(camera, field.ShadowCenters[lamp], c_ImageSize);
					CHECK(ReadRed(shadowed, pixel) < ReadRed(lit, pixel) * 0.5f);
				}
				images.push_back(RenderHDR(renderer, field.SceneData, camera));

				// Spread over frames (the quality's views per frame), every light's shadow is drawn and faded in within the frames
				// its views take.
				SceneRendererSpecification spreadSpecification = ManySpecification(mode, true);
				spreadSpecification.Quality = quality;
				SceneRenderer spreading(spreadSpecification);
				spreading.SetViewportSize(c_ImageSize, c_ImageSize);
				// Counted once the renderer's copy of the scene is uploaded: while new proxies wait, every view is drawn again.
				for (uint32_t frame = 0; frame < 30 && (frame == 0 || spreading.GetStats().DeferredProxies > 0); frame++)
					field.RenderUnshadowed(spreading, camera);
				REQUIRE(spreading.GetStats().DeferredProxies == 0);
				const uint32_t bound = (count * 6 + quality.LocalShadowViewsPerFrame - 1) / quality.LocalShadowViewsPerFrame + SceneRenderer::c_LocalShadowFadeFrames;
				uint32_t frames = 0;
				ReadbackImage settled;
				for (; frames < bound + 4; frames++)
				{
					settled = RenderHDR(spreading, field.SceneData, camera);
					CHECK(spreading.GetStats().LocalShadowViewsDrawn <= quality.LocalShadowViewsPerFrame);
					if (spreading.GetStats().LocalShadowViewsPending == 0 && countCasting(settled) == count)
						break;
				}
				MESSAGE(count, " lights settled in ", frames + 1, " frames (bound ", bound, ")");
				CHECK(frames + 1 <= bound);
				gpu.GetDevice().WaitForIdle();
			}
			// The paths draw the same shadows: the CPU decides which views both draw, each path what they hold.
			REQUIRE(images.size() == 2);
			float largest = 0.0f;
			for (uint32_t y = 0; y < c_ImageSize; y++)
			{
				for (uint32_t x = 0; x < c_ImageSize; x++)
					largest = std::max(largest, std::abs(ReadRed(images[0], glm::uvec2(x, y)) - ReadRed(images[1], glm::uvec2(x, y))));
			}
			CHECK(largest < 1e-3f);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Walking past more lamps than have shadows, every shadow fades in and out over frames instead of popping")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());

		SUBCASE("Seen from the side, two shadows go from lamp to lamp")
		{
			// A street of lamps 3.2 m apart, 2 m high: lamps enter and leave the view at its sides as much as they matter.
			std::vector<glm::vec3> positions;
			for (uint32_t lamp = 0; lamp < 16; lamp++)
				positions.emplace_back(static_cast<float>(lamp) * 3.2f, 2.0f, 0.0f);
			LampField field(positions, 2.8f, glm::vec3(-20.0f, -1.0f, -12.0f), glm::vec3(80.0f, 0.0f, 12.0f));
			RendererQualitySettings quality;
			quality.MaxShadowedLocalLights = 2;
			uint32_t crossings = 0;
			for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
			{
				CAPTURE(static_cast<int>(mode));
				const WalkResult result = WalkPastLamps(gpu, field, mode, quality, 512, 0, 120, [](uint32_t frame)
				{
					const glm::vec3 position(4.0f + static_cast<float>(frame) * 0.2f, 5.0f, 8.0f);
					return LookAt(position, position + glm::vec3(0.0f, -5.0f, -8.0f));
				});
				crossings += result.Gained + result.Lost;
			}
			MESSAGE("crossings of the cut in view: ", crossings);
			CHECK(crossings >= 2);
		}

		SUBCASE("Walking down a street, six shadows change size as their lamps come near and pass")
		{
			// Two rows of lamps 5 m apart along the street, 6 m across it; the camera walks between them.
			LampField field(StreetPositions(), 4.5f, glm::vec3(-20.0f, -1.0f, -12.0f), glm::vec3(80.0f, 0.0f, 12.0f));
			RendererQualitySettings quality;
			quality.MaxShadowedLocalLights = 6;
			for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
			{
				CAPTURE(static_cast<int>(mode));
				WalkPastLamps(gpu, field, mode, quality, 1024, 0, 100, StreetWalk);
			}
		}

		SUBCASE("An atlas that holds two lamps' tiles gives its shadows to the lamps that matter most")
		{
			// The street seen from the side again, every lamp allowed a shadow: the atlas is the limit. A 512 atlas holds 14 tiles
			// of 128 texels in the share its tiles are fitted into, two point lights' six each. The camera walks, then stands.
			constexpr float c_Range = 2.8f;
			constexpr uint32_t c_WalkFrames = 120;
			constexpr uint32_t c_Frames = c_WalkFrames + 5 * SceneRenderer::c_LocalShadowFadeFrames;
			std::vector<glm::vec3> positions;
			for (uint32_t lamp = 0; lamp < 16; lamp++)
				positions.emplace_back(static_cast<float>(lamp) * 3.2f, 2.0f, 0.0f);
			LampField field(positions, c_Range, glm::vec3(-20.0f, -1.0f, -12.0f), glm::vec3(80.0f, 0.0f, 12.0f));
			RendererQualitySettings quality;
			quality.LocalShadowAtlasSize = 512;
			quality.MaxShadowedLocalLights = 16;
			const auto path = [&](uint32_t frame)
			{
				const uint32_t walked = frame < c_WalkFrames ? frame : c_WalkFrames;
				const glm::vec3 position(4.0f + static_cast<float>(walked) * 0.2f, 5.0f, 8.0f);
				return LookAt(position, position + glm::vec3(0.0f, -5.0f, -8.0f));
			};
			for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
			{
				CAPTURE(static_cast<int>(mode));
				const WalkResult result = WalkPastLamps(gpu, field, mode, quality, 512, 0, c_Frames, path);
				// More lamps in view ask for a shadow than the atlas holds, and never more are given one.
				uint32_t mostRequested = 0;
				uint32_t mostShadowed = 0;
				for (const WalkFrame& frame : result.Frames)
				{
					mostRequested = std::max(mostRequested, frame.Requested);
					mostShadowed = std::max(mostShadowed, frame.Shadowed);
				}
				CHECK(mostRequested > 2);
				CHECK(mostShadowed == 2);
				// Lamps in view lost their shadows to lamps that came to matter more, fading (the step's bound, checked by the walk).
				CHECK(result.Lost >= 1);
				CHECK(result.Gained >= 1);
				// Where the camera stands at the end, two lamps cast full shadows and the others none; a lamp keeps its shadow until
				// another matters a quarter more (how large its range looks: r / (d - r), SceneRenderer::GetLocalShadowImportance).
				const glm::vec3 eye = path(c_Frames - 1).Position;
				const auto matters = [&](size_t lamp) { return c_Range / (glm::length(positions[lamp] - eye) - c_Range); };
				std::vector<size_t> shadowed;
				std::vector<size_t> bare;
				for (size_t lamp = 0; lamp < result.Last.size(); lamp++)
				{
					if (!result.Last[lamp])
						continue;
					CAPTURE(lamp);
					CAPTURE(*result.Last[lamp]);
					const bool casts = *result.Last[lamp] > 0.9f;
					CHECK((casts || *result.Last[lamp] < 0.1f));
					(casts ? shadowed : bare).push_back(lamp);
				}
				CHECK(shadowed.size() == 2);
				CHECK(bare.size() >= 2);
				for (const size_t kept : shadowed)
				{
					for (const size_t other : bare)
					{
						CAPTURE(kept);
						CAPTURE(other);
						CHECK(matters(other) <= matters(kept) * 1.25f * 1.001f);
					}
				}
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Walking down a street whose lamps want larger tiles than the atlas holds, no lamp loses the shadow it shows")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr uint32_t c_ImageSize = 1024;
		constexpr uint64_t c_AtlasSize = 2048;
		constexpr uint32_t c_ViewsPerFrame = 4;
		// The street of 24 lamps. A 2048 atlas holds every lamp's six tiles at 128 texels and a few lamps' at 256 in the share its
		// tiles are fitted into, while the lamps near the camera want 512: every lamp in view has a shadow, the tiles fill their
		// share, and their sizes are fitted again as lamps come near and pass. Fewer views are drawn a frame than a lamp has, so
		// a lamp whose tiles change size waits frames for its new ones.
		LampField field(StreetPositions(), 4.5f, glm::vec3(-20.0f, -1.0f, -12.0f), glm::vec3(80.0f, 0.0f, 12.0f));
		const uint32_t lamps = static_cast<uint32_t>(field.Lamps.size());
		RendererQualitySettings quality;
		quality.LocalShadowAtlasSize = static_cast<uint32_t>(c_AtlasSize);
		quality.LocalShadowMaxTile = 512;
		quality.MaxShadowedLocalLights = lamps;
		quality.LocalShadowViewsPerFrame = c_ViewsPerFrame;
		// Standing at the start until every view is drawn and every shadow has faded in.
		const uint32_t settleFrames = lamps * 6 / c_ViewsPerFrame + 2 * SceneRenderer::c_LocalShadowFadeFrames;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			const WalkResult result = WalkPastLamps(gpu, field, mode, quality, c_ImageSize, settleFrames, 100, StreetWalk);
			uint64_t mostTexels = 0;
			uint32_t grown = 0;
			uint32_t shrunk = 0;
			uint32_t largeBefore = 0;
			uint32_t shadowedBefore = 0;
			for (size_t frame = 0; frame < result.Frames.size(); frame++)
			{
				CAPTURE(frame);
				const WalkFrame& stats = result.Frames[frame];
				// Every lamp in view keeps its rank: none fades.
				CHECK(stats.Shadowed == stats.Requested);
				CHECK(stats.Fading == 0);
				uint64_t texels = 0;
				uint32_t large = 0;
				for (size_t sizeClass = 0; sizeClass < stats.SizeCounts.size(); sizeClass++)
				{
					const uint64_t size = 128ull << sizeClass;
					texels += stats.SizeCounts[sizeClass] * 6 * size * size;
					large += sizeClass > 0 ? stats.SizeCounts[sizeClass] : 0u;
				}
				mostTexels = std::max(mostTexels, texels);
				// No lamp enters the view on this walk, so more lamps with tiles above the smallest are lamps whose tiles grew, and
				// fewer among as many lamps are lamps whose tiles shrank.
				if (frame > 0)
				{
					grown += large > largeBefore ? 1u : 0u;
					shrunk += large < largeBefore && stats.Shadowed == shadowedBefore ? 1u : 0u;
				}
				largeBefore = large;
				shadowedBefore = stats.Shadowed;
			}
			MESSAGE("most texels in tiles: ", mostTexels, " of ", c_AtlasSize * c_AtlasSize, "; frames in which tiles grew: ", grown, ", shrank: ", shrunk,
				"; weakest shadow kept: ", result.WeakestKept);
			// The tiles filled what an atlas of tiles of two sizes can of its seven eighths: within a lamp's step from 128 to 256.
			CHECK(mostTexels + 6 * (256 * 256 - 128 * 128) > c_AtlasSize * c_AtlasSize / 8 * 7);
			CHECK(grown >= 2);
			CHECK(result.Lost == 0);
			CHECK(result.WeakestKept > 0.9f);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("After a walk that resizes tiles and moves casters, the cached views hold what drawing them again gives")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr uint32_t c_ImageSize = 1024;
		const std::vector<glm::vec3> positions = StreetPositions();
		LampField field(positions, 4.5f, glm::vec3(-20.0f, -1.0f, -12.0f), glm::vec3(80.0f, 0.0f, 12.0f));
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			for (size_t lamp = 0; lamp < field.Casters.size(); lamp++)
				Move(field.Casters[lamp], positions[lamp] + glm::vec3(0.5f, -1.5f, 0.0f));
			SceneRendererSpecification specification = ManySpecification(mode, true);
			specification.Quality.MaxShadowedLocalLights = 6;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_ImageSize, c_ImageSize);
			// The walk: lamps come near (their tiles grow) and pass; the casters of the lamps ahead sway every frame.
			for (uint32_t frame = 0; frame < 60; frame++)
			{
				for (size_t lamp = 0; lamp < field.Casters.size(); lamp++)
				{
					const float sway = 0.1f * std::sin(static_cast<float>(frame + lamp) * 0.7f);
					Move(field.Casters[lamp], positions[lamp] + glm::vec3(0.5f, -1.5f, sway));
				}
				RenderHDR(renderer, field.SceneData, StreetWalk(frame));
			}
			const SceneCamera camera = StreetWalk(60);
			const auto settle = [&]()
			{
				ReadbackImage image;
				uint32_t settled = 0;
				for (uint32_t frame = 0; frame < 120 && settled < SceneRenderer::c_LocalShadowFadeFrames + 1; frame++)
				{
					image = RenderHDR(renderer, field.SceneData, camera);
					const SceneRendererStats& stats = renderer.GetStats();
					settled = stats.LocalShadowViewsPending == 0 && stats.LocalShadowFading == 0 ? settled + 1 : 0;
				}
				CHECK(settled == SceneRenderer::c_LocalShadowFadeFrames + 1);
				return image;
			};
			const ReadbackImage cached = settle();
			CHECK(renderer.GetStats().LocalShadowLights == 6);
			// A new level-of-detail error draws every view again in its tile (LocalShadowContent); back at the old one, again.
			RendererQualitySettings quality = renderer.GetQuality();
			const float error = quality.LODErrorPixels;
			quality.LODErrorPixels = error + 1.0f;
			renderer.SetQuality(quality);
			settle();
			quality.LODErrorPixels = error;
			renderer.SetQuality(quality);
			const ReadbackImage redrawn = settle();
			REQUIRE(cached.Pixels.size() == redrawn.Pixels.size());
			CHECK(std::memcmp(cached.Pixels.data(), redrawn.Pixels.data(), cached.Pixels.size()) == 0);
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A far light's changed view is drawn within a few frames while nearer lights move every frame")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr uint32_t c_ImageSize = 256;
		// Three near lamps that move every frame ask for 18 views a frame, more than the 12 of the budget; a far lamp's caster
		// moves once.
		LampField field({ glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(6.0f, 3.0f, 0.0f), glm::vec3(12.0f, 3.0f, 0.0f), glm::vec3(30.0f, 3.0f, 0.0f) }, 4.5f,
			glm::vec3(-10.0f, -1.0f, -10.0f), glm::vec3(40.0f, 0.0f, 10.0f));
		const SceneCamera camera = LookAt(glm::vec3(-6.0f, 8.0f, 10.0f), glm::vec3(18.0f, 0.0f, 0.0f));
		const glm::vec3 before = field.ShadowCenters[3];
		// The far caster moves along -Z by 0.75 m: its shadow twice as far (it hangs halfway down).
		const glm::vec3 after = before + glm::vec3(0.0f, 0.0f, -1.5f);
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			for (uint32_t lamp = 0; lamp < 3; lamp++)
				Move(field.Lamps[lamp], glm::vec3(static_cast<float>(lamp) * 6.0f, 3.0f, 0.0f));
			Move(field.Casters[3], glm::vec3(30.5f, 1.5f, 0.0f));
			SceneRenderer renderer(ManySpecification(mode, true));
			renderer.SetViewportSize(c_ImageSize, c_ImageSize);
			ReadbackImage settled;
			for (uint32_t frame = 0; frame < 30; frame++)
				settled = RenderHDR(renderer, field.SceneData, camera);
			REQUIRE(renderer.GetStats().LocalShadowViewsPending == 0);
			const float dark = ReadRed(settled, ProjectToPixel(camera, before, c_ImageSize));
			const float lit = ReadRed(settled, ProjectToPixel(camera, after, c_ImageSize));
			REQUIRE(dark < lit * 0.5f);

			Move(field.Casters[3], glm::vec3(30.5f, 1.5f, -0.75f));
			uint32_t frames = 0;
			for (; frames < 40; frames++)
			{
				for (uint32_t lamp = 0; lamp < 3; lamp++)
				{
					const float offset = 0.02f * static_cast<float>((frames + lamp) % 5);
					Move(field.Lamps[lamp], glm::vec3(static_cast<float>(lamp) * 6.0f + offset, 3.0f, 0.0f));
				}
				const ReadbackImage image = RenderHDR(renderer, field.SceneData, camera);
				CHECK(renderer.GetStats().LocalShadowViewsDrawn > 0);
				if (ReadRed(image, ProjectToPixel(camera, after, c_ImageSize)) < (dark + lit) * 0.5f)
					break;
			}
			MESSAGE("frames until the far view was drawn: ", frames + 1);
			CHECK(frames < 8);
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A light whose tiles change size keeps its shadow until the new tiles are drawn")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr uint32_t c_ImageSize = 1024;
		// The lamp at the origin, seen from afar (small tiles), then from within its range (large ones). Two small lamps nearer
		// the camera, whose light reaches no surface, move every frame and fill the budget with their twelve views.
		LampField field({ glm::vec3(0.0f, 3.0f, 0.0f) }, 4.5f, glm::vec3(-20.0f, -1.0f, -20.0f), glm::vec3(20.0f, 0.0f, 20.0f));
		std::array<Entity, 2> movers = {};
		const std::array<glm::vec3, 2> moverPositions = { glm::vec3(-1.5f, 2.5f, 2.0f), glm::vec3(3.5f, 2.5f, 2.0f) };
		for (size_t index = 0; index < movers.size(); index++)
		{
			movers[index] = AddPointLight(field.SceneData, moverPositions[index], glm::vec3(1.0f), 30.0f, 1.2f);
			movers[index].GetComponent<PointLightComponent>().CastShadows = true;
		}
		const SceneCamera farCamera = LookAt(glm::vec3(1.0f, 30.0f, 40.0f), field.ShadowCenters[0]);
		const SceneCamera nearCamera = LookAt(glm::vec3(1.0f, 4.0f, 4.0f), field.ShadowCenters[0]);
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			SceneRenderer renderer(ManySpecification(mode, true));
			renderer.SetViewportSize(c_ImageSize, c_ImageSize);
			SceneRenderer unshadowed(ManySpecification(mode, false));
			unshadowed.SetViewportSize(c_ImageSize, c_ImageSize);
			const auto moveOthers = [&](uint32_t frame)
			{
				const float offset = 0.02f * static_cast<float>(frame % 5);
				for (size_t index = 0; index < movers.size(); index++)
					Move(movers[index], moverPositions[index] + glm::vec3(offset, 0.0f, 0.0f));
			};
			for (uint32_t frame = 0; frame < 20; frame++)
				RenderHDR(renderer, field.SceneData, farCamera);
			REQUIRE(renderer.GetStats().LocalShadowViewsPending == 0);
			const uint16_t farSize = renderer.GetStats().LocalShadowTileSizes[0];
			const glm::uvec2 pixel = ProjectToPixel(nearCamera, field.ShadowCenters[0], c_ImageSize);
			const float lit = ReadRed(field.RenderUnshadowed(unshadowed, nearCamera), pixel);
			uint16_t nearSize = farSize;
			for (uint32_t frame = 0; frame < 12; frame++)
			{
				CAPTURE(frame);
				moveOthers(frame);
				const ReadbackImage image = RenderHDR(renderer, field.SceneData, nearCamera);
				CHECK(ReadRed(image, pixel) < lit * 0.5f);
				nearSize = std::max(nearSize, *std::max_element(renderer.GetStats().LocalShadowTileSizes.begin(), renderer.GetStats().LocalShadowTileSizes.end()));
			}
			CHECK(nearSize > farSize);
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
