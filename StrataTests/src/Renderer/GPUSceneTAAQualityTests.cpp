#include "Renderer/TAATestUtils.h"

#include <algorithm>
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
}
