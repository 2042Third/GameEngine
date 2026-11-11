#include "stpch.h"
#include "Strata/Renderer/AreaLights.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

// The edge integral's fit and the horizon term's use are ported from webgl/shaders/ltc/ltc_quad.fs (IntegrateEdgeVec,
// LTC_Evaluate) of https://github.com/selfshadow/ltc_code at commit 31e5e96b54f98f33098f8503003119ba2231a1c6:
//
//   Copyright (c) 2017, Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt. All rights reserved.
//   The BSD 2-Clause terms with a third condition, a reference to the paper; the whole text is in ThirdPartyNotices.md.
//
//   Real-Time Polygonal-Light Shading with Linearly Transformed Cosines.
//   Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt.
//   ACM Transactions on Graphics (Proceedings of ACM SIGGRAPH 2016) 35(4), 2016.

namespace Strata::AreaLights
{

	namespace
	{

#include "Strata/Renderer/LTCTable.inl"

		static_assert(sizeof(c_LTCTable) == sizeof(float) * c_TableLayers * c_TableSize * c_TableSize * c_TableChannels,
			"LTCTable.inl holds c_TableLayers layers of c_TableSize x c_TableSize texels");

		// Below these an edge's ends or its angle's sine count as zero: far under anything a scene's sizes give, and their
		// inverses stay finite.
		constexpr float c_MinLengthProduct = 1.0e-30f;
		constexpr float c_MinSineSquared = 1.0e-30f;

		// Under this squared length the view's direction along the surface is too short to orient the lobe's frame (the
		// view is within 1e-4 radians of the normal, where the lobe is isotropic anyway).
		constexpr float c_MinTangentLengthSquared = 1.0e-8f;

		// The least size is compared with this much less: the axes of a turned transform (glm::mat4_cast of its rotation) are
		// a few float steps longer or shorter than its scale, and a face of exactly c_MinSize stays drawn however it is
		// turned (a turn of 90 degrees about X leaves a Y axis of 0.99999994).
		constexpr float c_MinSizeTolerance = 1.0e-5f;

		bool IsFinite(const glm::vec2& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
		}

		bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// A NaN gives 0, as HLSL's saturate does.
		float Saturate(float value)
		{
			return value > 0.0f ? std::min(value, 1.0f) : 0.0f;
		}

		glm::vec4 Lerp(const glm::vec4& from, const glm::vec4& to, float weight)
		{
			return from + (to - from) * weight;
		}

		float InverseSqrt(float value)
		{
			return 1.0f / std::sqrt(value);
		}

		// An edge's share of the vector form factor: (the angle it is seen under) / (2 pi) along cross(start, end).
		// `crossProduct` is cross(start, end) from the caller, who knows the edge itself: for a small, distant light the two
		// ends are almost parallel and their own cross product would be mostly rounding.
		glm::vec3 IntegrateEdge(const glm::vec3& start, const glm::vec3& end, const glm::vec3& crossProduct)
		{
			const float inverse = 1.0f / std::max(glm::length(start) * glm::length(end), c_MinLengthProduct);
			const float cosine = glm::dot(start, end) * inverse;
			const glm::vec3 sine = crossProduct * inverse; // Its length is the angle's sine
			const float fit = GetEdgeFit(std::abs(cosine));
			// Past a right angle the angle is pi minus the one the fit covers. The sine is taken from the cross product, not
			// from 1 - cosine^2: close to a straight angle (a point almost on the edge's line, as on a surface flush with the
			// light) the cosine rounds to -1 and the difference loses the sine, which would leave a term of the wrong size
			// along a direction that is still exact.
			const float weight = cosine > 0.0f ? fit : 0.5f * InverseSqrt(std::max(glm::dot(sine, sine), c_MinSineSquared)) - fit;
			return sine * weight;
		}

		// The face as seen from a point at the origin: its corners in AreaLight's order, and the edge that leaves each corner.
		struct Polygon
		{
			glm::vec3 Corners[4];
			glm::vec3 Edges[4];
		};

