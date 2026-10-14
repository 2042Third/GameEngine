#include "Editor/WorldCellIO.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Math/AABB.h>
#include <Strata/Physics/PhysicsWorld.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <algorithm>
#include <array>

namespace Strata
{

	namespace
	{

		// Grows the bounds by a local box transformed into the world.
		void AddBox(AABB& bounds, const glm::mat4& transform, const glm::vec3& minimum, const glm::vec3& maximum)
		{
			for (int corner = 0; corner < 8; corner++)
			{
				const glm::vec3 local((corner & 1) ? maximum.x : minimum.x, (corner & 2) ? maximum.y : minimum.y, (corner & 4) ? maximum.z : minimum.z);
				const glm::vec3 world = glm::vec3(transform * glm::vec4(local, 1.0f));
				bounds.Min = glm::min(bounds.Min, world);
				bounds.Max = glm::max(bounds.Max, world);
			}
		}

	}

	namespace WorldCellIO
	{

		CellSummary SummarizeScene(const Scene& scene, const AssetManagerBase& assets, AssetHandle asset)
		{
			CellSummary result;
			AABB bounds;
			const entt::registry& registry = scene.GetRegistry();
			for (const entt::entity handle : registry.view<const IDComponent>())
			{
				const Entity entity(handle, const_cast<Scene*>(&scene));
				result.EntityIDs.push_back(registry.get<IDComponent>(handle).ID);
				const glm::mat4 transform = scene.GetWorldTransform(entity);
				const glm::vec3 position(transform[3]);
				bounds.Min = glm::min(bounds.Min, position);
				bounds.Max = glm::max(bounds.Max, position);
				if (const MeshRendererComponent* renderer = registry.try_get<MeshRendererComponent>(handle))
				{
					if (const std::optional<AABB> mesh = assets.GetBounds(renderer->Mesh))
						AddBox(bounds, transform, mesh->Min, mesh->Max);
				}
				if (const BoxColliderComponent* box = registry.try_get<BoxColliderComponent>(handle))
					AddBox(bounds, transform, box->Offset - box->HalfExtents, box->Offset + box->HalfExtents);
				if (const SphereColliderComponent* sphere = registry.try_get<SphereColliderComponent>(handle))
					AddBox(bounds, transform, sphere->Offset - glm::vec3(sphere->Radius), sphere->Offset + glm::vec3(sphere->Radius));
				if (const CapsuleColliderComponent* capsule = registry.try_get<CapsuleColliderComponent>(handle))
				{
					const glm::vec3 extent(capsule->Radius, capsule->HalfHeight + capsule->Radius, capsule->Radius);
					AddBox(bounds, transform, capsule->Offset - extent, capsule->Offset + extent);
				}
				if (const MeshColliderComponent* collider = registry.try_get<MeshColliderComponent>(handle))
				{
					const MeshRendererComponent* renderer = registry.try_get<MeshRendererComponent>(handle);
					const AssetHandle mesh = collider->Mesh.IsValid() ? collider->Mesh : (renderer ? renderer->Mesh : UUID::Null());
					if (const std::optional<AABB> meshBounds = assets.GetBounds(mesh))
						AddBox(bounds, transform, meshBounds->Min, meshBounds->Max);
				}
			}
			std::sort(result.EntityIDs.begin(), result.EntityIDs.end());
			result.Summary.Entities = result.EntityIDs.size();
			result.Summary.Bodies = PhysicsWorld::CountBodyCandidates(scene);
			if (bounds.IsValid())
			{
				result.Summary.BoundsMin = bounds.Min;
				result.Summary.BoundsMax = bounds.Max;
			}
			if (asset.IsValid())
			{
				for (const AssetHandle member : assets.GetDependencyClosure(asset, AssetClosureScope::Load))
				{
					if (const std::optional<AssetMetadata> metadata = assets.GetMetadata(member))
						result.Summary.ClosureBytes += metadata->StoredSize;
				}
			}
			return result;
		}

		std::optional<CellSummary> ReadCellSummary(AssetManagerBase& assets, AssetHandle scene, std::string* outError)
		{
			std::vector<uint8_t> bytes;
			std::string error;
			if (!assets.ReadStoredData(scene, bytes, &error))
			{
				if (outError)
					*outError = fmt::format("cannot read it: {}", error);
				return std::nullopt;
			}
			const std::optional<nlohmann::json> document = JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &error);
			Scene cell("Cell");
			if (!document || !SceneSerializer::Deserialize(cell, *document, &error))
			{
				if (outError)
					*outError = fmt::format("it is no scene: {}", error);
				return std::nullopt;
			}
			cell.UpdateWorldTransforms();
			return SummarizeScene(cell, assets, scene);
		}

	}

}
