#pragma once

#include "Strata/Math/AABB.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Strata
{

	enum class DebugDrawDepth : uint8_t
	{
		Tested = 0, // Hidden behind scene geometry
		OnTop       // Always visible
	};

	// One end of a debug line.
	struct DebugLineVertex
	{
		glm::vec3 Position;
		uint32_t Color; // RGBA8 (red in the lowest byte), sRGB-encoded like any display color
	};
	static_assert(sizeof(DebugLineVertex) == 16, "DebugLineVertex is uploaded as is");

	// Immediate-mode debug shapes in world space: editor gizmos and gameplay debugging fill one during a frame, pass it to
	// SceneRenderer::Render (SceneRenderOptions::DebugShapes) and clear it for the next frame. Shapes are drawn as
	// one-pixel lines over the finished image, with exact display (sRGB-encoded) colors. Not thread-safe.
	class DebugDraw
	{
	public:
		void Line(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color, DebugDrawDepth depth = DebugDrawDepth::Tested);
		// Axis-aligned box.
		void Box(const AABB& box, const glm::vec4& color, DebugDrawDepth depth = DebugDrawDepth::Tested);
		// Box of the given half extents around the origin of `transform` (which may rotate and scale it).
		void Box(const glm::mat4& transform, const glm::vec3& halfExtents, const glm::vec4& color, DebugDrawDepth depth = DebugDrawDepth::Tested);
		void Circle(const glm::vec3& center, const glm::vec3& normal, float radius, const glm::vec4& color, DebugDrawDepth depth = DebugDrawDepth::Tested,
			uint32_t segments = 32);
		// Three great circles around the axes.
		void Sphere(const glm::vec3& center, float radius, const glm::vec4& color, DebugDrawDepth depth = DebugDrawDepth::Tested, uint32_t segments = 32);
		// Line with a four-sided arrow head at `to` (a fifth of the length, at most headSize).
		void Arrow(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color, DebugDrawDepth depth = DebugDrawDepth::Tested, float headSize = 0.25f);
		// Cone from the apex along the direction, with a circle at its base and four side lines. halfAngle in radians.
		void Cone(const glm::vec3& apex, const glm::vec3& direction, float length, float halfAngle, const glm::vec4& color,
			DebugDrawDepth depth = DebugDrawDepth::Tested, uint32_t segments = 32);
		// Capsule along the local Y axis of `transform`: two hemispheres of `radius` joined by a cylinder of 2 * halfHeight.
		void Capsule(const glm::mat4& transform, float radius, float halfHeight, const glm::vec4& color, DebugDrawDepth depth = DebugDrawDepth::Tested,
			uint32_t segments = 32);
		// Frustum of a view-projection (Strata clip space: reversed-Z, the near plane at depth 1). The projection must have
		// a finite far plane; returns false (drawing nothing) for degenerate or infinite ones.
		bool Frustum(const glm::mat4& viewProjection, const glm::vec4& color, DebugDrawDepth depth = DebugDrawDepth::Tested);

		void Clear();
		bool IsEmpty() const;
		// Line list (vertex pairs) of one depth mode.
		std::span<const DebugLineVertex> GetLines(DebugDrawDepth depth) const { return m_Lines[static_cast<size_t>(depth)]; }
		size_t GetLineCount() const;

		static uint32_t PackColor(const glm::vec4& color);
	private:
		void AddLine(const glm::vec3& from, const glm::vec3& to, uint32_t color, DebugDrawDepth depth);
		void AddCircle(const glm::vec3& center, const glm::vec3& axisU, const glm::vec3& axisV, float radius, uint32_t color, DebugDrawDepth depth,
			uint32_t segments, float startAngle = 0.0f, float endAngle = 6.28318530718f);
	private:
		std::array<std::vector<DebugLineVertex>, 2> m_Lines;
	};

}
