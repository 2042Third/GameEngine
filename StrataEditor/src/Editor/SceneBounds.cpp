#include "Editor/SceneBounds.h"

#include "Editor/SceneEdit.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

namespace Strata
{

	namespace
	{

		// Local-space bounds of the entity's own content. Mesh bounds come from the loaded mesh; GetAsset never blocks
		// (it requests the load, so the next call may know the mesh).
		AABB GetLocalBounds(Entity entity)
		{
			if (const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>())
			{
				if (Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(renderer->Mesh))
				{
					if (mesh->GetBounds().IsValid())
						return mesh->GetBounds();
				}
			}
			return AABB(glm::vec3(-SceneBounds::c_PlaceholderExtent), glm::vec3(SceneBounds::c_PlaceholderExtent));
		}

		// World bounds from the cached world transform (call Scene::UpdateWorldTransforms first).
		AABB GetCachedWorldBounds(const Scene& scene, Entity entity)
		{
			const WorldTransformComponent* world = entity.TryGetComponent<WorldTransformComponent>();
			return GetLocalBounds(entity).Transformed(world ? world->Matrix : scene.GetWorldTransform(entity));
		}

		void ExpandBySubtree(const Scene& scene, Entity entity, AABB& bounds)
		{
			for (UUID id : SceneEdit::CollectSubtree(scene, entity.GetUUID()))
			{
				if (Entity member = scene.GetEntityByUUID(id))
					bounds.Expand(GetCachedWorldBounds(scene, member));
			}
		}

	}

	namespace SceneBounds
	{

		AABB GetEntityBounds(Scene& scene, Entity entity, bool includeDescendants)
		{
			if (!entity || entity.GetScene() != &scene)
				return {};
			scene.UpdateWorldTransforms();
			if (!includeDescendants)
				return GetCachedWorldBounds(scene, entity);
			AABB bounds;
			ExpandBySubtree(scene, entity, bounds);
			return bounds;
		}

		AABB GetEntitiesBounds(Scene& scene, std::span<const Entity> entities)
		{
			scene.UpdateWorldTransforms();
			AABB bounds;
			for (Entity entity : entities)
			{
				if (entity && entity.GetScene() == &scene)
					ExpandBySubtree(scene, entity, bounds);
			}
			return bounds;
		}

		AABB GetSceneBounds(Scene& scene)
		{
			scene.UpdateWorldTransforms();
			AABB bounds;
			auto view = scene.GetRegistry().view<WorldTransformComponent>();
			for (const entt::entity handle : view)
			{
				const WorldTransformComponent& world = view.get<WorldTransformComponent>(handle);
				if (world.ActiveInHierarchy)
					bounds.Expand(GetLocalBounds(Entity(handle, &scene)).Transformed(world.Matrix));
			}
			return bounds;
		}

	}

}
