#pragma once

#include <Strata/Math/AABB.h>
#include <Strata/Renderer/SceneRenderer.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace Strata
{

	// One frame of viewport input for the editor camera, in viewport pixels. The viewport fills it only while it is
	// hovered or focused, or while a drag that started in it continues.
	struct EditorCameraInput
	{
		glm::vec2 MouseDelta = { 0.0f, 0.0f }; // Pixels the mouse moved since the last frame (+Y down)
		float Scroll = 0.0f;                    // Mouse wheel steps (positive: away from the user)
		bool Orbit = false;                     // Rotate around the target (Alt + left button)
		bool Pan = false;                       // Move the target with the mouse (middle button)
		// Fly mode (right button): the mouse turns the camera in place, the movement keys move it (WASD, Q down, E up) and
		// the wheel changes the fly speed instead of dollying.
		bool Fly = false;
		bool MoveForward = false;
		bool MoveBackward = false;
		bool MoveLeft = false;
		bool MoveRight = false;
		bool MoveUp = false;
		bool MoveDown = false;
		bool Fast = false; // Shift: flies c_FastFlyMultiplier times faster
	};

	// The editor's viewport camera: a perspective camera looking at a target point (the orbit pivot) from a distance,
	// oriented by yaw (around world +Y, 0 looks along -Z) and pitch (positive looks up, limited to +-c_MaxPitch so the
	// view never flips). Its position is the target minus the forward direction times the distance. Angles are in
	// degrees at the interface.
	class EditorCamera
	{
	public:
		static constexpr float c_MaxPitch = 89.0f;
		static constexpr float c_MinFOV = 1.0f;
		static constexpr float c_MaxFOV = 170.0f;
		static constexpr float c_MinNear = 1.0e-4f;
		static constexpr float c_MaxFar = 1.0e7f;
		static constexpr float c_MinDistance = 0.01f;
		static constexpr float c_MaxDistance = 1.0e6f;
		static constexpr float c_MinFlySpeed = 0.01f;
		static constexpr float c_MaxFlySpeed = 10000.0f;
		static constexpr float c_MaxCoordinate = 1.0e9f;
		static constexpr float c_RotationPerPixel = 0.3f;  // Degrees per pixel (orbit and fly look)
		static constexpr float c_DollyPerStep = 0.85f;     // Distance factor per wheel step toward the target
		static constexpr float c_FlySpeedPerStep = 1.25f;  // Fly speed factor per wheel step
		static constexpr float c_FastFlyMultiplier = 4.0f;
		static constexpr float c_FocusMargin = 1.1f;       // Framed bounds keep this much room around them
		static constexpr float c_MinFocusRadius = 0.1f;

		EditorCamera() = default;

		// Back to the default view: the origin from above at an angle, 10 units away.
		void Reset();

		// Applies one frame of input: fly mode while Fly is held, otherwise orbit or pan, and dolly with the wheel.
		void Update(const EditorCameraInput& input, float timestep, const glm::vec2& viewportSize);

		// Rotates around the target, as if dragging the scene with the mouse.
		void Orbit(const glm::vec2& deltaPixels);
		// Turns the camera in place (fly mode): the target moves, the position stays.
		void Look(const glm::vec2& deltaPixels);
		// Moves the camera and target parallel to the view so the point at the target's depth follows the mouse.
		void Pan(const glm::vec2& deltaPixels, float viewportHeight);
		// Moves toward the target (positive steps) or away from it, by c_DollyPerStep per step.
		void Dolly(float steps);
		// Moves camera and target: x along the view's right, y along world up, z along the view direction.
		void Move(const glm::vec3& localDirection, float distance);
		void AdjustFlySpeed(float steps);

		// Keeps the orientation and frames the bounds: the target moves to their center and the distance makes their
		// bounding sphere fit the view. Returns false (changing nothing) for invalid bounds.
		bool Focus(const AABB& bounds, float aspectRatio);
		// Looks from `position` at `target`. Returns false (changing nothing) when they coincide or are not finite.
		bool LookAt(const glm::vec3& position, const glm::vec3& target);
		// Moves the camera so it looks at `target`, keeping orientation and distance.
		void SetTarget(const glm::vec3& target);
		// Moves the camera to `position`, keeping orientation and distance (the target moves along).
		void SetPosition(const glm::vec3& position);
		// Orbits to the given angles around the target. The pitch is limited to +-c_MaxPitch.
		void SetOrientation(float yawDegrees, float pitchDegrees);
		void SetDistance(float distance);
		void SetFOV(float degrees);
		// Sets both clip planes; returns false (changing nothing) unless c_MinNear <= near < far <= c_MaxFar.
		bool SetClipPlanes(float nearClip, float farClip);
		void SetFlySpeed(float speed);

		glm::vec3 GetPosition() const;
		const glm::vec3& GetTarget() const { return m_Target; }
		float GetDistance() const { return m_Distance; }
		float GetYaw() const { return m_Yaw; }
		float GetPitch() const { return m_Pitch; }
		float GetFOV() const { return m_FOV; }
		float GetNear() const { return m_Near; }
		float GetFar() const { return m_Far; }
		float GetFlySpeed() const { return m_FlySpeed; }
		glm::quat GetOrientation() const;
		glm::vec3 GetForward() const;
		glm::vec3 GetRight() const;
		glm::vec3 GetUp() const;

		glm::mat4 GetViewMatrix() const;
		// Reversed-Z perspective projection (Strata clip space).
		glm::mat4 GetProjectionMatrix(float aspectRatio) const;
		SceneCamera GetSceneCamera(float aspectRatio) const;

		// {"Target": [x, y, z], "Distance", "Yaw", "Pitch", "FOV", "Near", "Far", "FlySpeed"} (angles in degrees).
		nlohmann::json ToJson() const;
		// Applies a ToJson document; absent values keep their current value. Fails (changing nothing) on values that
		// are not numbers, not finite or out of range.
		bool FromJson(const nlohmann::json& json, std::string* outError = nullptr);
	private:
		glm::vec3 m_Target = { 0.0f, 0.0f, 0.0f };
		float m_Distance = 10.0f;
		float m_Yaw = 45.0f;
		float m_Pitch = -30.0f;
		float m_FOV = 60.0f;
		float m_Near = 0.1f;
		float m_Far = 2000.0f;
		float m_FlySpeed = 5.0f;
	};

}
