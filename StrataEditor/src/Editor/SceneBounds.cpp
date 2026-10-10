#include "Editor/SceneBounds.h"

#include "Editor/SceneEdit.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace Strata
{

	namespace
	{

		// Approximate text metrics in em: the exact layout needs the font's glyphs, framing does not.
		constexpr float c_TextAdvancePerCharacter = 0.6f;
		constexpr float c_TextLineHeight = 1.2f;

		AABB GetPlaceholderBounds()
		{
			return AABB(glm::vec3(-SceneBounds::c_PlaceholderExtent), glm::vec3(SceneBounds::c_PlaceholderExtent));
		}

		// Local-space bounds of world-space text, which lies in the entity's XY plane, vertically centered on its origin
		// (see TextRenderer), from the number of lines and the longest line's characters.
		AABB GetTextBounds(const TextComponent& text)
		{
			size_t lines = 1;
			size_t longestLine = 0;
			size_t currentLine = 0;
			for (const char character : std::string_view(text.Text))
			{
				if (character == '\n')
				{
					lines++;
					currentLine = 0;
				}
				else if ((static_cast<unsigned char>(character) & 0xC0) != 0x80) // Count code points, not UTF-8 continuation bytes
				{
					longestLine = std::max(longestLine, ++currentLine);
				}
			}
			const float width = static_cast<float>(longestLine) * c_TextAdvancePerCharacter * text.FontSize;
			const float halfHeight = static_cast<float>(lines) * c_TextLineHeight * text.FontSize * 0.5f;
			float left = -width * 0.5f;
			if (text.Alignment == TextAlignment::Left)
				left = 0.0f;
			else if (text.Alignment == TextAlignment::Right)
				left = -width;
			return AABB(glm::vec3(left, -halfHeight, 0.0f), glm::vec3(left + width, halfHeight, 0.0f));
		}

		// Local-space bounds of what the entity renders: its mesh (the placeholder box while the mesh loads, which is then
		// reported in outPendingMeshes) and its world-space text. Invalid when it renders nothing.
		AABB GetRenderableLocalBounds(Entity entity, std::vector<AssetHandle>* outPendingMeshes)
		{
			AABB bounds;
			if (const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>(); renderer && renderer->Mesh.IsValid())
			{
				// GetAsset never blocks: it requests the load, so a later call may know the mesh.
				Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(renderer->Mesh);
				if (mesh && mesh->GetBounds().IsValid())
				{
					bounds.Expand(mesh->GetBounds());
				}
				else
				{
					bounds.Expand(GetPlaceholderBounds());
					// GetAsset started loading it unless it cannot be loaded (unknown handle, failed load).
					if (outPendingMeshes && AssetManager::GetAssetState(renderer->Mesh) == AssetState::Loading)
						outPendingMeshes->push_back(renderer->Mesh);
				}
			}
			if (const TextComponent* text = entity.TryGetComponent<TextComponent>(); text && !text->ScreenSpace && !text->Text.empty()
				&& text->FontSize > 0.0f && std::isfinite(text->FontSize))
			{
				bounds.Expand(GetTextBounds(*text));
			}
			return bounds;
		}

		glm::mat4 GetCachedWorldTransform(const Scene& scene, Entity entity)
		{
			const WorldTransformComponent* world = entity.TryGetComponent<WorldTransformComponent>();
			return world ? world->Matrix : scene.GetWorldTransform(entity);
		}

		// The renderable bounds of an entity's subtree (or of the entity alone), or its placeholder boxes when nothing in
		// it renders. Uses the cached world transforms (call Scene::UpdateWorldTransforms first).
		AABB GetContentBounds(const Scene& scene, Entity entity, bool includeDescendants)
		{
			std::vector<UUID> members;
			if (includeDescendants)
				members = SceneEdit::CollectSubtree(scene, entity.GetUUID());
			else
				members.push_back(entity.GetUUID());

			AABB renderable;
			AABB placeholders;
			for (UUID id : members)
			{
				Entity member = scene.GetEntityByUUID(id);
				if (!member)
					continue;
				const glm::mat4 world = GetCachedWorldTransform(scene, member);
				const AABB local = GetRenderableLocalBounds(member, nullptr);
				if (local.IsValid())
					renderable.Expand(local.Transformed(world));
				else if (!renderable.IsValid())
					placeholders.Expand(GetPlaceholderBounds().Transformed(world));
			}
			return renderable.IsValid() ? renderable : placeholders;
		}

	}

	namespace SceneBounds
	{

		AABB GetEntityBounds(Scene& scene, Entity entity, bool includeDescendants)
		{
			if (!entity || entity.GetScene() != &scene)
				return {};
			scene.UpdateWorldTransforms();
			return GetContentBounds(scene, entity, includeDescendants);
		}

		AABB GetEntitiesBounds(Scene& scene, std::span<const Entity> entities)
		{
			scene.UpdateWorldTransforms();
			// Each entity is measured on its own: an entity that renders nothing still counts when it was asked for.
			AABB bounds;
			for (Entity entity : entities)
			{
				if (entity && entity.GetScene() == &scene)
					bounds.Expand(GetContentBounds(scene, entity, true));
			}
			return bounds;
		}

		AABB GetSceneBounds(Scene& scene, std::vector<AssetHandle>* outPendingMeshes)
		{
			scene.UpdateWorldTransforms();
			AABB renderable;
			AABB placeholders;
			auto view = scene.GetRegistry().view<WorldTransformComponent>();
			for (const entt::entity handle : view)
			{
				const WorldTransformComponent& world = view.get<WorldTransformComponent>(handle);
				if (!world.ActiveInHierarchy)
					continue;
				const AABB local = GetRenderableLocalBounds(Entity(handle, &scene), outPendingMeshes);
				if (local.IsValid())
					renderable.Expand(local.Transformed(world.Matrix));
				else if (!renderable.IsValid())
					placeholders.Expand(GetPlaceholderBounds().Transformed(world.Matrix));
			}
			if (outPendingMeshes)
			{
				std::sort(outPendingMeshes->begin(), outPendingMeshes->end());
				outPendingMeshes->erase(std::unique(outPendingMeshes->begin(), outPendingMeshes->end()), outPendingMeshes->end());
			}
			return renderable.IsValid() ? renderable : placeholders;
		}

	}

}
