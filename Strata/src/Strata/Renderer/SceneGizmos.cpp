#include "stpch.h"
#include "Strata/Renderer/SceneGizmos.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Assert.h"
#include "Strata/Math/Frustum.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <type_traits>

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

		// What the settings' view lets through: shapes in its frustum and distance, and whether they show as icons.
		class GizmoView
		{
		public:
			explicit GizmoView(const SceneGizmoSettings& settings)
				: m_Settings(settings)
			{
				if (settings.View)
				{
					m_Frustum.emplace(settings.View->ViewProjection);
					m_TanHalfFOV = std::tan(settings.View->VerticalFOV * 0.5f);
				}
			}

			// Whether a shape within the sphere is drawn at all.
			bool IsVisible(const glm::vec3& center, float radius) const
			{
				if (!m_Settings.View)
					return true;
				if (glm::length(center - m_Settings.View->Position) - radius > m_Settings.MaxDistance)
					return false;
				return m_Frustum->IsSphereVisible(center, radius);
			}

			// Whether a shape of this bounding sphere would be too small on screen to read (MinShapePixels): an icon then.
			bool IsIcon(const glm::vec3& center, float radius) const
			{
				return m_Settings.View && 2.0f * radius < GetWorldSize(center, m_Settings.MinShapePixels);
			}

			// The world size of an icon at a position.
			float GetIconSize(const glm::vec3& center) const
			{
				return GetWorldSize(center, m_Settings.IconPixels);
			}

			// A diamond facing the view, `size` across, in a packed color (DebugDraw::PackColor).
			void DrawDiamond(DebugDraw& debugDraw, const glm::vec3& center, float size, uint32_t color, DebugDrawDepth depth) const
			{
				const glm::vec3 right = m_Settings.View->Right * (size * 0.5f);
				const glm::vec3 up = m_Settings.View->Up * (size * 0.5f);
				debugDraw.Line(center + up, center + right, color, depth);
				debugDraw.Line(center + right, center - up, color, depth);
				debugDraw.Line(center - up, center - right, color, depth);
				debugDraw.Line(center - right, center + up, color, depth);
			}

			const SceneGizmoView& GetView() const { return *m_Settings.View; }
		private:
			// The world size of `pixels` (of the viewport's height) at a position.
			float GetWorldSize(const glm::vec3& center, float pixels) const
			{
				const SceneGizmoView& view = *m_Settings.View;
				const float fraction = std::max(pixels, 1.0f) / std::max(view.ViewportHeight, 1.0f);
				if (view.Orthographic)
					return fraction * view.VerticalFOV;
				const float distance = std::max(glm::length(center - view.Position), 1.0e-3f);
				return fraction * 2.0f * distance * m_TanHalfFOV;
			}
		private:
			const SceneGizmoSettings& m_Settings;
			std::optional<Frustum> m_Frustum;
			float m_TanHalfFOV = 0.0f;
		};

		// The shapes of colliders, culled by the view (one function per collider type).
		void DrawCollider(DebugDraw& debugDraw, const BoxColliderComponent& collider, const glm::mat4& world, const SceneGizmoSettings& settings, const GizmoView& view)
		{
			GizmoTransform transform;
			if (!Decompose(world, transform))
				return;
			const glm::mat4 rigid = RigidTransform(transform, collider.Offset);
			const glm::vec3 halfExtents = collider.HalfExtents * glm::abs(transform.Scale);
			if (view.IsVisible(glm::vec3(rigid[3]), glm::length(halfExtents)))
				debugDraw.Box(rigid, halfExtents, settings.ColliderColor, settings.Depth);
		}

		void DrawCollider(DebugDraw& debugDraw, const SphereColliderComponent& collider, const glm::mat4& world, const SceneGizmoSettings& settings, const GizmoView& view)
		{
			GizmoTransform transform;
			if (!Decompose(world, transform))
				return;
			const glm::vec3 scale = glm::abs(transform.Scale);
			const glm::vec3 center(RigidTransform(transform, collider.Offset)[3]);
			const float radius = collider.Radius * std::max({ scale.x, scale.y, scale.z });
			if (view.IsVisible(center, radius))
				debugDraw.Sphere(center, radius, settings.ColliderColor, settings.Depth);
		}

		void DrawCollider(DebugDraw& debugDraw, const CapsuleColliderComponent& collider, const glm::mat4& world, const SceneGizmoSettings& settings, const GizmoView& view)
		{
			GizmoTransform transform;
			if (!Decompose(world, transform))
				return;
			const glm::vec3 scale = glm::abs(transform.Scale);
			const glm::mat4 rigid = RigidTransform(transform, collider.Offset);
			const float radius = collider.Radius * std::max(scale.x, scale.z);
			const float halfHeight = collider.HalfHeight * scale.y;
			if (view.IsVisible(glm::vec3(rigid[3]), radius + halfHeight))
				debugDraw.Capsule(rigid, radius, halfHeight, settings.ColliderColor, settings.Depth);
		}

		// A character controller's capsule stands upright on the entity's origin, whatever the entity's rotation and scale.
		void DrawCollider(DebugDraw& debugDraw, const CharacterControllerComponent& controller, const glm::mat4& world, const SceneGizmoSettings& settings, const GizmoView& view)
		{
			const float height = std::abs(controller.Height);
			const float radius = std::min(std::abs(controller.Radius), height * 0.5f);
			if (!std::isfinite(height) || !std::isfinite(radius))
				return;
			const glm::vec3 center = glm::vec3(world[3]) + glm::vec3(0.0f, height * 0.5f, 0.0f);
			if (view.IsVisible(center, height * 0.5f))
				debugDraw.Capsule(glm::translate(glm::mat4(1.0f), center), radius, height * 0.5f - radius, settings.ColliderColor, settings.Depth);
		}

		void DrawMeshCollider(DebugDraw& debugDraw, const entt::registry& registry, entt::entity entity, const MeshColliderComponent& collider, const glm::mat4& world,
			const SceneGizmoSettings& settings, const GizmoView& view)
		{
			AssetHandle meshHandle = collider.Mesh;
			if (!meshHandle.IsValid())
			{
				const MeshRendererComponent* renderer = registry.try_get<MeshRendererComponent>(entity);
				meshHandle = renderer ? renderer->Mesh : UUID::Null();
			}
			// Gizmos never trigger loads: a mesh that is not loaded yet shows nothing.
			Ref<Mesh> mesh = AssetManager::HasActive() && AssetManager::GetAssetState(meshHandle) == AssetState::Ready ? AssetManager::GetAsset<Mesh>(meshHandle) : nullptr;
			if (!mesh || !mesh->GetBounds().IsValid())
				return;
			const AABB& bounds = mesh->GetBounds();
			const AABB worldBounds = bounds.Transformed(world);
			if (view.IsVisible(worldBounds.GetCenter(), glm::length(worldBounds.GetExtents())))
				debugDraw.Box(world * glm::translate(glm::mat4(1.0f), bounds.GetCenter()), bounds.GetExtents(), settings.ColliderColor, settings.Depth);
		}

		// Every collider of one entity (an entity of the settings' list).
		void DrawEntityColliders(DebugDraw& debugDraw, const entt::registry& registry, entt::entity entity, const SceneGizmoSettings& settings, const GizmoView& view)
		{
			const WorldTransformComponent* world = registry.try_get<WorldTransformComponent>(entity);
			if (!world || !world->ActiveInHierarchy)
				return;
			if (const BoxColliderComponent* collider = registry.try_get<BoxColliderComponent>(entity))
				DrawCollider(debugDraw, *collider, world->Matrix, settings, view);
			if (const SphereColliderComponent* collider = registry.try_get<SphereColliderComponent>(entity))
				DrawCollider(debugDraw, *collider, world->Matrix, settings, view);
			if (const CapsuleColliderComponent* collider = registry.try_get<CapsuleColliderComponent>(entity))
				DrawCollider(debugDraw, *collider, world->Matrix, settings, view);
			if (const MeshColliderComponent* collider = registry.try_get<MeshColliderComponent>(entity))
				DrawMeshCollider(debugDraw, registry, entity, *collider, world->Matrix, settings, view);
			if (const CharacterControllerComponent* controller = registry.try_get<CharacterControllerComponent>(entity))
				DrawCollider(debugDraw, *controller, world->Matrix, settings, view);
		}

		template<typename Component>
		size_t CountComponents(const entt::registry& registry)
		{
			const auto* storage = registry.storage<Component>();
			return storage ? storage->size() : 0;
		}

		// The colliders of one type of every active entity that `include` accepts.
		template<typename Collider, typename Include>
		void DrawAllColliders(DebugDraw& debugDraw, const entt::registry& registry, const SceneGizmoSettings& settings, const GizmoView& view, const Include& include)
		{
			for (auto [entity, collider, world] : registry.view<Collider, WorldTransformComponent>().each())
			{
				if (!world.ActiveInHierarchy || !include(entity))
					continue;
				if constexpr (std::is_same_v<Collider, MeshColliderComponent>)
					DrawMeshCollider(debugDraw, registry, entity, collider, world.Matrix, settings, view);
				else
					DrawCollider(debugDraw, collider, world.Matrix, settings, view);
			}
		}

	}

	void DrawSceneGizmos(DebugDraw& debugDraw, const Scene& scene, const SceneGizmoSettings& settings)
	{
		ST_PROFILE_FUNCTION();
		const entt::registry& registry = scene.GetRegistry();
		const DebugDrawDepth depth = settings.Depth;
		const GizmoView view(settings);

		if (settings.Lights)
		{
			for (auto [entity, light, world] : registry.view<DirectionalLightComponent, WorldTransformComponent>().each())
			{
				if (!world.ActiveInHierarchy)
					continue;
				const glm::vec3 position(world.Matrix[3]);
				const glm::vec3 direction(world.Matrix * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
				const float length = glm::length(direction);
				if (!(length > 1e-6f) || !view.IsVisible(position, c_DirectionalArrowLength))
					continue;
				if (view.IsIcon(position, c_DirectionalArrowLength))
				{
					const float size = view.GetIconSize(position);
					const uint32_t color = DebugDraw::PackColor(LightColor(light.Color));
					view.DrawDiamond(debugDraw, position, size, color, depth);
					debugDraw.Line(position, position + direction / length * size, color, depth);
					continue;
				}
				debugDraw.Arrow(position, position + direction / length * c_DirectionalArrowLength, LightColor(light.Color), depth);
			}
			for (auto [entity, light, world] : registry.view<PointLightComponent, WorldTransformComponent>().each())
			{
				const glm::vec3 position(world.Matrix[3]);
				if (!world.ActiveInHierarchy || !view.IsVisible(position, light.Range))
					continue;
				if (view.IsIcon(position, light.Range))
				{
					// Far away, a point light is a diamond with a cross, small and the same size on screen at any distance.
					const float size = view.GetIconSize(position);
					if (!view.IsVisible(position, size))
						continue;
					const uint32_t color = DebugDraw::PackColor(LightColor(light.Color));
					const glm::vec3 right = view.GetView().Right * (size * 0.25f);
					const glm::vec3 up = view.GetView().Up * (size * 0.25f);
					view.DrawDiamond(debugDraw, position, size, color, depth);
					debugDraw.Line(position - right, position + right, color, depth);
					debugDraw.Line(position - up, position + up, color, depth);
					continue;
				}
				debugDraw.Sphere(position, light.Range, LightColor(light.Color), depth);
			}
			for (auto [entity, light, world] : registry.view<SpotLightComponent, WorldTransformComponent>().each())
			{
				if (!world.ActiveInHierarchy)
					continue;
				const glm::vec3 position(world.Matrix[3]);
				const glm::vec3 direction(world.Matrix * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
				const float directionLength = glm::length(direction);
				const float outer = glm::radians(std::clamp(light.OuterConeAngle, 0.1f, 89.9f));
				// The cone's slant edge is as long as the range.
				const float coneLength = light.Range * std::cos(outer);
				const float baseRadius = light.Range * std::sin(outer);
				const glm::vec3 axis = directionLength > 1e-6f ? direction / directionLength : glm::vec3(0.0f, 0.0f, -1.0f);
				const glm::vec3 coneCenter = position + axis * (coneLength * 0.5f);
				const float coneRadius = std::sqrt(coneLength * coneLength * 0.25f + baseRadius * baseRadius);
				if (!view.IsVisible(coneCenter, coneRadius))
					continue;
				if (view.IsIcon(coneCenter, coneRadius))
				{
					// A diamond with a stroke along the light's direction.
					const float size = view.GetIconSize(position);
					if (!view.IsVisible(position, size))
						continue;
					const uint32_t color = DebugDraw::PackColor(LightColor(light.Color));
					view.DrawDiamond(debugDraw, position, size, color, depth);
					debugDraw.Line(position, position + axis * size, color, depth);
					continue;
				}
				debugDraw.Cone(position, direction, coneLength, outer, LightColor(light.Color), depth);
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
				float length = 0.0f;
				float halfHeight = 0.0f;
				if (shown.Projection == ProjectionType::Perspective)
				{
					shown.PerspectiveFar = std::min(shown.PerspectiveFar, std::max(maxLength, shown.PerspectiveNear + 1e-3f));
					length = shown.PerspectiveFar;
					halfHeight = length * std::tan(glm::radians(shown.PerspectiveFOV) * 0.5f);
				}
				else
				{
					shown.OrthographicFar = std::min(shown.OrthographicFar, std::max(maxLength, shown.OrthographicNear + 1e-3f));
					length = shown.OrthographicFar;
					halfHeight = shown.OrthographicSize * 0.5f;
				}
				// The frustum's corners are within this distance of the camera.
				const float radius = std::sqrt(length * length + halfHeight * halfHeight * (1.0f + settings.CameraAspectRatio * settings.CameraAspectRatio));
				if (!std::isfinite(radius) || !view.IsVisible(transform.Translation, radius))
					continue;
				if (view.IsIcon(transform.Translation, radius))
				{
					// A small body facing the view, with a stroke along the camera's view direction.
					const float size = view.GetIconSize(transform.Translation);
					if (!view.IsVisible(transform.Translation, size))
						continue;
					const glm::vec3 right = view.GetView().Right * (size * 0.5f);
					const glm::vec3 up = view.GetView().Up * (size * 0.35f);
					const glm::vec3& center = transform.Translation;
					const uint32_t color = DebugDraw::PackColor(settings.CameraColor);
					debugDraw.Line(center - right - up, center + right - up, color, depth);
					debugDraw.Line(center + right - up, center + right + up, color, depth);
					debugDraw.Line(center + right + up, center - right + up, color, depth);
					debugDraw.Line(center - right + up, center - right - up, color, depth);
					debugDraw.Line(center, center + transform.Rotation * glm::vec3(0.0f, 0.0f, -size), color, depth);
					continue;
				}
				// The view ignores the entity's scale, like SceneCamera::FromEntity.
				const glm::mat4 cameraView = glm::inverse(glm::translate(glm::mat4(1.0f), transform.Translation) * glm::mat4_cast(transform.Rotation));
				debugDraw.Frustum(shown.GetProjection(settings.CameraAspectRatio) * cameraView, settings.CameraColor, depth);
			}
		}

		if (!settings.Colliders)
			return;
		auto drawColliders = [&](const auto& include)
		{
			DrawAllColliders<BoxColliderComponent>(debugDraw, registry, settings, view, include);
			DrawAllColliders<SphereColliderComponent>(debugDraw, registry, settings, view, include);
			DrawAllColliders<CapsuleColliderComponent>(debugDraw, registry, settings, view, include);
			DrawAllColliders<MeshColliderComponent>(debugDraw, registry, settings, view, include);
			DrawAllColliders<CharacterControllerComponent>(debugDraw, registry, settings, view, include);
		};
		if (settings.AllColliders)
		{
			drawColliders([](entt::entity) { return true; });
			return;
		}

		// The listed entities' colliders, from whichever side is smaller: a large selection in a scene with few colliders
		// costs the colliders, a small selection in a scene with many costs the selection.
		const std::span<const Entity> listed = settings.ColliderEntities;
		ST_CORE_ASSERT(std::is_sorted(listed.begin(), listed.end(), [](const Entity& a, const Entity& b) { return a.GetHandle() < b.GetHandle(); }),
			"SceneGizmoSettings::ColliderEntities must be sorted by entity handle");
		const size_t colliders = CountComponents<BoxColliderComponent>(registry) + CountComponents<SphereColliderComponent>(registry)
			+ CountComponents<CapsuleColliderComponent>(registry) + CountComponents<MeshColliderComponent>(registry)
			+ CountComponents<CharacterControllerComponent>(registry);
		if (colliders < listed.size())
		{
			drawColliders([&](entt::entity entity)
			{
				const auto it = std::lower_bound(listed.begin(), listed.end(), entity, [](const Entity& a, entt::entity b) { return a.GetHandle() < b; });
				return it != listed.end() && it->GetHandle() == entity && it->GetScene() == &scene;
			});
			return;
		}
		for (const Entity& entity : listed)
		{
			if (entity.GetScene() == &scene && entity.IsValid())
				DrawEntityColliders(debugDraw, registry, entity.GetHandle(), settings, view);
		}
	}

}