		Polygon MakePolygon(const glm::vec3& center, const glm::vec3& axisX, const glm::vec3& axisY)
		{
			Polygon polygon;
			polygon.Corners[0] = center - axisX - axisY;
			polygon.Corners[1] = center + axisX - axisY;
			polygon.Corners[2] = center + axisX + axisY;
			polygon.Corners[3] = center - axisX + axisY;
			polygon.Edges[0] = axisX * 2.0f;
			polygon.Edges[1] = axisY * 2.0f;
			polygon.Edges[2] = axisX * -2.0f;
			polygon.Edges[3] = axisY * -2.0f;
			return polygon;
		}

		glm::vec3 IntegratePolygon(const Polygon& polygon)
		{
			glm::vec3 sum(0.0f);
			for (uint32_t index = 0; index < 4; index++)
			{
				const glm::vec3& start = polygon.Corners[index];
				sum += IntegrateEdge(start, polygon.Corners[(index + 1) % 4], glm::cross(start, polygon.Edges[index]));
			}
			return sum;
		}

		// The corners' heights over the receiver's horizon (the plane through the origin across `normal`).
		void GetHeights(const Polygon& polygon, const glm::vec3& normal, float (&heights)[4])
		{
			for (uint32_t index = 0; index < 4; index++)
				heights[index] = glm::dot(polygon.Corners[index], normal);
		}

		// The vector form factor of the polygon's part above the receiver's horizon, whose corners lie `heights` over it: the
		// polygon's edges above the horizon, cut where they cross it, and an edge along the horizon that closes each stretch
		// of the outline under it, from where the outline goes under to where it next comes back. The crossings are scaled by
		// positive numbers (the edge integral takes directions), which keeps them free of a division. With every corner above
		// the horizon it is IntegratePolygon's sum, operation for operation; with none, 0.
		//
		// The heights may be the corners' before a linear transform took them where they are: a crossing is the same weighted
		// sum of its edge's ends in either space, and the cross product of two transformed vectors is the transform's cofactor
		// times theirs, so the result is the transformed image's sum of the part above the untransformed horizon.
		//
		// A plane cuts a rectangle's outline twice at most, but the heights are computed: for a receiver in the light's own
		// plane all four are rounding, and their signs can alternate around the outline, which then goes under twice. So
		// every exit is closed onto the entry that follows it, never onto another stretch's.
		glm::vec3 IntegratePolygonAbove(const Polygon& polygon, const float (&heights)[4])
		{
			glm::vec3 sum(0.0f);
			glm::vec3 exit(0.0f);       // Where the outline last went under, while no entry has closed it
			glm::vec3 firstEntry(0.0f); // An entry met before any exit: it follows the last exit, around the outline's start
			bool exitOpen = false;
			for (uint32_t index = 0; index < 4; index++)
			{
				const uint32_t next = (index + 1) % 4;
				const glm::vec3& start = polygon.Corners[index];
				const glm::vec3& end = polygon.Corners[next];
				const float startHeight = heights[index];
				const float endHeight = heights[next];
				const glm::vec3 crossProduct = glm::cross(start, polygon.Edges[index]);
				if (startHeight > 0.0f && endHeight > 0.0f)
				{
					sum += IntegrateEdge(start, end, crossProduct);
				}
				else if (startHeight > 0.0f)
				{
					exit = end * startHeight - start * endHeight;
					exitOpen = true;
					sum += IntegrateEdge(start, exit, crossProduct * startHeight);
				}
				else if (endHeight > 0.0f)
				{
					const glm::vec3 entry = start * endHeight - end * startHeight;
					sum += IntegrateEdge(entry, end, crossProduct * endHeight);
					if (exitOpen)
					{
						sum += IntegrateEdge(exit, entry, glm::cross(exit, entry));
						exitOpen = false;
					}
					else
					{
						firstEntry = entry;
					}
				}
			}
			// Exits and entries alternate around the outline, so an exit still open after the last edge is closed by the entry
			// that came before every exit.
			if (exitOpen)
				sum += IntegrateEdge(exit, firstEntry, glm::cross(exit, firstEntry));
			return sum;
		}

