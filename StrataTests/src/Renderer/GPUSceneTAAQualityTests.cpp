#include "Renderer/TAATestUtils.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// The TAA's presets and tunables (PostProcessComponent::TAAPreset, TemporalAntiAliasingSettings): each technique of the
// resolve and the tone mapping's sharpening, against Original, the first TAA.

using namespace Strata;
using namespace Strata::Tests;
using namespace Strata::Tests::TAA;

namespace
{

	// A half-transparent quad over the scene (blended, drawn after the opaque surfaces).
	Entity AddTransparentQuad(Scene& scene, SceneTestAssets& assets, const glm::vec4& color, const glm::vec3& position, float size)
	{
		MaterialProperties properties = UnlitColor(color);
		properties.AlphaMode = MaterialAlphaMode::Blend;
		Entity quad = AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(properties), position, "Quad");
		quad.GetComponent<TransformComponent>().Scale = glm::vec3(size);
		return quad;
	}

	// Original's table with another way of keeping the history consistent (each technique's case differs from Original in it
	// alone).
	TemporalAntiAliasingSettings WithVarianceClipping(float clipWidth)
	{
		TemporalAntiAliasingSettings settings = c_TAAOriginalSettings;
		settings.HistoryClamp = TemporalHistoryClamp::Variance;
		settings.ClipWidth = clipWidth;
		return settings;
	}

	// The trail case's glints: squares of c_GlintSize texels at a period of c_GlintPeriod texels, aligned with the texels (their
	// edges lie on texel boundaries, so the jitter never moves them).
	constexpr int c_GlintPeriod = 6;
	constexpr int c_GlintSize = 2;

	// The CPU computation the trail case's luminances come from. An achromatic, still image of a gray ground with glints
	// (above); every pixel's history starts at the cube's color, then each frame the history is clamped to its 3x3
	// neighborhood's range or clipped toward its mean (PostProcess/TAA.comp, in the luminance weights' compressed space at
	// exposure 1) and blended with the frame at 1/8 with the luminance weights. Returns the mean absolute luminance difference
	// from the ground of pixels 1 to `frames` frames after they were uncovered: the trail behind a cube that uncovers a band
	// of pixels each frame.
	double PredictTrail(float gray, float glint, float cube, TemporalHistoryClamp clamp, float clipWidth, uint32_t frames)
	{
		const auto image = [&](int x, int y)
		{
			x = (x % c_GlintPeriod + c_GlintPeriod) % c_GlintPeriod;
			y = (y % c_GlintPeriod + c_GlintPeriod) % c_GlintPeriod;
			return static_cast<double>(x < c_GlintSize && y < c_GlintSize ? glint : gray);
		};
		const auto compress = [](double value) { return value / (1.0 + value); };
		const auto expand = [](double value) { return value / std::max(1.0 - value, 1e-6); };
		std::vector<double> history(static_cast<size_t>(c_GlintPeriod) * c_GlintPeriod, static_cast<double>(cube));
		double total = 0.0;
		for (uint32_t frame = 0; frame < frames; frame++)
		{
			std::vector<double> next(history.size());
			double residual = 0.0;
			for (int y = 0; y < c_GlintPeriod; y++)
			{
				for (int x = 0; x < c_GlintPeriod; x++)
				{
					double low = 1e30;
					double high = -1e30;
					double sum = 0.0;
					double squares = 0.0;
					for (int dy = -1; dy <= 1; dy++)
					{
						for (int dx = -1; dx <= 1; dx++)
						{
							const double tap = image(x + dx, y + dy);
							const double value = clamp == TemporalHistoryClamp::Variance ? compress(tap) : tap;
							low = std::min(low, value);
							high = std::max(high, value);
							sum += value;
							squares += value * value;
						}
					}
					double value = history[static_cast<size_t>(y) * c_GlintPeriod + x];
					if (clamp == TemporalHistoryClamp::Variance)
					{
						const double mean = sum / 9.0;
						const double deviation = std::sqrt(std::max(squares / 9.0 - mean * mean, 0.0));
						const double offset = compress(value) - mean;
						double fraction = 1.0;
						if (offset != 0.0)
						{
							const double bound = offset > 0.0 ? std::min(mean + clipWidth * deviation, high) : std::max(mean - clipWidth * deviation, low);
							fraction = std::min(fraction, (bound - mean) / offset);
						}
						value = expand(mean + std::max(fraction, 0.0) * offset);
					}
					else
					{
						value = std::clamp(value, low, high);
					}
					const double current = image(x, y);
					const double currentWeight = 0.125 / (1.0 + current);
					const double historyWeight = 0.875 / (1.0 + value);
					next[static_cast<size_t>(y) * c_GlintPeriod + x] = (current * currentWeight + value * historyWeight) / (currentWeight + historyWeight);
					residual += std::abs(next[static_cast<size_t>(y) * c_GlintPeriod + x] - current);
				}
			}
			history = std::move(next);
			total += residual / static_cast<double>(history.size());
		}
		return total / static_cast<double>(frames);
	}

	float Luminance(const glm::vec3& color)
	{
		return glm::dot(color, c_LuminanceWeights);
	}

	TemporalAntiAliasingSettings WithResampleWeight(float weight)
	{
		TemporalAntiAliasingSettings settings = c_TAAOriginalSettings;
		settings.ResampleWeight = weight;
		return settings;
	}

	// Texture detail: a low-contrast texture (linear luminance 0.3 to 0.7, 0.18 apart in compressed luminance) on a plane facing
	// an orthographic camera from above, one texel per pixel (8 per unit). Four bands of c_Size / 4 rows hold sinusoids along x
	// of the periods in c_DetailPeriods. The plane is twice the view's width, so the camera can pan along it.
	constexpr std::array<float, 4> c_DetailPeriods = { 3.0f, 4.0f, 5.0f, 6.0f };
	constexpr uint32_t c_DetailBandRows = c_Size / 4;

	struct DetailScene
	{
		Scene Contents;

		DetailScene(SceneTestAssets& assets, const TemporalAntiAliasingSettings& settings)
		{
			SetTemporalAntiAliasingSettings(AddTemporalPostProcess(Contents, TemporalAntiAliasingPreset::Custom), settings);
			constexpr uint32_t width = c_Size * 2;
			std::vector<uint8_t> pixels;
			pixels.reserve(static_cast<size_t>(width) * c_Size * 4);
			for (uint32_t y = 0; y < c_Size; y++)
			{
				const float period = c_DetailPeriods[y / c_DetailBandRows];
				for (uint32_t x = 0; x < width; x++)
				{
					const float value = 0.5f + 0.2f * std::sin(glm::two_pi<float>() * static_cast<float>(x) / period + 0.3f);
					const uint8_t level = static_cast<uint8_t>(std::lround(value * 255.0f));
					pixels.insert(pixels.end(), { level, level, level, 255 });
				}
			}
			MaterialProperties properties = UnlitColor({ 1.0f, 1.0f, 1.0f, 1.0f });
			properties.BaseColorMap = assets.AddTexture(width, c_Size, pixels, TextureFormat::RGBA8, TextureFilter::Linear, TextureWrap::Clamp);
			// x from -4 to 12, z from -4 to 4.
			Entity plane = AddMesh(Contents, BuiltinAssets::PlaneMesh, assets.AddMaterial(properties), glm::vec3(4.0f, 0.0f, 0.0f), "Detail");
			plane.GetComponent<TransformComponent>().Scale = glm::vec3(16.0f, 1.0f, 8.0f);
		}

		// The view of the plane's left half moved `pixels` to the right.
		static SceneCamera Camera(float pixels)
		{
			const float x = pixels / 8.0f;
			return OrthographicLookAt(glm::vec3(x, 10.0f, 0.0f), glm::vec3(x, 0.0f, 0.0f), 8.0f);
		}

		// The standard deviation of the luminance in each band (its rows away from the other bands, the columns away from the
		// view's sides, where a pan brings in what the history does not hold).
		static std::array<double, 4> Amplitudes(const std::vector<glm::vec3>& image)
		{
			std::array<double, 4> amplitudes = {};
			for (uint32_t band = 0; band < c_DetailPeriods.size(); band++)
			{
				double sum = 0.0;
				double squares = 0.0;
				uint32_t count = 0;
				for (uint32_t y = band * c_DetailBandRows + 3; y < (band + 1) * c_DetailBandRows - 3; y++)
				{
					for (uint32_t x = 2; x < c_Size - 4; x++)
					{
						const double value = Luminance(image[static_cast<size_t>(y) * c_Size + x]);
						sum += value;
						squares += value * value;
						count++;
					}
				}
				const double mean = sum / count;
				amplitudes[band] = std::sqrt(std::max(squares / count - mean * mean, 0.0));
			}
			return amplitudes;
		}
	};

	TemporalAntiAliasingSettings WithTransparentWeight(float weight)
	{
		TemporalAntiAliasingSettings settings = c_TAAOriginalSettings;
		settings.TransparentWeight = weight;
		return settings;
	}

	// The world position (y 0) at the center of a pixel of DetailScene::Camera(pixels) and of the trail case's camera (8 pixels
	// per unit, x to the right and z down from the view's corner).
	glm::vec2 PixelToWorld(uint32_t x, uint32_t y, float panPixels = 0.0f)
	{
		return glm::vec2((static_cast<float>(x) + 0.5f + panPixels) / 8.0f - 4.0f, (static_cast<float>(y) + 0.5f) / 8.0f - 4.0f);
	}

	// What a steady pan between texels keeps of a sinusoid of `period` texels, relative to a still view: the current frame
	// weighs `weight`, and the history, read halfway between texels every frame, passes the Catmull-Rom filter's response
	// there (its taps at 1.5 and 0.5 texels weigh -0.0625 and 0.5625 on each side), so w / (1 - (1 - w) a).
	double PredictPanDetail(float period, float weight)
	{
		const double frequency = glm::two_pi<double>() / period;
		const double response = 2.0 * (0.5625 * std::cos(frequency * 0.5) - 0.0625 * std::cos(frequency * 1.5));
		return weight / (1.0 - (1.0 - weight) * response);
	}

	// Moves the boxes, the quad and the camera of a TemporalScene one frame on.
	void Animate(TemporalScene& scene, Entity quad, uint32_t frame)
	{
		Entity box = scene.Boxes[static_cast<size_t>(frame) % scene.Boxes.size()];
		box.GetComponent<TransformComponent>().Translation.y += 0.1f;
		box.MarkModified<TransformComponent>();
		quad.GetComponent<TransformComponent>().Translation.x += 0.15f;
		quad.MarkModified<TransformComponent>();
		scene.Camera = LookAt(glm::vec3(0.05f * static_cast<float>(frame + 1), 9.0f, 8.0f), glm::vec3(0.0f, 0.0f, -2.5f));
	}

}

