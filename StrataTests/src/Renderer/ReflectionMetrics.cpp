#include "Renderer/ReflectionMetrics.h"

#include "Strata/Asset/BuiltinAssets.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Strata::Tests
{

	NoiseMeasure MeasureNoise(const ReadbackImage& image, const glm::uvec4& rect)
	{
		REQUIRE(image.BytesPerPixel >= 3);
		REQUIRE(rect.x < rect.z);
		REQUIRE(rect.y < rect.w);
		REQUIRE(rect.z <= image.Width);
		REQUIRE(rect.w <= image.Height);
		constexpr uint32_t c_Radius = 4; // The 9x9 window
		NoiseMeasure measure;
		const uint32_t width = rect.z - rect.x;
		const uint32_t height = rect.w - rect.y;
		if (width <= 2 * c_Radius || height <= 2 * c_Radius)
			return measure;

		// The encoded luma and its summed-area table over the rectangle (one row and column of zeros first).
		std::vector<double> luma(static_cast<size_t>(width) * height);
		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				const uint8_t* pixel = image.Pixels.data() + (static_cast<size_t>(rect.y + y) * image.Width + rect.x + x) * image.BytesPerPixel;
				luma[static_cast<size_t>(y) * width + x] = (0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2]) / 255.0;
			}
		}
		const size_t stride = static_cast<size_t>(width) + 1;
		std::vector<double> table(stride * (static_cast<size_t>(height) + 1), 0.0);
		for (uint32_t y = 0; y < height; y++)
		{
			double row = 0.0;
			for (uint32_t x = 0; x < width; x++)
			{
				row += luma[static_cast<size_t>(y) * width + x];
				table[(y + 1) * stride + x + 1] = table[y * stride + x + 1] + row;
			}
		}

		double squares = 0.0;
		std::vector<double> phaseSum3(9, 0.0), phaseSum4(16, 0.0);
		std::vector<uint32_t> phaseCount3(9, 0), phaseCount4(16, 0);
		for (uint32_t y = c_Radius; y < height - c_Radius; y++)
		{
			for (uint32_t x = c_Radius; x < width - c_Radius; x++)
			{
				const size_t x0 = x - c_Radius;
				const size_t y0 = y - c_Radius;
				const size_t x1 = x + c_Radius + 1;
				const size_t y1 = y + c_Radius + 1;
				const double window = table[y1 * stride + x1] - table[y0 * stride + x1] - table[y1 * stride + x0] + table[y0 * stride + x0];
				const double residual = luma[static_cast<size_t>(y) * width + x] - window / 81.0;
				squares += residual * residual;
				// Phases of the image's own pixel coordinates: a pattern locked to the screen keeps its phase.
				const uint32_t imageX = rect.x + x;
				const uint32_t imageY = rect.y + y;
				const uint32_t phase3 = (imageY % 3) * 3 + imageX % 3;
				const uint32_t phase4 = (imageY % 4) * 4 + imageX % 4;
				phaseSum3[phase3] += residual;
				phaseCount3[phase3]++;
				phaseSum4[phase4] += residual;
				phaseCount4[phase4]++;
				measure.Pixels++;
			}
		}
		const auto pattern = [](const std::vector<double>& sums, const std::vector<uint32_t>& counts)
		{
			double sum = 0.0;
			uint32_t phases = 0;
			for (size_t phase = 0; phase < sums.size(); phase++)
			{
				if (counts[phase] == 0)
					continue;
				const double mean = sums[phase] / counts[phase];
				sum += mean * mean;
				phases++;
			}
			return phases > 0 ? static_cast<float>(100.0 * std::sqrt(sum / phases)) : 0.0f;
		};
		measure.Noise = static_cast<float>(100.0 * std::sqrt(squares / measure.Pixels));
		measure.Pattern3 = pattern(phaseSum3, phaseCount3);
		measure.Pattern4 = pattern(phaseSum4, phaseCount4);
		return measure;
	}

	SpecularLab::SpecularLab(SceneTestAssets& assets)
	{
		for (const AssetHandle builtin : { BuiltinAssets::CubeMesh, BuiltinAssets::PlaneMesh, BuiltinAssets::SphereMesh, BuiltinAssets::DefaultMaterial })
			REQUIRE(assets.Manager->LoadAssetSync(builtin));
		AddNeutralPostProcess(Contents).ScreenSpaceReflections = true;
		Contents.CreateEntity("Sky").AddComponent<SkyLightComponent>().EnvironmentMap =
			assets.Add(CreateEnvironmentTexture(glm::vec3(0.30f, 0.45f, 0.80f), glm::vec3(0.08f, 0.07f, 0.06f)), "Sky");
		// The look-dev scene's sun (Clear at 30 degrees, azimuth 75: toward the sun), at the tests' scale.
		Sun = AddSun(Contents, SunDirection(30.0f, 75.0f), 3.0f);

		const auto material = [&assets](const glm::vec3& color, float metallic, float roughness)
		{
			MaterialProperties properties;
			properties.BaseColor = glm::vec4(color, 1.0f);
			properties.Metallic = metallic;
			properties.Roughness = roughness;
			return assets.AddMaterial(properties);
		};
		const auto add = [this](AssetHandle mesh, AssetHandle surface, const glm::vec3& position, const glm::vec3& scale, const char* name)
		{
			Entity entity = AddMesh(Contents, mesh, surface, position, name);
			entity.GetComponent<TransformComponent>().Scale = scale;
			return entity;
		};
		const AssetHandle ground = material(glm::vec3(0.35f), 0.0f, 0.85f);
		const AssetHandle wet = material(glm::vec3(0.05f, 0.05f, 0.052f), 0.0f, 0.06f);
		const AssetHandle sheet = material(glm::vec3(0.91f, 0.92f, 0.92f), 1.0f, 0.18f);
		const AssetHandle postWhite = material(glm::vec3(0.85f), 0.0f, 0.5f);
		const AssetHandle postRed = material(glm::vec3(0.8f, 0.08f, 0.05f), 0.0f, 0.5f);
		const AssetHandle brick = material(glm::vec3(0.45f, 0.2f, 0.12f), 0.0f, 0.85f);
		const AssetHandle teal = material(glm::vec3(0.05f, 0.4f, 0.4f), 0.0f, 0.7f);
		const AssetHandle yellow = material(glm::vec3(0.7f, 0.55f, 0.08f), 0.0f, 0.7f);
		const AssetHandle crate = material(glm::vec3(0.4f, 0.28f, 0.16f), 0.0f, 0.75f);

		add(BuiltinAssets::PlaneMesh, ground, glm::vec3(0.0f), glm::vec3(400.0f, 1.0f, 400.0f), "Ground");
		add(BuiltinAssets::PlaneMesh, wet, glm::vec3(-14.0f, 0.01f, 14.0f), glm::vec3(24.0f, 1.0f, 16.0f), "Wet")
			.GetComponent<MeshRendererComponent>().CastShadows = false;
		add(BuiltinAssets::CubeMesh, sheet, glm::vec3(8.0f, 1.7f, -6.0f), glm::vec3(12.0f, 3.0f, 2.5f), "Sheet");
		add(BuiltinAssets::CubeMesh, postWhite, glm::vec3(-8.0f, 1.7f, -6.0f), glm::vec3(12.0f, 3.0f, 2.5f), "Box");
		add(BuiltinAssets::CubeMesh, brick, glm::vec3(0.0f, 4.0f, 45.0f), glm::vec3(120.0f, 8.0f, 1.0f), "Back Wall");
		add(BuiltinAssets::CubeMesh, teal, glm::vec3(45.0f, 7.0f, 0.0f), glm::vec3(2.0f, 14.0f, 60.0f), "East Block");
		add(BuiltinAssets::CubeMesh, yellow, glm::vec3(-45.0f, 7.0f, 0.0f), glm::vec3(2.0f, 14.0f, 60.0f), "West Block");
		for (const float x : { -20.0f, -14.0f, -8.0f, 8.0f, 14.0f, 20.0f })
		{
			const bool red = x == -14.0f || x == 14.0f;
			add(BuiltinAssets::CubeMesh, red ? postRed : postWhite, glm::vec3(x, 1.5f, 9.0f), glm::vec3(0.12f, 3.0f, 0.12f), "Post");
		}
		for (const glm::vec2& place : { glm::vec2(-17.0f, 13.0f), glm::vec2(-11.0f, 11.0f), glm::vec2(12.0f, 11.0f), glm::vec2(17.0f, 14.0f) })
			add(BuiltinAssets::CubeMesh, crate, glm::vec3(place.x, 0.5f, place.y), glm::vec3(1.0f), "Crate");
		constexpr float c_LadderRoughness[8] = { 0.0f, 0.05f, 0.1f, 0.2f, 0.3f, 0.5f, 0.7f, 1.0f };
		for (int index = 0; index < 8; index++)
		{
			add(BuiltinAssets::SphereMesh, material(glm::vec3(0.95f), 1.0f, c_LadderRoughness[index]), glm::vec3(-7.0f + 2.0f * static_cast<float>(index), 0.5f, 1.0f),
				glm::vec3(1.0f), "Ladder");
		}
	}

	size_t MaskFlickerFrame(FlickerFrame& frame, const MeasuredRectangle& rectangle, uint32_t inset)
	{
		const uint32_t width = frame.Width;
		const uint32_t height = frame.Height;
		REQUIRE(frame.ViewDepth.size() == static_cast<size_t>(width) * height);
		const glm::vec3 normalVector = glm::normalize(glm::cross(rectangle.AxisU, rectangle.AxisV));
		const glm::vec3 centerVector = glm::vec3(glm::dvec3(rectangle.Center) - frame.CameraPosition); // Relative to the camera
		const glm::vec3 axisU = rectangle.AxisU / glm::dot(rectangle.AxisU, rectangle.AxisU);
		const glm::vec3 axisV = rectangle.AxisV / glm::dot(rectangle.AxisV, rectangle.AxisV);
		const glm::mat4 inverseViewProjection = glm::inverse(frame.ViewProjection);
		// Plain numbers in the loop over pixels: Debug builds run it for every frame of the strafe tests.
		float inverse[4][4];
		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 4; row++)
				inverse[column][row] = inverseViewProjection[column][row];
		}
		const float normal[3] = { normalVector.x, normalVector.y, normalVector.z };
		const float center[3] = { centerVector.x, centerVector.y, centerVector.z };
		const float u[3] = { axisU.x, axisU.y, axisU.z };
		const float v[3] = { axisV.x, axisV.y, axisV.z };
		const float forward[3] = { frame.CameraForward.x, frame.CameraForward.y, frame.CameraForward.z };
		const float* viewDepth = frame.ViewDepth.data();
		// Inside the rectangle and showing it (the depth drawn is the rectangle's), before the inset.
		std::vector<uint8_t> inside(static_cast<size_t>(width) * height, 0);
		for (uint32_t y = 0; y < height; y++)
		{
			const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
			for (uint32_t x = 0; x < width; x++)
			{
				const size_t pixel = static_cast<size_t>(y) * width + x;
				const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(width) - 1.0f;
				float nearClip[4];
				float farClip[4];
				for (int row = 0; row < 4; row++)
				{
					const float common = inverse[0][row] * ndcX + inverse[1][row] * ndcY + inverse[3][row];
					nearClip[row] = common + inverse[2][row];
					farClip[row] = common + 0.5f * inverse[2][row];
				}
				float origin[3];
				float direction[3];
				for (int axis = 0; axis < 3; axis++)
				{
					origin[axis] = nearClip[axis] / nearClip[3];
					direction[axis] = farClip[axis] / farClip[3] - origin[axis];
				}
				const float facing = direction[0] * normal[0] + direction[1] * normal[1] + direction[2] * normal[2];
				if (!(std::abs(facing) > 1e-12f))
					continue;
				const float t = ((center[0] - origin[0]) * normal[0] + (center[1] - origin[1]) * normal[1] + (center[2] - origin[2]) * normal[2]) / facing;
				if (!(t > 0.0f))
					continue;
				float hit[3];
				float offset[3];
				for (int axis = 0; axis < 3; axis++)
				{
					hit[axis] = origin[axis] + direction[axis] * t;
					offset[axis] = hit[axis] - center[axis];
				}
				const float alongU = offset[0] * u[0] + offset[1] * u[1] + offset[2] * u[2];
				const float alongV = offset[0] * v[0] + offset[1] * v[1] + offset[2] * v[2];
				if (std::abs(alongU) > 1.0f || std::abs(alongV) > 1.0f)
					continue;
				const float depth = hit[0] * forward[0] + hit[1] * forward[1] + hit[2] * forward[2];
				inside[pixel] = std::abs(viewDepth[pixel] - depth) <= 0.01f * depth + 0.01f ? 1 : 0;
			}
		}
		// Kept where every pixel within `inset` of it is inside (a summed-area table of the inside pixels).
		const size_t stride = static_cast<size_t>(width) + 1;
		std::vector<uint32_t> table(stride * (static_cast<size_t>(height) + 1), 0);
		for (uint32_t y = 0; y < height; y++)
		{
			uint32_t row = 0;
			for (uint32_t x = 0; x < width; x++)
			{
				row += inside[static_cast<size_t>(y) * width + x];
				table[(y + 1) * stride + x + 1] = table[y * stride + x + 1] + row;
			}
		}
		const uint32_t window = (2 * inset + 1) * (2 * inset + 1);
		size_t kept = 0;
		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				const size_t pixel = static_cast<size_t>(y) * width + x;
				bool keep = x >= inset && y >= inset && x + inset < width && y + inset < height;
				if (keep)
				{
					const size_t x0 = x - inset;
					const size_t y0 = y - inset;
					const size_t x1 = x + inset + 1;
					const size_t y1 = y + inset + 1;
					keep = table[y1 * stride + x1] - table[y0 * stride + x1] - table[y1 * stride + x0] + table[y0 * stride + x0] == window;
				}
				if (keep)
					kept++;
				else
					frame.ViewDepth[pixel] = 0.0f;
			}
		}
		return kept;
	}

}
