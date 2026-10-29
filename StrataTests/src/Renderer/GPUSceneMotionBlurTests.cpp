#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/PostProcessMath.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/RendererQuality.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// Motion blur: the tile maxima, their neighborhoods and the gather (PostProcess/MotionBlur*.comp), judged against the gather's
// CPU model (PostProcessMath::GetMotionBlurProfile).
namespace
{

	constexpr uint32_t c_Width = 1024;
	constexpr uint32_t c_Height = 576;
	constexpr float c_WallDistance = 2.0f;

	// Pixels per world unit on a wall c_WallDistance away, with LookAt's 60 degree view.
	float PixelsPerUnit()
	{
		return static_cast<float>(c_Height) / (2.0f * std::tan(glm::radians(30.0f))) / c_WallDistance;
	}

	// A wall black left of x = 0 and white (1, not above: the blur averages light) right of it, on one surface, seen square-on by a camera at x; manual exposure, no
	// tone curve, bloom, vignette or anti-aliasing.
	struct EdgeWall
	{
		Scene SceneData;
		PostProcessComponent* PostProcess = nullptr;

		explicit EdgeWall(SceneTestAssets& assets)
		{
			PostProcess = &AddNeutralPostProcess(SceneData);
			PostProcess->MotionBlur = true;
			PostProcess->MotionBlurShutterAngle = 180.0f;
			PostProcess->MotionBlurMaxLength = 0.05f;
			AddMesh(SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(0.0f)), glm::vec3(-10.0f, 0.0f, 0.0f), "Black")
				.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f, 20.0f, 1.0f);
			AddMesh(SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(1.0f)), glm::vec3(10.0f, 0.0f, 0.0f), "White")
				.GetComponent<TransformComponent>().Scale = glm::vec3(20.0f, 20.0f, 1.0f);
		}

		static SceneCamera CameraAt(float x)
		{
			return LookAt(glm::vec3(x, 0.0f, c_WallDistance), glm::vec3(x, 0.0f, 0.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height));
		}
	};

	// Renders the camera at `first`, then at `second` (the measured frame), each frame waited for.
	ReadbackImage RenderMove(SceneRenderer& renderer, Scene& scene, float first, float second, const SceneRenderOptions& options = {})
	{
		REQUIRE(renderer.Render(scene, EdgeWall::CameraAt(first), nullptr, options));
		REQUIRE(renderer.Render(scene, EdgeWall::CameraAt(second), nullptr, options));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
		return image;
	}

	// The red channel's linear value per row and column of the band of rows [top, bottom).
	std::vector<std::vector<float>> LinearRows(const ReadbackImage& image, uint32_t top, uint32_t bottom)
	{
		std::vector<std::vector<float>> rows;
		for (uint32_t y = top; y < bottom; y++)
		{
			std::vector<float> row(image.Width);
			for (uint32_t x = 0; x < image.Width; x++)
				row[x] = PostProcessMath::SRGBToLinear(static_cast<float>(Red(image, x, y)) / 255.0f);
			rows.push_back(std::move(row));
		}
		return rows;
	}

	std::vector<float> AverageRows(const std::vector<std::vector<float>>& rows)
	{
		std::vector<float> mean(rows.front().size(), 0.0f);
		for (const std::vector<float>& row : rows)
		{
			for (size_t x = 0; x < row.size(); x++)
				mean[x] += row[x] / static_cast<float>(rows.size());
		}
		return mean;
	}

	// Columns neither black nor white: the blur's 0-100% width.
	uint32_t BlurWidth(const std::vector<float>& profile)
	{
		uint32_t width = 0;
		for (float value : profile)
			width += value > 1e-3f && value < 1.0f - 1e-3f ? 1u : 0u;
		return width;
	}

	// The first white column of an unblurred image's middle row.
	uint32_t EdgeColumn(const ReadbackImage& image)
	{
		for (uint32_t x = 0; x < image.Width; x++)
		{
			if (Red(image, x, image.Height / 2) == 255)
				return x;
		}
		FAIL("no edge");
		return 0;
	}

	SceneRendererSpecification MotionSpecification(MotionBlurSamples samples = MotionBlurSamples::Samples12)
	{
		SceneRendererSpecification specification = ExactSpecification();
		specification.Quality.MotionBlurQuality = samples;
		return specification;
	}

}

