#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/RendererComponents.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// The sun's screen-space shadow mask (SceneRendererScreenShadows.cpp, Effects/ScreenShadows*.frag): opaque surfaces get the
// per-pixel filter where it is not blurred, the blur takes the disk's grain without crossing silhouettes, and nothing exists
// without a shadowed sun.

namespace
{

	constexpr uint32_t c_Size = 256;

	// A plate of 4 x 0.05 x 4 m 4 m above a floor, its edge along z at x = 0, under a sun straight down; an orthographic camera 1 m
	// above the floor looks straight down at the edge's shadow (1 m across).
	struct PlateScene
	{
		Scene SceneData;
		Entity Sun;
		Entity Floor;
		SceneCamera Camera = OrthographicLookAt(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f), 1.0f);

		explicit PlateScene(AssetHandle floorMaterial = UUID::Null())
		{
			AddNeutralPostProcess(SceneData);
			Floor = AddMesh(SceneData, BuiltinAssets::PlaneMesh, floorMaterial, glm::vec3(0.0f), "Floor");
			Floor.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
			Entity plate = AddMesh(SceneData, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(-2.0f, 4.0f, 0.0f), "Plate");
			plate.GetComponent<TransformComponent>().Scale = glm::vec3(4.0f, 0.05f, 4.0f);
			Sun = AddSun(SceneData, glm::vec3(0.0f, -1.0f, 0.0f), 3.0f);
			Sun.GetComponent<DirectionalLightComponent>().ShadowDistance = 20.0f;
		}

		void SetSoftness(float degrees)
		{
			Sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = degrees;
			Sun.MarkModified<DirectionalLightComponent>();
		}
	};

	SceneRendererSpecification MaskSpecification(GPUDrivenMode mode = GPUDrivenMode::Off, uint32_t taps = 16)
	{
		SceneRendererSpecification specification;
		specification.GPUDriven = mode;
		specification.TextureStreaming = false;
		specification.Quality.ShadowCascades = 1;
		specification.Quality.ShadowResolution = ShadowMapResolution::Size4096;
		specification.Quality.ShadowFilterQuality = taps;
		return specification;
	}

	bool DeviceDrawsGPUDriven()
	{
		return Renderer::GetGraphicsDevice().GetInfo().SupportsDrawIndirectFirstInstance;
	}

	// The mask's channels (RG8) of the renderer's latest frame.
	std::vector<glm::vec2> ReadMask(SceneRenderer& renderer)
	{
		ReadbackImage image;
		REQUIRE(renderer.GetScreenShadowTexture());
		REQUIRE(Renderer::ReadTexture(renderer.GetScreenShadowTexture(), image));
		REQUIRE(image.BytesPerPixel == 2);
		std::vector<glm::vec2> mask(static_cast<size_t>(image.Width) * image.Height);
		for (size_t pixel = 0; pixel < mask.size(); pixel++)
			mask[pixel] = glm::vec2(image.Pixels[pixel * 2], image.Pixels[pixel * 2 + 1]) / 255.0f;
		return mask;
	}

	// The audit's noise measure (shadows/tools/noise.py): the RMS of a profile's residual against its 9-pixel moving average,
	// relative to the profile's contrast.
	double RelativeNoise(const std::vector<double>& profile)
	{
		const auto [darkest, brightest] = std::minmax_element(profile.begin(), profile.end());
		const double contrast = *brightest - *darkest;
		double sum = 0.0;
		size_t count = 0;
		for (size_t x = 4; x + 4 < profile.size(); x++)
		{
			double mean = 0.0;
			for (size_t k = x - 4; k <= x + 4; k++)
				mean += profile[k];
			mean /= 9.0;
			sum += (profile[x] - mean) * (profile[x] - mean);
			count++;
		}
		return contrast > 0.0 && count > 0 ? std::sqrt(sum / static_cast<double>(count)) / contrast : 0.0;
	}

}

