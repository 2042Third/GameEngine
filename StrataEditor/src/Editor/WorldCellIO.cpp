#include "Editor/WorldCellIO.h"

#include <Strata/Asset/AssetDependencies.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Math/AABB.h>
#include <Strata/Physics/PhysicsComponents.h>
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

		// Adds an entity to a summary: its ID, its bounds (position, mesh, colliders) and whether it may own a body (as
		// PhysicsWorld::CountBodyCandidates counts: a rigid body or a collider, and a character controller's presence body).
		void AddEntity(const Scene& scene, entt::entity handle, const AssetManagerBase& assets, WorldCellIO::CellSummary& result, AABB& bounds, uint64_t& bodies)
		{
			const entt::registry& registry = scene.GetRegistry();
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
			if (registry.any_of<RigidBodyComponent, BoxColliderComponent, SphereColliderComponent, CapsuleColliderComponent, MeshColliderComponent>(handle))
				bodies++;
			if (registry.all_of<CharacterControllerComponent>(handle))
				bodies++;
		}

		void FinishSummary(WorldCellIO::CellSummary& result, const AABB& bounds, uint64_t bodies, const AssetManagerBase& assets, AssetHandle asset)
		{
			std::sort(result.EntityIDs.begin(), result.EntityIDs.end());
			result.Summary.Entities = result.EntityIDs.size();
			result.Summary.Bodies = static_cast<uint32_t>(std::min<uint64_t>(bodies, UINT32_MAX));
			if (bounds.IsValid())
			{
				result.Summary.BoundsMin = bounds.Min;
				result.Summary.BoundsMax = bounds.Max;
			}
			result.Summary.ClosureBytes = WorldCellIO::GetClosureBytes(assets, asset);
		}

	}

	namespace WorldCellIO
	{

		uint64_t GetClosureBytes(const AssetManagerBase& assets, AssetHandle asset)
		{
			uint64_t bytes = 0;
			if (!asset.IsValid())
				return bytes;
			for (const AssetHandle member : assets.GetDependencyClosure(asset, AssetClosureScope::Load))
			{
				if (const std::optional<AssetMetadata> metadata = assets.GetMetadata(member))
					bytes += metadata->StoredSize;
			}
			return bytes;
		}

		CellSummary SummarizeScene(const Scene& scene, const AssetManagerBase& assets, AssetHandle asset)
		{
			CellSummary result;
			AABB bounds;
			uint64_t bodies = 0;
			for (const entt::entity handle : scene.GetRegistry().view<const IDComponent>())
				AddEntity(scene, handle, assets, result, bounds, bodies);
			FinishSummary(result, bounds, bodies, assets, asset);
			return result;
		}

		CellSummary SummarizeEntities(const Scene& scene, std::span<const Entity> roots, const AssetManagerBase& assets, AssetHandle asset)
		{
			CellSummary result;
			AABB bounds;
			uint64_t bodies = 0;
			std::vector<Entity> stack(roots.begin(), roots.end());
			while (!stack.empty())
			{
				const Entity entity = stack.back();
				stack.pop_back();
				if (!entity)
					continue;
				AddEntity(scene, entity.GetHandle(), assets, result, bounds, bodies);
				const std::vector<Entity> children = entity.GetChildren();
				stack.insert(stack.end(), children.begin(), children.end());
			}
			FinishSummary(result, bounds, bodies, assets, asset);
			return result;
		}

		AssetHandle CreateDocument(EditorAssetManager& assets, const std::string& path, AssetType type, AssetHandle handle, const nlohmann::json& document,
			std::string* outError)
		{
			const std::filesystem::path file = assets.GetAssetDirectory() / FileSystem::FromUTF8(path);
			if (FileSystem::Exists(file))
			{
				if (outError)
					*outError = fmt::format("'{}' exists already", path);
				return UUID::Null();
			}
			const std::filesystem::path metaFile = FileSystem::FromUTF8(FileSystem::ToUTF8(file) + std::string(EditorAssetManager::c_MetaExtension));
			const nlohmann::json meta = { { "Handle", handle.ToString() }, { "ImportSettings", nlohmann::json::object() },
				{ "Strata", { { "Format", "AssetMeta" }, { "Version", 1 } } }, { "Type", AssetTypeToString(type) } };
			if (!FileSystem::CreateDirectories(file.parent_path()) || !FileSystem::WriteText(metaFile, JsonUtils::Dump(meta, 1, '\t') + "\n"))
			{
				if (outError)
					*outError = fmt::format("'{}' cannot be written", FileSystem::ToUTF8(metaFile));
				return UUID::Null();
			}
			std::vector<AssetHandle> references;
			const bool scanned = AssetDependencies::ScanDocument(type, document, references);
			const std::string text = JsonUtils::Dump(document, 1, '\t') + "\n";
			const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
			const AssetHandle created = assets.CreateNativeAsset(path, bytes, outError, scanned ? &references : nullptr);
			if (!created.IsValid())
				FileSystem::Remove(metaFile);
			return created;
		}

		bool ReadScene(AssetManagerBase& assets, AssetHandle asset, Scene& outScene, const std::function<bool()>& isCancelled, std::string* outError)
		{
			std::vector<uint8_t> bytes;
			std::string error;
			if (!assets.ReadStoredData(asset, bytes, &error))
			{
				if (outError)
					*outError = fmt::format("cannot read it: {}", error);
				return false;
			}
			SceneReadOptions options;
			options.IsCancelled = isCancelled;
			// Read on an I/O job already: its entities are parsed there, not on the job system's threads.
			options.Parallel = false;
			if (!SceneSerializer::DeserializeText(outScene, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), options, &error))
			{
				if (outError)
					*outError = fmt::format("it is no scene: {}", error);
				return false;
			}
			outScene.UpdateWorldTransforms();
			return true;
		}

		std::optional<CellSummary> ReadCellSummary(AssetManagerBase& assets, AssetHandle scene, const std::function<bool()>& isCancelled, std::string* outError)
		{
			Scene cell("Cell");
			if (!ReadScene(assets, scene, cell, isCancelled, outError))
				return std::nullopt;
			return SummarizeScene(cell, assets, scene);
		}


	}

}
