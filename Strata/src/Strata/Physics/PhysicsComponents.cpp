#include "stpch.h"
#include "Strata/Physics/PhysicsComponents.h"

#include "Strata/Math/Math.h"
#include "Strata/Physics/PhysicsRegistration.h"
#include "Strata/Reflection/ComponentGizmo.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyOptions.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/PhysicsLayerSettings.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace Strata
{

	namespace
	{

		// A collision layer: an index the inspector offers by the project's layer names (the scene's copy of its table).
		PropertyOptions PhysicsLayer(std::string tooltip)
		{
			PropertyOptions options = WithTooltip(Range(0.0f, static_cast<float>(c_PhysicsLayerCount - 1)), std::move(tooltip));
			options.Flags = PropertyFlags::PhysicsLayer;
			return options;
		}

		// Collider gizmos follow the entity's scale like rigid bodies do (box extents per axis, sphere radii by the largest
		// axis, capsule radii by X and Z and heights by Y) and never its shear: the shape's frame is the entity's rotation and
		// translation, moved by the collider's offset (scaled like the entity). False for a transform that does not decompose.
		bool GetColliderFrame(const glm::mat4& world, const glm::vec3& offset, glm::mat4& outFrame, glm::vec3& outScale)
		{
			glm::vec3 translation(0.0f);
			glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 scale(1.0f);
			if (!Math::DecomposeTransform(world, translation, rotation, scale))
				return false;
			const glm::vec3 center = translation + rotation * (offset * scale);
			outFrame = glm::translate(glm::mat4(1.0f), center) * glm::mat4_cast(rotation);
			outScale = glm::abs(scale);
			return true;
		}

		bool BoxColliderGizmo(const entt::registry&, entt::entity, const void* component, const glm::mat4& world, ComponentGizmoShape& outShape)
		{
			const BoxColliderComponent& collider = *static_cast<const BoxColliderComponent*>(component);
			glm::vec3 scale(1.0f);
			if (!GetColliderFrame(world, collider.Offset, outShape.Transform, scale))
				return false;
			outShape.Type = ComponentGizmoShape::Kind::Box;
			outShape.HalfExtents = collider.HalfExtents * scale;
			return true;
		}

		bool SphereColliderGizmo(const entt::registry&, entt::entity, const void* component, const glm::mat4& world, ComponentGizmoShape& outShape)
		{
			const SphereColliderComponent& collider = *static_cast<const SphereColliderComponent*>(component);
			glm::vec3 scale(1.0f);
			if (!GetColliderFrame(world, collider.Offset, outShape.Transform, scale))
				return false;
			outShape.Type = ComponentGizmoShape::Kind::Sphere;
			outShape.Radius = collider.Radius * std::max({ scale.x, scale.y, scale.z });
			return true;
		}

		bool CapsuleColliderGizmo(const entt::registry&, entt::entity, const void* component, const glm::mat4& world, ComponentGizmoShape& outShape)
		{
			const CapsuleColliderComponent& collider = *static_cast<const CapsuleColliderComponent*>(component);
			glm::vec3 scale(1.0f);
			if (!GetColliderFrame(world, collider.Offset, outShape.Transform, scale))
				return false;
			outShape.Type = ComponentGizmoShape::Kind::Capsule;
			outShape.Radius = collider.Radius * std::max(scale.x, scale.z);
			outShape.HalfHeight = collider.HalfHeight * scale.y;
			return true;
		}

		// The bounds of the collider's mesh, or of the Mesh Renderer's when it names none.
		bool MeshColliderGizmo(const entt::registry& registry, entt::entity entity, const void* component, const glm::mat4& world, ComponentGizmoShape& outShape)
		{
			const MeshColliderComponent& collider = *static_cast<const MeshColliderComponent*>(component);
			AssetHandle mesh = collider.Mesh;
			if (!mesh.IsValid())
			{
				const MeshRendererComponent* renderer = registry.try_get<MeshRendererComponent>(entity);
				mesh = renderer ? renderer->Mesh : AssetHandle::Null();
			}
			if (!mesh.IsValid())
				return false;
			outShape.Type = ComponentGizmoShape::Kind::MeshBounds;
			outShape.Transform = world;
			outShape.Mesh = mesh;
			return true;
		}

		// A character controller's capsule stands upright on the entity's origin, whatever the entity's rotation and scale.
		bool CharacterControllerGizmo(const entt::registry&, entt::entity, const void* component, const glm::mat4& world, ComponentGizmoShape& outShape)
		{
			const CharacterControllerComponent& controller = *static_cast<const CharacterControllerComponent*>(component);
			const float height = std::abs(controller.Height);
			const float radius = std::min(std::abs(controller.Radius), height * 0.5f);
			if (!std::isfinite(height) || !std::isfinite(radius))
				return false;
			const glm::vec3 center = glm::vec3(world[3]) + glm::vec3(0.0f, height * 0.5f, 0.0f);
			outShape.Type = ComponentGizmoShape::Kind::Capsule;
			outShape.Transform = glm::translate(glm::mat4(1.0f), center);
			outShape.Radius = radius;
			outShape.HalfHeight = height * 0.5f - radius;
			return true;
		}

	}

	void RegisterPhysicsComponents()
	{
		ComponentRegistry::Register<RigidBodyComponent>("RigidBody")
			.DisplayName("Rigid Body")
			.Category("Physics")
			.EnumProperty("Type", &RigidBodyComponent::Type, { { "Static", 0 }, { "Dynamic", 1 }, { "Kinematic", 2 } })
			.Property("Mass", &RigidBodyComponent::Mass, Range(0.001f, c_Unbounded, 0.1f))
			.Property("LinearDamping", &RigidBodyComponent::LinearDamping, Range(0.0f, 100.0f, 0.01f))
			.Property("AngularDamping", &RigidBodyComponent::AngularDamping, Range(0.0f, 100.0f, 0.01f))
			.Property("GravityScale", &RigidBodyComponent::GravityScale, Range(-100.0f, 100.0f, 0.05f))
			.Property("Friction", &RigidBodyComponent::Friction, Range(0.0f, 10.0f, 0.01f))
			.Property("Restitution", &RigidBodyComponent::Restitution, Slider(0.0f, 1.0f))
			.Property("IsTrigger", &RigidBodyComponent::IsTrigger, WithTooltip({}, "Reports overlaps without colliding"))
			.Property("ContinuousCollision", &RigidBodyComponent::ContinuousCollision, WithTooltip({}, "Also swept against kinematic and dynamic bodies, for bullets (every dynamic body is swept against static ones anyway)"))
			.Property("LockRotationX", &RigidBodyComponent::LockRotationX)
			.Property("LockRotationY", &RigidBodyComponent::LockRotationY)
			.Property("LockRotationZ", &RigidBodyComponent::LockRotationZ)
			.Property("Layer", &RigidBodyComponent::Layer, PhysicsLayer("Collision layer: it collides with the layers the project's layer matrix lets it"));

		ComponentRegistry::Register<BoxColliderComponent>("BoxCollider")
			.DisplayName("Box Collider")
			.Category("Physics")
			.Property("HalfExtents", &BoxColliderComponent::HalfExtents, Range(0.0f, 0.0f, 0.01f))
			.Property("Offset", &BoxColliderComponent::Offset, Range(0.0f, 0.0f, 0.01f))
			.Gizmo(ComponentGizmoGroup::Collider, BoxColliderGizmo);

		ComponentRegistry::Register<SphereColliderComponent>("SphereCollider")
			.DisplayName("Sphere Collider")
			.Category("Physics")
			.Property("Radius", &SphereColliderComponent::Radius, Range(0.001f, c_Unbounded, 0.01f))
			.Property("Offset", &SphereColliderComponent::Offset, Range(0.0f, 0.0f, 0.01f))
			.Gizmo(ComponentGizmoGroup::Collider, SphereColliderGizmo);

		ComponentRegistry::Register<CapsuleColliderComponent>("CapsuleCollider")
			.DisplayName("Capsule Collider")
			.Category("Physics")
			.Property("Radius", &CapsuleColliderComponent::Radius, Range(0.001f, c_Unbounded, 0.01f))
			.Property("HalfHeight", &CapsuleColliderComponent::HalfHeight, Range(0.0f, c_Unbounded, 0.01f))
			.Property("Offset", &CapsuleColliderComponent::Offset, Range(0.0f, 0.0f, 0.01f))
			.Gizmo(ComponentGizmoGroup::Collider, CapsuleColliderGizmo);

		ComponentRegistry::Register<MeshColliderComponent>("MeshCollider")
			.DisplayName("Mesh Collider")
			.Category("Physics")
			.AssetProperty("Mesh", &MeshColliderComponent::Mesh, AssetType::Mesh, { .Tooltip = "Uses the Mesh Renderer's mesh when unset" })
			.Property("Convex", &MeshColliderComponent::Convex, WithTooltip({}, "Non-convex meshes are only supported on static bodies"))
			.Gizmo(ComponentGizmoGroup::Collider, MeshColliderGizmo);

		ComponentRegistry::Register<CharacterControllerComponent>("CharacterController")
			.DisplayName("Character Controller")
			.Category("Physics")
			.Description("An upright capsule that scripts walk, jump and teleport: it slides along walls, walks up steps and walkable slopes and "
				"pushes dynamic bodies; its bottom is the entity's origin")
			.Property("Height", &CharacterControllerComponent::Height,
				WithTooltip(Range(0.01f, 1000.0f, 0.01f), "Total height of the capsule in world units; its bottom is the entity's origin"))
			.Property("Radius", &CharacterControllerComponent::Radius, WithTooltip(Range(0.005f, 500.0f, 0.01f), "World units"))
			.Property("StepHeight", &CharacterControllerComponent::StepHeight,
				WithTooltip(Range(0.0f, 100.0f, 0.01f), "Highest step it walks up, and how far it follows the ground down (world units)"))
			.Property("MaxSlopeDegrees", &CharacterControllerComponent::MaxSlopeDegrees,
				WithTooltip(Range(0.0f, 89.0f, 0.5f), "Steepest slope it walks up, in degrees from the horizontal"))
			.Property("Layer", &CharacterControllerComponent::Layer, PhysicsLayer("Collision layer: it collides with the layers the project's layer matrix lets it"))
			.Property("GravityScale", &CharacterControllerComponent::GravityScale, WithTooltip(Range(-100.0f, 100.0f, 0.05f), "Times the scene's gravity"))
			.Property("PushStrength", &CharacterControllerComponent::PushStrength,
				WithTooltip(Range(0.0f, c_Unbounded, 1.0f), "Largest force with which it pushes dynamic bodies, in newtons"))
			.Property("Enabled", &CharacterControllerComponent::Enabled, WithTooltip({}, "A disabled controller neither moves nor collides"))
			.Gizmo(ComponentGizmoGroup::Collider, CharacterControllerGizmo);
	}

}
