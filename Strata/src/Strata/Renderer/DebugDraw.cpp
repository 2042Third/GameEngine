#include "stpch.h"
#include "Strata/Renderer/DebugDraw.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_MinSegments = 3;
		constexpr uint32_t c_MaxSegments = 256;
		// Circles of up to this many segments take their points from a table (the common ones: gizmos use 32).
		constexpr uint32_t c_MaxTableSegments = 128;

		// The points of unit circles (cos, sin) of every segment count up to c_MaxTableSegments, segment count n at
		// offset n * (n - 1) / 2 (each holds n points, the first at angle 0), computed once.
		const std::vector<glm::vec2>& GetUnitCircleTables()
		{
			static const std::vector<glm::vec2> s_Tables = []()
			{
				std::vector<glm::vec2> tables;
				tables.reserve(static_cast<size_t>(c_MaxTableSegments) * (c_MaxTableSegments + 1) / 2);
				for (uint32_t segments = 1; segments <= c_MaxTableSegments; segments++)
				{
					for (uint32_t point = 0; point < segments; point++)
					{
						const float angle = glm::two_pi<float>() * static_cast<float>(point) / static_cast<float>(segments);
						tables.emplace_back(std::cos(angle), std::sin(angle));
					}
				}
				return tables;
			}();
			return s_Tables;
		}

		// Point `index` of a unit circle divided into `segments` (index may equal segments: the first point again).
		glm::vec2 UnitCirclePoint(uint32_t segments, uint32_t index)
		{
			if (index == segments)
				index = 0;
			if (segments <= c_MaxTableSegments)
				return GetUnitCircleTables()[static_cast<size_t>(segments) * (segments - 1) / 2 + index];
			const float angle = glm::two_pi<float>() * static_cast<float>(index) / static_cast<float>(segments);
			return glm::vec2(std::cos(angle), std::sin(angle));
		}

		bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// Two unit vectors perpendicular to a unit normal and to each other.
		void PerpendicularAxes(const glm::vec3& normal, glm::vec3& outU, glm::vec3& outV)
		{
			const glm::vec3 helper = std::abs(normal.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
			outU = glm::normalize(glm::cross(helper, normal));
			outV = glm::cross(normal, outU);
		}

		// The 12 edges of a box given by its 8 corners (index bits: x, y, z).
		constexpr uint8_t c_BoxEdges[12][2] = {
			{ 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
			{ 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
			{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
		};

	}

	uint32_t DebugDraw::PackColor(const glm::vec4& color)
	{
		// Non-finite components become 0 first: clamp keeps NaN, and converting NaN to an integer is undefined.
		glm::vec4 finite = color;
		for (int component = 0; component < 4; component++)
		{
			if (!std::isfinite(finite[component]))
				finite[component] = 0.0f;
		}
		const glm::vec4 clamped = glm::clamp(finite, glm::vec4(0.0f), glm::vec4(1.0f)) * 255.0f + 0.5f;
		return static_cast<uint32_t>(clamped.r) | (static_cast<uint32_t>(clamped.g) << 8) | (static_cast<uint32_t>(clamped.b) << 16)
			| (static_cast<uint32_t>(clamped.a) << 24);
	}

	void DebugDraw::AddLine(const glm::vec3& from, const glm::vec3& to, uint32_t color, DebugDrawDepth depth)
	{
		if (!IsFinite(from) || !IsFinite(to))
			return;
		std::vector<DebugLineVertex>& lines = m_Lines[static_cast<size_t>(depth)];
		lines.push_back(DebugLineVertex { from, color });
		lines.push_back(DebugLineVertex { to, color });
	}

	void DebugDraw::AddCircle(const glm::vec3& center, const glm::vec3& axisU, const glm::vec3& axisV, float radius, uint32_t color, DebugDrawDepth depth,
		uint32_t segments)
	{
		segments = std::clamp(segments, c_MinSegments, c_MaxSegments);
		glm::vec3 previous = center + axisU * radius;
		for (uint32_t segment = 1; segment <= segments; segment++)
		{
			const glm::vec2 unit = UnitCirclePoint(segments, segment);
			const glm::vec3 point = center + (axisU * unit.x + axisV * unit.y) * radius;
			AddLine(previous, point, color, depth);
			previous = point;
		}
	}

	void DebugDraw::Line(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color, DebugDrawDepth depth)
	{
		AddLine(from, to, PackColor(color), depth);
	}

	void DebugDraw::Line(const glm::vec3& from, const glm::vec3& to, uint32_t packedColor, DebugDrawDepth depth)
	{
		AddLine(from, to, packedColor, depth);
	}

	void DebugDraw::Box(const AABB& box, const glm::vec4& color, DebugDrawDepth depth)
	{
		if (!box.IsValid())
			return;
		const glm::vec3 center = box.GetCenter();
		glm::mat4 transform(1.0f);
		transform[3] = glm::vec4(center, 1.0f);
		Box(transform, box.GetExtents(), color, depth);
	}

	void DebugDraw::Box(const glm::mat4& transform, const glm::vec3& halfExtents, const glm::vec4& color, DebugDrawDepth depth)
	{
		const uint32_t packed = PackColor(color);
		std::array<glm::vec3, 8> corners;
		for (uint32_t corner = 0; corner < 8; corner++)
		{
			const glm::vec3 local((corner & 1) ? halfExtents.x : -halfExtents.x, (corner & 2) ? halfExtents.y : -halfExtents.y,
				(corner & 4) ? halfExtents.z : -halfExtents.z);
			corners[corner] = glm::vec3(transform * glm::vec4(local, 1.0f));
		}
		for (const auto& edge : c_BoxEdges)
			AddLine(corners[edge[0]], corners[edge[1]], packed, depth);
	}

	void DebugDraw::Circle(const glm::vec3& center, const glm::vec3& normal, float radius, const glm::vec4& color, DebugDrawDepth depth, uint32_t segments)
	{
		const float length = glm::length(normal);
		if (!(length > 1e-6f) || !(radius > 0.0f) || !std::isfinite(radius))
			return;
		glm::vec3 axisU;
		glm::vec3 axisV;
		PerpendicularAxes(normal / length, axisU, axisV);
		AddCircle(center, axisU, axisV, radius, PackColor(color), depth, segments);
	}

	void DebugDraw::Sphere(const glm::vec3& center, float radius, const glm::vec4& color, DebugDrawDepth depth, uint32_t segments)
	{
		if (!(radius > 0.0f) || !std::isfinite(radius))
			return;
		const uint32_t packed = PackColor(color);
		const glm::vec3 x(1.0f, 0.0f, 0.0f);
		const glm::vec3 y(0.0f, 1.0f, 0.0f);
		const glm::vec3 z(0.0f, 0.0f, 1.0f);
		AddCircle(center, x, y, radius, packed, depth, segments);
		AddCircle(center, y, z, radius, packed, depth, segments);
		AddCircle(center, z, x, radius, packed, depth, segments);
	}

	void DebugDraw::Arrow(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color, DebugDrawDepth depth, float headSize)
	{
		const glm::vec3 shaft = to - from;
		const float length = glm::length(shaft);
		if (!(length > 1e-6f))
			return;
		const uint32_t packed = PackColor(color);
		AddLine(from, to, packed, depth);

		const glm::vec3 direction = shaft / length;
		const float head = std::min(length * 0.2f, std::max(headSize, 0.0f));
		glm::vec3 axisU;
		glm::vec3 axisV;
		PerpendicularAxes(direction, axisU, axisV);
		const glm::vec3 headBase = to - direction * head;
		for (const glm::vec3& side : { axisU, -axisU, axisV, -axisV })
			AddLine(to, headBase + side * (head * 0.5f), packed, depth);
	}

	void DebugDraw::Cone(const glm::vec3& apex, const glm::vec3& direction, float length, float halfAngle, const glm::vec4& color, DebugDrawDepth depth,
		uint32_t segments)
	{
		const float directionLength = glm::length(direction);
		if (!(directionLength > 1e-6f) || !(length > 0.0f) || !std::isfinite(length) || !(halfAngle > 0.0f) || !(halfAngle < glm::half_pi<float>()))
			return;
		const glm::vec3 axis = direction / directionLength;
		const glm::vec3 baseCenter = apex + axis * length;
		const float baseRadius = length * std::tan(halfAngle);
		glm::vec3 axisU;
		glm::vec3 axisV;
		PerpendicularAxes(axis, axisU, axisV);
		const uint32_t packed = PackColor(color);
		AddCircle(baseCenter, axisU, axisV, baseRadius, packed, depth, segments);
		for (const glm::vec3& side : { axisU, -axisU, axisV, -axisV })
			AddLine(apex, baseCenter + side * baseRadius, packed, depth);
	}

	void DebugDraw::Capsule(const glm::mat4& transform, float radius, float halfHeight, const glm::vec4& color, DebugDrawDepth depth, uint32_t segments)
	{
		if (!(radius > 0.0f) || !std::isfinite(radius) || !(halfHeight >= 0.0f) || !std::isfinite(halfHeight))
			return;
		segments = std::clamp(segments, c_MinSegments * 2, c_MaxSegments);
		const uint32_t packed = PackColor(color);
		auto point = [&](const glm::vec3& local) { return glm::vec3(transform * glm::vec4(local, 1.0f)); };
		// Polyline through local-space points `first` to `first + count` of a circle divided into `circleSegments`.
		auto arc = [&](const glm::vec3& center, const glm::vec3& axisU, const glm::vec3& axisV, uint32_t circleSegments, uint32_t first, uint32_t count)
		{
			auto circlePoint = [&](uint32_t index)
			{
				const glm::vec2 unit = UnitCirclePoint(circleSegments, index % circleSegments);
				return point(center + (axisU * unit.x + axisV * unit.y) * radius);
			};
			glm::vec3 previous = circlePoint(first);
			for (uint32_t segment = 1; segment <= count; segment++)
			{
				const glm::vec3 next = circlePoint(first + segment);
				AddLine(previous, next, packed, depth);
				previous = next;
			}
		};

		const glm::vec3 x(1.0f, 0.0f, 0.0f);
		const glm::vec3 y(0.0f, 1.0f, 0.0f);
		const glm::vec3 z(0.0f, 0.0f, 1.0f);
		const glm::vec3 top(0.0f, halfHeight, 0.0f);
		const glm::vec3 bottom(0.0f, -halfHeight, 0.0f);
		// Rings where the hemispheres meet the cylinder, the cylinder's sides and the hemisphere arcs in the XY and ZY planes
		// (half circles: the upper half above the top ring, the lower half below the bottom one).
		const uint32_t half = segments / 2;
		arc(top, x, z, segments, 0, segments);
		arc(bottom, x, z, segments, 0, segments);
		for (const glm::vec3& side : { x, -x, z, -z })
			AddLine(point(top + side * radius), point(bottom + side * radius), packed, depth);
		arc(top, x, y, half * 2, 0, half);
		arc(top, z, y, half * 2, 0, half);
		arc(bottom, x, y, half * 2, half, half);
		arc(bottom, z, y, half * 2, half, half);
	}

	bool DebugDraw::Frustum(const glm::mat4& viewProjection, const glm::vec4& color, DebugDrawDepth depth)
	{
		const glm::mat4 inverse = glm::inverse(viewProjection);
		std::array<glm::vec3, 8> corners;
		for (uint32_t corner = 0; corner < 8; corner++)
		{
			// Reversed-Z: depth 1 is the near plane, 0 the far plane.
			const glm::vec4 clip((corner & 1) ? 1.0f : -1.0f, (corner & 2) ? 1.0f : -1.0f, (corner & 4) ? 0.0f : 1.0f, 1.0f);
			const glm::vec4 world = inverse * clip;
			if (!(std::abs(world.w) > 1e-7f))
				return false;
			corners[corner] = glm::vec3(world) / world.w;
			if (!IsFinite(corners[corner]))
				return false;
		}
		const uint32_t packed = PackColor(color);
		for (const auto& edge : c_BoxEdges)
			AddLine(corners[edge[0]], corners[edge[1]], packed, depth);
		return true;
	}

	void DebugDraw::Clear()
	{
		for (std::vector<DebugLineVertex>& lines : m_Lines)
			lines.clear();
	}

	bool DebugDraw::IsEmpty() const
	{
		return m_Lines[0].empty() && m_Lines[1].empty();
	}

	size_t DebugDraw::GetLineCount() const
	{
		return (m_Lines[0].size() + m_Lines[1].size()) / 2;
	}

}
