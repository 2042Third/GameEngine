#include <doctest/doctest.h>

#include "Strata/Math/Math.h"
#include "Strata/Renderer/DebugDraw.h"
#include "Strata/Renderer/SceneGizmos.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <limits>

using namespace Strata;

namespace
{

	// Largest distance of any vertex from a point.
	float MaxDistance(std::span<const DebugLineVertex> vertices, const glm::vec3& point)
	{
		float distance = 0.0f;
		for (const DebugLineVertex& vertex : vertices)
			distance = std::max(distance, glm::length(vertex.Position - point));
		return distance;
	}

	float MinDistance(std::span<const DebugLineVertex> vertices, const glm::vec3& point)
	{
		float distance = std::numeric_limits<float>::max();
		for (const DebugLineVertex& vertex : vertices)
			distance = std::min(distance, glm::length(vertex.Position - point));
		return distance;
	}

}

TEST_SUITE("Renderer.DebugDraw")
{
	TEST_CASE("Lines go into the list of their depth mode with packed colors")
	{
		DebugDraw debugDraw;
		CHECK(debugDraw.IsEmpty());
		debugDraw.Line(glm::vec3(0.0f), glm::vec3(1.0f, 2.0f, 3.0f), glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));
		debugDraw.Line(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(0.0f, 0.0f, 1.0f, 0.5f), DebugDrawDepth::OnTop);
		REQUIRE(debugDraw.GetLines(DebugDrawDepth::Tested).size() == 2);
		REQUIRE(debugDraw.GetLines(DebugDrawDepth::OnTop).size() == 2);
		CHECK(debugDraw.GetLineCount() == 2);
		CHECK(debugDraw.GetLines(DebugDrawDepth::Tested)[1].Position == glm::vec3(1.0f, 2.0f, 3.0f));
		CHECK(debugDraw.GetLines(DebugDrawDepth::Tested)[0].Color == 0xFF0080FFu); // RGBA8, red in the lowest byte
		CHECK(debugDraw.GetLines(DebugDrawDepth::OnTop)[0].Color == 0x80FF0000u);
		CHECK(DebugDraw::PackColor(glm::vec4(2.0f, -1.0f, 0.0f, 1.0f)) == 0xFF0000FFu); // Clamped
		// Non-finite components (e.g. a broken TextComponent color) become 0 instead of an undefined conversion.
		const float nan = std::numeric_limits<float>::quiet_NaN();
		const float infinity = std::numeric_limits<float>::infinity();
		CHECK(DebugDraw::PackColor(glm::vec4(nan, infinity, -infinity, 1.0f)) == 0xFF000000u);
		CHECK(DebugDraw::PackColor(glm::vec4(1.0f, 1.0f, 1.0f, nan)) == 0x00FFFFFFu);

		// Non-finite input is dropped instead of producing garbage lines.
		debugDraw.Line(glm::vec3(std::numeric_limits<float>::quiet_NaN()), glm::vec3(0.0f), glm::vec4(1.0f));
		debugDraw.Line(glm::vec3(0.0f), glm::vec3(std::numeric_limits<float>::infinity()), glm::vec4(1.0f));
		CHECK(debugDraw.GetLineCount() == 2);

		debugDraw.Clear();
		CHECK(debugDraw.IsEmpty());
		CHECK(debugDraw.GetLineCount() == 0);
	}

	TEST_CASE("Boxes, spheres, circles and cones have the expected outlines")
	{
		DebugDraw debugDraw;
		const glm::vec4 white(1.0f);

		debugDraw.Box(AABB(glm::vec3(-1.0f, 0.0f, 2.0f), glm::vec3(1.0f, 2.0f, 4.0f)), white);
		CHECK(debugDraw.GetLineCount() == 12);
		for (const DebugLineVertex& vertex : debugDraw.GetLines(DebugDrawDepth::Tested))
		{
			CHECK(std::abs(vertex.Position.x) == doctest::Approx(1.0f));
			CHECK((vertex.Position.y == doctest::Approx(0.0f) || vertex.Position.y == doctest::Approx(2.0f)));
			CHECK((vertex.Position.z == doctest::Approx(2.0f) || vertex.Position.z == doctest::Approx(4.0f)));
		}
		debugDraw.Clear();
		debugDraw.Box(AABB(), white); // Empty boxes draw nothing
		CHECK(debugDraw.IsEmpty());

		// Oriented boxes follow their transform, scale included.
		const glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f)) * glm::mat4_cast(glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f)))
			* glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
		debugDraw.Box(transform, glm::vec3(0.5f), white);
		CHECK(debugDraw.GetLineCount() == 12);
		CHECK(MaxDistance(debugDraw.GetLines(DebugDrawDepth::Tested), glm::vec3(5.0f, 0.0f, 0.0f)) == doctest::Approx(std::sqrt(3.0f)));
		debugDraw.Clear();

		const glm::vec3 center(1.0f, 2.0f, 3.0f);
		debugDraw.Sphere(center, 2.0f, white, DebugDrawDepth::Tested, 16);
		CHECK(debugDraw.GetLineCount() == 3 * 16);
		CHECK(MaxDistance(debugDraw.GetLines(DebugDrawDepth::Tested), center) == doctest::Approx(2.0f));
		CHECK(MinDistance(debugDraw.GetLines(DebugDrawDepth::Tested), center) == doctest::Approx(2.0f));
		debugDraw.Clear();
		debugDraw.Sphere(center, 0.0f, white);
		debugDraw.Circle(center, glm::vec3(0.0f), 1.0f, white);
		CHECK(debugDraw.IsEmpty());

		// A circle lies in the plane perpendicular to its normal.
		debugDraw.Circle(center, glm::vec3(0.0f, 0.0f, 3.0f), 1.5f, white, DebugDrawDepth::Tested, 8);
		CHECK(debugDraw.GetLineCount() == 8);
		for (const DebugLineVertex& vertex : debugDraw.GetLines(DebugDrawDepth::Tested))
		{
			CHECK(vertex.Position.z == doctest::Approx(3.0f));
			CHECK(glm::length(vertex.Position - center) == doctest::Approx(1.5f));
		}
		debugDraw.Clear();

		// Cone: base circle plus four side lines from the apex.
		debugDraw.Cone(glm::vec3(0.0f), glm::vec3(0.0f, -2.0f, 0.0f), 4.0f, glm::radians(45.0f), white, DebugDrawDepth::Tested, 12);
		CHECK(debugDraw.GetLineCount() == 12 + 4);
		CHECK(MaxDistance(debugDraw.GetLines(DebugDrawDepth::Tested), glm::vec3(0.0f, -4.0f, 0.0f)) == doctest::Approx(4.0f));
		debugDraw.Clear();
		debugDraw.Cone(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 4.0f, glm::radians(90.0f), white);
		CHECK(debugDraw.IsEmpty());
	}

	TEST_CASE("Arrows, capsules and frustums")
	{
		DebugDraw debugDraw;
		const glm::vec4 white(1.0f);

		debugDraw.Arrow(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -5.0f), white, DebugDrawDepth::OnTop);
		CHECK(debugDraw.GetLines(DebugDrawDepth::OnTop).size() == 2 * 5);
		// The head (four lines from the tip) is at most a fifth of the length and at most the head size.
		for (size_t index = 2; index < 10; index += 2)
		{
			const DebugLineVertex& tip = debugDraw.GetLines(DebugDrawDepth::OnTop)[index];
			const DebugLineVertex& barb = debugDraw.GetLines(DebugDrawDepth::OnTop)[index + 1];
			CHECK(tip.Position == glm::vec3(0.0f, 0.0f, -5.0f));
			CHECK(barb.Position.z == doctest::Approx(-4.75f));
		}
		debugDraw.Clear();
		debugDraw.Arrow(glm::vec3(1.0f), glm::vec3(1.0f), white);
		CHECK(debugDraw.IsEmpty());

		// Capsule: every point lies on the surface around its axis segment.
		const glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.0f, 0.0f));
		debugDraw.Capsule(transform, 0.5f, 1.0f, white, DebugDrawDepth::Tested, 16);
		CHECK(debugDraw.GetLineCount() > 30);
		for (const DebugLineVertex& vertex : debugDraw.GetLines(DebugDrawDepth::Tested))
		{
			const float axisY = std::clamp(vertex.Position.y, 1.0f, 3.0f);
			CHECK(glm::length(vertex.Position - glm::vec3(0.0f, axisY, 0.0f)) == doctest::Approx(0.5f).epsilon(0.001));
		}
		debugDraw.Clear();

		// Frustum: near corners at depth 1, far corners at depth 0 (reversed-Z).
		const glm::mat4 projection = Math::PerspectiveReverseZ(glm::radians(90.0f), 1.0f, 1.0f, 10.0f);
		REQUIRE(debugDraw.Frustum(projection, white));
		CHECK(debugDraw.GetLineCount() == 12);
		float nearest = std::numeric_limits<float>::max();
		float farthest = 0.0f;
		for (const DebugLineVertex& vertex : debugDraw.GetLines(DebugDrawDepth::Tested))
		{
			nearest = std::min(nearest, -vertex.Position.z);
			farthest = std::max(farthest, -vertex.Position.z);
		}
		CHECK(nearest == doctest::Approx(1.0f));
		CHECK(farthest == doctest::Approx(10.0f));
		debugDraw.Clear();
		CHECK_FALSE(debugDraw.Frustum(Math::PerspectiveReverseZInfinite(glm::radians(90.0f), 1.0f, 1.0f), white));
		CHECK(debugDraw.IsEmpty());
	}

	TEST_CASE("Scene gizmos show lights, cameras and colliders")
	{
		Scene scene;
		Entity lamp = scene.CreateEntity("Lamp");
		lamp.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f, 2.0f, 3.0f);
		PointLightComponent& pointLight = lamp.AddComponent<PointLightComponent>();
		pointLight.Range = 4.0f;
		pointLight.Color = glm::vec3(1.0f, 0.0f, 0.0f);

		Entity sun = scene.CreateEntity("Sun");
		sun.AddComponent<DirectionalLightComponent>();
		Entity spot = scene.CreateEntity("Spot");
		spot.AddComponent<SpotLightComponent>();
		Entity camera = scene.CreateEntity("Camera");
		camera.AddComponent<CameraComponent>();
		Entity crate = scene.CreateEntity("Crate");
		crate.GetComponent<TransformComponent>().Translation = glm::vec3(10.0f, 0.0f, 0.0f);
		crate.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f, 1.0f, 1.0f);
		crate.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.5f);
		Entity ball = scene.CreateEntity("Ball");
		ball.GetComponent<TransformComponent>().Scale = glm::vec3(1.0f, 3.0f, 1.0f);
		ball.AddComponent<SphereColliderComponent>().Radius = 0.5f;
		Entity pill = scene.CreateEntity("Pill");
		pill.AddComponent<CapsuleColliderComponent>();
		Entity hidden = scene.CreateEntity("Hidden");
		hidden.AddComponent<PointLightComponent>();
		hidden.SetActive(false);
		scene.UpdateWorldTransforms();

		DebugDraw debugDraw;
		SceneGizmoSettings settings;
		settings.Cameras = false;
		settings.Colliders = false;
		DrawSceneGizmos(debugDraw, scene, settings);
		// Directional arrow (5 lines), point light sphere (3 x 32), spot cone (32 + 4); the inactive light draws nothing.
		CHECK(debugDraw.GetLineCount() == 5 + 96 + 36);
		const std::span<const DebugLineVertex> sphere = debugDraw.GetLines(DebugDrawDepth::Tested).subspan(2 * 5, 2 * 96);
		CHECK(sphere[0].Color == DebugDraw::PackColor(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)));
		CHECK(MaxDistance(sphere, glm::vec3(1.0f, 2.0f, 3.0f)) == doctest::Approx(4.0f));
		CHECK(MinDistance(sphere, glm::vec3(1.0f, 2.0f, 3.0f)) == doctest::Approx(4.0f));

		debugDraw.Clear();
		settings = SceneGizmoSettings();
		settings.Lights = false;
		settings.Depth = DebugDrawDepth::OnTop;
		DrawSceneGizmos(debugDraw, scene, settings);
		CHECK(debugDraw.GetLines(DebugDrawDepth::Tested).empty());
		// Camera frustum (12), box (12), sphere (96) and capsule.
		const std::span<const DebugLineVertex> shapes = debugDraw.GetLines(DebugDrawDepth::OnTop);
		CHECK(shapes.size() / 2 > 12 + 12 + 96);
		// The box collider is scaled with its entity: 2 x 1 x 1 around (10, 0, 0).
		const std::span<const DebugLineVertex> box = shapes.subspan(2 * 12, 2 * 12);
		for (const DebugLineVertex& vertex : box)
		{
			CHECK(std::abs(vertex.Position.x - 10.0f) == doctest::Approx(1.0f));
			CHECK(std::abs(vertex.Position.y) == doctest::Approx(0.5f));
		}
		// The sphere collider takes the largest scale: radius 1.5.
		CHECK(MaxDistance(shapes.subspan(2 * 24, 2 * 96), glm::vec3(0.0f)) == doctest::Approx(1.5f));
		// The camera frustum stops at the maximum length.
		float farthest = 0.0f;
		for (const DebugLineVertex& vertex : shapes.subspan(0, 2 * 12))
			farthest = std::max(farthest, -vertex.Position.z);
		CHECK(farthest == doctest::Approx(settings.MaxCameraFrustumLength));
	}
}
