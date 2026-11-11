#include "Renderer/SceneRendererTestUtils.h"
#include "Renderer/TAATestUtils.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/AreaLights.h"
#include "Strata/Renderer/RendererComponents.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Rect lights over planes, read in the HDR target (pre-exposure 1: neutral post-processing, no ambient light) and compared
	// with the CPU reference (AreaLights::Shade) at every pixel of the plane: the reference is what the shading must show.

	constexpr double c_Pi = 3.14159265358979323846;

	// The rotation whose -Z (the way a light shines) points along `direction` and whose X lies along `axisX`'s part
	// perpendicular to it.
	glm::quat FacingRotation(const glm::vec3& direction, const glm::vec3& axisX)
	{
		const glm::vec3 z = -glm::normalize(direction);
		const glm::vec3 x = glm::normalize(axisX - z * glm::dot(axisX, z));
		return glm::quat_cast(glm::mat3(x, glm::cross(z, x), z));
	}

	Entity AddRectLight(Scene& scene, const glm::vec3& center, const glm::vec3& direction, const glm::vec3& axisX, float width, float height, float nits,
		float range, bool twoSided = false)
	{
		Entity entity = scene.CreateEntity("Rect Light");
		TransformComponent& transform = entity.GetComponent<TransformComponent>();
		transform.Translation = center;
		transform.Rotation = FacingRotation(direction, axisX);
		RectLightComponent& light = entity.AddComponent<RectLightComponent>();
		light.Width = width;
		light.Height = height;
		light.Intensity = nits;
		light.Range = range;
		light.TwoSided = twoSided;
		return entity;
	}

	// No light but the scene's own: no ambient, no environment.
	void AddDarkSky(Scene& scene)
	{
		scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = glm::vec3(0.0f);
	}

	struct PlaneMaterial
	{
		glm::vec3 Albedo = glm::vec3(0.8f);
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		float Clearcoat = 0.0f;
		float ClearcoatRoughness = 0.0f;
		float Transmission = 0.0f;
		float Alpha = 1.0f; // Below 1: alpha blended

		MaterialProperties ToProperties() const
		{
			MaterialProperties properties;
			properties.BaseColor = glm::vec4(Albedo, Alpha);
			properties.Metallic = Metallic;
			properties.Roughness = Roughness;
			properties.Clearcoat = Clearcoat;
			properties.ClearcoatRoughness = ClearcoatRoughness;
			properties.Transmission = Transmission;
			if (Alpha < 1.0f)
				properties.AlphaMode = MaterialAlphaMode::Blend;
			return properties;
		}
	};

	// A horizontal plane of the built-in mesh (normal +Y) at height `y`, `size` meters across.
	Entity AddPlane(Scene& scene, SceneTestAssets& assets, const PlaneMaterial& material, float y, float size)
	{
		Entity plane = AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(material.ToProperties()), glm::vec3(0.0f, y, 0.0f), "Plane");
		plane.GetComponent<TransformComponent>().Scale = glm::vec3(size, 1.0f, size);
		return plane;
	}

	// The light a rect light entity's record holds, as the shading takes it, camera-relative (in double, as both paths upload).
	AreaLights::AreaLight GetRelativeLight(Entity entity, const glm::dvec3& cameraPosition)
	{
		entity.GetScene()->UpdateWorldTransforms();
		const RectLightComponent& light = entity.GetComponent<RectLightComponent>();
		const glm::mat4& world = entity.GetComponent<WorldTransformComponent>().Matrix;
		const glm::vec2 halfExtents = glm::vec2(light.Width, light.Height) * 0.5f;
		const float luminance = AreaLights::GetLuminance(light.Intensity, light.Units, AreaLights::GetWorldHalfExtents(world, halfExtents), light.TwoSided);
		AreaLights::Record record = AreaLights::MakeRecord(world, true, light.Color, luminance, halfExtents, light.Range, light.TwoSided);
		REQUIRE(record.Drawn);
		record.Center = glm::vec3(glm::dvec3(record.Center) - cameraPosition);
		return AreaLights::LoadAreaLight(record);
	}

	// The BRDF table as the forward pass samples it (linear, clamped to its edge; the sampler's weights are finer than any
	// difference the tests resolve).
	glm::vec2 SampleBRDF(const ReadbackImage& lut, const glm::vec2& uv)
	{
		const glm::vec2 texel = uv * glm::vec2(static_cast<float>(lut.Width), static_cast<float>(lut.Height)) - 0.5f;
		const glm::vec2 base = glm::floor(texel);
		const glm::vec2 weight = texel - base;
		const auto load = [&](int x, int y)
		{
			const uint32_t cx = static_cast<uint32_t>(std::clamp(x, 0, static_cast<int>(lut.Width) - 1));
			const uint32_t cy = static_cast<uint32_t>(std::clamp(y, 0, static_cast<int>(lut.Height) - 1));
			return glm::vec2(ReadHalfTexel(lut, cx, cy));
		};
		const int x0 = static_cast<int>(base.x);
		const int y0 = static_cast<int>(base.y);
		const glm::vec2 upper = glm::mix(load(x0, y0), load(x0 + 1, y0), weight.x);
		const glm::vec2 lower = glm::mix(load(x0, y0 + 1), load(x0 + 1, y0 + 1), weight.x);
		return glm::mix(upper, lower, weight.y);
	}

	glm::vec3 GetSpecularEnergy(const glm::vec3& f0, const glm::vec2& dfg)
	{
		return glm::vec3(1.0f) + f0 * (1.0f / std::max(dfg.x + dfg.y, 0.05f) - 1.0f);
	}

	// Where a pixel's ray (through its center, moved by `offset` pixels) meets the plane y = `planeY` within `halfSize` of its
	// center, camera-relative; false where it misses.
	bool FindPlanePoint(const SceneCamera& camera, uint32_t x, uint32_t y, uint32_t width, uint32_t height, float planeY, float halfSize, glm::dvec3& outRelative,
		const glm::dvec2& offset = glm::dvec2(0.0))
	{
		const glm::dmat4 inverse = glm::inverse(glm::dmat4(camera.Projection) * glm::dmat4(camera.View));
		const glm::dvec2 ndc((static_cast<double>(x) + 0.5 + offset.x) / width * 2.0 - 1.0, 1.0 - (static_cast<double>(y) + 0.5 + offset.y) / height * 2.0);
		const glm::dvec4 nearPoint = inverse * glm::dvec4(ndc, 1.0, 1.0); // Reversed-Z: the near plane at depth 1
		const glm::dvec4 farPoint = inverse * glm::dvec4(ndc, 0.5, 1.0);
		const glm::dvec3 start = glm::dvec3(nearPoint) / nearPoint.w;
		const glm::dvec3 direction = glm::dvec3(farPoint) / farPoint.w - start;
		if (std::abs(direction.y) < 1e-12)
			return false;
		const double t = (planeY - start.y) / direction.y;
		if (!(t > 0.0))
			return false;
		const glm::dvec3 hit = start + direction * t;
		if (std::abs(hit.x) > halfSize || std::abs(hit.z) > halfSize)
			return false;
		outRelative = hit - glm::dvec3(camera.Position);
		return true;
	}

	// How the GPU's image agrees with the reference: the largest relative difference where the reference is above
	// c_RelativeFloor of its peak, and the largest absolute one over the peak everywhere. The rasterizer places a pixel's
	// surface point to a fraction of a pixel (its vertices snap to c_SubpixelStep of a pixel), which moves it by a pixel's
	// footprint times that, much at grazing views: where the light changes so fast that this shows, what the reference
	// changes over c_SubpixelStep around the pixel's center is allowed on top.
	constexpr double c_RelativeFloor = 0.01;
	constexpr double c_SubpixelStep = 1.0 / 256.0;
	constexpr double c_HalfRounding = 1.0 / 2048.0; // Half a half float's step, relative

	struct Agreement
	{
		double MaxRelative = 0.0;
		double MaxAbsolute = 0.0; // Over the peak
		double Peak = 0.0;
		uint32_t Compared = 0;
		glm::uvec2 WorstPixel = glm::uvec2(0); // Where MaxRelative is
		glm::vec2 WorstValues = glm::vec2(0.0f); // There: shown and expected
	};

	struct ReferenceView
	{
		const SceneCamera* Camera = nullptr;
		uint32_t Width = 0;
		uint32_t Height = 0;
		float PlaneY = 0.0f;
		float PlaneHalfSize = 0.0f;
		float DiffuseWeight = 1.0f;
		float SpecularWeight = 1.0f;
		float Scale = 1.0f; // What blending keeps of the surface's light (its alpha)
	};

	// The reference's radiance at a plane point (camera-relative), from every light.
	glm::vec3 ShadeReference(const ReferenceView& view, const glm::dvec3& relative, const PlaneMaterial& material, std::span<const AreaLights::AreaLight> lights,
		const ReadbackImage& lut)
	{
		AreaLights::Surface surface;
		surface.Position = glm::vec3(relative);
		surface.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		surface.View = view.Camera->Orthographic ? glm::normalize(glm::vec3(glm::inverse(view.Camera->View)[2])) : glm::vec3(glm::normalize(-relative));
		surface.Albedo = material.Albedo;
		surface.Metallic = material.Metallic;
		surface.Roughness = material.Roughness;
		surface.F0 = glm::mix(glm::vec3(0.04f), material.Albedo, material.Metallic);
		const float noV = std::max(glm::dot(surface.Normal, surface.View), 1e-4f);
		const glm::vec2 dfg = SampleBRDF(lut, glm::vec2(noV, material.Roughness));
		surface.SpecularEnergy = GetSpecularEnergy(surface.F0, dfg);
		AreaLights::Coat coat;
		coat.Normal = surface.Normal;
		glm::vec2 coatDFG(1.0f, 0.0f);
		if (material.Clearcoat > 0.0f)
		{
			coat.Strength = material.Clearcoat;
			coat.Roughness = material.ClearcoatRoughness;
			coatDFG = SampleBRDF(lut, glm::vec2(noV, material.ClearcoatRoughness));
			coat.SpecularEnergy = GetSpecularEnergy(glm::vec3(AreaLights::c_ClearcoatF0), coatDFG).x;
		}
		glm::vec3 radiance(0.0f);
		for (const AreaLights::AreaLight& light : lights)
			radiance += AreaLights::Shade(surface, coat, coatDFG, dfg, view.DiffuseWeight, light, view.SpecularWeight);
		return radiance * view.Scale;
	}

	Agreement CompareWithReference(const ReferenceView& view, const ReadbackImage& hdr, const PlaneMaterial& material, std::span<const AreaLights::AreaLight> lights,
		const ReadbackImage& lut, const ReadbackImage* subtract = nullptr)
	{
		REQUIRE(hdr.Width == view.Width);
		REQUIRE(hdr.Height == view.Height);
		std::vector<glm::vec3> expected(static_cast<size_t>(view.Width) * view.Height, glm::vec3(-1.0f));
		Agreement agreement;
		for (uint32_t y = 0; y < view.Height; y++)
		{
			for (uint32_t x = 0; x < view.Width; x++)
			{
				glm::dvec3 relative(0.0);
				if (!FindPlanePoint(*view.Camera, x, y, view.Width, view.Height, view.PlaneY, view.PlaneHalfSize, relative))
					continue;
				const glm::vec3 value = ShadeReference(view, relative, material, lights, lut);
				expected[static_cast<size_t>(y) * view.Width + x] = value;
				agreement.Peak = std::max(agreement.Peak, static_cast<double>(std::max(value.r, std::max(value.g, value.b))));
			}
		}
		for (uint32_t y = 0; y < view.Height; y++)
		{
			for (uint32_t x = 0; x < view.Width; x++)
			{
				const glm::vec3 value = expected[static_cast<size_t>(y) * view.Width + x];
				if (value.r < 0.0f)
					continue;
				glm::vec3 shown = glm::vec3(ReadHalfTexel(hdr, x, y));
				if (subtract)
					shown -= glm::vec3(ReadHalfTexel(*subtract, x, y));
				agreement.Compared++;
				// What the reference changes within the rasterizer's placement of the point, looked at only where the
				// difference is beyond the half float's rounding.
				std::optional<glm::vec3> allowance;
				for (int channel = 0; channel < 3; channel++)
				{
					double difference = std::abs(static_cast<double>(shown[channel]) - value[channel]);
					if (difference > c_HalfRounding * value[channel] && !allowance)
					{
						allowance = glm::vec3(0.0f);
						for (const glm::dvec2 offset : { glm::dvec2(c_SubpixelStep, 0.0), glm::dvec2(-c_SubpixelStep, 0.0), glm::dvec2(0.0, c_SubpixelStep),
								 glm::dvec2(0.0, -c_SubpixelStep) })
						{
							glm::dvec3 moved(0.0);
							if (FindPlanePoint(*view.Camera, x, y, view.Width, view.Height, view.PlaneY, view.PlaneHalfSize, moved, offset))
								allowance = glm::max(*allowance, glm::abs(ShadeReference(view, moved, material, lights, lut) - value));
						}
					}
					if (allowance)
						difference = std::max(difference - static_cast<double>((*allowance)[channel]), 0.0);
					agreement.MaxAbsolute = std::max(agreement.MaxAbsolute, difference / std::max(agreement.Peak, 1e-30));
					if (value[channel] > c_RelativeFloor * agreement.Peak && difference / value[channel] > agreement.MaxRelative)
					{
						agreement.MaxRelative = difference / value[channel];
						agreement.WorstPixel = glm::uvec2(x, y);
						agreement.WorstValues = glm::vec2(shown[channel], value[channel]);
					}
				}
			}
		}
		return agreement;
	}

	ReadbackImage RenderHDR(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, const SceneRenderOptions& options = {})
	{
		REQUIRE(renderer.Render(scene, camera, nullptr, options));
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), image));
		return image;
	}

	ReadbackImage ReadBRDF(SceneRenderer& renderer)
	{
		ReadbackImage lut;
		REQUIRE(Renderer::ReadTexture(renderer.GetBRDFLut(), lut));
		return lut;
	}

	// An orthographic camera `height` meters high looking at `target` from the side `toCamera` points to.
	SceneCamera OrthographicFrom(const glm::vec3& target, const glm::vec3& toCamera, float height, float distance = 4.0f)
	{
		return OrthographicLookAt(target + glm::normalize(toCamera) * distance, target, height);
	}

	// The view direction (surface to camera) whose cosine to +Y is `cosine`, tilted toward +Z.
	glm::vec3 ViewWithCosine(float cosine)
	{
		return glm::vec3(0.0f, cosine, std::sqrt(std::max(1.0f - cosine * cosine, 0.0f)));
	}

}