		// 1 in front of the light, -1 behind a two-sided one (where the corners' order is reversed and with it the sign of
		// every edge sum) and 0 where it lights nothing: behind a one-sided light and on its plane.
		float GetSide(const glm::vec3& position, const AreaLight& light)
		{
			const float distance = glm::dot(light.Normal, position - light.Center);
			if (distance > 0.0f)
				return 1.0f;
			return light.TwoSided && distance < 0.0f ? -1.0f : 0.0f;
		}

		// A tangent of the normal that does not depend on the view (Include/IBL.slang's TangentBasis).
		glm::vec3 GetFixedTangent(const glm::vec3& normal)
		{
			const glm::vec3 up = std::abs(normal.z) < 0.999f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
			return glm::normalize(glm::cross(up, normal));
		}

	}

	std::span<const float> GetTableLayer(uint32_t layer)
	{
		if (layer >= c_TableLayers)
			return {};
		return std::span<const float>(c_LTCTable[layer]);
	}

	glm::vec4 LoadTable(uint32_t layer, uint32_t x, uint32_t y)
	{
		if (layer >= c_TableLayers)
			return glm::vec4(0.0f);
		const uint32_t texel = std::min(x, c_TableSize - 1) + std::min(y, c_TableSize - 1) * c_TableSize;
		const float* values = c_LTCTable[layer] + static_cast<size_t>(texel) * c_TableChannels;
		return glm::vec4(values[0], values[1], values[2], values[3]);
	}

	glm::vec4 SampleTable(uint32_t layer, const glm::vec2& uv)
	{
		// The tables are fitted at their texels' centers: 0 is the first texel and 1 the last one.
		const float last = static_cast<float>(c_TableSize - 1);
		const glm::vec2 texel = glm::vec2(Saturate(uv.x), Saturate(uv.y)) * last;
		const glm::vec2 base = glm::floor(texel);
		const glm::vec2 weight = texel - base;
		const uint32_t x0 = static_cast<uint32_t>(base.x);
		const uint32_t y0 = static_cast<uint32_t>(base.y);
		const uint32_t x1 = std::min(x0 + 1, c_TableSize - 1);
		const uint32_t y1 = std::min(y0 + 1, c_TableSize - 1);
		const glm::vec4 upper = Lerp(LoadTable(layer, x0, y0), LoadTable(layer, x1, y0), weight.x);
		const glm::vec4 lower = Lerp(LoadTable(layer, x0, y1), LoadTable(layer, x1, y1), weight.x);
		return Lerp(upper, lower, weight.y);
	}

