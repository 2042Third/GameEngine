#include "stpch.h"
#include "Strata/Physics/PhysicsMeshShapes.h"

#include "Strata/Core/Hash.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Physics/PhysicsRuntime.h"

#include <box3d/box3d.h>
#include <box3d/collision.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Strata
{

	namespace
	{

		struct CookKey
		{
			const PhysicsMeshData* Data = nullptr;
			bool Convex = false;

			bool operator==(const CookKey& other) const { return Data == other.Data && Convex == other.Convex; }
		};

		struct CookKeyHash
		{
			size_t operator()(const CookKey& key) const
			{
				return static_cast<size_t>(Hash::Combine(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(key.Data)), key.Convex ? 1u : 0u));
			}
		};

		struct CookEntry
		{
			// The data the shape is cooked from. Another object may later occupy the same address, so an entry only
			// belongs to its key while the source is alive.
			std::weak_ptr<const PhysicsMeshData> Source;
			PhysicsMeshShapes::Result Result;
		};

		struct CookCache
		{
			std::mutex Mutex;
			std::unordered_map<CookKey, CookEntry, CookKeyHash> Entries;
			std::atomic<uint64_t> CompletedCount = 0;
			std::atomic<uint64_t> CookCount = 0;
		};

		CookCache& GetCache()
		{
			static CookCache s_Cache;
			return s_Cache;
		}

		bool HasFinitePositions(const PhysicsMeshData& data)
		{
			return std::all_of(data.Positions.begin(), data.Positions.end(), [](const glm::vec3& position)
			{
				return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
			});
		}

		std::atomic<uint64_t> s_LiveShapes = 0;

		// The convex hull of the points: at most c_PreferredHullVertices vertices, else c_SafeHullVertices (see
		// PhysicsMeshShapes); null if box3d refuses both.
		b3HullData* CookHull(const std::vector<b3Vec3>& points)
		{
			const int count = static_cast<int>(points.size());
			{
				// The first attempt may exceed box3d's face or edge limits, which box3d logs; that is expected here.
				Box3DLogSuppression suppression;
				if (b3HullData* hull = b3CreateHull(points.data(), count, std::min(count, PhysicsMeshShapes::c_PreferredHullVertices)))
					return hull;
			}
			if (count <= PhysicsMeshShapes::c_SafeHullVertices)
				return nullptr;
			return b3CreateHull(points.data(), count, PhysicsMeshShapes::c_SafeHullVertices);
		}

		// Validates the data and builds the shape; the result is Ready or Failed.
		PhysicsMeshShapes::Result CookShape(const PhysicsMeshData& data, bool convex)
		{
			PhysicsMeshShapes::Result result;
			result.CookState = PhysicsMeshShapes::State::Failed;
			if (!HasFinitePositions(data))
			{
				result.Error = "its vertex positions are not all finite";
				return result;
			}
			if (data.Positions.size() > static_cast<size_t>(INT32_MAX) || data.Indices.size() / 3 > static_cast<size_t>(INT32_MAX))
			{
				result.Error = "it has too many vertices or triangles";
				return result;
			}

			std::vector<b3Vec3> vertices;
			vertices.reserve(data.Positions.size());
			for (const glm::vec3& position : data.Positions)
				vertices.push_back(b3Vec3 { position.x, position.y, position.z });

			if (convex)
			{
				if (vertices.size() < 4)
				{
					result.Error = fmt::format("a convex hull needs at least 4 points, it has {}", vertices.size());
					return result;
				}
				b3HullData* hull = CookHull(vertices);
				if (!hull)
				{
					result.Error = "its points do not span a volume (a convex hull cannot be built)";
					return result;
				}
				result.CookState = PhysicsMeshShapes::State::Ready;
				result.Shape = CreateRef<PhysicsCookedShape>(hull, nullptr);
				return result;
			}

			if (data.Indices.empty() || data.Indices.size() % 3 != 0)
			{
				result.Error = fmt::format("a triangle list needs a positive multiple of 3 indices, it has {}", data.Indices.size());
				return result;
			}
			if (vertices.size() < 3)
			{
				result.Error = fmt::format("a triangle mesh needs at least 3 vertices, it has {}", vertices.size());
				return result;
			}
			std::vector<int32_t> indices;
			indices.reserve(data.Indices.size());
			for (const uint32_t index : data.Indices)
			{
				if (index >= vertices.size())
				{
					result.Error = fmt::format("it references vertices beyond its {} vertices", vertices.size());
					return result;
				}
				indices.push_back(static_cast<int32_t>(index));
			}

			b3MeshDef definition = {};
			definition.vertices = vertices.data();
			definition.stride = 0;
			definition.indices = indices.data();
			definition.vertexCount = static_cast<int>(vertices.size());
			definition.triangleCount = static_cast<int>(indices.size() / 3);
			// Shared edges let convex bodies slide over the inner edges of the mesh without catching on them.
			definition.identifyEdges = true;
			definition.clockWiseWinding = false; // Counter-clockwise front faces (PhysicsMeshData)
			// box3d skips degenerate triangles; a mesh without any other fails to build.
			b3MeshData* mesh = b3CreateMesh(&definition, nullptr, 0);
			if (!mesh)
			{
				result.Error = "it has no triangle with an area (a triangle mesh cannot be built)";
				return result;
			}
			result.CookState = PhysicsMeshShapes::State::Ready;
			result.Shape = CreateRef<PhysicsCookedShape>(nullptr, mesh);
			return result;
		}

		void Cook(const Ref<const PhysicsMeshData>& data, bool convex)
		{
			PhysicsMeshShapes::Result result;
			{
				// box3d's hooks are installed for the cook (they route its logs and asserts).
				PhysicsRuntimeReference runtime;
				result = CookShape(*data, convex);
			}

			CookCache& cache = GetCache();
			{
				std::scoped_lock<std::mutex> lock(cache.Mutex);
				auto it = cache.Entries.find(CookKey { data.get(), convex });
				if (it != cache.Entries.end() && it->second.Source.lock() == data)
					it->second.Result = std::move(result);
			}
			cache.CompletedCount.fetch_add(1, std::memory_order_release);
		}

	}

	PhysicsCookedShape::PhysicsCookedShape(b3HullData* hull, b3MeshData* mesh)
		: m_Hull(hull), m_Mesh(mesh)
	{
		s_LiveShapes.fetch_add(1, std::memory_order_relaxed);
	}

	PhysicsCookedShape::~PhysicsCookedShape()
	{
		if (m_Hull)
			b3DestroyHull(m_Hull);
		if (m_Mesh)
			b3DestroyMesh(m_Mesh);
		s_LiveShapes.fetch_sub(1, std::memory_order_relaxed);
	}

	uint64_t PhysicsCookedShape::GetLiveCount()
	{
		return s_LiveShapes.load(std::memory_order_relaxed);
	}

	PhysicsMeshShapes::Result PhysicsMeshShapes::Request(const Ref<const PhysicsMeshData>& data, bool convex)
	{
		ST_CORE_ASSERT(data, "PhysicsMeshShapes::Request needs mesh data");
		CookCache& cache = GetCache();
		const CookKey key { data.get(), convex };
		{
			std::scoped_lock<std::mutex> lock(cache.Mutex);
			auto it = cache.Entries.find(key);
			if (it != cache.Entries.end() && !it->second.Source.expired())
				return it->second.Result;

			// Not cached: drop the shapes of data that no longer exists (which includes an entry left at this address),
			// then cook.
			std::erase_if(cache.Entries, [](const auto& entry) { return entry.second.Source.expired(); });
			cache.Entries[key] = CookEntry { data, Result() };
		}
		cache.CookCount.fetch_add(1, std::memory_order_relaxed);

		// Submitted outside the lock: without an initialized JobSystem the job runs right here.
		JobSystem::Submit([data, convex]() { Cook(data, convex); }, JobPriority::Normal);

		std::scoped_lock<std::mutex> lock(cache.Mutex);
		auto it = cache.Entries.find(key);
		return it != cache.Entries.end() ? it->second.Result : Result();
	}

	uint64_t PhysicsMeshShapes::GetCompletedCount()
	{
		return GetCache().CompletedCount.load(std::memory_order_acquire);
	}

	uint64_t PhysicsMeshShapes::GetCookCount()
	{
		return GetCache().CookCount.load(std::memory_order_relaxed);
	}

	size_t PhysicsMeshShapes::GetCachedCount()
	{
		CookCache& cache = GetCache();
		std::scoped_lock<std::mutex> lock(cache.Mutex);
		return cache.Entries.size();
	}

}