namespace
{

	// The light the reference shades a view with: the plane's material and the scene's rect lights.
	std::vector<AreaLights::AreaLight> GetRelativeLights(std::span<const Entity> entities, const SceneCamera& camera)
	{
		std::vector<AreaLights::AreaLight> lights;
		for (const Entity entity : entities)
			lights.push_back(GetRelativeLight(entity, glm::dvec3(camera.Position)));
		return lights;
	}

	ReferenceView MakeView(const SceneCamera& camera, uint32_t width, uint32_t height, float planeHalfSize)
	{
		ReferenceView view;
		view.Camera = &camera;
		view.Width = width;
		view.Height = height;
		view.PlaneHalfSize = planeHalfSize;
		return view;
	}

	// How far the GPU's shading may be from the reference (CompareWithReference), 1.5 times the largest differences measured
	// (2026-11-10, RTX 5090: 1.21e-3 relative and 1.21e-3 of the peak over the first case's 331,776 pixels since faces are cut
	// at the receiver's horizon, 1.22e-3 and 1.20e-3 before; 1.04e-3 of the peak on wet ground): the HDR target's half floats
	// round to 4.9e-4 of a value, the rest is the GPU's division and square roots and its sampler's weights in the BRDF table.
	constexpr double c_AgreementRelative = 1.8e-3;
	constexpr double c_AgreementAbsolute = 1.8e-3;
	// What the fit's lobe keeps under the horizon of a white metal at grazing views, short of 1 under a light filling the sky
	// (AreaLights::SpecularFraction): 1.5 times the largest shortfall measured, 0.0879 (roughness 0.3 at a view cosine of 0.2;
	// 0.082 at 0.6 and 0.2, 0.058 at 0.6 and 0.5, 0.0205 at 0.3 and 0.5).
	constexpr double c_FurnaceMetalShortfall = 0.13;
	// How far the highlight's light at roughness 0.3 is from GGX's (the fit's error, Docs/DeveloperGuide.md "Area lights"):
	// 1.5 times the measured 0.0665 (it holds 0.9335).
	constexpr double c_RoughHighlightEnergy = 0.099;

	// The form factor from a point to a rectangle parallel to its surface, `height` above it, with a corner straight over it
	// and sides `a` and `b` (signed: the factor is odd in each, so rectangles anywhere over the point add up from four):
	// Howell's catalogue, C-11.
	double CornerFormFactor(double a, double b, double height)
	{
		const double x = a / height;
		const double y = b / height;
		const double sx = std::sqrt(1.0 + x * x);
		const double sy = std::sqrt(1.0 + y * y);
		return (x / sx * std::atan(y / sx) + y / sy * std::atan(x / sy)) / (2.0 * c_Pi);
	}

	// The rectangle [x0, x1] x [z0, z1] (relative to the point) `height` over the point, parallel to its surface.
	double ParallelFormFactor(double x0, double x1, double z0, double z1, double height)
	{
		return CornerFormFactor(x1, z1, height) - CornerFormFactor(x0, z1, height) - CornerFormFactor(x1, z0, height) + CornerFormFactor(x0, z0, height);
	}

