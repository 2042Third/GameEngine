#include "Editor/SceneBounds.h"

#include "Editor/SceneEdit.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneServices.h>

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
		// reported in outPendingMeshes; a skinned mesh in its animator's pose, its bind pose while the pose's skeleton loads,
		// which is then reported) and its world-space text. Invalid when it renders nothing.
		AABB GetRenderableLocalBounds(Entity entity, std::vector<AssetHandle>* outPendingMeshes)
		{
			AABB bounds;
			if (const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>(); renderer && renderer->Mesh.IsValid())
			{
				// GetAsset never blocks: it requests the load, so a later call may know the mesh.
				Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(renderer->Mesh);
				AssetHandle waitingSkeleton = UUID::Null();
				const AABB posed = mesh && mesh->IsSkinned() ? SceneBounds::GetPosedMeshBounds(*entity.GetScene(), entity, *mesh, &waitingSkeleton) : AABB();
				if (posed.IsValid())
				{
					bounds.Expand(posed);
				}
				else if (mesh && mesh->GetBounds().IsValid())
				{
					bounds.Expand(mesh->GetBounds());
					if (outPendingMeshes && waitingSkeleton.IsValid())
						outPendingMeshes->push_back(waitingSkeleton);
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

		bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// World-space bounds of an entity's physical shape: a character controller's capsule (upright from the entity's
		// origin, as physics places it, whatever the entity's rotation and scale) and its box, sphere and capsule colliders
		// (with their offsets, in the entity's space). Invalid without any. For framing what renders nothing: a character made
		// of a controller is framed by its capsule, not by a box at its feet.
		AABB GetShapeBounds(Entity entity, const glm::mat4& world)
		{
			AABB bounds;
			if (const CharacterControllerComponent* controller = entity.TryGetComponent<CharacterControllerComponent>())
			{
				const float height = std::abs(controller->Height);
				const float radius = std::min(std::abs(controller->Radius), height * 0.5f);
				const glm::vec3 origin(world[3]);
				if (height > 0.0f && std::isfinite(height) && IsFinite(origin))
					bounds.Expand(AABB(origin - glm::vec3(radius, 0.0f, radius), origin + glm::vec3(radius, height, radius)));
			}
			AABB local;
			if (const BoxColliderComponent* box = entity.TryGetComponent<BoxColliderComponent>(); box && IsFinite(box->HalfExtents) && IsFinite(box->Offset))
				local.Expand(AABB(box->Offset - glm::abs(box->HalfExtents), box->Offset + glm::abs(box->HalfExtents)));
			if (const SphereColliderComponent* sphere = entity.TryGetComponent<SphereColliderComponent>(); sphere && std::isfinite(sphere->Radius) && IsFinite(sphere->Offset))
				local.Expand(AABB(sphere->Offset - glm::vec3(std::abs(sphere->Radius)), sphere->Offset + glm::vec3(std::abs(sphere->Radius))));
			if (const CapsuleColliderComponent* capsule = entity.TryGetComponent<CapsuleColliderComponent>();
				capsule && std::isfinite(capsule->Radius) && std::isfinite(capsule->HalfHeight) && IsFinite(capsule->Offset))
			{
				const float radius = std::abs(capsule->Radius);
				const glm::vec3 extent(radius, std::abs(capsule->HalfHeight) + radius, radius);
				local.Expand(AABB(capsule->Offset - extent, capsule->Offset + extent));
			}
			if (local.IsValid())
				bounds.Expand(local.Transformed(world));
			return bounds;
		}

		glm::mat4 GetCachedWorldTransform(const Scene& scene, Entity entity)
		{
			const WorldTransformComponent* world = entity.TryGetComponent<WorldTransformComponent>();
			return world ? world->Matrix : scene.GetWorldTransform(entity);
		}

		// The renderable bounds of an entity's subtree (or of the entity alone), or its placeholder boxes when nothing in
		// it renders. Uses the cached world transforms (call Scene::UpdateWorldTransforms first).
		AABB GetContentBounds(const Scene& scene, Entity entity, bool includeDescendants, std::vector<AssetHandle>* outPending)
		{
			std::vector<UUID> members;
			if (includeDescendants)
				members = SceneEdit::CollectSubtree(scene, entity.GetUUID());
			else
				members.push_back(entity.GetUUID());

			AABB renderable;
			AABB shapes;
			AABB placeholders;
			for (UUID id : members)
			{
				Entity member = scene.GetEntityByUUID(id);
				if (!member)
					continue;
				const glm::mat4 world = GetCachedWorldTransform(scene, member);
				const AABB local = GetRenderableLocalBounds(member, outPending);
				if (local.IsValid())
				{
					renderable.Expand(local.Transformed(world));
					continue;
				}
				if (renderable.IsValid())
					continue;
				if (const AABB shape = GetShapeBounds(member, world); shape.IsValid())
					shapes.Expand(shape);
				else
					placeholders.Expand(GetPlaceholderBounds().Transformed(world));
			}
			return renderable.IsValid() ? renderable : (shapes.IsValid() ? shapes : placeholders);
		}

	}

	namespace SceneBounds
	{

		AABB GetPosedMeshBounds(const Scene& scene, Entity entity, const Mesh& mesh, AssetHandle* outWaitingSkeleton)
		{
			const SkinComponent* skin = entity.TryGetComponent<SkinComponent>();
			const Ref<const MeshSkinBinding>& binding = mesh.GetSkinBinding();
			const ISkinningPoses* poses = scene.GetService<ISkinningPoses>();
			if (!skin || !binding || !poses || !skin->Animator.IsValid())
				return {};
			const Entity animator = scene.GetEntityByUUID(skin->Animator);
			if (!animator)
				return {};
			SkeletonPose pose;
			if (!poses->GetPose(animator.GetHandle(), pose))
			{
				// The pose comes once its skeleton is in: ask for it, so that whoever measures again knows when.
				const AnimatorComponent* component = animator.TryGetComponent<AnimatorComponent>();
				if (component && component->Skeleton.IsValid() && outWaitingSkeleton)
				{
					if (AssetManagerBase* manager = scene.GetAssetManager())
					{
						manager->GetAsset(component->Skeleton);
						if (manager->GetAssetState(component->Skeleton) == AssetState::Loading)
							*outWaitingSkeleton = component->Skeleton;
					}
				}
				return {};
			}
			if (binding->Joints.size() != binding->InverseBindMatrices.size() || binding->Joints.size() != binding->JointBounds.size())
				return {};
			const glm::mat4 meshWorld = GetCachedWorldTransform(scene, entity);
			const glm::mat4 animatorWorld = GetCachedWorldTransform(scene, animator);
			const float determinant = glm::determinant(glm::mat3(meshWorld));
			if (!std::isfinite(determinant) || std::abs(determinant) < Scene::c_MinInvertibleDeterminant)
				return {};
			const glm::mat4 animatorToMesh = glm::inverse(meshWorld) * animatorWorld;
			AABB bounds;
			for (size_t joint = 0; joint < binding->Joints.size(); joint++)
			{
				const uint16_t skeletonJoint = binding->Joints[joint];
				if (skeletonJoint >= pose.Joints.size())
					return {};
				bounds.Expand(binding->JointBounds[joint].Transformed(animatorToMesh * pose.Joints[skeletonJoint] * binding->InverseBindMatrices[joint]));
			}
			return bounds;
		}

		AABB GetEntityBounds(Scene& scene, Entity entity, bool includeDescendants)
		{
			return GetEntityBounds(scene, entity, includeDescendants, nullptr);
		}

		AABB GetEntityBounds(Scene& scene, Entity entity, bool includeDescendants, std::vector<AssetHandle>* outPending)
		{
			if (!entity || entity.GetScene() != &scene)
				return {};
			scene.UpdateWorldTransforms();
			return GetContentBounds(scene, entity, includeDescendants, outPending);
		}

		AABB GetOwnBounds(const Scene& scene, Entity entity)
		{
			if (!entity || entity.GetScene() != &scene)
				return {};
			const glm::mat4 world = GetCachedWorldTransform(scene, entity);
			if (const AABB local = GetRenderableLocalBounds(entity, nullptr); local.IsValid())
				return local.Transformed(world);
			if (const AABB shape = GetShapeBounds(entity, world); shape.IsValid())
				return shape;
			const glm::vec3 origin(world[3]);
			return AABB(origin, origin);
		}

		AABB GetEntitiesBounds(Scene& scene, std::span<const Entity> entities)
		{
			scene.UpdateWorldTransforms();
			// Each entity is measured on its own: an entity that renders nothing still counts when it was asked for.
			AABB bounds;
			for (Entity entity : entities)
			{
				if (entity && entity.GetScene() == &scene)
					bounds.Expand(GetContentBounds(scene, entity, true, nullptr));
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
