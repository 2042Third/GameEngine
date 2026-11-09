#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/RendererQuality.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
		// A shadow may change by a step of the fade per frame (SceneRenderer::c_LocalShadowFadeFrames), and the measurement by a
		// little more (the filter's edge moves under the pixel as the camera does).
		const float bound = 1.0f / static_cast<float>(SceneRenderer::c_LocalShadowFadeFrames) + 0.08f;
		// Walks the camera along a path, measuring each lamp's shadow every frame (1 full, 0 none) at its center against the lit
		// and the fully shadowed floor; returns how often a lamp in view went from no shadow to a full one or back.
		const auto walk = [&](LampField& field, uint32_t shadowed, uint32_t imageSize, uint32_t frames, const std::function<SceneCamera(uint32_t)>& path)
		{
			uint32_t crossings = 0;
			for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
			{
				CAPTURE(static_cast<int>(mode));
				SceneRendererSpecification walkSpecification = ManySpecification(mode, true);
				walkSpecification.Quality.MaxShadowedLocalLights = shadowed;
				SceneRenderer walker(walkSpecification);
				walker.SetViewportSize(imageSize, imageSize);
				SceneRendererSpecification fullSpecification = ManySpecification(mode, false);
				fullSpecification.Quality.MaxShadowedLocalLights = static_cast<uint32_t>(field.Lamps.size());
				SceneRenderer full(fullSpecification);
				full.SetViewportSize(imageSize, imageSize);
				SceneRenderer unshadowed(ManySpecification(mode, false));
				unshadowed.SetViewportSize(imageSize, imageSize);
				std::vector<std::vector<float>> strengths(field.Lamps.size());
				float largestStep = 0.0f;
				for (uint32_t frame = 0; frame < frames; frame++)
				{
					const SceneCamera camera = path(frame);
					const ReadbackImage walked = RenderHDR(walker, field.SceneData, camera);
					const ReadbackImage dark = RenderHDR(full, field.SceneData, camera);
					const ReadbackImage lit = field.RenderUnshadowed(unshadowed, camera);
					for (size_t lamp = 0; lamp < field.Lamps.size(); lamp++)
					{
						std::vector<float>& history = strengths[lamp];
						const std::optional<glm::uvec2> pixel = FindPixel(camera, field.ShadowCenters[lamp], imageSize);
						const float contrast = pixel ? ReadRed(lit, *pixel) - ReadRed(dark, *pixel) : 0.0f;
						if (!pixel || !(contrast > ReadRed(lit, *pixel) * 0.5f))
						{
							history.clear(); // Out of view, or too small to measure: a new run of frames starts
							continue;
						}
						const float strength = (ReadRed(lit, *pixel) - ReadRed(walked, *pixel)) / contrast;
						if (!history.empty())
						{
							CAPTURE(frame);
							CAPTURE(lamp);
							CAPTURE(history.back());
							CAPTURE(strength);
							const float step = std::abs(strength - history.back());
							largestStep = std::max(largestStep, step);
							CHECK(step <= bound);
							const float lowest = *std::min_element(history.begin(), history.end());
							const float highest = *std::max_element(history.begin(), history.end());
							if ((strength > 0.9f && lowest < 0.1f) || (strength < 0.1f && highest > 0.9f))
							{
								crossings++;
								history.clear();
							}
						}
						history.push_back(strength);
					}
				}
				MESSAGE("largest change of a shadow in a frame: ", largestStep);
				gpu.GetDevice().WaitForIdle();
			}
			return crossings;
		};

		SUBCASE("Seen from the side, two shadows go from lamp to lamp")
		{
			// A street of lamps 3.2 m apart, 2 m high: lamps enter and leave the view at its sides as much as they matter.
			std::vector<glm::vec3> positions;
			for (uint32_t lamp = 0; lamp < 16; lamp++)
				positions.emplace_back(static_cast<float>(lamp) * 3.2f, 2.0f, 0.0f);
			LampField field(positions, 2.8f, glm::vec3(-20.0f, -1.0f, -12.0f), glm::vec3(80.0f, 0.0f, 12.0f));
			const uint32_t crossings = walk(field, 2, 512, 120, [](uint32_t frame)
			{
				const glm::vec3 position(4.0f + static_cast<float>(frame) * 0.2f, 5.0f, 8.0f);
				return LookAt(position, position + glm::vec3(0.0f, -5.0f, -8.0f));
			});
			MESSAGE("crossings of the cut in view: ", crossings);
			CHECK(crossings >= 2);
		}

		SUBCASE("Walking down a street, six shadows change size as their lamps come near and pass")
		{
			// Two rows of lamps 5 m apart along the street, 6 m across it; the camera walks between them.
			std::vector<glm::vec3> positions;
			for (uint32_t lamp = 0; lamp < 24; lamp++)
				positions.emplace_back(static_cast<float>(lamp / 2) * 5.0f, 3.0f, lamp % 2 == 0 ? 3.0f : -3.0f);
			LampField field(positions, 4.5f, glm::vec3(-20.0f, -1.0f, -12.0f), glm::vec3(80.0f, 0.0f, 12.0f));
			walk(field, 6, 1024, 100, [](uint32_t frame)
			{
				const glm::vec3 position(-6.0f + static_cast<float>(frame) * 0.3f, 2.5f, 0.0f);
				return LookAt(position, position + glm::vec3(10.0f, -2.5f, 0.0f));
			});
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("After a walk that resizes tiles and moves casters, the cached views hold what drawing them again gives")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		constexpr uint32_t c_ImageSize = 1024;
		std::vector<glm::vec3> positions;
		for (uint32_t lamp = 0; lamp < 24; lamp++)
			positions.emplace_back(static_cast<float>(lamp / 2) * 5.0f, 3.0f, lamp % 2 == 0 ? 3.0f : -3.0f);
		LampField field(positions, 4.5f, glm::vec3(-20.0f, -1.0f, -12.0f), glm::vec3(80.0f, 0.0f, 12.0f));
		const auto cameraAt = [](uint32_t frame)
		{
			const glm::vec3 position(-6.0f + static_cast<float>(frame) * 0.3f, 2.5f, 0.0f);
			return LookAt(position, position + glm::vec3(10.0f, -2.5f, 0.0f));
		};
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
				RenderHDR(renderer, field.SceneData, cameraAt(frame));
			}
			const SceneCamera camera = cameraAt(60);
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
		const SceneCamera far = LookAt(glm::vec3(1.0f, 30.0f, 40.0f), field.ShadowCenters[0]);
		const SceneCamera near = LookAt(glm::vec3(1.0f, 4.0f, 4.0f), field.ShadowCenters[0]);
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
				RenderHDR(renderer, field.SceneData, far);
			REQUIRE(renderer.GetStats().LocalShadowViewsPending == 0);
			const uint16_t farSize = renderer.GetStats().LocalShadowTileSizes[0];
			const glm::uvec2 pixel = ProjectToPixel(near, field.ShadowCenters[0], c_ImageSize);
			const float lit = ReadRed(field.RenderUnshadowed(unshadowed, near), pixel);
			uint16_t nearSize = farSize;
			for (uint32_t frame = 0; frame < 12; frame++)
			{
				CAPTURE(frame);
				moveOthers(frame);
				const ReadbackImage image = RenderHDR(renderer, field.SceneData, near);
				CHECK(ReadRed(image, pixel) < lit * 0.5f);
				nearSize = std::max(nearSize, *std::max_element(renderer.GetStats().LocalShadowTileSizes.begin(), renderer.GetStats().LocalShadowTileSizes.end()));
			}
			CHECK(nearSize > farSize);
			gpu.GetDevice().WaitForIdle();
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