	// GGX's specular radiance (the engine's BRDF: Include/PBR.slang, without the compensation) from a light's face at a surface
	// point, by a midpoint quadrature of `steps` x `steps` cells over the face: the reference of the fit's own error.
	double QuadratureSpecular(const AreaLights::AreaLight& light, const glm::dvec3& position, const glm::dvec3& normal, const glm::dvec3& view, double roughness,
		const glm::dvec3& f0, uint32_t steps)
	{
		const double alpha = std::max(roughness, static_cast<double>(AreaLights::c_MinRoughness));
		const double alpha2 = alpha * alpha * alpha * alpha;
		const glm::dvec3 axisX(light.AxisX);
		const glm::dvec3 axisY(light.AxisY);
		const double cellArea = 4.0 * glm::length(axisX) * glm::length(axisY) / (static_cast<double>(steps) * steps);
		const double noV = std::max(glm::dot(normal, view), 1e-4);
		double sum = 0.0;
		for (uint32_t row = 0; row < steps; row++)
		{
			for (uint32_t column = 0; column < steps; column++)
			{
				const double u = (static_cast<double>(column) + 0.5) / steps * 2.0 - 1.0;
				const double v = (static_cast<double>(row) + 0.5) / steps * 2.0 - 1.0;
				const glm::dvec3 point = glm::dvec3(light.Center) + axisX * u + axisY * v;
				const glm::dvec3 toLight = point - position;
				const double distanceSquared = glm::dot(toLight, toLight);
				const glm::dvec3 direction = toLight / std::sqrt(distanceSquared);
				const double noL = glm::dot(normal, direction);
				const double emitted = -glm::dot(glm::dvec3(light.Normal), direction);
				if (noL <= 0.0 || emitted <= 0.0)
					continue;
				const glm::dvec3 half = glm::normalize(view + direction);
				const double noH = std::clamp(glm::dot(normal, half), 0.0, 1.0);
				const double voH = std::clamp(glm::dot(view, half), 0.0, 1.0);
				const double f = (noH * alpha2 - noH) * noH + 1.0;
				const double distribution = alpha2 / (c_Pi * f * f);
				const double lambdaV = noL * std::sqrt((noV - noV * alpha2) * noV + alpha2);
				const double lambdaL = noV * std::sqrt((noL - noL * alpha2) * noL + alpha2);
				const double visibility = 0.5 / std::max(lambdaV + lambdaL, 1e-5);
				const double fresnel = std::pow(1.0 - voH, 5.0);
				const double reflectance = f0.g + (1.0 - f0.g) * fresnel;
				sum += distribution * visibility * reflectance * noL * emitted * cellArea / distanceSquared;
			}
		}
		return sum * light.Radiance.g;
	}

	// Where a row (or column) of an image crosses half of `peak` going out from `inside` toward `outside` (pixel coordinates of
	// pixel centers' line, sub-pixel by linear interpolation), or a negative value if it never does.
	double FindHalfPeakCrossing(const ReadbackImage& hdr, bool alongRow, uint32_t line, int inside, int outside, float peak)
	{
		const int step = outside > inside ? 1 : -1;
		float previous = 0.0f;
		for (int position = inside; position != outside + step; position += step)
		{
			const uint32_t x = alongRow ? static_cast<uint32_t>(position) : line;
			const uint32_t y = alongRow ? line : static_cast<uint32_t>(position);
			const float value = ReadHalfTexel(hdr, x, y).g;
			if (position != inside && value < 0.5f * peak && previous >= 0.5f * peak)
			{
				const double fraction = (previous - 0.5 * peak) / (previous - value);
				return static_cast<double>(position - step) + 0.5 + fraction * step;
			}
			previous = value;
		}
		return -1.0;
	}

	// Where a projected line from `a` to `b` (pixel coordinates) crosses the row y (or the column x), or a negative value.
	double CrossLine(const glm::dvec2& a, const glm::dvec2& b, bool alongRow, double at)
	{
		const double from = alongRow ? a.y : a.x;
		const double to = alongRow ? b.y : b.x;
		if ((at - from) * (at - to) > 0.0 || from == to)
			return -1.0;
		const double t = (at - from) / (to - from);
		return alongRow ? a.x + (b.x - a.x) * t : a.y + (b.y - a.y) * t;
	}

	glm::dvec2 ProjectToPixelCoordinates(const SceneCamera& camera, const glm::dvec3& point, uint32_t width, uint32_t height)
	{
		const glm::dvec4 clip = glm::dmat4(camera.Projection) * glm::dmat4(camera.View) * glm::dvec4(point, 1.0);
		const glm::dvec2 ndc = glm::dvec2(clip) / clip.w;
		return glm::dvec2((ndc.x * 0.5 + 0.5) * width, (0.5 - ndc.y * 0.5) * height);
	}

	MaterialProperties DarkUnlit()
	{
		return UnlitColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
	}

}