TEST_SUITE("GPU.SceneRenderer.TAA")
{
	TEST_CASE("Original resolves exactly as Custom with Original's values, whatever Custom's unused fields hold")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		TemporalScene scene(TemporalAntiAliasingPreset::Original);
		Entity quad = AddTransparentQuad(scene.Contents, assets, { 1.0f, 0.2f, 0.1f, 0.5f }, glm::vec3(-3.0f, 1.5f, -1.0f), 2.0f);
		PostProcessComponent& postProcess = GetPostProcess(scene.Contents);
		// Custom's start from Original (the stored tunables are Sharp's table, the defaults, so ClipWidth and the threshold
		// keep Sharp's values, which Original's modes do not read).
		PostProcessComponent custom = postProcess;
		custom.TAAPreset = TemporalAntiAliasingPreset::Custom;
		SetTemporalAntiAliasingSettings(custom, GetTemporalAntiAliasingCustomStart(postProcess));
		REQUIRE(custom.TAAClipWidth == c_TAASharpSettings.ClipWidth);
		REQUIRE(custom.TAADisocclusionThreshold == c_TAASharpSettings.DisocclusionThreshold);
		const PostProcessComponent original = postProcess;

		SceneRenderer originalRenderer = CreateRenderer("Original");
		SceneRenderer customRenderer = CreateRenderer("Custom");
		originalRenderer.SetViewportSize(c_Size, c_Size);
		customRenderer.SetViewportSize(c_Size, c_Size);
		for (uint32_t frame = 0; frame < 16; frame++)
		{
			CAPTURE(frame);
			postProcess = original;
			const ReadbackImage originalImage = RenderToImage(originalRenderer, scene.Contents, scene.Camera);
			postProcess = custom;
			const ReadbackImage customImage = RenderToImage(customRenderer, scene.Contents, scene.Camera);
			CHECK(customRenderer.GetStats().TemporalFrames == frame + 1);
			CHECK(ReadHDRBits(originalRenderer.GetResolvedHDRTexture()) == ReadHDRBits(customRenderer.GetResolvedHDRTexture()));
			CHECK(originalImage.Pixels == customImage.Pixels);
			Animate(scene, quad, frame);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The history's alpha holds each pixel's signed view distance, and the sky's where nothing was drawn")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		Scene scene;
		AddTemporalPostProcess(scene, TemporalAntiAliasingPreset::Original);
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(UnlitColor({ 0.4f, 0.4f, 0.4f, 1.0f })), glm::vec3(0.0f), "Ground")
			.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
		// Above an orthographic camera whose near plane lies behind it: a negative distance.
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(UnlitColor({ 0.8f, 0.2f, 0.2f, 1.0f })), glm::vec3(-1.5f, 12.0f, 0.0f), "Above")
			.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f);
		SceneCamera orthographic = OrthographicLookAt(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f), 8.0f);
		orthographic.Projection = Math::OrthographicReverseZ(-4.0f, 4.0f, -4.0f, 4.0f, -10.0f, 100.0f);
		orthographic.Near = -10.0f;
		SceneRenderer renderer = CreateRenderer("Distance");
		renderer.SetViewportSize(c_Size, c_Size);
		// The first frame (a reset) and a blended one write it alike.
		for (uint32_t frame = 0; frame < 2; frame++)
		{
			CAPTURE(frame);
			REQUIRE(renderer.Render(scene, orthographic));
			uint32_t ground = 0;
			uint32_t above = 0;
			for (const glm::vec4& texel : ReadHDRA(renderer.GetResolvedHDRTexture()))
			{
				ground += std::abs(texel.a - 10.0f) < 0.01f ? 1u : 0u;
				above += std::abs(texel.a + 2.0f) < 0.01f ? 1u : 0u;
			}
			CHECK(above >= 15u * 15u);
			CHECK(ground + above == c_Size * c_Size);
		}

		// Level with the ground: the sky above the horizon, the ground below it, nearer toward the bottom.
		const SceneCamera level = LookAt(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 1.0f, -10.0f));
		REQUIRE(renderer.Render(scene, level));
		const std::vector<glm::vec4> texels = ReadHDRA(renderer.GetResolvedHDRTexture());
		for (uint32_t x = 0; x < c_Size; x++)
		{
			CAPTURE(x);
			CHECK(texels[x].a == RenderData::c_TemporalSkyDistance);
			const float bottom = texels[static_cast<size_t>(c_Size - 1) * c_Size + x].a;
			CHECK(bottom > 0.5f);
			CHECK(bottom < 3.0f);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Variance clipping leaves at most half of the range clamp's trail where glints widen the neighborhood")
	{
		// A cube brighter than the glints crosses them at 6 pixels a frame under a still camera. The range clamp keeps its
		// color in the pixels whose neighborhood holds a glint (only the glint's own brightness is taken off); variance
		// clipping pulls it toward the neighborhood's mean. The luminances come from PredictTrail, where the band behind the
		// cube that it uncovered in its last six frames keeps 0.38 of the clamp's trail (the side neighbors of a 2-texel glint,
		// whose neighborhoods hold two glint texels, keep the most). The cube's trailing edge stops at columns 6k + 4, so the
		// column it uncovers next to itself, which reads its history along the cube's motion, never holds a glint (the glints
		// lie in columns 6k and 6k + 1): such a glint would lose its history alike under both ways.
		constexpr float c_Gray = 0.2f;
		constexpr float c_Glint = 16.0f;
		constexpr float c_Cube = 24.0f;
		constexpr float c_ClipWidth = 1.25f;
		constexpr uint32_t c_TrailFrames = 6;
		const double predictedRange = PredictTrail(c_Gray, c_Glint, c_Cube, TemporalHistoryClamp::MinMax, 0.0f, c_TrailFrames);
		const double predictedVariance = PredictTrail(c_Gray, c_Glint, c_Cube, TemporalHistoryClamp::Variance, c_ClipWidth, c_TrailFrames);
		INFO("predicted trails: range " << predictedRange << ", variance " << predictedVariance);
		CHECK(predictedVariance < 0.4 * predictedRange);

		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		const AssetHandle grayMaterial = assets.AddMaterial(UnlitColor({ c_Gray, c_Gray, c_Gray, 1.0f }));
		const AssetHandle glintMaterial = assets.AddMaterial(Emitting(c_Glint));
		const AssetHandle cubeMaterial = assets.AddMaterial(Emitting(c_Cube));
		// Seen from above, 8 pixels per unit: glints of 2 pixels at a period of 6 from the view's corner, on texel boundaries.
		const SceneCamera camera = OrthographicLookAt(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f), 8.0f);
		const auto build = [&](Scene& scene, const TemporalAntiAliasingSettings& settings)
		{
			SetTemporalAntiAliasingSettings(AddTemporalPostProcess(scene, TemporalAntiAliasingPreset::Custom), settings);
			AddMesh(scene, BuiltinAssets::PlaneMesh, grayMaterial, glm::vec3(0.0f), "Ground").GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
			for (int row = 0; row < 11; row++)
			{
				for (int column = 0; column < 11; column++)
				{
					Entity glint = AddMesh(scene, BuiltinAssets::PlaneMesh, glintMaterial, glm::vec3(-3.875f + 0.75f * static_cast<float>(column), 0.01f,
						-3.875f + 0.75f * static_cast<float>(row)), "Glint");
					glint.GetComponent<TransformComponent>().Scale = glm::vec3(0.25f);
				}
			}
		};

		// The trail of each way: the scene with the cube against the same scene without it, rendered in step (the same jitter
		// phases and history), over the band the cube uncovered in its last six frames.
		const auto measureTrail = [&](const TemporalAntiAliasingSettings& settings)
		{
			Scene crossed;
			Scene background;
			build(crossed, settings);
			build(background, settings);
			// 2 units (16 pixels) wide, from outside the view on the left (its trailing edge in column -20).
			Entity cube = AddMesh(crossed, BuiltinAssets::CubeMesh, cubeMaterial, glm::vec3(-5.5f, 0.3f, 0.0f), "Cube");
			cube.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f, 0.5f, 2.0f);
			SceneRenderer crossedRenderer = CreateRenderer("Crossed");
			SceneRenderer backgroundRenderer = CreateRenderer("Background");
			crossedRenderer.SetViewportSize(c_Size, c_Size);
			backgroundRenderer.SetViewportSize(c_Size, c_Size);
			for (uint32_t frame = 0; frame < 19; frame++)
			{
				// Eight frames settle, then the cube moves 0.75 units (6 pixels) a frame.
				if (frame >= 8)
				{
					cube.GetComponent<TransformComponent>().Translation.x += 0.75f;
					cube.MarkModified<TransformComponent>();
				}
				REQUIRE(crossedRenderer.Render(crossed, camera));
				REQUIRE(backgroundRenderer.Render(background, camera));
				FinishFrame(gpu);
			}
			// The cube's trailing edge is at x = 1.75 (column 46); the band behind it reaches 36 pixels (4.5 units) back, leaving
			// out the pixels next to the cube, and spans the cube's rows.
			const float trailingEdge = cube.GetComponent<TransformComponent>().Translation.x - 1.0f;
			REQUIRE(trailingEdge == 1.75f);
			const glm::uvec2 corner0 = ProjectToPixel(camera, glm::vec3(trailingEdge - 4.5f + 0.01f, 0.0f, -0.99f), c_Size);
			const glm::uvec2 corner1 = ProjectToPixel(camera, glm::vec3(trailingEdge - 0.125f - 0.01f, 0.0f, 0.99f), c_Size);
			const glm::uvec2 first = glm::min(corner0, corner1);
			const glm::uvec2 last = glm::max(corner0, corner1);
			REQUIRE(last.x - first.x + 1 == 35);
			REQUIRE(last.y - first.y + 1 == 16);
			const std::vector<glm::vec3> crossedImage = ReadHDR(crossedRenderer.GetResolvedHDRTexture());
			const std::vector<glm::vec3> backgroundImage = ReadHDR(backgroundRenderer.GetResolvedHDRTexture());
			double trail = 0.0;
			for (uint32_t y = first.y; y <= last.y; y++)
			{
				for (uint32_t x = first.x; x <= last.x; x++)
				{
					const size_t pixel = static_cast<size_t>(y) * c_Size + x;
					trail += std::abs(static_cast<double>(Luminance(crossedImage[pixel])) - static_cast<double>(Luminance(backgroundImage[pixel])));
				}
			}
			return trail / static_cast<double>((last.x - first.x + 1) * (last.y - first.y + 1));
		};
		const double range = measureTrail(c_TAAOriginalSettings);
		const double variance = measureTrail(WithVarianceClipping(c_ClipWidth));
		INFO("trails: range " << range << ", variance " << variance);
		CHECK(range >= 0.02);
		CHECK(variance <= 0.5 * range);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A slow pan keeps more texture detail with the resampling term, as its model predicts")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		constexpr float c_ResampleWeight = 0.25f;
		for (const float speed : { 0.5f, 1.5f })
		{
			CAPTURE(speed);
			// 32 frames of a pan at `speed` pixels a frame; the detail kept, relative to a still view of the last position.
			const auto pan = [&](const TemporalAntiAliasingSettings& settings)
			{
				DetailScene scene(assets, settings);
				SceneRenderer renderer = CreateRenderer("Pan");
				renderer.SetViewportSize(c_Size, c_Size);
				for (uint32_t frame = 0; frame < 32; frame++)
				{
					REQUIRE(renderer.Render(scene.Contents, DetailScene::Camera(speed * static_cast<float>(frame))));
					FinishFrame(gpu);
				}
				CHECK(renderer.GetStats().TemporalFrames == 32);
				const std::array<double, 4> panned = DetailScene::Amplitudes(ReadHDR(renderer.GetResolvedHDRTexture()));
				SceneRenderer still = CreateRenderer("Still");
				still.SetViewportSize(c_Size, c_Size);
				for (uint32_t frame = 0; frame < SceneRenderer::c_TemporalJitterPhases; frame++)
					REQUIRE(still.Render(scene.Contents, DetailScene::Camera(speed * 31.0f)));
				const std::array<double, 4> settled = DetailScene::Amplitudes(ReadHDR(still.GetResolvedHDRTexture()));
				std::array<double, 4> kept = {};
				for (size_t band = 0; band < kept.size(); band++)
					kept[band] = panned[band] / settled[band];
				return kept;
			};
			const std::array<double, 4> original = pan(c_TAAOriginalSettings);
			const std::array<double, 4> resampled = pan(WithResampleWeight(c_ResampleWeight));
			for (size_t band = 0; band < c_DetailPeriods.size(); band++)
			{
				const float period = c_DetailPeriods[band];
				CAPTURE(period);
				const double predicted = PredictPanDetail(period, c_ResampleWeight);
				INFO("kept: original " << original[band] << " (predicted " << PredictPanDetail(period, 0.125f) << "), resampled " << resampled[band]
					<< " (predicted " << predicted << ")");
				CHECK(resampled[band] >= 0.9 * predicted);
				// Short periods lose the most to the filter; from 5 texels on both keep most of the detail.
				if (period <= 4.0f)
					CHECK(resampled[band] >= 1.2 * original[band]);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Integer motion reads the history's texels: the resampling term changes nothing")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		DetailScene off(assets, WithResampleWeight(0.0f));
		DetailScene on(assets, WithResampleWeight(0.25f));
		SceneRenderer offRenderer = CreateRenderer("Off");
		SceneRenderer onRenderer = CreateRenderer("On");
		offRenderer.SetViewportSize(c_Size, c_Size);
		onRenderer.SetViewportSize(c_Size, c_Size);
		for (uint32_t frame = 0; frame < 16; frame++)
		{
			CAPTURE(frame);
			const SceneCamera camera = DetailScene::Camera(static_cast<float>(frame));
			const ReadbackImage offImage = RenderToImage(offRenderer, off.Contents, camera);
			const ReadbackImage onImage = RenderToImage(onRenderer, on.Contents, camera);
			CHECK(ReadHDRBits(offRenderer.GetResolvedHDRTexture()) == ReadHDRBits(onRenderer.GetResolvedHDRTexture()));
			CHECK(offImage.Pixels == onImage.Pixels);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Blended transparent surfaces leave the transmittance in the HDR image's alpha on both paths")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		Scene scene;
		AddNeutralPostProcess(scene);
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(UnlitColor({ 0.3f, 0.3f, 0.3f, 1.0f })), glm::vec3(0.0f), "Ground")
			.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
		// x and z from -3 to 1, and from -1 to 3: they overlap from -1 to 1.
		AddTransparentQuad(scene, assets, { 1.0f, 0.2f, 0.1f, 0.5f }, glm::vec3(-1.0f, 0.5f, -1.0f), 4.0f);
		AddTransparentQuad(scene, assets, { 0.1f, 0.2f, 1.0f, 0.25f }, glm::vec3(1.0f, 1.0f, 1.0f), 4.0f);
		const SceneCamera camera = DetailScene::Camera(0.0f);
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(PathName(mode));
			SceneRenderer renderer = CreateRenderer("Alpha", mode);
			renderer.SetViewportSize(c_Size, c_Size);
			REQUIRE(renderer.Render(scene, camera));
			const std::vector<glm::vec4> texels = ReadHDRA(renderer.GetHDRTexture());
			uint32_t overlapping = 0;
			for (uint32_t y = 0; y < c_Size; y++)
			{
				for (uint32_t x = 0; x < c_Size; x++)
				{
					const glm::vec2 position = PixelToWorld(x, y);
					const auto inside = [&](float low, float high, bool& result)
					{
						const glm::vec2 near = glm::min(position - low, high - position);
						result = glm::all(glm::greaterThan(near, glm::vec2(0.0f)));
						return glm::all(glm::greaterThan(glm::abs(near), glm::vec2(0.1f)));
					};
					bool inFirst = false;
					bool inSecond = false;
					// Pixels at an edge are left out.
					if (!inside(-3.0f, 1.0f, inFirst) || !inside(-1.0f, 3.0f, inSecond))
						continue;
					const float expected = (inFirst ? 0.5f : 1.0f) * (inSecond ? 0.75f : 1.0f);
					CAPTURE(x);
					CAPTURE(y);
					CHECK(std::abs(texels[static_cast<size_t>(y) * c_Size + x].a - expected) <= 1.0f / 1024.0f);
					overlapping += inFirst && inSecond ? 1u : 0u;
				}
			}
			CHECK(overlapping >= 14u * 14u);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Under a moving transparent quad the current frame weighs more, and the trail behind it does not grow")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		// A red quad of alpha 0.5, 2 units (16 pixels) across, moves 0.5 units (4 pixels) a frame over the still texture, its
		// edges on texel boundaries. The errors are against the composite as it converges where the quad stays: the same scene
		// with the quad still at its last position, rendered in step (the same jitter phases).
		struct Errors
		{
			double Covered = 0.0; // Pixels covered for exactly two frames
			double Trail = 0.0;   // Pixels it left one or two frames ago
		};
		const auto measure = [&](float transparentWeight)
		{
			DetailScene scene(assets, WithTransparentWeight(transparentWeight));
			DetailScene still(assets, WithTransparentWeight(transparentWeight));
			Entity quad = AddTransparentQuad(scene.Contents, assets, { 1.0f, 0.1f, 0.1f, 0.5f }, glm::vec3(-2.0f, 0.5f, 0.0f), 2.0f);
			AddTransparentQuad(still.Contents, assets, { 1.0f, 0.1f, 0.1f, 0.5f }, glm::vec3(2.0f, 0.5f, 0.0f), 2.0f);
			SceneRenderer renderer = CreateRenderer("Reactive");
			SceneRenderer stillRenderer = CreateRenderer("Converged");
			renderer.SetViewportSize(c_Size, c_Size);
			stillRenderer.SetViewportSize(c_Size, c_Size);
			const SceneCamera camera = DetailScene::Camera(0.0f);
			for (uint32_t frame = 0; frame < 16; frame++)
			{
				if (frame >= 8)
				{
					quad.GetComponent<TransformComponent>().Translation.x += 0.5f;
					quad.MarkModified<TransformComponent>();
				}
				REQUIRE(renderer.Render(scene.Contents, camera));
				REQUIRE(stillRenderer.Render(still.Contents, camera));
				FinishFrame(gpu);
			}
			// The quad now spans x from 1 to 3, from 0.5 to 2.5 in the previous frame and from 0 to 2 two frames ago.
			const float left = quad.GetComponent<TransformComponent>().Translation.x - 1.0f;
			REQUIRE(left == 1.0f);
			const std::vector<glm::vec3> resolved = ReadHDR(renderer.GetResolvedHDRTexture());
			const std::vector<glm::vec3> current = ReadHDR(stillRenderer.GetResolvedHDRTexture());
			Errors errors;
			uint32_t covered = 0;
			uint32_t trail = 0;
			for (uint32_t y = 0; y < c_Size; y++)
			{
				for (uint32_t x = 0; x < c_Size; x++)
				{
					const glm::vec2 position = PixelToWorld(x, y);
					if (std::abs(position.y) > 0.9f)
						continue;
					const size_t pixel = static_cast<size_t>(y) * c_Size + x;
					const glm::vec3 difference = glm::abs(resolved[pixel] - current[pixel]);
					const double error = static_cast<double>(difference.r + difference.g + difference.b) / 3.0;
					if (position.x > left + 1.0f && position.x < left + 1.5f)
					{
						errors.Covered += error;
						covered++;
					}
					else if (position.x > left - 1.0f && position.x < left)
					{
						errors.Trail += error;
						trail++;
					}
				}
			}
			REQUIRE(covered == 4u * 14u);
			REQUIRE(trail == 8u * 14u);
			errors.Covered /= covered;
			errors.Trail /= trail;
			return errors;
		};
		const Errors original = measure(0.0f);
		const Errors reactive = measure(0.5f);
		INFO("covered for two frames: " << original.Covered << " -> " << reactive.Covered << "; trail: " << original.Trail << " -> " << reactive.Trail);
		CHECK(original.Covered > 0.01);
		CHECK(reactive.Covered <= 0.5 * original.Covered);
		CHECK(reactive.Trail <= original.Trail * 1.05 + 1e-3);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The reactive mask is not a bypass: a still transparent quad's edges vary less than its jittered frames")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		LoadAssets(assets);
		// Over a plain ground, so only the quad's edges change with the jitter; its edges lie near texel centers.
		Scene scene;
		SetTemporalAntiAliasingSettings(AddTemporalPostProcess(scene, TemporalAntiAliasingPreset::Custom), WithTransparentWeight(0.5f));
		AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(UnlitColor({ 0.4f, 0.4f, 0.4f, 1.0f })), glm::vec3(0.0f), "Ground")
			.GetComponent<TransformComponent>().Scale = glm::vec3(40.0f);
		AddTransparentQuad(scene, assets, { 1.0f, 0.1f, 0.1f, 0.5f }, glm::vec3(0.06f, 0.5f, 0.06f), 3.0f);
		SceneRenderer renderer = CreateRenderer("Still");
		renderer.SetViewportSize(c_Size, c_Size);
		const SceneCamera camera = DetailScene::Camera(0.0f);
		const size_t pixels = static_cast<size_t>(c_Size) * c_Size;
		std::vector<glm::dvec2> resolvedMoments(pixels, glm::dvec2(0.0));
		std::vector<glm::dvec2> currentMoments(pixels, glm::dvec2(0.0));
		constexpr uint32_t c_Frames = 32;
		for (uint32_t frame = 0; frame < 8 + c_Frames; frame++)
		{
			REQUIRE(renderer.Render(scene, camera));
			if (frame < 8)
				continue;
			const std::vector<glm::vec3> resolved = ReadHDR(renderer.GetResolvedHDRTexture());
			const std::vector<glm::vec3> current = ReadHDR(renderer.GetHDRTexture());
			for (size_t pixel = 0; pixel < pixels; pixel++)
			{
				const double resolvedValue = Luminance(resolved[pixel]);
				const double currentValue = Luminance(current[pixel]);
				resolvedMoments[pixel] += glm::dvec2(resolvedValue, resolvedValue * resolvedValue);
				currentMoments[pixel] += glm::dvec2(currentValue, currentValue * currentValue);
			}
		}
		const auto deviation = [](const glm::dvec2& moments)
		{
			const glm::dvec2 mean = moments / static_cast<double>(c_Frames);
			return std::sqrt(std::max(mean.y - mean.x * mean.x, 0.0));
		};
		// The quad's edges: the pixels the jitter changes.
		double resolvedDeviation = 0.0;
		double currentDeviation = 0.0;
		uint32_t edges = 0;
		for (size_t pixel = 0; pixel < pixels; pixel++)
		{
			const double jittered = deviation(currentMoments[pixel]);
			if (jittered < 0.02)
				continue;
			currentDeviation += jittered;
			resolvedDeviation += deviation(resolvedMoments[pixel]);
			edges++;
		}
		INFO("edge pixels " << edges << ", deviation " << currentDeviation / edges << " -> " << resolvedDeviation / edges);
		CHECK(edges >= 4u * 16u);
		CHECK(resolvedDeviation <= 0.8 * currentDeviation);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
