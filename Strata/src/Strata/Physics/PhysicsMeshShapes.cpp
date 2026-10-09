#include "stpch.h"
#include "Strata/Physics/PhysicsMeshShapes.h"

#include "Strata/Core/Hash.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Physics/PhysicsRuntime.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

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

		class ByteStreamOut final : public JPH::StreamOut
		{
		public:
			explicit ByteStreamOut(std::vector<uint8_t>& bytes)
				: m_Bytes(bytes)
			{
			}

			void WriteBytes(const void* data, size_t byteCount) override
			{
				const uint8_t* begin = static_cast<const uint8_t*>(data);
				m_Bytes.insert(m_Bytes.end(), begin, begin + byteCount);
			}

			bool IsFailed() const override { return false; }
		private:
			std::vector<uint8_t>& m_Bytes;
		};

		bool HasFinitePositions(const PhysicsMeshData& data)
		{
			return std::all_of(data.Positions.begin(), data.Positions.end(), [](const glm::vec3& position)
			{
				return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
			});
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

			JPH::ShapeSettings::ShapeResult shape;
			if (convex)
			{
				JPH::Array<JPH::Vec3> points;
				points.reserve(data.Positions.size());
				for (const glm::vec3& position : data.Positions)
					points.push_back(JPH::Vec3(position.x, position.y, position.z));
				shape = JPH::ConvexHullShapeSettings(points, JPH::cDefaultConvexRadius).Create();
			}
			else
			{
				if (data.Indices.empty() || data.Indices.size() % 3 != 0)
				{
					result.Error = fmt::format("a triangle list needs a positive multiple of 3 indices, it has {}", data.Indices.size());
					return result;
				}

				JPH::VertexList vertices;
				vertices.reserve(data.Positions.size());
				for (const glm::vec3& position : data.Positions)
					vertices.push_back(JPH::Float3(position.x, position.y, position.z));

				JPH::IndexedTriangleList triangles;
				triangles.reserve(data.Indices.size() / 3);
				for (size_t index = 0; index < data.Indices.size(); index += 3)
				{
					const uint32_t i0 = data.Indices[index];
					const uint32_t i1 = data.Indices[index + 1];
					const uint32_t i2 = data.Indices[index + 2];
					if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
					{
						result.Error = fmt::format("it references vertices beyond its {} vertices", vertices.size());
						return result;
					}
					triangles.push_back(JPH::IndexedTriangle(i0, i1, i2));
				}
				// The settings remove degenerate and duplicate triangles; a mesh without any valid triangle fails to build.
				shape = JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
			}

			if (shape.HasError())
			{
				result.Error = shape.GetError().c_str();
				return result;
			}

			Ref<std::vector<uint8_t>> bytes = CreateRef<std::vector<uint8_t>>();
			ByteStreamOut stream(*bytes);
			shape.Get()->SaveBinaryState(stream);
			result.CookState = PhysicsMeshShapes::State::Ready;
			result.Shape = std::move(bytes);
			return result;
		}

		void Cook(const Ref<const PhysicsMeshData>& data, bool convex)
		{
			PhysicsMeshShapes::Result result;
			{
				// Jolt stays initialized while it cooks, also if the last world goes away meanwhile.
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