TEST_SUITE("GPU.SceneRenderer.AreaLights")
{
	TEST_CASE("Rect lights shade every pixel as the CPU reference does: roughness, view, metal, dielectric, coat, two sides and the horizon")
	{
		// Three lights over a plane (one tilted, one two-sided standing across the plane's horizon, one 5 cm across), seen from
		// views whose cosine to the plane runs from 1 to 0.05, for every roughness the reference's tests take (T0.4): the GPU's
		// image equals AreaLights::Shade at every pixel of the plane.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_ViewSize = 64;
		constexpr float c_PlaneHalfSize = 100.0f;
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		std::optional<ReadbackImage> lut;
		Agreement worst;
		uint32_t compared = 0;
		for (const float roughness : { 0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f })
		{
			for (const float cosine : { 1.0f, 0.8f, 0.5f, 0.2f, 0.05f })
			{
				for (int kind = 0; kind < 3; kind++)
				{
					CAPTURE(roughness);
					CAPTURE(cosine);
					CAPTURE(kind);
					PlaneMaterial material;
					material.Roughness = roughness;
					if (kind == 1)
					{
						material.Albedo = glm::vec3(0.95f, 0.64f, 0.54f); // Copper
						material.Metallic = 1.0f;
					}
					else if (kind == 2)
					{
						material.Albedo = glm::vec3(0.2f, 0.05f, 0.03f); // A coated paint
						material.Clearcoat = 1.0f;
						material.ClearcoatRoughness = std::min(roughness, 0.3f);
					}
					Scene scene;
					AddNeutralPostProcess(scene);
					AddDarkSky(scene);
					AddPlane(scene, assets, material, 0.0f, 2.0f * c_PlaneHalfSize);
					const std::vector<Entity> entities = {
						AddRectLight(scene, glm::vec3(0.3f, 0.6f, -0.2f), glm::vec3(0.34f, -0.94f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 1.0f, 0.5f, 800.0f, 20.0f),
						AddRectLight(scene, glm::vec3(-0.8f, 0.15f, 0.4f), glm::vec3(1.0f, -0.3f, 0.5f), glm::vec3(0.0f, 1.0f, 0.3f), 0.8f, 0.6f, 300.0f, 20.0f, true),
						AddRectLight(scene, glm::vec3(0.9f, 1.4f, 0.8f), glm::vec3(-0.3f, -1.0f, -0.2f), glm::vec3(1.0f, 0.0f, 0.0f), 0.05f, 0.05f, 40000.0f, 20.0f),
					};
					// An azimuth that lines the view up with none of the lights' axes.
					const glm::vec3 toCamera = glm::angleAxis(glm::radians(35.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * ViewWithCosine(cosine);
					const SceneCamera camera = OrthographicFrom(glm::vec3(0.0f), toCamera, 3.0f, 6.0f);
					const ReadbackImage hdr = RenderHDR(renderer, scene, camera);
					if (!lut)
						lut = ReadBRDF(renderer);
					CHECK(GetLightCounts(renderer).AreaLights == 3);
					const Agreement agreement = CompareWithReference(MakeView(camera, c_ViewSize, c_ViewSize, c_PlaneHalfSize), hdr, material,
						GetRelativeLights(entities, camera), *lut);
					// Grazing views show the plane in front of the near plane only.
					CHECK(agreement.Compared > c_ViewSize * c_ViewSize / 2);
					CHECK(agreement.Peak > 0.0);
					CAPTURE(agreement.WorstPixel);
					CAPTURE(agreement.WorstValues);
					CHECK(agreement.MaxRelative <= c_AgreementRelative);
					worst.MaxRelative = std::max(worst.MaxRelative, agreement.MaxRelative);
					worst.MaxAbsolute = std::max(worst.MaxAbsolute, agreement.MaxAbsolute);
					compared += agreement.Compared;
				}
			}
		}
		MESSAGE(fmt::format("Rect lights against the reference over {} pixels: largest relative difference {:.3g} (above {} of the peak), largest "
			"absolute {:.3g} of the peak", compared, worst.MaxRelative, c_RelativeFloor, worst.MaxAbsolute));
		CHECK(worst.MaxRelative <= c_AgreementRelative);
		CHECK(worst.MaxAbsolute <= c_AgreementAbsolute);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A rect light under a surface's horizon puts no ghost of its face on it")
	{
		// A polished metal floor and lights under its horizon on the viewer's side (where the fitted lobe used to reach under the
		// surface and draw a second image of each light) and straight under it: GGX reflects nothing from under the surface, so
		// every pixel stays exactly black. The same lights raised over the floor light it, so the check is not blind.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_ViewSize = 64;
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		for (const float roughness : { 0.05f, 0.2f })
		{
			for (const float cosine : { 0.05f, 0.2f, 0.5f })
			{
				for (const float side : { -1.0f, 1.0f })
				{
					CAPTURE(roughness);
					CAPTURE(cosine);
					CAPTURE(side);
					PlaneMaterial material;
					material.Albedo = glm::vec3(0.95f);
					material.Metallic = 1.0f;
					material.Roughness = roughness;
					Scene scene;
					AddNeutralPostProcess(scene);
					AddDarkSky(scene);
					AddPlane(scene, assets, material, 0.0f, 200.0f);
					const glm::vec3 toCamera = glm::angleAxis(glm::radians(35.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * ViewWithCosine(cosine);
					const glm::vec3 viewSide = glm::normalize(glm::vec3(toCamera.x, 0.0f, toCamera.z));
					// Every corner of each face is at least 25 cm under the floor (side -1), or over it (side 1).
					const glm::vec3 softbox = viewSide * 2.0f + glm::vec3(0.0f, 0.8f * side, 0.0f);
					AddRectLight(scene, softbox, -softbox, glm::vec3(0.0f, 1.0f, 0.0f), 0.8f, 1.2f, 3000.0f, 20.0f);
					const glm::vec3 strip = viewSide * 1.2f + glm::vec3(0.0f, 0.3f * side, 0.0f);
					AddRectLight(scene, strip, glm::vec3(-viewSide.x, -0.2f * side, -viewSide.z), glm::cross(viewSide, glm::vec3(0.0f, 1.0f, 0.0f)), 1.6f, 0.1f,
						3000.0f, 20.0f);
					AddRectLight(scene, glm::vec3(0.3f, 0.5f * side, -0.2f), glm::vec3(0.0f, -side, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 1.0f, 1.0f, 3000.0f, 20.0f);
					const SceneCamera camera = OrthographicFrom(glm::vec3(0.0f), toCamera, 3.0f, 6.0f);
					const ReadbackImage hdr = RenderHDR(renderer, scene, camera);
					CHECK(GetLightCounts(renderer).AreaLights == 3);
					float brightest = 0.0f;
					for (uint32_t y = 0; y < hdr.Height; y++)
					{
						for (uint32_t x = 0; x < hdr.Width; x++)
						{
							const glm::vec4 value = ReadHalfTexel(hdr, x, y);
							brightest = std::max(brightest, std::max(value.r, std::max(value.g, value.b)));
						}
					}
					MESSAGE(fmt::format("Lights {} the floor's horizon, roughness {}, view cosine {}: the brightest pixel {:.4g} nits", side < 0.0f ? "under" : "over",
						roughness, cosine, brightest));
					if (side < 0.0f)
						CHECK(brightest == 0.0f);
					else
						CHECK(brightest > 10.0f);
				}
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A floor under a rect light takes the analytic form factor's light")
	{
		// A white floor (albedo 0.8, roughness 1) under a 1 x 0.5 m one-sided light of 1,000 nits 1 m above it, seen from above:
		// at 25 points the diffuse light is albedo (1 - Es) L FF with FF from Howell's catalogue (the face parallel to the floor),
		// plus the highlight the reference gives. Before rect lights were shaded the floor was black.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_ViewSize = 128;
		constexpr float c_Height = 3.0f; // Meters the view shows
		PlaneMaterial material;
		material.Roughness = 1.0f;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddDarkSky(scene);
		AddPlane(scene, assets, material, 0.0f, 20.0f);
		// X along the world's X, its height along the world's -Z: the face spans x in [-0.5, 0.5] and z in [-0.25, 0.25].
		const Entity entity = AddRectLight(scene, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 1.0f, 0.5f, 1000.0f, 20.0f);
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		const SceneCamera camera = OrthographicLookAt(glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.0f), c_Height);
		const ReadbackImage hdr = RenderHDR(renderer, scene, camera);
		const ReadbackImage lut = ReadBRDF(renderer);
		const glm::vec2 dfg = SampleBRDF(lut, glm::vec2(1.0f, 1.0f));
		const float es = 0.04f * dfg.x + dfg.y;
		const std::vector<AreaLights::AreaLight> lights = { GetRelativeLight(entity, glm::dvec3(camera.Position)) };
		PlaneMaterial black = material;
		black.Albedo = glm::vec3(0.0f); // The reference's highlight alone
		ReferenceView view = MakeView(camera, c_ViewSize, c_ViewSize, 10.0f);
		double worst = 0.0;
		for (int row = 0; row < 5; row++)
		{
			for (int column = 0; column < 5; column++)
			{
				const glm::vec3 target(-1.0f + 0.5f * static_cast<float>(column), 0.0f, -0.8f + 0.4f * static_cast<float>(row));
				const glm::uvec2 pixel = ProjectToPixel(camera, target, c_ViewSize);
				glm::dvec3 relative(0.0);
				REQUIRE(FindPlanePoint(camera, pixel.x, pixel.y, c_ViewSize, c_ViewSize, 0.0f, 10.0f, relative));
				const glm::dvec3 point = relative + glm::dvec3(camera.Position);
				const double formFactor = ParallelFormFactor(-0.5 - point.x, 0.5 - point.x, -0.25 - point.z, 0.25 - point.z, 1.0);
				const double diffuse = 0.8 * (1.0 - es) * 1000.0 * formFactor;
				const double specular = ShadeReference(view, relative, black, lights, lut).g;
				const double shown = ReadHalfTexel(hdr, pixel.x, pixel.y).g;
				CAPTURE(point);
				CHECK(shown == doctest::Approx(diffuse + specular).epsilon(0.01));
				worst = std::max(worst, std::abs(shown - diffuse - specular) / (diffuse + specular));
			}
		}
		MESSAGE(fmt::format("The floor against the analytic form factor: largest relative difference {:.3g}", worst));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A white furnace under a rect light filling the sky reflects the light it receives, but for the fit's lobe under the horizon")
	{
		// A one-sided light of 1 nit, 2 km across and 1 m above a plane, fills its sky: a surface keeping its energy reflects 1.
		// Diffuse surfaces and coats do within 3% (5% at grazing views); metals keep their lobe's share above the horizon, which
		// the fitted lobe falls short of at grazing views of middling roughness (AreaLights::SpecularFraction), as the reference
		// says. Every reading equals the reference.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_ViewSize = 32;
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		struct FurnaceCase
		{
			const char* Name;
			PlaneMaterial Material;
		};
		const auto white = [](float roughness, float metallic, float clearcoat)
		{
			PlaneMaterial material;
			material.Albedo = glm::vec3(1.0f);
			material.Roughness = roughness;
			material.Metallic = metallic;
			material.Clearcoat = clearcoat;
			material.ClearcoatRoughness = clearcoat > 0.0f ? 0.1f : 0.0f;
			return material;
		};
		const FurnaceCase cases[] = {
			{ "dielectric 0.2", white(0.2f, 0.0f, 0.0f) },
			{ "dielectric 0.5", white(0.5f, 0.0f, 0.0f) },
			{ "dielectric 1", white(1.0f, 0.0f, 0.0f) },
			{ "metal 0.05", white(0.05f, 1.0f, 0.0f) },
			{ "metal 0.3", white(0.3f, 1.0f, 0.0f) },
			{ "metal 0.6", white(0.6f, 1.0f, 0.0f) },
			{ "metal 1", white(1.0f, 1.0f, 0.0f) },
			{ "coated", white(0.1f, 0.0f, 1.0f) },
		};
		double worstMetal = 0.0;
		double worstOther = 0.0;
		double worstGrazingOther = 0.0;
		for (const FurnaceCase& furnace : cases)
		{
			for (const float cosine : { 1.0f, 0.5f, 0.2f })
			{
				CAPTURE(furnace.Name);
				CAPTURE(cosine);
				Scene scene;
				AddNeutralPostProcess(scene);
				AddDarkSky(scene);
				AddPlane(scene, assets, furnace.Material, 0.0f, 400.0f);
				const Entity entity = AddRectLight(scene, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 2000.0f, 2000.0f,
					1.0f, 1000.0f);
				const SceneCamera camera = OrthographicFrom(glm::vec3(0.0f), ViewWithCosine(cosine), 0.5f, 0.6f);
				const ReadbackImage hdr = RenderHDR(renderer, scene, camera);
				const ReadbackImage lut = ReadBRDF(renderer);
				const std::vector<AreaLights::AreaLight> lights = { GetRelativeLight(entity, glm::dvec3(camera.Position)) };
				const Agreement agreement = CompareWithReference(MakeView(camera, c_ViewSize, c_ViewSize, 200.0f), hdr, furnace.Material, lights, lut);
				CHECK(agreement.MaxRelative <= c_AgreementRelative);
				const double reading = ReadHalfTexel(hdr, c_ViewSize / 2, c_ViewSize / 2).g;
				const double shortfall = std::abs(1.0 - reading);
				MESSAGE(fmt::format("Furnace, {} at a view cosine of {}: {:.4f}", furnace.Name, cosine, reading));
				if (furnace.Material.Metallic > 0.0f)
					worstMetal = std::max(worstMetal, shortfall);
				else if (cosine < 0.3f)
					worstGrazingOther = std::max(worstGrazingOther, shortfall);
				else
					worstOther = std::max(worstOther, shortfall);
			}
		}
		MESSAGE(fmt::format("Furnace shortfalls: metals {:.4f}, others {:.4f} ({:.4f} at a view cosine of 0.2)", worstMetal, worstOther, worstGrazingOther));
		CHECK(worstOther <= 0.03);
		CHECK(worstGrazingOther <= 0.05);
		CHECK(worstMetal <= c_FurnaceMetalShortfall);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A polished floor mirrors a rect light's face with crisp edges, and a rough one spreads the reference's light")
	{
		// A metal floor (F0 0.9) at the least roughness under a 0.5 x 0.25 m light of 1,000 nits, seen in perspective at 1920 x
		// 1080: the pixels above half the highlight's peak are the face's mirror image within 1.5 pixels along each edge, and the
		// center is L (F0 dfg.x + dfg.y) E. At roughness 0.3 the highlight's light (its sum) is GGX's (a quadrature over the face)
		// within the fit's error.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		const glm::vec3 lightCenter(0.0f, 1.2f, -1.5f);
		const glm::vec3 eye(0.0f, 0.6f, 1.0f);
		// The floor point where the mirror image's center shows: on the line to the reflected center.
		const glm::vec3 mirrored(lightCenter.x, -lightCenter.y, lightCenter.z);
		const glm::vec3 target = eye + (mirrored - eye) * (eye.y / (eye.y - mirrored.y));
		PlaneMaterial material;
		material.Albedo = glm::vec3(0.9f);
		material.Metallic = 1.0f;
		material.Roughness = AreaLights::c_MinRoughness;
		{
			constexpr uint32_t c_Width = 1920;
			constexpr uint32_t c_Height = 1080;
			Scene scene;
			AddNeutralPostProcess(scene);
			AddDarkSky(scene);
			AddPlane(scene, assets, material, 0.0f, 40.0f);
			const Entity entity = AddRectLight(scene, lightCenter, glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 0.5f, 0.25f, 1000.0f, 20.0f);
			SceneRenderer renderer(ExactSpecification());
			renderer.SetViewportSize(c_Width, c_Height);
			const SceneCamera camera = LookAt(eye, target, static_cast<float>(c_Width) / static_cast<float>(c_Height));
			const ReadbackImage hdr = RenderHDR(renderer, scene, camera);
			const ReadbackImage lut = ReadBRDF(renderer);
			const AreaLights::AreaLight light = GetRelativeLight(entity, glm::dvec3(camera.Position));

			// The mirror image's corners in pixel coordinates.
			const glm::dvec3 center(lightCenter);
			const glm::dvec3 axisX(light.AxisX);
			const glm::dvec3 axisY(light.AxisY);
			const auto reflect = [](glm::dvec3 point) { point.y = -point.y; return point; };
			const glm::dvec2 corners[4] = {
				ProjectToPixelCoordinates(camera, reflect(center - axisX - axisY), c_Width, c_Height),
				ProjectToPixelCoordinates(camera, reflect(center + axisX - axisY), c_Width, c_Height),
				ProjectToPixelCoordinates(camera, reflect(center + axisX + axisY), c_Width, c_Height),
				ProjectToPixelCoordinates(camera, reflect(center - axisX + axisY), c_Width, c_Height),
			};
			glm::dvec2 low = corners[0];
			glm::dvec2 high = corners[0];
			for (const glm::dvec2& corner : corners)
			{
				low = glm::min(low, corner);
				high = glm::max(high, corner);
			}
			const glm::uvec2 middle = ProjectToPixel(camera, target, c_Width, c_Height);
			const float peak = ReadHalfTexel(hdr, middle.x, middle.y).g;
			// The center: the lobe lies wholly within the face.
			glm::dvec3 relative(0.0);
			REQUIRE(FindPlanePoint(camera, middle.x, middle.y, c_Width, c_Height, 0.0f, 20.0f, relative));
			const glm::vec3 view = glm::vec3(glm::normalize(-relative));
			const glm::vec2 dfg = SampleBRDF(lut, glm::vec2(std::max(view.y, 1e-4f), material.Roughness));
			const glm::vec3 f0(0.9f);
			const float expected = 1000.0f * (f0.g * dfg.x + dfg.y) * GetSpecularEnergy(f0, dfg).g;
			CHECK(peak == doctest::Approx(expected).epsilon(0.05));
			// Each edge where rows (or columns) in the middle three fifths of the image cross it.
			double worstEdge = 0.0;
			uint32_t edgesMeasured = 0;
			for (const bool alongRow : { true, false })
			{
				const double from = alongRow ? low.y : low.x;
				const double to = alongRow ? high.y : high.x;
				for (int sample = 0; sample < 5; sample++)
				{
					const double at = from + (to - from) * (0.2 + 0.15 * sample);
					const uint32_t line = static_cast<uint32_t>(at);
					const double pixelCenter = static_cast<double>(line) + 0.5;
					std::vector<double> crossings;
					for (uint32_t edge = 0; edge < 4; edge++)
					{
						const double crossing = CrossLine(corners[edge], corners[(edge + 1) % 4], alongRow, pixelCenter);
						if (crossing >= 0.0)
							crossings.push_back(crossing);
					}
					REQUIRE(crossings.size() == 2);
					std::sort(crossings.begin(), crossings.end());
					const int inside = static_cast<int>((crossings[0] + crossings[1]) * 0.5);
					const int limit = static_cast<int>(alongRow ? c_Width : c_Height) - 1;
					const double first = FindHalfPeakCrossing(hdr, alongRow, line, inside, 0, peak);
					const double second = FindHalfPeakCrossing(hdr, alongRow, line, inside, limit, peak);
					REQUIRE(first >= 0.0);
					REQUIRE(second >= 0.0);
					worstEdge = std::max({ worstEdge, std::abs(first - crossings[0]), std::abs(second - crossings[1]) });
					edgesMeasured += 2;
				}
			}
			MESSAGE(fmt::format("The mirror image's edges: {} crossings, the largest {:.3f} pixels from the analytic edge; center {:.2f} nits "
				"(expected {:.2f})", edgesMeasured, worstEdge, peak, expected));
			CHECK(worstEdge <= 1.5);
			const Agreement agreement = CompareWithReference(MakeView(camera, c_Width, c_Height, 20.0f), hdr, material, std::vector<AreaLights::AreaLight> { light }, lut);
			CHECK(agreement.MaxRelative <= c_AgreementRelative);
		}
		{
			// Roughness 0.3: the highlight's sum against the quadrature of GGX over the face, at every other pixel.
			constexpr uint32_t c_Width = 480;
			constexpr uint32_t c_Height = 270;
			material.Roughness = 0.3f;
			Scene scene;
			AddNeutralPostProcess(scene);
			AddDarkSky(scene);
			AddPlane(scene, assets, material, 0.0f, 40.0f);
			const Entity entity = AddRectLight(scene, lightCenter, glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 0.5f, 0.25f, 1000.0f, 20.0f);
			SceneRenderer renderer(ExactSpecification());
			renderer.SetViewportSize(c_Width, c_Height);
			const SceneCamera camera = LookAt(eye, target, static_cast<float>(c_Width) / static_cast<float>(c_Height));
			const ReadbackImage hdr = RenderHDR(renderer, scene, camera);
			const ReadbackImage lut = ReadBRDF(renderer);
			const AreaLights::AreaLight light = GetRelativeLight(entity, glm::dvec3(camera.Position));
			double shownSum = 0.0;
			double quadratureSum = 0.0;
			for (uint32_t y = 0; y < c_Height; y += 2)
			{
				for (uint32_t x = 0; x < c_Width; x += 2)
				{
					glm::dvec3 relative(0.0);
					if (!FindPlanePoint(camera, x, y, c_Width, c_Height, 0.0f, 20.0f, relative))
						continue;
					const glm::dvec3 view = glm::normalize(-relative);
					const float noV = std::max(static_cast<float>(view.y), 1e-4f);
					const glm::vec2 dfg = SampleBRDF(lut, glm::vec2(noV, material.Roughness));
					const double energy = GetSpecularEnergy(glm::vec3(0.9f), dfg).g;
					shownSum += ReadHalfTexel(hdr, x, y).g;
					quadratureSum += QuadratureSpecular(light, relative, glm::dvec3(0.0, 1.0, 0.0), view, material.Roughness, glm::dvec3(0.9), 32) * energy;
				}
			}
			const double ratio = shownSum / quadratureSum;
			MESSAGE(fmt::format("The highlight at roughness 0.3 holds {:.4f} of GGX's light over the face", ratio));
			CHECK(std::abs(ratio - 1.0) <= c_RoughHighlightEnergy);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Wet ground shows a rect light's streak, on a smooth dark dielectric and from a clear coat over a rough base")
	{
		// The look-dev's wet patch (base color 0.05, roughness 0.06) and a coated form (a coat of roughness 0.05 over the same
		// base at roughness 0.8) under a 1.2 x 0.3 m light of 2,000 nits 2.5 m up, tilted 30 degrees, seen from 0.4 m above the
		// ground: each streak is the reference's. Without the coat's lobe the coated form shows only the rough base's faint glow.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_Width = 320;
		constexpr uint32_t c_Height = 180;
		const auto render = [&](const PlaneMaterial& material, ReadbackImage& outLut, AreaLights::AreaLight& outLight, SceneCamera& outCamera)
		{
			Scene scene;
			AddNeutralPostProcess(scene);
			AddDarkSky(scene);
			AddPlane(scene, assets, material, 0.0f, 60.0f);
			const glm::vec3 direction = glm::vec3(0.0f, -std::cos(glm::radians(30.0f)), std::sin(glm::radians(30.0f)));
			const Entity entity = AddRectLight(scene, glm::vec3(0.0f, 2.5f, -6.0f), direction, glm::vec3(1.0f, 0.0f, 0.0f), 1.2f, 0.3f, 2000.0f, 30.0f);
			SceneRenderer renderer(ExactSpecification());
			renderer.SetViewportSize(c_Width, c_Height);
			outCamera = LookAt(glm::vec3(0.0f, 0.4f, 2.0f), glm::vec3(0.0f, 0.0f, -3.0f), static_cast<float>(c_Width) / static_cast<float>(c_Height));
			const ReadbackImage hdr = RenderHDR(renderer, scene, outCamera);
			outLut = ReadBRDF(renderer);
			outLight = GetRelativeLight(entity, glm::dvec3(outCamera.Position));
			return hdr;
		};
		PlaneMaterial wet;
		wet.Albedo = glm::vec3(0.05f);
		wet.Roughness = 0.06f;
		PlaneMaterial coated;
		coated.Albedo = glm::vec3(0.05f);
		coated.Roughness = 0.8f;
		coated.Clearcoat = 1.0f;
		coated.ClearcoatRoughness = 0.05f;
		PlaneMaterial base = coated;
		base.Clearcoat = 0.0f;
		float peaks[3] = {};
		const PlaneMaterial materials[3] = { wet, coated, base };
		for (int index = 0; index < 3; index++)
		{
			CAPTURE(index);
			ReadbackImage lut;
			AreaLights::AreaLight light;
			SceneCamera camera;
			const ReadbackImage hdr = render(materials[index], lut, light, camera);
			const Agreement agreement = CompareWithReference(MakeView(camera, c_Width, c_Height, 30.0f), hdr, materials[index], std::vector<AreaLights::AreaLight> { light }, lut);
			CHECK(agreement.Compared > c_Width * c_Height / 3);
			CHECK(agreement.MaxRelative <= c_AgreementRelative);
			CHECK(agreement.MaxAbsolute <= c_AgreementAbsolute);
			peaks[index] = static_cast<float>(agreement.Peak);
		}
		MESSAGE(fmt::format("Streak peaks: wet {:.1f} nits, coated {:.1f}, the coated form's base alone {:.2f}", peaks[0], peaks[1], peaks[2]));
		CHECK(peaks[0] > 50.0f);
		CHECK(peaks[1] > 10.0f * peaks[2]);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Rect lights light what their range reaches from behind the camera, nothing behind a one-sided face, both sides of a two-sided one alike")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_ViewSize = 64;
		PlaneMaterial material;
		material.Roughness = 0.4f;
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_ViewSize, c_ViewSize);

		// A light behind the camera whose range reaches into the view lights the floor there; one whose range ends before the
		// view is culled and lights nothing.
		{
			Scene scene;
			AddNeutralPostProcess(scene);
			AddDarkSky(scene);
			AddPlane(scene, assets, material, 0.0f, 60.0f);
			const Entity behind = AddRectLight(scene, glm::vec3(0.0f, 1.5f, 4.0f), glm::vec3(0.0f, -0.4f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f), 1.0f, 1.0f, 500.0f, 8.0f);
			const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 2.0f), glm::vec3(0.0f, 0.0f, -1.0f));
			const ReadbackImage hdr = RenderHDR(renderer, scene, camera);
			const ReadbackImage lut = ReadBRDF(renderer);
			CHECK(GetLightCounts(renderer).AreaLights == 1);
			const Agreement agreement = CompareWithReference(MakeView(camera, c_ViewSize, c_ViewSize, 30.0f), hdr, material,
				std::vector<AreaLights::AreaLight> { GetRelativeLight(behind, glm::dvec3(camera.Position)) }, lut);
			CHECK(agreement.Peak > 1.0);
			CHECK(agreement.MaxRelative <= c_AgreementRelative);

			scene.GetRegistry().patch<RectLightComponent>(behind.GetHandle(), [](RectLightComponent& light) { light.Range = 1.0f; });
			const ReadbackImage dark = RenderHDR(renderer, scene, camera);
			CHECK(GetLightCounts(renderer).AreaLights == 0);
			CHECK(GetLightCounts(renderer).CulledLights == 1);
			for (uint32_t y = 0; y < c_ViewSize; y++)
			{
				for (uint32_t x = 0; x < c_ViewSize; x++)
					REQUIRE(ReadHalfTexel(dark, x, y).g == 0.0f);
			}
		}

		// A wall light at x = 0 shining along +X, seen from above: nothing at all behind it (exactly 0); two-sided, the two sides
		// mirror each other; under a mirroring parent it lights as its twin does.
		const auto renderWall = [&](bool twoSided, bool mirrored, std::vector<AreaLights::AreaLight>& outLights, SceneCamera& outCamera, ReadbackImage& outLut)
		{
			Scene scene;
			AddNeutralPostProcess(scene);
			AddDarkSky(scene);
			AddPlane(scene, assets, material, 0.0f, 60.0f);
			Entity wall = AddRectLight(scene, glm::vec3(0.0f, 0.6f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 1.0f, 0.8f, 400.0f, 10.0f, twoSided);
			if (mirrored)
			{
				// A parent mirroring X, and the light turned so that its world transform shines along +X as before.
				Entity parent = scene.CreateEntity("Mirror");
				parent.GetComponent<TransformComponent>().Scale = glm::vec3(-1.0f, 1.0f, 1.0f);
				REQUIRE(scene.SetParent(wall, parent, false));
				wall.GetComponent<TransformComponent>().Rotation = FacingRotation(glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
				wall.MarkModified<TransformComponent>();
			}
			outCamera = OrthographicLookAt(glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.0f), 3.0f);
			const ReadbackImage hdr = RenderHDR(renderer, scene, outCamera);
			outLut = ReadBRDF(renderer);
			scene.UpdateWorldTransforms();
			outLights = { GetRelativeLight(wall, glm::dvec3(outCamera.Position)) };
			return hdr;
		};
		std::vector<AreaLights::AreaLight> lights;
		SceneCamera camera;
		ReadbackImage lut;
		const ReadbackImage oneSided = renderWall(false, false, lights, camera, lut);
		CHECK(CompareWithReference(MakeView(camera, c_ViewSize, c_ViewSize, 30.0f), oneSided, material, lights, lut).MaxRelative <= c_AgreementRelative);
		const ReadbackImage twoSided = renderWall(true, false, lights, camera, lut);
		CHECK(CompareWithReference(MakeView(camera, c_ViewSize, c_ViewSize, 30.0f), twoSided, material, lights, lut).MaxRelative <= c_AgreementRelative);
		const ReadbackImage mirrored = renderWall(false, true, lights, camera, lut);
		CHECK(glm::dot(lights[0].Normal, glm::vec3(1.0f, 0.0f, 0.0f)) == doctest::Approx(1.0f));
		uint32_t front = 0;
		uint32_t back = 0;
		double worstSides = 0.0;
		double worstTwin = 0.0;
		for (uint32_t y = 0; y < c_ViewSize; y++)
		{
			for (uint32_t x = 0; x < c_ViewSize; x++)
			{
				glm::dvec3 relative(0.0);
				REQUIRE(FindPlanePoint(camera, x, y, c_ViewSize, c_ViewSize, 0.0f, 30.0f, relative));
				const float value = ReadHalfTexel(oneSided, x, y).g;
				const float twin = ReadHalfTexel(mirrored, x, y).g;
				if (relative.x < 0.0)
				{
					CHECK(value == 0.0f);
					CHECK(twin == 0.0f);
					back++;
					// The pixel mirrored across the light's plane (the view is centered on it).
					const float side = ReadHalfTexel(twoSided, x, y).g;
					const float other = ReadHalfTexel(twoSided, c_ViewSize - 1 - x, y).g;
					worstSides = std::max(worstSides, std::abs(static_cast<double>(side) - other) / std::max(static_cast<double>(other), 1e-6));
				}
				else if (value > 0.0f)
				{
					front++;
					worstTwin = std::max(worstTwin, std::abs(static_cast<double>(twin) - value) / value);
				}
			}
		}
		MESSAGE(fmt::format("Sides: {} pixels behind the one-sided face (all 0), {} lit; two-sided sides differ by {:.3g}, the mirrored twin by {:.3g}",
			back, front, worstSides, worstTwin));
		CHECK(back > 0);
		CHECK(front > 0);
		// The two pixels' surface points (where the rasterizer places them) and the order of their operations differ slightly,
		// so their values may round to neighboring half floats: one step, 2^-10 (9.8e-4) of a value at most.
		CHECK(worstSides <= 1.0e-3);
		CHECK(worstTwin <= 1.0e-3);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A rect light's range fades its light to nothing at Range from its face, without a step")
	{
		// A wall light at x = 0 shining along +X, its lower edge 5 cm above the floor: along the floor's line through its middle
		// the light reaches 0 where the floor is Range from the face, and fades smoothly before it.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_Width = 512;
		constexpr uint32_t c_Height = 16;
		constexpr float c_Range = 3.0f;
		PlaneMaterial material;
		material.Roughness = 1.0f;
		Scene scene;
		AddNeutralPostProcess(scene);
		AddDarkSky(scene);
		AddPlane(scene, assets, material, 0.0f, 60.0f);
		const Entity wall = AddRectLight(scene, glm::vec3(0.0f, 0.55f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 1.0f, 1.0f, 400.0f, c_Range);
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_Width, c_Height);
		// Looking down at the line x in [-0.2, 3.8] (z = 0), the image's rows along +X.
		SceneCamera camera;
		camera.View = glm::lookAt(glm::vec3(1.8f, 3.0f, 0.0f), glm::vec3(1.8f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
		camera.Projection = Math::OrthographicReverseZ(-2.0f, 2.0f, -2.0f / 32.0f, 2.0f / 32.0f, 0.1f, 100.0f);
		camera.Position = glm::vec3(1.8f, 3.0f, 0.0f);
		camera.Orthographic = true;
		camera.Near = 0.1f;
		camera.Far = 100.0f;
		const ReadbackImage hdr = RenderHDR(renderer, scene, camera);
		const ReadbackImage lut = ReadBRDF(renderer);
		const AreaLights::AreaLight light = GetRelativeLight(wall, glm::dvec3(camera.Position));
		CHECK(CompareWithReference(MakeView(camera, c_Width, c_Height, 30.0f), hdr, material, std::vector<AreaLights::AreaLight> { light }, lut).MaxRelative
			<= c_AgreementRelative);
		const uint32_t row = c_Height / 2;
		float lineMax = 0.0f;
		float atHalfRange = 0.0f;
		float lastLit = 0.0f;
		double lastLitDistance = 0.0;
		std::vector<std::pair<double, float>> line;
		for (uint32_t x = 0; x < c_Width; x++)
		{
			glm::dvec3 relative(0.0);
			REQUIRE(FindPlanePoint(camera, x, row, c_Width, c_Height, 0.0f, 30.0f, relative));
			const glm::dvec3 point = relative + glm::dvec3(camera.Position);
			if (point.x <= 0.0)
				continue;
			const double distance = std::sqrt(point.x * point.x + 0.05 * 0.05 + std::max(std::abs(point.z) - 0.5, 0.0) * std::max(std::abs(point.z) - 0.5, 0.0));
			const float value = ReadHalfTexel(hdr, x, row).g;
			line.emplace_back(distance, value);
			lineMax = std::max(lineMax, value);
			if (std::abs(distance - 0.5 * c_Range) < 0.01)
				atHalfRange = value;
			if (distance >= c_Range * 1.0005)
				CHECK(value == 0.0f);
			else if (distance < 0.99 * c_Range)
				CHECK(value > 0.0f);
			if (value > 0.0f)
			{
				lastLit = value;
				lastLitDistance = distance;
			}
		}
		// Where the window fades the light (from half the range on), the light's slope between neighbors changes by less than a
		// hundredth of the light there: a step (a window cut short) would change it by the step's height.
		double worstStep = 0.0;
		for (size_t index = 2; index < line.size(); index++)
		{
			if (line[index - 2].first >= 0.5 * c_Range)
			{
				const double slope = static_cast<double>(line[index].second) - line[index - 1].second;
				const double previousSlope = static_cast<double>(line[index - 1].second) - line[index - 2].second;
				worstStep = std::max(worstStep, std::abs(slope - previousSlope) / atHalfRange);
			}
		}
		MESSAGE(fmt::format("Range: the last lit pixel {:.4f} m from the face holds {:.3g} of the light at half the range; the slope changes by {:.3g} "
			"of the light there at most beyond half the range (the line's peak {:.1f})", lastLitDistance, lastLit / atHalfRange, worstStep, lineMax));
		CHECK(atHalfRange > 0.0f);
		CHECK(lastLitDistance > 0.98 * c_Range);
		CHECK(lastLit < 0.01f * atHalfRange);
		CHECK(worstStep <= 0.01);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A rect light 10 km from the origin lights as it does at the origin, on both draw paths")
	{
		// The floor under a light of the first case's kind, moved with its camera by (10,000, 0, 10,000) m: the same picture.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_ViewSize = 64;
		PlaneMaterial material;
		material.Roughness = 0.35f;
		const auto render = [&](const glm::vec3& offset, GPUDrivenMode mode)
		{
			Scene scene;
			AddNeutralPostProcess(scene);
			AddDarkSky(scene);
			AddPlane(scene, assets, material, 0.0f, 20.0f).GetComponent<TransformComponent>().Translation = offset;
			AddRectLight(scene, offset + glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.2f, -1.0f, 0.1f), glm::vec3(1.0f, 0.0f, 0.0f), 1.0f, 0.5f, 1000.0f, 20.0f);
			SceneRendererSpecification specification = ExactSpecification();
			specification.GPUDriven = mode;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			const SceneCamera camera = LookAt(offset + glm::vec3(0.5f, 1.8f, 2.0f), offset + glm::vec3(0.0f, 0.0f, -0.5f));
			return RenderHDR(renderer, scene, camera);
		};
		const glm::vec3 distant(10000.0f, 0.0f, 10000.0f);
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			const ReadbackImage atOrigin = render(glm::vec3(0.0f), mode);
			const ReadbackImage moved = render(distant, mode);
			float peak = 0.0f;
			for (uint32_t y = 0; y < c_ViewSize; y++)
			{
				for (uint32_t x = 0; x < c_ViewSize; x++)
					peak = std::max(peak, ReadHalfTexel(atOrigin, x, y).g);
			}
			REQUIRE(peak > 0.0f);
			double worst = 0.0;
			for (uint32_t y = 0; y < c_ViewSize; y++)
			{
				for (uint32_t x = 0; x < c_ViewSize; x++)
				{
					const float value = ReadHalfTexel(atOrigin, x, y).g;
					if (value > 0.01f * peak)
						worst = std::max(worst, std::abs(static_cast<double>(ReadHalfTexel(moved, x, y).g) - value) / value);
				}
			}
			MESSAGE(fmt::format("10 km from the origin: the largest relative difference {:.3g}", worst));
			CHECK(worst <= 1.0e-3);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Glass and blended surfaces take a rect light's highlight, glass's diffuse light weighted by what it does not transmit")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_ViewSize = 64;
		PlaneMaterial glass;
		glass.Albedo = glm::vec3(0.9f, 0.95f, 1.0f);
		glass.Roughness = 0.15f;
		glass.Transmission = 0.8f;
		PlaneMaterial blended;
		blended.Albedo = glm::vec3(0.7f, 0.3f, 0.2f);
		blended.Roughness = 0.3f;
		blended.Alpha = 0.5f;
		for (const bool isGlass : { true, false })
		{
			CAPTURE(isGlass);
			const PlaneMaterial& material = isGlass ? glass : blended;
			Scene scene;
			AddNeutralPostProcess(scene);
			AddDarkSky(scene);
			// What the pane shows through it: a dark floor below.
			AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(DarkUnlit()), glm::vec3(0.0f, -0.5f, 0.0f), "Floor")
				.GetComponent<TransformComponent>().Scale = glm::vec3(60.0f, 1.0f, 60.0f);
			AddPlane(scene, assets, material, 0.0f, 20.0f);
			const Entity entity = AddRectLight(scene, glm::vec3(0.2f, 1.0f, -0.3f), glm::vec3(0.3f, -1.0f, 0.2f), glm::vec3(1.0f, 0.0f, 0.0f), 0.8f, 0.4f, 1000.0f, 20.0f);
			SceneRenderer renderer(ExactSpecification());
			renderer.SetViewportSize(c_ViewSize, c_ViewSize);
			const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.5f, 2.0f), glm::vec3(0.0f, 0.0f, -0.4f));
			const ReadbackImage lit = RenderHDR(renderer, scene, camera);
			const ReadbackImage lut = ReadBRDF(renderer);
			const std::vector<AreaLights::AreaLight> lights = { GetRelativeLight(entity, glm::dvec3(camera.Position)) };
			// The same frame without the light: what the pane passes or blends with stays, the light's share is the difference.
			scene.GetRegistry().patch<RectLightComponent>(entity.GetHandle(), [](RectLightComponent& light) { light.Intensity = 0.0f; });
			const ReadbackImage unlit = RenderHDR(renderer, scene, camera);
			ReferenceView view = MakeView(camera, c_ViewSize, c_ViewSize, 10.0f);
			view.DiffuseWeight = isGlass ? 1.0f - glass.Transmission : 1.0f;
			view.Scale = isGlass ? 1.0f : blended.Alpha;
			const Agreement agreement = CompareWithReference(view, lit, material, lights, lut, &unlit);
			MESSAGE(fmt::format("{}: the light's share against the reference: {:.3g} (relative), peak {:.1f}", isGlass ? "Glass" : "Blended", agreement.MaxRelative,
				agreement.Peak));
			CHECK(agreement.Peak > 1.0);
			CHECK(agreement.MaxRelative <= c_AgreementRelative);
			CHECK(agreement.MaxAbsolute <= c_AgreementAbsolute);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Frames draw with the forward shaders that shade rect lights while the scene has a rect light, drawn or not")
	{
		// The shaders' forms without the rect lights' code are those of before (their registers would cost every pixel): a
		// scene draws with the others from its first rect light on, in view or not, and goes back once it has none. Drawn or
		// not: the forms shade the rest of the light a little differently, so turning a light's intensity or its entity off
		// and on must leave every pixel it does not reach as it was, on both paths.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		for (const GPUDrivenMode mode : { GPUDrivenMode::Off, GPUDrivenMode::Auto })
		{
			CAPTURE(static_cast<int>(mode));
			Scene scene;
			AddNeutralPostProcess(scene);
			AddDarkSky(scene);
			AddPlane(scene, assets, PlaneMaterial(), 0.0f, 20.0f);
			AddPointLight(scene, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f), 5.0f, 4.0f);
			SceneRendererSpecification specification = ExactSpecification();
			specification.GPUDriven = mode;
			SceneRenderer renderer(specification);
			renderer.SetViewportSize(32, 32);
			const SceneCamera camera = LookAt(glm::vec3(0.0f, 2.0f, 2.0f), glm::vec3(0.0f));
			RenderHDR(renderer, scene, camera);
			CHECK_FALSE(renderer.GetStats().AreaLightShading);
			// Unlit (not drawn), behind the camera and out of its reach.
			Entity panel = AddRectLight(scene, glm::vec3(0.0f, 1.0f, 40.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 1.0f, 1.0f, 0.0f, 2.0f);
			const ReadbackImage unlit = RenderHDR(renderer, scene, camera);
			CHECK(renderer.GetStats().AreaLightShading);
			// Lit, its entity turned off, and lit again: the same forms and the same pixels every frame.
			scene.GetRegistry().patch<RectLightComponent>(panel.GetHandle(), [](RectLightComponent& light) { light.Intensity = 100.0f; });
			const ReadbackImage lit = RenderHDR(renderer, scene, camera);
			CHECK(renderer.GetStats().AreaLightShading);
			CHECK(GetLightCounts(renderer).AreaLights == 0);
			CHECK(lit.Pixels == unlit.Pixels);
			panel.SetActive(false);
			const ReadbackImage inactive = RenderHDR(renderer, scene, camera);
			CHECK(renderer.GetStats().AreaLightShading);
			CHECK(inactive.Pixels == unlit.Pixels);
			panel.SetActive(true);
			RenderHDR(renderer, scene, camera);
			CHECK(renderer.GetStats().AreaLightShading);
			// Without the component, and with it again until its entity goes.
			panel.RemoveComponent<RectLightComponent>();
			RenderHDR(renderer, scene, camera);
			CHECK_FALSE(renderer.GetStats().AreaLightShading);
			panel.AddComponent<RectLightComponent>();
			RenderHDR(renderer, scene, camera);
			CHECK(renderer.GetStats().AreaLightShading);
			scene.DestroyEntity(panel);
			RenderHDR(renderer, scene, camera);
			CHECK_FALSE(renderer.GetStats().AreaLightShading);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A reflection probe's capture takes a rect light's diffuse light only")
	{
		// Highlights depend on where they are seen from: a capture (SceneRenderOptions::ProbeCapture) keeps none, so a metal
		// shows nothing and a dielectric its diffuse light alone, as the reference without its specular lobe.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_ViewSize = 48;
		SceneRenderer renderer(ExactSpecification());
		renderer.SetViewportSize(c_ViewSize, c_ViewSize);
		SceneRenderOptions options;
		options.ProbeCapture = true;
		for (const float metallic : { 0.0f, 1.0f })
		{
			CAPTURE(metallic);
			PlaneMaterial material;
			material.Albedo = glm::vec3(0.8f);
			material.Metallic = metallic;
			material.Roughness = 0.2f;
			material.Clearcoat = metallic > 0.0f ? 0.0f : 1.0f;
			material.ClearcoatRoughness = 0.1f;
			Scene scene;
			AddNeutralPostProcess(scene);
			AddDarkSky(scene);
			AddPlane(scene, assets, material, 0.0f, 20.0f);
			const Entity entity = AddRectLight(scene, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 1.0f, 0.5f, 1000.0f, 20.0f);
			const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.5f, 1.5f), glm::vec3(0.0f, 0.0f, -0.2f));
			const ReadbackImage hdr = RenderHDR(renderer, scene, camera, options);
			const ReadbackImage lut = ReadBRDF(renderer);
			ReferenceView view = MakeView(camera, c_ViewSize, c_ViewSize, 10.0f);
			view.SpecularWeight = 0.0f;
			const Agreement agreement = CompareWithReference(view, hdr, material, std::vector<AreaLights::AreaLight> { GetRelativeLight(entity, glm::dvec3(camera.Position)) }, lut);
			if (metallic > 0.0f)
			{
				CHECK(agreement.Peak == 0.0);
				CHECK(agreement.MaxAbsolute == 0.0);
			}
			else
			{
				CHECK(agreement.Peak > 1.0);
				CHECK(agreement.MaxRelative <= c_AgreementRelative);
			}
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Screen-space reflections count a light surface once: the light's own highlight is the panel's reflection")
	{
		// L1: a tagged emissive panel 2 m over a polished metal floor, a one-sided light 1 mm under its face with its luminance, no
		// sky: the panel's mirror image in the floor reads the same with screen-space reflections on as off (within 5%), on both
		// tracers. Without the prepass's marker the reflections added the panel's image to the light's highlight: twice the light.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SceneTestAssets assets;
		constexpr uint32_t c_ViewSize = 256;
		constexpr float c_Nits = 2000.0f;
		// The panel and its mirror image both on screen (the reflections can only find what the screen shows).
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 1.0f, 3.0f), glm::vec3(0.0f, 0.19f, -1.0f));
		for (const ReflectionsQuality reflections : { ReflectionsQuality::Checkerboard, ReflectionsQuality::Spatial })
		{
			CAPTURE(static_cast<int>(reflections));
			const auto measure = [&](bool screenReflections)
			{
				Scene scene;
				PostProcessComponent& post = AddNeutralPostProcess(scene);
				post.ScreenSpaceReflections = screenReflections;
				// The checkerboard tracer runs under TAA only (its frames fill the blocks in turn); the spatial one needs no history.
				post.AntiAliasing = true;
				post.AntiAliasingMethod = reflections == ReflectionsQuality::Checkerboard ? AntiAliasingMode::TAA : AntiAliasingMode::FXAA;
				AddDarkSky(scene);
				PlaneMaterial mirror;
				mirror.Albedo = glm::vec3(0.9f);
				mirror.Metallic = 1.0f;
				mirror.Roughness = 0.02f;
				AddPlane(scene, assets, mirror, 0.0f, 20.0f);
				MaterialProperties emitter = DarkUnlit();
				emitter.EmissiveColor = glm::vec3(1.0f);
				emitter.EmissiveIntensity = c_Nits;
				Entity panel = AddMesh(scene, BuiltinAssets::CubeMesh, assets.AddMaterial(emitter), glm::vec3(0.0f, 2.025f, -1.0f), "Panel");
				panel.GetComponent<TransformComponent>().Scale = glm::vec3(1.0f, 0.05f, 0.6f);
				panel.AddComponent<LightSurfaceComponent>();
				AddRectLight(scene, glm::vec3(0.0f, 1.999f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 1.0f, 0.6f, c_Nits, 20.0f);
				SceneRendererSpecification specification = ExactSpecification();
				specification.Quality.Reflections = reflections;
				SceneRenderer renderer(specification);
				renderer.SetViewportSize(c_ViewSize, c_ViewSize);
				ReadbackImage hdr;
				for (int frame = 0; frame < 8; frame++)
				{
					hdr = RenderHDR(renderer, scene, camera);
					TAA::FinishFrame(gpu);
				}
				CHECK(renderer.GetStats().ScreenSpaceReflections == screenReflections);
				// The middle half of the panel's mirror image (its face reflected in the floor's plane).
				const glm::dvec2 low = ProjectToPixelCoordinates(camera, glm::dvec3(-0.25, -2.0, -1.15), c_ViewSize, c_ViewSize);
				const glm::dvec2 high = ProjectToPixelCoordinates(camera, glm::dvec3(0.25, -2.0, -0.85), c_ViewSize, c_ViewSize);
				const uint32_t x0 = static_cast<uint32_t>(std::ceil(std::min(low.x, high.x)));
				const uint32_t x1 = static_cast<uint32_t>(std::floor(std::max(low.x, high.x)));
				const uint32_t y0 = static_cast<uint32_t>(std::ceil(std::min(low.y, high.y)));
				const uint32_t y1 = static_cast<uint32_t>(std::floor(std::max(low.y, high.y)));
				REQUIRE(x1 > x0 + 4);
				REQUIRE(y1 > y0 + 1);
				REQUIRE(y1 < c_ViewSize);
				double sum = 0.0;
				uint32_t count = 0;
				for (uint32_t y = y0; y <= y1; y++)
				{
					for (uint32_t x = x0; x <= x1; x++)
					{
						const glm::vec4 value = ReadHalfTexel(hdr, x, y);
						sum += 0.2126 * value.r + 0.7152 * value.g + 0.0722 * value.b;
						count++;
					}
				}
				return sum / static_cast<double>(count);
			};
			const double without = measure(false);
			const double with = measure(true);
			MESSAGE(fmt::format("L1: the panel's mirror image reads {:.1f} nits with screen-space reflections, {:.1f} without ({:+.2f}%)", with, without,
				100.0 * (with / without - 1.0)));
			REQUIRE(without > 0.1 * c_Nits);
			CHECK(std::abs(with / without - 1.0) <= 0.05);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