TEST_SUITE("GPU.SceneRenderer.ScreenShadows")
{
	TEST_CASE("Opaque surfaces get the per-pixel shadow where it is not blurred")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		// The floor opaque in A, and alpha-blended at alpha 1 in B: the transparent variant filters per pixel, without the mask.
		MaterialProperties blended;
		blended.AlphaMode = MaterialAlphaMode::Blend;
		const AssetHandle transparent = assets.AddMaterial(blended);
		PlateScene opaque;
		PlateScene glassy(transparent);

		for (const float softness : { 0.53f, 4.0f })
		{
			CAPTURE(softness);
			opaque.SetSoftness(softness);
			glassy.SetSoftness(softness);
			SceneRenderer reference(MaskSpecification());
			reference.SetViewportSize(c_Size, c_Size);
			const SunlightImage perPixel = RenderSunlight(reference, glassy.SceneData, glassy.Camera, glassy.Sun);
			for (GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
			{
				if (mode == GPUDrivenMode::Auto && !DeviceDrawsGPUDriven())
					continue;
				CAPTURE(static_cast<int>(mode));
				SceneRenderer renderer(MaskSpecification(mode));
				renderer.SetViewportSize(c_Size, c_Size);
				const SunlightImage masked = RenderSunlight(renderer, opaque.SceneData, opaque.Camera, opaque.Sun);
				CHECK(masked.Stats.ScreenShadows);
				// The mask of the shadowed frame: render it again (RenderSunlight's last frame had no shadows).
				REQUIRE(renderer.Render(opaque.SceneData, opaque.Camera));
				const std::vector<glm::vec2> mask = ReadMask(renderer);
				uint32_t sharp = 0;
				uint32_t blurred = 0;
				float sharpWorst = 0.0f;
				float blurredWorst = 0.0f;
				for (uint32_t y = 0; y < c_Size; y++)
				{
					for (uint32_t x = 0; x < c_Size; x++)
					{
						if (mask[static_cast<size_t>(y) * c_Size + x].y == 0.0f)
						{
							sharpWorst = std::max(sharpWorst, std::abs(masked.At(x, y) - perPixel.At(x, y)));
							sharp++;
						}
						else
						{
							// Where the mask is blurred, against the per-pixel filter's mean over 17 pixels along the edge (which runs
							// down the columns: the mean keeps its profile and takes the disk's grain, what the blur removes).
							float mean = 0.0f;
							uint32_t count = 0;
							for (uint32_t v = (y >= 8 ? y - 8 : 0); v <= std::min(y + 8, c_Size - 1); v++)
							{
								mean += perPixel.At(x, v);
								count++;
							}
							blurredWorst = std::max(blurredWorst, std::abs(masked.At(x, y) - mean / static_cast<float>(count)));
							blurred++;
						}
					}
				}
				MESSAGE("Sharp pixels ", sharp, " (worst ", sharpWorst, "), blurred ", blurred, " (worst ", blurredWorst, ")");
				CHECK(sharp > 0);
				CHECK(sharpWorst <= 0.01f);
				CHECK(blurredWorst <= 0.08f);
				if (softness > 1.0f)
					CHECK(blurred > 0);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The blur never crosses a silhouette")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		// A plate whose soft shadow (a 4 degree sun from +Z, 63 degrees high) has its edge on the floor at z = -1, and a pole 1 m
		// in front of that edge, lit on the face the camera sees, which casts nothing: seen from the front, the floor in the
		// blurred penumbra lies right beside the pole's silhouette.
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::PlaneMesh, UUID::Null(), glm::vec3(0.0f), "Floor").GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
		Entity plate = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 4.0f, -1.0f), "Plate");
		plate.GetComponent<TransformComponent>().Scale = glm::vec3(8.0f, 0.05f, 4.0f);
		Entity pole = AddMesh(scene, BuiltinAssets::CubeMesh, UUID::Null(), glm::vec3(0.0f, 0.75f, 0.0f), "Pole");
		pole.GetComponent<TransformComponent>().Scale = glm::vec3(0.2f, 1.5f, 0.2f);
		pole.GetComponent<MeshRendererComponent>().CastShadows = false;
		Entity sun = AddSun(scene, glm::normalize(glm::vec3(0.0f, -1.0f, -0.5f)), 3.0f);
		sun.GetComponent<DirectionalLightComponent>().ShadowSoftness = 4.0f;
		sun.GetComponent<DirectionalLightComponent>().ShadowDistance = 20.0f;
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 4.0f), glm::vec3(0.0f, 0.0f, -1.0f));

		SceneRenderer renderer(MaskSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		REQUIRE(renderer.Render(scene, camera));
		const std::vector<glm::vec2> withPole = ReadMask(renderer);
		ReadbackImage ids;
		REQUIRE(Renderer::ReadTexture(renderer.GetEntityIDTexture(), ids));
		pole.RemoveComponent<MeshRendererComponent>();
		REQUIRE(renderer.Render(scene, camera));
		const std::vector<glm::vec2> withoutPole = ReadMask(renderer);

		const uint32_t poleID = static_cast<uint32_t>(pole.GetHandle()) + 1;
		const auto idAt = [&](uint32_t x, uint32_t y)
		{
			uint32_t id = 0;
			std::memcpy(&id, ids.Pixels.data() + (static_cast<size_t>(y) * ids.Width + x) * 4, sizeof(id));
			return id;
		};
		uint32_t poleEdges = 0;
		uint32_t blurredFloorEdges = 0;
		for (uint32_t y = 1; y + 1 < c_Size; y++)
		{
			for (uint32_t x = 1; x + 1 < c_Size; x++)
			{
				const size_t index = static_cast<size_t>(y) * c_Size + x;
				const bool onPole = idAt(x, y) == poleID;
				const bool poleBeside = idAt(x - 1, y) == poleID || idAt(x + 1, y) == poleID;
				if (onPole && !(idAt(x - 1, y) == poleID && idAt(x + 1, y) == poleID))
				{
					// The pole's own, lit value next to the floor in shadow: whatever its blur took, it took from the pole.
					CAPTURE(x);
					CAPTURE(y);
					CHECK(withPole[index].x * withPole[index].x >= 0.98f);
					poleEdges++;
				}
				else if (!onPole && poleBeside)
				{
					// The floor beside the pole is what it is without the pole.
					CAPTURE(x);
					CAPTURE(y);
					CHECK(std::abs(withPole[index].x - withoutPole[index].x) <= 1.5f / 255.0f);
					blurredFloorEdges += withPole[index].y > 0.0f ? 1u : 0u;
				}
			}
		}
		CHECK(poleEdges > 10);
		CHECK(blurredFloorEdges > 10);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Wide penumbrae have no grain without anti-aliasing")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlateScene plate;
		plate.SetSoftness(4.0f);
		// High's 24 taps: the mask's blur against the transparent per-pixel version.
		SceneRenderer renderer(MaskSpecification(GPUDrivenMode::Off, 24));
		renderer.SetViewportSize(c_Size, c_Size);
		const SunlightImage sunlight = RenderSunlight(renderer, plate.SceneData, plate.Camera, plate.Sun);
		double worst = 0.0;
		for (uint32_t y = 16; y < c_Size; y += 32)
		{
			std::vector<double> row(c_Size);
			for (uint32_t x = 0; x < c_Size; x++)
				row[x] = sunlight.At(x, y);
			worst = std::max(worst, RelativeNoise(row));
		}
		MESSAGE("Worst row's RMS noise ", worst * 100.0, "% of the contrast");
		CHECK(worst <= 0.01);

		// The same filter per pixel, without the blur (the transparent variant's): what the blur takes away.
		MaterialProperties blended;
		blended.AlphaMode = MaterialAlphaMode::Blend;
		PlateScene glassy(assets.AddMaterial(blended));
		glassy.SetSoftness(4.0f);
		const SunlightImage perPixel = RenderSunlight(renderer, glassy.SceneData, glassy.Camera, glassy.Sun);
		double perPixelWorst = 0.0;
		for (uint32_t y = 16; y < c_Size; y += 32)
		{
			std::vector<double> row(c_Size);
			for (uint32_t x = 0; x < c_Size; x++)
				row[x] = perPixel.At(x, y);
			perPixelWorst = std::max(perPixelWorst, RelativeNoise(row));
		}
		MESSAGE("Per pixel, without the blur: ", perPixelWorst * 100.0, "%");
		CHECK(perPixelWorst > worst);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The mask is drawn only for a shadowed sun")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlateScene plate;
		plate.Sun.GetComponent<DirectionalLightComponent>().CastShadows = false;
		plate.Sun.MarkModified<DirectionalLightComponent>();
		SceneRenderer renderer(MaskSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		const ReadbackImage image = RenderToImage(renderer, plate.SceneData, plate.Camera);
		CHECK_FALSE(renderer.GetStats().ScreenShadows);
		CHECK(renderer.GetScreenShadowTexture() == nullptr);
		renderer.ResolveGPUStats(true);
		const SceneRendererGPUStats& gpuStats = renderer.GetGPUStats();
		const FrameStageID stage = RendererGPUStages::Get().ScreenShadows;
		for (const GPUStageTime& time : gpuStats.StageMs)
			CHECK(time.Stage != stage);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A quality change's new cascades reach the mask")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlateScene plate;
		plate.SetSoftness(4.0f);
		// The mask's evaluation names the cascades it was made with: after SetQuality released them, the next frame draws as a
		// renderer made at the new quality does.
		SceneRenderer changed(MaskSpecification());
		changed.SetViewportSize(c_Size, c_Size);
		RenderToImage(changed, plate.SceneData, plate.Camera);
		RendererQualitySettings quality = changed.GetQuality();
		quality.ShadowResolution = ShadowMapResolution::Size2048;
		changed.SetQuality(quality);
		const ReadbackImage afterChange = RenderToImage(changed, plate.SceneData, plate.Camera);
		SceneRendererSpecification specification = MaskSpecification();
		specification.Quality.ShadowResolution = ShadowMapResolution::Size2048;
		SceneRenderer fresh(specification);
		fresh.SetViewportSize(c_Size, c_Size);
		const ReadbackImage reference = RenderToImage(fresh, plate.SceneData, plate.Camera);
		CHECK(changed.GetShadowMapSize() == 2048);
		CHECK(afterChange.Pixels == reference.Pixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("HDROutput frames and one cascade use the mask")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		PlateScene plate;
		plate.SetSoftness(4.0f);
		SceneRenderer renderer(MaskSpecification());
		renderer.SetViewportSize(c_Size, c_Size);
		SceneRenderOptions options;
		options.PreExposure = 1.0f;
		REQUIRE(renderer.Render(plate.SceneData, plate.Camera, nullptr, options));
		ReadbackImage normal;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), normal));
		options.HDROutput = true;
		REQUIRE(renderer.Render(plate.SceneData, plate.Camera, nullptr, options));
		CHECK(renderer.GetStats().ScreenShadows);
		ReadbackImage hdr;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), hdr));
		CHECK(hdr.Pixels == normal.Pixels);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
