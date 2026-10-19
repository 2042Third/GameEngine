#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/UUID.h"
#include "Strata/Math/Math.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace Strata
{

	// The scene's own components, and the types of components other modules register that sibling layers read. Components
	// hold authored data only and are reflected (registered by the module that owns them, see
	// ComponentRegistration.cpp for these), which gives them serialization, inspector UI, automation and undo for free.
	// Runtime state owned by engine systems lives in separate, unregistered components (e.g. WorldTransformComponent).
	// Every other component lives in its module: Renderer/RendererComponents.h (lights, sky, post-processing, text),
	// Physics/PhysicsComponents.h, Audio/AudioComponents.h, Animation/AnimationComponents.h and
	// Scripting/ScriptingComponents.h.

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
	// How a prefab instance differs from its prefab (PrefabOverrides): a value of an entity of the instance, a component it
	// added or removed, an entity of the prefab it removed.
	enum class PrefabOverrideKind : uint8_t
	{
		Property = 0,     // The component's property differs (script fields: Component "Script", Property "<Class>.<Field>")
		AddedComponent,   // The instance's entity has a component its prefab entity lacks (a script: Property "<Class>")
		RemovedComponent, // The instance's entity lacks a component its prefab entity has (a script: Property "<Class>")
		RemovedEntity     // The instance lacks the prefab's entity (Component and Property empty)
	};

	struct PrefabOverride
	{
		UUID Entity = UUID::Null(); // The prefab's entity id (PrefabInstanceComponent::PrefabEntityID)
		std::string Component;      // Its registered name
		std::string Property;
		PrefabOverrideKind Kind = PrefabOverrideKind::Property;

		bool operator==(const PrefabOverride& other) const = default;
	};

	struct PrefabInstanceComponent
	{
		AssetHandle Prefab = UUID::Null();
		UUID PrefabEntityID = UUID::Null();
		// On an instance root, from when the instance last took its prefab's values (PrefabInstances::MarkSynced): the version of
		// its part of the prefab then and a hash of its own values then. 0: not known (scenes from before, game code's
		// instances). A prefab that changed while the instance's scene was closed updates it once the scene is open again,
		// when its values still have that hash.
		uint64_t PrefabVersion = 0;
		uint64_t SyncedHash = 0;
		// On an instance root: how the instance differs from its prefab as the editor last saw it (PrefabOverrides), kept by
		// the editor's edits; empty when it does not differ or nothing was recorded (scenes from before). Not part of
		// templates, comparisons or content hashes.
		std::vector<PrefabOverride> Overrides;
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
		// Background when no sky is rendered (sRGB-encoded; exposed and tone mapped with the scene, see SceneCamera)
		glm::vec4 ClearColor = { 0.05f, 0.05f, 0.07f, 1.0f };

		// View-to-clip transform for the given aspect ratio, in Strata's reversed-Z convention.
		glm::mat4 GetProjection(float aspectRatio) const;
		float GetNearClip() const { return Projection == ProjectionType::Perspective ? PerspectiveNear : OrthographicNear; }
		float GetFarClip() const { return Projection == ProjectionType::Perspective ? PerspectiveFar : OrthographicFar; }
	};

	// Registered by the renderer module (RegisterRendererComponents): physics' mesh colliders default to its mesh, the
	// animation module's preview and pose bounds read it, and render proxies follow its signals.
	struct MeshRendererComponent
	{
		AssetHandle Mesh = UUID::Null();
		AssetHandle Material = UUID::Null(); // Overrides every submesh material when set
		bool CastShadows = true;
	};

	////////////////////////////////////////////////////////////////////////////////
	// Animation
	////////////////////////////////////////////////////////////////////////////////

	// Registered by the animation module (RegisterAnimationComponents): the renderer, which may not include Animation, makes
	// skinned proxies from it.
	//
	// Makes the entity's MeshRendererComponent a skinned mesh posed by an animator: the mesh's joints follow the pose of
	// the entity named by Animator (an entity with an AnimatorComponent, whose skeleton the mesh was bound to). The pose
	// places the skinned vertices relative to the animator's entity, so the mesh follows the animator wherever this entity
	// is. Without a pose (no animator, no poses provider) the mesh shows its bind pose; the editor's scenes show their
	// animators' rest poses (AnimationPreviewSpecification::RestPoses).
	struct SkinComponent
	{
		UUID Animator = UUID::Null();
	};

}
