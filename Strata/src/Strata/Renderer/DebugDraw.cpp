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
		const glm::vec4 clamped = glm::clamp(color, glm::vec4(0.0f), glm::vec4(1.0f)) * 255.0f + 0.5f;
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
		uint32_t segments, float startAngle, float endAngle)
	{
		segments = std::clamp(segments, c_MinSegments, c_MaxSegments);
		glm::vec3 previous = center + (axisU * std::cos(startAngle) + axisV * std::sin(startAngle)) * radius;
		for (uint32_t segment = 1; segment <= segments; segment++)
		{
			const float angle = startAngle + (endAngle - startAngle) * static_cast<float>(segment) / static_cast<float>(segments);
			const glm::vec3 point = center + (axisU * std::cos(angle) + axisV * std::sin(angle)) * radius;
			AddLine(previous, point, color, depth);
			previous = point;
		}
	}

	void DebugDraw::Line(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color, DebugDrawDepth depth)
	{
		AddLine(from, to, PackColor(color), depth);
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
		// Polyline through local-space points on a circle or arc.
		auto arc = [&](const glm::vec3& center, const glm::vec3& axisU, const glm::vec3& axisV, float startAngle, float endAngle, uint32_t arcSegments)
		{
			glm::vec3 previous = point(center + (axisU * std::cos(startAngle) + axisV * std::sin(startAngle)) * radius);
			for (uint32_t segment = 1; segment <= arcSegments; segment++)
			{
				const float angle = startAngle + (endAngle - startAngle) * static_cast<float>(segment) / static_cast<float>(arcSegments);
				const glm::vec3 next = point(center + (axisU * std::cos(angle) + axisV * std::sin(angle)) * radius);
				AddLine(previous, next, packed, depth);
				previous = next;
			}
		};

		const glm::vec3 x(1.0f, 0.0f, 0.0f);
		const glm::vec3 y(0.0f, 1.0f, 0.0f);
		const glm::vec3 z(0.0f, 0.0f, 1.0f);
		const glm::vec3 top(0.0f, halfHeight, 0.0f);
		const glm::vec3 bottom(0.0f, -halfHeight, 0.0f);
		const float pi = glm::pi<float>();
		// Rings where the hemispheres meet the cylinder, the cylinder's sides and the hemisphere arcs in the XY and ZY planes.
		arc(top, x, z, 0.0f, 2.0f * pi, segments);
		arc(bottom, x, z, 0.0f, 2.0f * pi, segments);
		for (const glm::vec3& side : { x, -x, z, -z })
			AddLine(point(top + side * radius), point(bottom + side * radius), packed, depth);
		arc(top, x, y, 0.0f, pi, segments / 2);
		arc(top, z, y, 0.0f, pi, segments / 2);
		arc(bottom, x, y, pi, 2.0f * pi, segments / 2);
		arc(bottom, z, y, pi, 2.0f * pi, segments / 2);
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
