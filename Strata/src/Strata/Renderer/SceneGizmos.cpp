#include "stpch.h"
#include "Strata/Renderer/SceneGizmos.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Strata
{

	namespace
	{

		constexpr float c_DirectionalArrowLength = 1.5f;

		struct GizmoTransform
		{
			glm::vec3 Translation = glm::vec3(0.0f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 Scale = glm::vec3(1.0f);
		};

		bool Decompose(const glm::mat4& matrix, GizmoTransform& outTransform)
		{
			return Math::DecomposeTransform(matrix, outTransform.Translation, outTransform.Rotation, outTransform.Scale);
		}

		// Rotation and translation of the entity, without its scale, moved by a local offset (scaled like the entity).
		glm::mat4 RigidTransform(const GizmoTransform& transform, const glm::vec3& offset)
		{
			const glm::vec3 center = transform.Translation + transform.Rotation * (offset * transform.Scale);
			return glm::translate(glm::mat4(1.0f), center) * glm::mat4_cast(transform.Rotation);
		}

		glm::vec4 LightColor(const glm::vec3& color)
		{
			return glm::vec4(glm::clamp(color, glm::vec3(0.0f), glm::vec3(1.0f)), 1.0f);
		}

	}

	void DrawSceneGizmos(DebugDraw& debugDraw, const Scene& scene, const SceneGizmoSettings& settings)
	{
		const entt::registry& registry = scene.GetRegistry();
		const DebugDrawDepth depth = settings.Depth;

		if (settings.Lights)
		{
			for (auto [entity, light, world] : registry.view<DirectionalLightComponent, WorldTransformComponent>().each())
			{
				if (!world.ActiveInHierarchy)
					continue;
				const glm::vec3 position(world.Matrix[3]);
				const glm::vec3 direction(world.Matrix * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
				const float length = glm::length(direction);
				if (length > 1e-6f)
					debugDraw.Arrow(position, position + direction / length * c_DirectionalArrowLength, LightColor(light.Color), depth);
			}
			for (auto [entity, light, world] : registry.view<PointLightComponent, WorldTransformComponent>().each())
			{
				if (world.ActiveInHierarchy)
					debugDraw.Sphere(glm::vec3(world.Matrix[3]), light.Range, LightColor(light.Color), depth);
			}
			for (auto [entity, light, world] : registry.view<SpotLightComponent, WorldTransformComponent>().each())
			{
				if (!world.ActiveInHierarchy)
					continue;
				const glm::vec3 direction(world.Matrix * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
				const float outer = glm::radians(std::clamp(light.OuterConeAngle, 0.1f, 89.9f));
				// The cone's slant edge is as long as the range.
				debugDraw.Cone(glm::vec3(world.Matrix[3]), direction, light.Range * std::cos(outer), outer, LightColor(light.Color), depth);
			}
		}

		if (settings.Cameras)
		{
			for (auto [entity, camera, world] : registry.view<CameraComponent, WorldTransformComponent>().each())
			{
				GizmoTransform transform;
				if (!world.ActiveInHierarchy || !Decompose(world.Matrix, transform))
					continue;
				CameraComponent shown = camera;
				const float maxLength = std::max(settings.MaxCameraFrustumLength, 1e-3f);
				if (shown.Projection == ProjectionType::Perspective)
					shown.PerspectiveFar = std::min(shown.PerspectiveFar, std::max(maxLength, shown.PerspectiveNear + 1e-3f));
				else
					shown.OrthographicFar = std::min(shown.OrthographicFar, std::max(maxLength, shown.OrthographicNear + 1e-3f));
				// The view ignores the entity's scale, like SceneCamera::FromEntity.
				const glm::mat4 view = glm::inverse(glm::translate(glm::mat4(1.0f), transform.Translation) * glm::mat4_cast(transform.Rotation));
				debugDraw.Frustum(shown.GetProjection(settings.CameraAspectRatio) * view, settings.CameraColor, depth);
			}
		}

		if (!settings.Colliders)
			return;
		const glm::vec4& color = settings.ColliderColor;
		for (auto [entity, collider, world] : registry.view<BoxColliderComponent, WorldTransformComponent>().each())
		{
			GizmoTransform transform;
			if (world.ActiveInHierarchy && Decompose(world.Matrix, transform))
				debugDraw.Box(RigidTransform(transform, collider.Offset), collider.HalfExtents * glm::abs(transform.Scale), color, depth);
		}
		for (auto [entity, collider, world] : registry.view<SphereColliderComponent, WorldTransformComponent>().each())
		{
			GizmoTransform transform;
			if (!world.ActiveInHierarchy || !Decompose(world.Matrix, transform))
				continue;
			const glm::vec3 scale = glm::abs(transform.Scale);
			debugDraw.Sphere(glm::vec3(RigidTransform(transform, collider.Offset)[3]), collider.Radius * std::max({ scale.x, scale.y, scale.z }), color, depth);
		}
		for (auto [entity, collider, world] : registry.view<CapsuleColliderComponent, WorldTransformComponent>().each())
		{
			GizmoTransform transform;
			if (!world.ActiveInHierarchy || !Decompose(world.Matrix, transform))
				continue;
			const glm::vec3 scale = glm::abs(transform.Scale);
			debugDraw.Capsule(RigidTransform(transform, collider.Offset), collider.Radius * std::max(scale.x, scale.z), collider.HalfHeight * scale.y, color, depth);
		}
		for (auto [entity, collider, world] : registry.view<MeshColliderComponent, WorldTransformComponent>().each())
		{
			if (!world.ActiveInHierarchy)
				continue;
			AssetHandle meshHandle = collider.Mesh;
			if (!meshHandle.IsValid())
			{
				const MeshRendererComponent* renderer = registry.try_get<MeshRendererComponent>(entity);
				meshHandle = renderer ? renderer->Mesh : UUID::Null();
			}
			// Gizmos never trigger loads: a mesh that is not loaded yet shows nothing.
			Ref<Mesh> mesh = AssetManager::HasActive() && AssetManager::GetAssetState(meshHandle) == AssetState::Ready ? AssetManager::GetAsset<Mesh>(meshHandle) : nullptr;
			if (!mesh || !mesh->GetBounds().IsValid())
				continue;
			const AABB& bounds = mesh->GetBounds();
			debugDraw.Box(world.Matrix * glm::translate(glm::mat4(1.0f), bounds.GetCenter()), bounds.GetExtents(), color, depth);
		}
	}

}
