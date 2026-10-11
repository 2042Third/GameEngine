#include "Editor/SceneRaycast.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Renderer/RenderScene.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Strata
{

	namespace
	{

		// How far a ray without a limit reaches (the spatial index is queried with a finite segment).
		constexpr float c_MaxRayLength = 1.0e6f;
		// The half width of the prism around the ray the spatial index is asked for (boxes it touches are tested exactly).
		constexpr float c_QueryRadius = 1.0e-3f;

		// The normal of the box face the point lies on (the axis it is closest to a face of).
		glm::vec3 GetBoxNormal(const AABB& box, const glm::vec3& point)
		{
			glm::vec3 normal(0.0f, 1.0f, 0.0f);
			float nearest = std::numeric_limits<float>::max();
			for (int axis = 0; axis < 3; axis++)
			{
				const float toMin = std::abs(point[axis] - box.Min[axis]);
				const float toMax = std::abs(point[axis] - box.Max[axis]);
				if (toMin < nearest)
				{
					nearest = toMin;
					normal = glm::vec3(0.0f);
					normal[axis] = -1.0f;
				}
				if (toMax < nearest)
				{
					nearest = toMax;
					normal = glm::vec3(0.0f);
					normal[axis] = 1.0f;
				}
			}
			return normal;
		}

		// The nearest triangle of the mesh the ray hits (in world space), nearer than `limit`.
		std::optional<SceneRayHit> CastMesh(const Mesh& mesh, const glm::mat4& world, const Ray& ray, float limit)
		{
			const glm::mat4 inverse = glm::inverse(world);
			const glm::vec3 localOrigin(inverse * glm::vec4(ray.Origin, 1.0f));
			const glm::vec3 localDirection(inverse * glm::vec4(ray.Direction, 0.0f));
			if (!(glm::length(localDirection) > 0.0f) || !std::isfinite(localOrigin.x))
				return std::nullopt;
			const Ray local(localOrigin, localDirection);
			const std::vector<glm::vec3>& positions = mesh.GetPositions();
			const std::vector<uint32_t>& indices = mesh.GetIndices();

			std::optional<SceneRayHit> best;
			for (const Submesh& submesh : mesh.GetSubmeshes())
			{
				if (submesh.LODs.empty() || (submesh.Bounds.IsValid() && !IntersectRayAABB(local, submesh.Bounds)))
					continue;
				const MeshLOD& lod = submesh.LODs.front();
				if (static_cast<uint64_t>(lod.IndexOffset) + lod.IndexCount > indices.size())
					continue;
				for (uint32_t index = lod.IndexOffset; index + 2 < lod.IndexOffset + lod.IndexCount; index += 3)
				{
					const uint64_t a = static_cast<uint64_t>(submesh.BaseVertex) + indices[index];
					const uint64_t b = static_cast<uint64_t>(submesh.BaseVertex) + indices[index + 1];
					const uint64_t c = static_cast<uint64_t>(submesh.BaseVertex) + indices[index + 2];
					if (a >= positions.size() || b >= positions.size() || c >= positions.size())
						continue;
					const std::optional<float> t = IntersectRayTriangle(local, positions[a], positions[b], positions[c]);
					if (!t)
						continue;
					const glm::vec3 point(world * glm::vec4(local.GetPoint(*t), 1.0f));
					const float distance = glm::dot(point - ray.Origin, ray.Direction);
					if (distance < 0.0f || distance >= limit || (best && distance >= best->Distance))
						continue;
					glm::vec3 normal = glm::transpose(glm::mat3(inverse)) * glm::cross(positions[b] - positions[a], positions[c] - positions[a]);
					normal = glm::length(normal) > 0.0f ? glm::normalize(normal) : -ray.Direction;
					if (glm::dot(normal, ray.Direction) > 0.0f)
						normal = -normal;
					best = SceneRayHit { Entity(), point, normal, distance, true };
				}
			}
			return best;
		}

	}

	namespace SceneRaycast
	{

		std::optional<SceneRayHit> Cast(Scene& scene, const Ray& ray, float maxDistance, std::span<const Entity> ignore)
		{
			RenderScene& renderScene = RenderScene::Get(scene);
			renderScene.Sync(AssetManager::GetActive());

			std::unordered_set<entt::entity> ignored;
			std::vector<Entity> pending(ignore.begin(), ignore.end());
			while (!pending.empty())
			{
				const Entity entity = pending.back();
				pending.pop_back();
				if (!entity || entity.GetScene() != &scene || !ignored.insert(entity.GetHandle()).second)
					continue;
				for (const Entity child : entity.GetChildren())
					pending.push_back(child);
			}

			// The boxes around the segment, from the spatial index: a thin prism along the ray (inward planes).
			const float length = std::min(maxDistance, c_MaxRayLength);
			const glm::vec3& direction = ray.Direction;
			const glm::vec3 helper = std::abs(direction.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
			const glm::vec3 u = glm::normalize(glm::cross(direction, helper));
			const glm::vec3 v = glm::cross(direction, u);
			const glm::vec3& origin = ray.Origin;
			const glm::vec4 planes[] = {
				glm::vec4(u, -glm::dot(u, origin) + c_QueryRadius),
				glm::vec4(-u, glm::dot(u, origin) + c_QueryRadius),
				glm::vec4(v, -glm::dot(v, origin) + c_QueryRadius),
				glm::vec4(-v, glm::dot(v, origin) + c_QueryRadius),
				glm::vec4(direction, -glm::dot(direction, origin)),
				glm::vec4(-direction, glm::dot(direction, origin) + length) };
			std::vector<uint32_t> proxies;
			renderScene.GetSpatialIndex().QueryPlanes(planes, proxies);

			// The candidates the ray enters, nearest first: a box nearer than the best hit so far may hold a nearer surface.
			std::vector<std::pair<float, uint32_t>> candidates;
			for (const uint32_t proxy : proxies)
			{
				const entt::entity handle = renderScene.GetEntity(proxy);
				if (handle == entt::null || (renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Active) == 0 || ignored.contains(handle))
					continue;
				if (const std::optional<float> entry = IntersectRayAABB(ray, renderScene.GetWorldBounds(proxy)); entry && *entry <= length)
					candidates.emplace_back(*entry, proxy);
			}
			std::sort(candidates.begin(), candidates.end());

			std::optional<SceneRayHit> best;
			for (const auto& [entry, proxy] : candidates)
			{
				if (best && entry >= best->Distance)
					break;
				const Entity entity(renderScene.GetEntity(proxy), &scene);
				const uint32_t slot = renderScene.GetMeshSlot(proxy);
				if (slot == RenderScene::c_InvalidIndex)
					continue;
				const AssetHandle meshHandle = renderScene.GetMeshHandle(slot);
				const Ref<Mesh> mesh = AssetManager::HasActive() ? AssetManager::GetAsset<Mesh>(meshHandle) : nullptr;
				if (mesh && !mesh->GetPositions().empty() && !mesh->GetIndices().empty())
				{
					if (std::optional<SceneRayHit> hit = CastMesh(*mesh, renderScene.GetWorldTransform(proxy), ray, best ? best->Distance : length))
					{
						hit->HitEntity = entity;
						best = hit;
					}
					continue;
				}
				// Not loaded: its bounds stand in.
				const AABB& bounds = renderScene.GetWorldBounds(proxy);
				const glm::vec3 point = ray.GetPoint(entry);
				best = SceneRayHit { entity, point, GetBoxNormal(bounds, point), entry, false };
			}
			return best;
		}

	}

}
