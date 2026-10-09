#include <doctest/doctest.h>

#include "Editor/EditorCamera.h"

#include <Strata/Math/Math.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace Strata;

namespace
{

	constexpr float c_Epsilon = 1e-4f;

	bool Near(const glm::vec3& a, const glm::vec3& b, float epsilon = c_Epsilon)
	{
		return Math::IsNearlyEqual(a, b, epsilon);
	}

	// Pixel position of a world point in a viewport of the given size (origin top-left, +Y down).
	glm::vec2 Project(const EditorCamera& camera, const glm::vec3& point, const glm::vec2& viewport)
	{
		const glm::vec4 clip = camera.GetProjectionMatrix(viewport.x / viewport.y) * camera.GetViewMatrix() * glm::vec4(point, 1.0f);
		const glm::vec2 ndc = glm::vec2(clip) / clip.w;
		return glm::vec2((ndc.x * 0.5f + 0.5f) * viewport.x, (0.5f - ndc.y * 0.5f) * viewport.y);
	}

}

TEST_SUITE("Editor.Camera")
{
	TEST_CASE("The default view looks at the origin from above at an angle")
	{
		EditorCamera camera;
		CHECK(Near(camera.GetTarget(), glm::vec3(0.0f)));
		CHECK(camera.GetDistance() == doctest::Approx(10.0f));
		CHECK(Near(camera.GetPosition(), camera.GetTarget() - camera.GetForward() * 10.0f));
		CHECK(camera.GetPosition().x > 0.0f);
		CHECK(camera.GetPosition().y > 0.0f);
		CHECK(camera.GetPosition().z > 0.0f);

		// The view matrix puts the target straight ahead (-Z) at the distance; the projection is reversed-Z.
		const glm::vec4 viewTarget = camera.GetViewMatrix() * glm::vec4(camera.GetTarget(), 1.0f);
		CHECK(Near(glm::vec3(viewTarget), glm::vec3(0.0f, 0.0f, -10.0f)));
		const glm::mat4 projection = camera.GetProjectionMatrix(16.0f / 9.0f);
		const glm::vec4 nearPoint = projection * glm::vec4(0.0f, 0.0f, -camera.GetNear(), 1.0f);
		const glm::vec4 farPoint = projection * glm::vec4(0.0f, 0.0f, -camera.GetFar(), 1.0f);
		CHECK(nearPoint.z / nearPoint.w == doctest::Approx(1.0f));
		CHECK(farPoint.z / farPoint.w == doctest::Approx(0.0f).epsilon(1e-4));

		const SceneCamera scene = camera.GetSceneCamera(2.0f);
		CHECK(Near(scene.Position, camera.GetPosition()));
		CHECK(scene.VerticalFOV == doctest::Approx(glm::radians(camera.GetFOV())));
		CHECK(scene.Near == camera.GetNear());
		CHECK(scene.Far == camera.GetFar());
		CHECK_FALSE(scene.Orthographic);
		CHECK(scene.View == camera.GetViewMatrix());
		CHECK(scene.Projection == camera.GetProjectionMatrix(2.0f));
	}

	TEST_CASE("Yaw and pitch follow the documented directions")
	{
		EditorCamera camera;
		camera.SetOrientation(0.0f, 0.0f);
		CHECK(Near(camera.GetForward(), glm::vec3(0.0f, 0.0f, -1.0f)));
		CHECK(Near(camera.GetRight(), glm::vec3(1.0f, 0.0f, 0.0f)));
		camera.SetOrientation(90.0f, 0.0f);
		CHECK(Near(camera.GetForward(), glm::vec3(-1.0f, 0.0f, 0.0f)));
		camera.SetOrientation(0.0f, 30.0f);
		CHECK(camera.GetForward().y == doctest::Approx(0.5f));
		camera.SetOrientation(0.0f, 120.0f);
		CHECK(camera.GetPitch() == doctest::Approx(EditorCamera::c_MaxPitch));
		camera.SetOrientation(540.0f, -200.0f);
		CHECK(camera.GetYaw() == doctest::Approx(-180.0f));
		CHECK(camera.GetPitch() == doctest::Approx(-EditorCamera::c_MaxPitch));
	}

	TEST_CASE("Orbiting turns around the target, looking turns in place")
	{
		EditorCamera camera;
		camera.SetTarget(glm::vec3(1.0f, 2.0f, 3.0f));
		const float yaw = camera.GetYaw();
		const float pitch = camera.GetPitch();

		// Dragging right and down: the view turns right and looks further down, as if dragging the scene.
		camera.Orbit(glm::vec2(10.0f, 5.0f));
		CHECK(camera.GetYaw() == doctest::Approx(yaw - 10.0f * EditorCamera::c_RotationPerPixel));
		CHECK(camera.GetPitch() == doctest::Approx(pitch - 5.0f * EditorCamera::c_RotationPerPixel));
		CHECK(Near(camera.GetTarget(), glm::vec3(1.0f, 2.0f, 3.0f)));
		CHECK(glm::distance(camera.GetPosition(), camera.GetTarget()) == doctest::Approx(10.0f));

		// The pitch stops short of straight down, so the view never flips.
		camera.Orbit(glm::vec2(0.0f, 10000.0f));
		CHECK(camera.GetPitch() == doctest::Approx(-EditorCamera::c_MaxPitch));
		CHECK(std::isfinite(camera.GetViewMatrix()[0][0]));

		const glm::vec3 position = camera.GetPosition();
		camera.Look(glm::vec2(-50.0f, -40.0f));
		CHECK(Near(camera.GetPosition(), position, 1e-3f));
		CHECK_FALSE(Near(camera.GetTarget(), glm::vec3(1.0f, 2.0f, 3.0f), 1e-3f));
		CHECK(camera.GetDistance() == doctest::Approx(10.0f));
	}

	TEST_CASE("Panning keeps the point at the target's depth under the mouse")
	{
		EditorCamera camera;
		const glm::vec2 viewport(1600.0f, 900.0f);
		const glm::vec3 point = camera.GetTarget() + camera.GetRight() * 0.5f;
		const glm::vec2 before = Project(camera, point, viewport);
		camera.Pan(glm::vec2(120.0f, -45.0f), viewport.y);
		const glm::vec2 after = Project(camera, point, viewport);
		CHECK(after.x - before.x == doctest::Approx(120.0f).epsilon(1e-3));
		CHECK(after.y - before.y == doctest::Approx(-45.0f).epsilon(1e-3));
		CHECK(camera.GetDistance() == doctest::Approx(10.0f));

		// Nothing happens without a viewport height.
		const glm::vec3 target = camera.GetTarget();
		camera.Pan(glm::vec2(100.0f, 100.0f), 0.0f);
		CHECK(Near(camera.GetTarget(), target));
	}

	TEST_CASE("The wheel dollies toward the target within limits")
	{
		EditorCamera camera;
		camera.Dolly(1.0f);
		CHECK(camera.GetDistance() == doctest::Approx(10.0f * EditorCamera::c_DollyPerStep));
		camera.Dolly(-2.0f);
		CHECK(camera.GetDistance() == doctest::Approx(10.0f / EditorCamera::c_DollyPerStep));
		camera.Dolly(1000.0f);
		CHECK(camera.GetDistance() == doctest::Approx(EditorCamera::c_MinDistance));
		camera.Dolly(-100000.0f);
		CHECK(camera.GetDistance() == doctest::Approx(EditorCamera::c_MaxDistance));
		CHECK(Near(camera.GetTarget(), glm::vec3(0.0f)));

		// The fly speed has limits too.
		camera.AdjustFlySpeed(100000.0f);
		CHECK(camera.GetFlySpeed() == doctest::Approx(EditorCamera::c_MaxFlySpeed));
		camera.AdjustFlySpeed(-1.0e6f);
		CHECK(camera.GetFlySpeed() == doctest::Approx(EditorCamera::c_MinFlySpeed));
	}

	TEST_CASE("Input maps to orbit, pan, dolly and fly mode")
	{
		const glm::vec2 viewport(800.0f, 600.0f);
		EditorCamera camera;
		EditorCameraInput input;
		input.Orbit = true;
		input.MouseDelta = glm::vec2(10.0f, 0.0f);
		camera.Update(input, 0.016f, viewport);
		CHECK(camera.GetYaw() == doctest::Approx(45.0f - 10.0f * EditorCamera::c_RotationPerPixel));
		CHECK(Near(camera.GetTarget(), glm::vec3(0.0f)));

		input = {};
		input.Pan = true;
		input.MouseDelta = glm::vec2(30.0f, 0.0f);
		camera.Update(input, 0.016f, viewport);
		CHECK_FALSE(Near(camera.GetTarget(), glm::vec3(0.0f)));

		input = {};
		input.Scroll = 2.0f;
		camera.Update(input, 0.016f, viewport);
		CHECK(camera.GetDistance() == doctest::Approx(10.0f * EditorCamera::c_DollyPerStep * EditorCamera::c_DollyPerStep));

		// Fly mode: the wheel changes the speed instead, and the keys move camera and target together.
		camera.Reset();
		camera.SetOrientation(0.0f, 0.0f);
		input = {};
		input.Fly = true;
		input.Scroll = 1.0f;
		camera.Update(input, 0.0f, viewport);
		CHECK(camera.GetDistance() == doctest::Approx(10.0f));
		CHECK(camera.GetFlySpeed() == doctest::Approx(5.0f * EditorCamera::c_FlySpeedPerStep));
		camera.SetFlySpeed(2.0f);

		input = {};
		input.Fly = true;
		input.MoveForward = true;
		glm::vec3 position = camera.GetPosition();
		camera.Update(input, 0.5f, viewport);
		CHECK(Near(camera.GetPosition() - position, glm::vec3(0.0f, 0.0f, -1.0f)));
		CHECK(camera.GetDistance() == doctest::Approx(10.0f));

		// Diagonals are as fast as straight lines; shift is faster; up and down follow the world axis.
		input.MoveRight = true;
		position = camera.GetPosition();
		camera.Update(input, 0.5f, viewport);
		CHECK(glm::distance(camera.GetPosition(), position) == doctest::Approx(1.0f));
		input = {};
		input.Fly = true;
		input.MoveUp = true;
		input.Fast = true;
		position = camera.GetPosition();
		camera.Update(input, 0.5f, viewport);
		CHECK(Near(camera.GetPosition() - position, glm::vec3(0.0f, EditorCamera::c_FastFlyMultiplier, 0.0f)));

		// Looking in fly mode turns in place.
		input = {};
		input.Fly = true;
		input.MouseDelta = glm::vec2(20.0f, 10.0f);
		position = camera.GetPosition();
		camera.Update(input, 0.016f, viewport);
		CHECK(Near(camera.GetPosition(), position, 1e-3f));
		CHECK(camera.GetYaw() == doctest::Approx(-20.0f * EditorCamera::c_RotationPerPixel));
	}

	TEST_CASE("Focusing frames bounds without changing the orientation")
	{
		const float aspect = 16.0f / 9.0f;
		const glm::vec2 viewport(1600.0f, 900.0f);
		for (const float fov : { 30.0f, 60.0f, 100.0f })
		{
			CAPTURE(fov);
			EditorCamera camera;
			camera.SetFOV(fov);
			const float yaw = camera.GetYaw();
			const float pitch = camera.GetPitch();
			const AABB bounds(glm::vec3(4.0f, -1.0f, 2.0f), glm::vec3(10.0f, 3.0f, 3.0f));
			REQUIRE(camera.Focus(bounds, aspect));
			CHECK(Near(camera.GetTarget(), bounds.GetCenter()));
			CHECK(camera.GetYaw() == doctest::Approx(yaw));
			CHECK(camera.GetPitch() == doctest::Approx(pitch));

			// Every corner is in view, and the framing is tight: the bounding sphere touches the narrower view edge
			// within the margin.
			for (int corner = 0; corner < 8; corner++)
			{
				const glm::vec3 point((corner & 1) ? bounds.Max.x : bounds.Min.x, (corner & 2) ? bounds.Max.y : bounds.Min.y, (corner & 4) ? bounds.Max.z : bounds.Min.z);
				const glm::vec2 pixel = Project(camera, point, viewport);
				CHECK(pixel.x >= 0.0f);
				CHECK(pixel.x <= viewport.x);
				CHECK(pixel.y >= 0.0f);
				CHECK(pixel.y <= viewport.y);
			}
			const float radius = glm::length(bounds.GetExtents());
			const float halfAngle = glm::radians(fov) * 0.5f; // Vertical is narrower for a wide aspect ratio
			CHECK(camera.GetDistance() == doctest::Approx(radius * EditorCamera::c_FocusMargin / std::sin(halfAngle)));
		}

		// Tall viewports fit horizontally; tiny bounds still get a minimum radius; invalid bounds change nothing.
		EditorCamera camera;
		REQUIRE(camera.Focus(AABB(glm::vec3(-1.0f), glm::vec3(1.0f)), 0.5f));
		const float halfHorizontal = std::atan(std::tan(glm::radians(camera.GetFOV()) * 0.5f) * 0.5f);
		CHECK(camera.GetDistance() == doctest::Approx(std::sqrt(3.0f) * EditorCamera::c_FocusMargin / std::sin(halfHorizontal)));
		REQUIRE(camera.Focus(AABB(glm::vec3(2.0f), glm::vec3(2.0f)), 1.0f));
		CHECK(camera.GetDistance() == doctest::Approx(EditorCamera::c_MinFocusRadius * EditorCamera::c_FocusMargin / std::sin(glm::radians(camera.GetFOV()) * 0.5f)));
		const glm::vec3 target = camera.GetTarget();
		CHECK_FALSE(camera.Focus(AABB(), 1.0f));
		CHECK(Near(camera.GetTarget(), target));
	}

	TEST_CASE("LookAt, positions and settings")
	{
		EditorCamera camera;
		REQUIRE(camera.LookAt(glm::vec3(3.0f, 4.0f, 5.0f), glm::vec3(-1.0f, 0.0f, 2.0f)));
		CHECK(Near(camera.GetPosition(), glm::vec3(3.0f, 4.0f, 5.0f), 1e-3f));
		CHECK(Near(camera.GetTarget(), glm::vec3(-1.0f, 0.0f, 2.0f)));
		CHECK(Near(camera.GetForward(), glm::normalize(glm::vec3(-4.0f, -4.0f, -3.0f))));
		CHECK_FALSE(camera.LookAt(glm::vec3(1.0f), glm::vec3(1.0f)));
		CHECK_FALSE(camera.LookAt(glm::vec3(std::nanf("")), glm::vec3(1.0f)));
		CHECK(Near(camera.GetTarget(), glm::vec3(-1.0f, 0.0f, 2.0f)));

		// Straight down is limited to the maximum pitch.
		REQUIRE(camera.LookAt(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f)));
		CHECK(camera.GetPitch() == doctest::Approx(-EditorCamera::c_MaxPitch));

		// Moving the camera keeps the orientation and distance.
		const glm::vec3 forward = camera.GetForward();
		camera.SetPosition(glm::vec3(7.0f, 8.0f, 9.0f));
		CHECK(Near(camera.GetPosition(), glm::vec3(7.0f, 8.0f, 9.0f), 1e-3f));
		CHECK(Near(camera.GetForward(), forward));
		camera.SetTarget(glm::vec3(1.0f, 1.0f, 1.0f));
		CHECK(Near(camera.GetPosition(), glm::vec3(1.0f) - forward * camera.GetDistance(), 1e-3f));

		camera.SetFOV(500.0f);
		CHECK(camera.GetFOV() == EditorCamera::c_MaxFOV);
		CHECK(camera.SetClipPlanes(0.5f, 50.0f));
		CHECK_FALSE(camera.SetClipPlanes(5.0f, 5.0f));
		CHECK_FALSE(camera.SetClipPlanes(0.0f, 5.0f));
		CHECK_FALSE(camera.SetClipPlanes(1.0f, 1.0e9f));
		CHECK(camera.GetNear() == 0.5f);
		CHECK(camera.GetFar() == 50.0f);
		camera.SetFlySpeed(0.0f);
		CHECK(camera.GetFlySpeed() == EditorCamera::c_MinFlySpeed);
	}

	TEST_CASE("The camera round trips through JSON and rejects invalid documents")
	{
		EditorCamera camera;
		REQUIRE(camera.LookAt(glm::vec3(-3.0f, 2.0f, 8.0f), glm::vec3(1.0f, 0.5f, -2.0f)));
		camera.SetFOV(75.0f);
		REQUIRE(camera.SetClipPlanes(0.05f, 500.0f));
		camera.SetFlySpeed(12.0f);

		EditorCamera restored;
		std::string error;
		REQUIRE_MESSAGE(restored.FromJson(camera.ToJson(), &error), error);
		CHECK(restored.ToJson() == camera.ToJson());
		CHECK(Near(restored.GetPosition(), camera.GetPosition()));

		// Partial documents keep the other values.
		REQUIRE(restored.FromJson({ { "FOV", 40.0 } }, &error));
		CHECK(restored.GetFOV() == 40.0f);
		CHECK(restored.GetFlySpeed() == 12.0f);

		const nlohmann::json before = restored.ToJson();
		const nlohmann::json invalid[] = {
			nlohmann::json::array(),
			{ { "Target", { 1, 2 } } },
			{ { "Target", { 1, 2, "x" } } },
			{ { "Target", { 1, 2, 1e30 } } },
			{ { "Distance", 0.0 } },
			{ { "Pitch", 95.0 } },
			{ { "FOV", "wide" } },
			{ { "Near", 10.0 }, { "Far", 5.0 } },
			{ { "Far", 1e12 } },
			{ { "FlySpeed", -1.0 } },
			{ { "FOV", 70.0 }, { "Yaw", true } } // A valid value next to an invalid one changes nothing either
		};
		for (const nlohmann::json& document : invalid)
		{
			CAPTURE(document.dump());
			error.clear();
			CHECK_FALSE(restored.FromJson(document, &error));
			CHECK_FALSE(error.empty());
			CHECK(restored.ToJson() == before);
		}
	}
}
