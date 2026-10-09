#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/UUID.h"
#include "Strata/Math/Math.h"
#include "Strata/Reflection/Property.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace Strata
{

	// Built-in components. Components hold authored data only and are reflected (see
	// ComponentRegistration.cpp), which gives them serialization, inspector UI, automation and undo for free.
	// Runtime state owned by engine systems lives in separate, unregistered components (e.g. WorldTransformComponent).

	////////////////////////////////////////////////////////////////////////////////
	// Core
	////////////////////////////////////////////////////////////////////////////////

	struct IDComponent
	{
		UUID ID;
	};

	struct NameComponent
	{
		std::string Name;
	};

	// Optional gameplay label used to find groups of entities ("Enemy", "Pickup").
	struct TagComponent
	{
		std::string Tag;
	};

	// Present while an entity is deactivated. Inactive entities (and their descendants) are not rendered,
	// simulated or updated by scripts.
	struct InactiveComponent
	{
	};

	struct TransformComponent
	{
		glm::vec3 Translation = { 0.0f, 0.0f, 0.0f };
		glm::quat Rotation = { 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 Scale = { 1.0f, 1.0f, 1.0f };

		// Local transform relative to the parent entity.
		glm::mat4 GetTransform() const
		{
			return Math::ComposeTransform(Translation, Rotation, Scale);
		}

		// Returns false (keeping the current values) if the matrix cannot be decomposed.
		bool SetTransform(const glm::mat4& transform)
		{
			return Math::DecomposeTransform(transform, Translation, Rotation, Scale);
		}
	};

	// Hierarchy links. Children order is the display/serialization order.
	struct RelationshipComponent
	{
		UUID Parent = UUID::Null();
		std::vector<UUID> Children;
	};

	// Runtime cache computed by Scene::UpdateWorldTransforms(); not serialized.
	struct WorldTransformComponent
	{
		glm::mat4 Matrix = glm::mat4(1.0f);
		bool ActiveInHierarchy = true;
	};

	// Marks entities created from a prefab asset (links each instance entity to its prefab source entity).
	struct PrefabInstanceComponent
	{
		AssetHandle Prefab = UUID::Null();
		UUID PrefabEntityID = UUID::Null();
	};

	////////////////////////////////////////////////////////////////////////////////
	// Rendering
	////////////////////////////////////////////////////////////////////////////////

	enum class ProjectionType : uint8_t
	{
		Perspective = 0,
		Orthographic = 1
	};

	struct CameraComponent
	{
		ProjectionType Projection = ProjectionType::Perspective;
		float PerspectiveFOV = 60.0f; // Vertical field of view in degrees
		float PerspectiveNear = 0.1f;
		float PerspectiveFar = 1000.0f;
		float OrthographicSize = 10.0f; // Vertical extent in world units
		float OrthographicNear = 0.1f;
		float OrthographicFar = 1000.0f;
		bool Primary = true;
		glm::vec4 ClearColor = { 0.05f, 0.05f, 0.07f, 1.0f }; // Background when no sky is rendered

		// View-to-clip transform for the given aspect ratio, in Strata's reversed-Z convention.
		glm::mat4 GetProjection(float aspectRatio) const;
		float GetNearClip() const { return Projection == ProjectionType::Perspective ? PerspectiveNear : OrthographicNear; }
		float GetFarClip() const { return Projection == ProjectionType::Perspective ? PerspectiveFar : OrthographicFar; }
	};

	struct MeshRendererComponent
	{
		AssetHandle Mesh = UUID::Null();
		AssetHandle Material = UUID::Null(); // Overrides every submesh material when set
		bool CastShadows = true;
	};

	struct DirectionalLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 3.0f;
		bool CastShadows = true;
		float ShadowDistance = 80.0f;   // Shadows are rendered up to this distance from the camera
		float ShadowSoftness = 1.0f;    // Apparent light size; larger values give softer penumbrae
		float ShadowBias = 0.002f;
		float ShadowNormalBias = 0.02f;
	};

	struct PointLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 10.0f;
		float Range = 10.0f;
		bool CastShadows = false;
		float SourceRadius = 0.05f; // Light size for soft shadows
	};

	struct SpotLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 20.0f;
		float Range = 15.0f;
		float InnerConeAngle = 20.0f; // Degrees, half angle
		float OuterConeAngle = 30.0f; // Degrees, half angle
		bool CastShadows = true;
		float SourceRadius = 0.05f;
	};

	// Image-based lighting and sky background from an HDR environment map (equirectangular texture).
	struct SkyLightComponent
	{
		AssetHandle EnvironmentMap = UUID::Null();
		float Intensity = 1.0f;
		float Rotation = 0.0f; // Degrees around +Y
		bool ShowBackground = true;
		float BackgroundBlur = 0.0f; // 0 = sharp, 1 = fully blurred
		glm::vec3 AmbientColor = { 0.03f, 0.03f, 0.04f }; // Used when no environment map is assigned
	};

	enum class TonemapOperator : uint8_t
	{
		None = 0,
		Reinhard,
		ACES,
		AgX,
		KhronosNeutral
	};

	// Scene-wide post-processing and screen-space effect settings (the first enabled instance is used).
	struct PostProcessComponent
	{
		TonemapOperator Tonemapper = TonemapOperator::ACES;
		float Exposure = 0.0f; // Exposure compensation in EV
		bool AutoExposure = true;
		float AutoExposureMinEV = -4.0f;
		float AutoExposureMaxEV = 12.0f;
		float AutoExposureSpeed = 1.5f;
		bool Bloom = true;
		float BloomIntensity = 0.04f;
		float BloomThreshold = 1.0f;
		bool AmbientOcclusion = true;
		float AmbientOcclusionRadius = 0.6f;
		float AmbientOcclusionIntensity = 1.0f;
		bool AntiAliasing = true;
		float Vignette = 0.2f;
		float Saturation = 1.0f;
		float Contrast = 1.0f;
	};

	enum class TextAlignment : uint8_t
	{
		Left = 0,
		Center,
		Right
	};

	struct TextComponent
	{
		std::string Text = "Text";
		AssetHandle Font = UUID::Null(); // Default engine font when unset
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
		float FontSize = 32.0f; // Pixels in screen space, world units in world space
		bool ScreenSpace = true;
		glm::vec2 ScreenAnchor = { 0.5f, 0.5f }; // Normalized viewport position (0,0 = top-left)
		glm::vec2 ScreenOffset = { 0.0f, 0.0f }; // Pixels added to the anchor (+Y down)
		TextAlignment Alignment = TextAlignment::Center;
	};

	////////////////////////////////////////////////////////////////////////////////
	// Physics
	////////////////////////////////////////////////////////////////////////////////

	enum class RigidBodyType : uint8_t
	{
		Static = 0,
		Dynamic,
		Kinematic
	};

	struct RigidBodyComponent
	{
		RigidBodyType Type = RigidBodyType::Dynamic;
		float Mass = 1.0f;
		float LinearDamping = 0.05f;
		float AngularDamping = 0.05f;
		float GravityScale = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		bool IsTrigger = false; // Reports overlaps without colliding
		bool ContinuousCollision = false;
		bool LockRotationX = false;
		bool LockRotationY = false;
		bool LockRotationZ = false;
		uint32_t Layer = 0; // Collision layer (0-31)
	};

	struct BoxColliderComponent
	{
		glm::vec3 HalfExtents = { 0.5f, 0.5f, 0.5f };
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
	};

	struct SphereColliderComponent
	{
		float Radius = 0.5f;
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
	};

	// Capsule aligned with the entity's local Y axis.
	struct CapsuleColliderComponent
	{
		float Radius = 0.5f;
		float HalfHeight = 0.5f; // Half height of the cylindrical section
		glm::vec3 Offset = { 0.0f, 0.0f, 0.0f };
	};

	struct MeshColliderComponent
	{
		AssetHandle Mesh = UUID::Null(); // Uses the MeshRenderer's mesh when unset
		bool Convex = true; // Non-convex meshes are only supported on static bodies
	};

	////////////////////////////////////////////////////////////////////////////////
	// Audio
	////////////////////////////////////////////////////////////////////////////////

	struct AudioSourceComponent
	{
		AssetHandle Clip = UUID::Null();
		float Volume = 1.0f;
		float Pitch = 1.0f;
		bool Loop = false;
		bool PlayOnStart = true;
		bool Spatial = true;
		float MinDistance = 1.0f;
		float MaxDistance = 50.0f;
		float Rolloff = 1.0f;
	};

	// The active listener; falls back to the primary camera when no listener exists.
	struct AudioListenerComponent
	{
		bool Active = true;
	};

	////////////////////////////////////////////////////////////////////////////////
	// Scripting
	////////////////////////////////////////////////////////////////////////////////

	struct ScriptFieldValue
	{
		std::string Name;
		PropertyType Type = PropertyType::Float;
		PropertyValue Value = 0.0f;
	};

	struct ScriptEntry
	{
		std::string ClassName;
		std::vector<ScriptFieldValue> Fields; // Overrides of the script's default field values

		ScriptFieldValue* FindField(std::string_view name);
		const ScriptFieldValue* FindField(std::string_view name) const;
	};

	// Native C++ behaviours from the project's script module attached to this entity.
	struct ScriptComponent
	{
		std::vector<ScriptEntry> Scripts;

		ScriptEntry* FindScript(std::string_view className);
		const ScriptEntry* FindScript(std::string_view className) const;
	};

}