TEST_SUITE("GPU.SceneRenderer.MotionBlur")
{
	TEST_CASE("A still camera over a still scene renders bit-identically with motion blur on and off")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		SceneRenderer blurred(MotionSpecification());
		SceneRenderer plain(MotionSpecification());
		blurred.SetViewportSize(c_Width, c_Height);
		plain.SetViewportSize(c_Width, c_Height);
		SceneRenderOptions off;
		off.MotionBlur = false;
		for (int frame = 1; frame <= 5; frame++)
		{
			REQUIRE(blurred.Render(wall.SceneData, EdgeWall::CameraAt(0.0f)));
			REQUIRE(plain.Render(wall.SceneData, EdgeWall::CameraAt(0.0f), nullptr, off));
			ReadbackImage a;
			ReadbackImage b;
			REQUIRE(Renderer::ReadTexture(blurred.GetOutputTexture(), a));
			REQUIRE(Renderer::ReadTexture(plain.GetOutputTexture(), b));
			CAPTURE(frame);
			CHECK(blurred.GetStats().MotionBlurred == (frame > 1)); // The first frame has no history
			CHECK(MaxDifference(a, b) == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A shutter of 0 and motion below the threshold render bit-identically to motion blur off")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		SceneRenderer renderer(MotionSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		SceneRenderOptions off;
		off.MotionBlur = false;
		const float step = 24.0f / PixelsPerUnit();
		const ReadbackImage reference = RenderMove(renderer, wall.SceneData, 0.0f, step, off);
		wall.PostProcess->MotionBlurShutterAngle = 0.0f;
		CHECK(MaxDifference(RenderMove(renderer, wall.SceneData, 0.0f, step), reference) == 0);
		CHECK_FALSE(renderer.GetStats().MotionBlurred);

		// One pixel a frame at 180 degrees blurs half a pixel: under the threshold (576 / 1080 pixels), so nothing changes.
		wall.PostProcess->MotionBlurShutterAngle = 180.0f;
		const float slow = 1.0f / PixelsPerUnit();
		const ReadbackImage still = RenderMove(renderer, wall.SceneData, 0.0f, slow, off);
		CHECK(MaxDifference(RenderMove(renderer, wall.SceneData, 0.0f, slow), still) == 0);
		CHECK(renderer.GetStats().MotionBlurred);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A moving camera blurs an edge over the shutter's share of its motion")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		SceneRenderer renderer(MotionSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		SceneRenderOptions off;
		off.MotionBlur = false;
		const float step = 24.0f / PixelsPerUnit();
		const uint32_t edge = EdgeColumn(RenderMove(renderer, wall.SceneData, 0.0f, step, off));
		for (const float shutter : { 180.0f, 90.0f })
		{
			wall.PostProcess->MotionBlurShutterAngle = shutter;
			const std::vector<float> profile = AverageRows(LinearRows(RenderMove(renderer, wall.SceneData, 0.0f, step), c_Height / 4, 3 * c_Height / 4));
			const float length = 24.0f * shutter / 360.0f;
			CAPTURE(shutter);
			CHECK(renderer.GetStats().MotionBlurred);
			CHECK(std::abs(static_cast<float>(BlurWidth(profile)) - length) <= 1.0f);
			// The row-averaged profile against the model, in linear light (the blur averages light): within 2 of 255.
			const PostProcessMath::MotionBlurProfile model = PostProcessMath::GetMotionBlurProfile(length, 12);
			for (size_t index = 0; index < model.Mean.size(); index++)
			{
				const int column = static_cast<int>(edge) + model.FirstPixel + static_cast<int>(index);
				CAPTURE(column);
				CHECK(std::abs(profile[static_cast<size_t>(column)] - model.Mean[index]) * 255.0f <= 2.0f);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The gather's noise stays within the stratified estimate's")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		const float step = 24.0f / PixelsPerUnit();
		for (const MotionBlurSamples samples : { MotionBlurSamples::Samples8, MotionBlurSamples::Samples12, MotionBlurSamples::Samples16 })
		{
			SceneRenderer renderer(MotionSpecification(samples));
			renderer.SetViewportSize(c_Width, c_Height);
			const std::vector<std::vector<float>> rows = LinearRows(RenderMove(renderer, wall.SceneData, 0.0f, step), c_Height / 4, 3 * c_Height / 4);
			const std::vector<float> mean = AverageRows(rows);
			const uint32_t count = RendererQuality::GetMotionBlurSampleCount(samples);
			CHECK(renderer.GetStats().MotionBlurSamples == count);
			const PostProcessMath::MotionBlurProfile model = PostProcessMath::GetMotionBlurProfile(12.0f, count);
			float modelSpread = 0.0f;
			for (float spread : model.Spread)
				modelSpread = std::max(modelSpread, spread);
			float largest = 0.0f;
			double squares = 0.0;
			size_t samplesCounted = 0;
			for (const std::vector<float>& row : rows)
			{
				for (size_t x = 0; x < row.size(); x++)
				{
					if (mean[x] <= 1e-3f || mean[x] >= 1.0f - 1e-3f)
						continue;
					largest = std::max(largest, std::abs(row[x] - mean[x]));
					squares += (row[x] - mean[x]) * (row[x] - mean[x]);
					samplesCounted++;
				}
			}
			REQUIRE(samplesCounted > 0);
			CAPTURE(count);
			CHECK(largest <= 1.0f / static_cast<float>(count) + 2.0f / 255.0f);
			CHECK(std::sqrt(squares / static_cast<double>(samplesCounted)) <= 1.5 * modelSpread);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The blur is clamped")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		wall.PostProcess->MotionBlurMaxLength = 0.02f; // 11.5 pixels at 576 rows
		SceneRenderer renderer(MotionSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const std::vector<float> profile = AverageRows(LinearRows(RenderMove(renderer, wall.SceneData, 0.0f, 400.0f / PixelsPerUnit()), c_Height / 4,
			3 * c_Height / 4));
		CHECK(renderer.GetStats().MotionBlurred);
		CHECK(BlurWidth(profile) <= static_cast<uint32_t>(0.02f * c_Height + 1.0f));
		CHECK(BlurWidth(profile) > 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A foreground moving with the camera stays sharp over a blurred background")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		// A gray card a meter in front of the camera, moving with it.
		Entity card = AddMesh(wall.SceneData, BuiltinAssets::QuadMesh, assets.AddMaterial(Emitting(0.2f)), glm::vec3(0.0f, -0.3f, c_WallDistance - 1.0f), "Card");
		card.GetComponent<TransformComponent>().Scale = glm::vec3(0.3f);
		SceneRenderer renderer(MotionSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		const float step = 24.0f / PixelsPerUnit();
		auto renderAt = [&](float x, bool blur)
		{
			card.GetComponent<TransformComponent>().Translation.x = x;
			card.MarkModified<TransformComponent>();
			SceneRenderOptions options;
			options.MotionBlur = blur;
			REQUIRE(renderer.Render(wall.SceneData, EdgeWall::CameraAt(x), nullptr, options));
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
			return image;
		};
		renderAt(0.0f, false);
		const ReadbackImage sharp = renderAt(step, false);
		renderAt(0.0f, true);
		const ReadbackImage blurred = renderAt(step, true);
		CHECK(renderer.GetStats().MotionBlurred);
		// The card's row: its interior as without blur (within a code: the gather's weighted mean of samples of one color can
		// round), its edges within a pixel; the wall's edge above it blurred.
		const glm::uvec2 center = ProjectToPixel(EdgeWall::CameraAt(step), glm::vec3(step, -0.3f, c_WallDistance - 1.0f), c_Width, c_Height);
		const int cardCode = Red(sharp, center);
		int largest = 0;
		for (uint32_t x = center.x - 60; x < center.x + 60; x++)
			largest = std::max(largest, std::abs(Red(sharp, x, center.y) - Red(blurred, x, center.y)));
		CHECK(largest <= 1);
		auto cardEnds = [&](const ReadbackImage& image)
		{
			uint32_t left = center.x;
			uint32_t right = center.x;
			while (left > 0 && std::abs(Red(image, left - 1, center.y) - cardCode) <= 2)
				left--;
			while (right + 1 < c_Width && std::abs(Red(image, right + 1, center.y) - cardCode) <= 2)
				right++;
			return glm::ivec2(left, right);
		};
		const glm::ivec2 sharpEnds = cardEnds(sharp);
		const glm::ivec2 blurredEnds = cardEnds(blurred);
		CHECK(std::abs(sharpEnds.x - blurredEnds.x) <= 1);
		CHECK(std::abs(sharpEnds.y - blurredEnds.y) <= 1);
		CHECK(BlurWidth(AverageRows(LinearRows(blurred, 0, c_Height / 8))) >= 10);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Camera cuts and frames without history do not blur")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		SceneRenderer renderer(MotionSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		REQUIRE(renderer.Render(wall.SceneData, EdgeWall::CameraAt(0.0f)));
		CHECK_FALSE(renderer.GetStats().MotionBlurred); // The first frame
		REQUIRE(renderer.Render(wall.SceneData, EdgeWall::CameraAt(0.05f)));
		CHECK(renderer.GetStats().MotionBlurred);
		REQUIRE(renderer.Render(wall.SceneData, EdgeWall::CameraAt(20.05f)));
		CHECK_FALSE(renderer.GetStats().MotionBlurred); // A 20 m jump is a cut
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Both draw paths blur equally")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		Entity box = AddMesh(wall.SceneData, BuiltinAssets::CubeMesh, assets.AddMaterial(Emitting(0.5f)), glm::vec3(0.3f, 0.2f, 0.5f), "Box");
		box.GetComponent<TransformComponent>().Scale = glm::vec3(0.2f);
		SceneRendererSpecification cpuSpecification = MotionSpecification();
		cpuSpecification.GPUDriven = GPUDrivenMode::Off;
		SceneRendererSpecification gpuSpecification = MotionSpecification();
		gpuSpecification.GPUDriven = GPUDrivenMode::Auto;
		SceneRenderer cpu(cpuSpecification);
		SceneRenderer gpuDriven(gpuSpecification);
		ReadbackImage images[2];
		SceneRenderer* renderers[2] = { &cpu, &gpuDriven };
		for (int index = 0; index < 2; index++)
		{
			renderers[index]->SetViewportSize(c_Width, c_Height);
			for (int frame = 0; frame < 3; frame++)
			{
				box.GetComponent<TransformComponent>().Translation.y = 0.2f + 0.05f * static_cast<float>(frame);
				box.MarkModified<TransformComponent>();
				REQUIRE(renderers[index]->Render(wall.SceneData, EdgeWall::CameraAt(0.03f * static_cast<float>(frame))));
			}
			CHECK(renderers[index]->GetStats().MotionBlurred);
			REQUIRE(Renderer::ReadTexture(renderers[index]->GetOutputTexture(), images[index]));
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
			bool Component;
			MotionBlurSamples Quality;
			SceneEffectChoice Player;
			bool Option;
		};
		const Case cases[] = {
			{ "component off", false, MotionBlurSamples::Samples12, SceneEffectChoice::Scene, true },
			{ "quality row off", true, MotionBlurSamples::Off, SceneEffectChoice::Scene, true },
			{ "player off", true, MotionBlurSamples::Samples12, SceneEffectChoice::Off, true },
			{ "options off", true, MotionBlurSamples::Samples12, SceneEffectChoice::Scene, false },
		};
		for (const Case& testCase : cases)
		{
			SceneRendererSpecification specification = MotionSpecification(testCase.Quality);
			specification.Quality.MotionBlur = testCase.Player;
			wall.PostProcess->MotionBlur = testCase.Component;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_Width, c_Height);
			SceneRenderOptions options;
			options.MotionBlur = testCase.Option;
			RenderMove(renderer, wall.SceneData, 0.0f, 0.1f, options);
			CAPTURE(testCase.Name);
			CHECK_FALSE(renderer.GetStats().MotionBlurred);
			CHECK(renderer.GetMotionBlurTexture() == nullptr);
			CHECK(renderer.GetVelocityTexture() == nullptr); // Nothing else asks for motion vectors
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The quality row sets the samples")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		for (const MotionBlurSamples samples : { MotionBlurSamples::Samples8, MotionBlurSamples::Samples12, MotionBlurSamples::Samples16 })
		{
			SceneRenderer renderer(MotionSpecification(samples));
			renderer.SetViewportSize(c_Width, c_Height);
			RenderMove(renderer, wall.SceneData, 0.0f, 0.1f);
			CHECK(renderer.GetStats().MotionBlurSamples == RendererQuality::GetMotionBlurSampleCount(samples));
			CHECK(renderer.GetStats().MotionBlurTileSize == static_cast<uint32_t>(std::ceil(0.05f * c_Height * 0.5f)));
		}
		CHECK(RendererQuality::GetPreset(RendererQualityPreset::Low).MotionBlurQuality == MotionBlurSamples::Off);
		CHECK(RendererQuality::GetPreset(RendererQualityPreset::Medium).MotionBlurQuality == MotionBlurSamples::Samples8);
		CHECK(RendererQuality::GetPreset(RendererQualityPreset::High).MotionBlurQuality == MotionBlurSamples::Samples12);
		CHECK(RendererQuality::GetPreset(RendererQualityPreset::Ultra).MotionBlurQuality == MotionBlurSamples::Samples16);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Blurred pixels are not sharpened")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		EdgeWall wall(assets);
		wall.PostProcess->AntiAliasing = true;
		wall.PostProcess->AntiAliasingMethod = AntiAliasingMode::TAA;
		wall.PostProcess->TAAPreset = TemporalAntiAliasingPreset::Custom;
		SetTemporalAntiAliasingSettings(*wall.PostProcess, c_TAASharpSettings);
		// A box moving over the still wall: the pixels around it blur, the wall's far parts stay still (mask 0).
		Entity box = AddMesh(wall.SceneData, BuiltinAssets::CubeMesh, assets.AddMaterial(Emitting(0.5f)), glm::vec3(0.3f, 0.2f, 0.5f), "Box");
		box.GetComponent<TransformComponent>().Scale = glm::vec3(0.2f);
		auto render = [&](float sharpening, ReadbackImage& outMask)
		{
			wall.PostProcess->TAASharpening = sharpening;
			SceneRenderer renderer(MotionSpecification());
			renderer.SetViewportSize(c_Width, c_Height);
			for (int frame = 0; frame < 3; frame++)
			{
				box.GetComponent<TransformComponent>().Translation.x = 0.3f + 0.1f * static_cast<float>(frame);
				box.MarkModified<TransformComponent>();
				REQUIRE(renderer.Render(wall.SceneData, EdgeWall::CameraAt(0.0f)));
			}
			CHECK(renderer.GetStats().MotionBlurred);
			CHECK(renderer.GetStats().TemporalAntiAliasing);
			REQUIRE(Renderer::ReadTexture(renderer.GetMotionBlurTexture(), outMask));
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), image));
			return image;
		};
		ReadbackImage mask;
		const ReadbackImage sharpened = render(0.5f, mask);
		ReadbackImage unusedMask;
		const ReadbackImage unsharpened = render(0.0f, unusedMask);
		uint32_t blurredPixels = 0;
		uint32_t differences = 0;
		for (uint32_t y = 0; y < c_Height; y++)
		{
			for (uint32_t x = 0; x < c_Width; x++)
			{
				if (ReadHalfTexel(mask, x, y).a < 0.5f)
					continue;
				blurredPixels++;
				differences += GetPixelRGBA8(sharpened, x, y) != GetPixelRGBA8(unsharpened, x, y) ? 1u : 0u;
			}
		}
		CHECK(blurredPixels > 100);
		CHECK(differences == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