	glm::vec2 GetWorldHalfExtents(const glm::mat4& world, const glm::vec2& halfExtents)
	{
		return halfExtents * glm::vec2(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])));
	}

	float GetLuminance(float intensity, AreaLightUnits units, const glm::vec2& halfExtents, bool twoSided)
	{
		if (units != AreaLightUnits::Lumens)
			return intensity;
		const float area = 4.0f * halfExtents.x * halfExtents.y;
		if (!(area > 0.0f))
			return 0.0f;
		return intensity / (glm::pi<float>() * area * (twoSided ? 2.0f : 1.0f));
	}

	Record MakeRecord(const glm::mat4& world, bool active, const glm::vec3& color, float luminance, const glm::vec2& halfExtents, float range,
		bool twoSided)
	{
		Record record;
		if (!active || !(luminance > 0.0f) || !(range > 0.0f) || !std::isfinite(luminance) || !std::isfinite(range))
			return record; // Not drawn
		const glm::vec3 x = glm::vec3(world[0]);
		const glm::vec3 y = glm::vec3(world[1]);
		const glm::vec3 z = glm::vec3(world[2]);
		const glm::vec3 center = glm::vec3(world[3]);
		if (!IsFinite(color) || !IsFinite(halfExtents) || !IsFinite(x) || !IsFinite(y) || !IsFinite(z) || !IsFinite(center))
			return record;

		// The face's plane holds the X and Y axes; of its two normals the light shines along the one on its -Z axis's side,
		// also under a mirroring transform, whose X and Y axes turn the other way around it.
		const glm::vec3 face = glm::cross(x, y);
		const float lengthX = glm::length(x);
		const float faceLength = glm::length(face);
		const float handedness = glm::dot(face, z);
		if (!(lengthX > 0.0f) || !(faceLength > 0.0f) || !std::isfinite(faceLength) || !std::isfinite(handedness) || handedness == 0.0f)
			return record;

		const glm::vec2 worldHalfExtents = GetWorldHalfExtents(world, halfExtents);
		const float boundsRadius = range + glm::length(worldHalfExtents);
		const glm::vec3 radiance = color * luminance;
		const float minSize = c_MinSize * (1.0f - c_MinSizeTolerance);
		if (!(worldHalfExtents.x * 2.0f >= minSize) || !(worldHalfExtents.y * 2.0f >= minSize) || !std::isfinite(boundsRadius) || !IsFinite(radiance))
			return record;

		record.Center = center;
		record.Normal = face * ((handedness > 0.0f ? -1.0f : 1.0f) / faceLength);
		record.AxisX = x / lengthX;
		record.AxisY = glm::cross(record.AxisX, record.Normal);
		record.HalfExtents = worldHalfExtents;
		record.Radiance = radiance;
		record.Range = range;
		record.BoundsRadius = boundsRadius;
		record.TwoSided = twoSided;
		record.Drawn = true;
		return record;
	}

	AreaLight LoadAreaLight(const Record& record)
	{
		AreaLight light;
		light.Center = record.Center;
		light.Normal = record.Normal;
		light.AxisX = record.AxisX * record.HalfExtents.x;
		light.AxisY = record.AxisY * record.HalfExtents.y;
		light.Radiance = record.Radiance;
		light.InverseRangeSquared = record.Range > 0.0f ? 1.0f / (record.Range * record.Range) : 0.0f;
		light.TwoSided = record.TwoSided;
		return light;
	}

	float GetEdgeFit(float cosine)
	{
		const float numerator = 0.8543985f + (0.4965155f + 0.0145206f * cosine) * cosine;
		const float denominator = 3.4175940f + (4.1616724f + cosine) * cosine;
		return numerator / denominator;
	}

	glm::vec3 GetVectorFormFactor(const glm::vec3& position, const AreaLight& light)
	{
		return IntegratePolygon(MakePolygon(light.Center - position, light.AxisX, light.AxisY));
	}

	float ClipToHorizon(const glm::vec3& vectorFormFactor, float alongNormal)
	{
		const float length = glm::length(vectorFormFactor);
		if (!(length > 0.0f))
			return 0.0f;
		const float cosine = alongNormal / length;
		const float share = SampleTable(c_TermsLayer, glm::vec2(cosine * 0.5f + 0.5f, length)).w;
		return std::min(length * share, 1.0f);
	}

	float FormFactorClipped(const glm::vec3& position, const glm::vec3& normal, const AreaLight& light)
	{
		const float side = GetSide(position, light);
		if (side == 0.0f)
			return 0.0f;
		const Polygon polygon = MakePolygon(light.Center - position, light.AxisX, light.AxisY);
		float heights[4];
		GetHeights(polygon, normal, heights);
		return std::clamp(glm::dot(IntegratePolygonAbove(polygon, heights), normal) * side, 0.0f, 1.0f);
	}

	float GetLobeCoverage(const glm::vec4& transform, const glm::vec3& position, const glm::vec3& normal, const glm::vec3& view, const AreaLight& light)
	{
		const float side = GetSide(position, light);
		if (side == 0.0f)
			return 0.0f;

		// The frame the tables are fitted in: the normal and the view's direction along the surface.
		glm::vec3 tangent = view - normal * glm::dot(view, normal);
		const float tangentLengthSquared = glm::dot(tangent, tangent);
		if (tangentLengthSquared < c_MinTangentLengthSquared)
			tangent = GetFixedTangent(normal);
		else
			tangent *= InverseSqrt(tangentLengthSquared);
		const glm::vec3 bitangent = glm::cross(normal, tangent);

		// The transform is linear, so the face stays a parallelogram: its center and its two half axes are transformed, and
		// the corners and the exact edges follow from them.
		const auto transformVector = [&](const glm::vec3& vector)
		{
			const glm::vec3 local(glm::dot(tangent, vector), glm::dot(bitangent, vector), glm::dot(normal, vector));
			return glm::vec3(transform.x * local.x + transform.z * local.z, local.y, transform.y * local.x + transform.w * local.z);
		};
		// GGX reflects nothing from under the receiver's horizon, but the fitted lobe keeps a part of itself there on the
		// viewer's side (its transform lifts those directions above the transformed horizon): the face is cut at the
		// receiver's horizon before the transformed horizon's term, so a face under the horizon gives nothing and one sinking
		// under it fades out.
		const glm::vec3 center = light.Center - position;
		float heights[4];
		GetHeights(MakePolygon(center, light.AxisX, light.AxisY), normal, heights);
		const Polygon polygon = MakePolygon(transformVector(center), transformVector(light.AxisX), transformVector(light.AxisY));
		const glm::vec3 vectorFormFactor = IntegratePolygonAbove(polygon, heights) * side;
		return ClipToHorizon(vectorFormFactor, vectorFormFactor.z);
	}

	float SpecularFraction(const glm::vec3& position, const glm::vec3& normal, const glm::vec3& view, float roughness, const AreaLight& light)
	{
		const float cosine = Saturate(glm::dot(normal, view));
		const glm::vec2 uv(std::max(roughness, c_MinRoughness), std::sqrt(std::max(1.0f - cosine, 0.0f)));
		return GetLobeCoverage(SampleTable(c_TransformLayer, uv), position, normal, view, light);
	}

	glm::vec3 ClosestPoint(const glm::vec3& position, const AreaLight& light)
	{
		// The axes are perpendicular, so the nearest point is the nearest along each.
		const glm::vec3 offset = position - light.Center;
		const float x = glm::dot(offset, light.AxisX) / std::max(glm::dot(light.AxisX, light.AxisX), c_MinLengthProduct);
		const float y = glm::dot(offset, light.AxisY) / std::max(glm::dot(light.AxisY, light.AxisY), c_MinLengthProduct);
		return light.Center + light.AxisX * std::clamp(x, -1.0f, 1.0f) + light.AxisY * std::clamp(y, -1.0f, 1.0f);
	}

	float GetRangeWindow(const glm::vec3& position, const AreaLight& light)
	{
		const glm::vec3 offset = position - ClosestPoint(position, light);
		const float ratio = glm::dot(offset, offset) * light.InverseRangeSquared;
		const float window = std::clamp(1.0f - ratio * ratio, 0.0f, 1.0f);
		return window * window;
	}

	glm::vec3 Shade(const Surface& surface, const Coat& coat, const glm::vec2& coatDFG, const glm::vec2& dfg, float diffuseWeight, const AreaLight& light,
		float specularWeight)
	{
		if (GetSide(surface.Position, light) == 0.0f)
			return glm::vec3(0.0f);
		const float window = GetRangeWindow(surface.Position, light);
		if (!(window > 0.0f))
			return glm::vec3(0.0f);

		const glm::vec3 reflectance = surface.F0 * dfg.x + dfg.y;
		const float formFactor = FormFactorClipped(surface.Position, surface.Normal, light);
		glm::vec3 result = surface.Albedo * ((1.0f - surface.Metallic) * formFactor * diffuseWeight) * (glm::vec3(1.0f) - reflectance);
		if (specularWeight > 0.0f)
		{
			const float fraction = SpecularFraction(surface.Position, surface.Normal, surface.View, surface.Roughness, light);
			result += reflectance * surface.SpecularEnergy * (fraction * specularWeight);
		}
		if (coat.Strength > 0.0f)
		{
			const float coatReflectance = coat.Strength * (c_ClearcoatF0 * coatDFG.x + coatDFG.y) * coat.SpecularEnergy;
			result *= 1.0f - coatReflectance;
			if (specularWeight > 0.0f)
				result += glm::vec3(SpecularFraction(surface.Position, coat.Normal, surface.View, coat.Roughness, light) * coatReflectance * specularWeight);
		}
		return light.Radiance * (result * window);
	}

}
