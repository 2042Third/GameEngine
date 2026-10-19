#include "Editor/WorldCook.h"

#include "Editor/WorldCellIO.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/Assert.h>
#include <Strata/Physics/PhysicsSystem.h>
#include <Strata/Physics/PhysicsWorld.h>
#include <Strata/Scene/Scene.h>
#include <Strata/World/WorldComponents.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

namespace Strata
{

	namespace
	{

		// How many entity IDs used twice a failed cook names.
		constexpr size_t c_NamedDuplicates = 10;

		std::string FormatStamp(uint64_t stamp)
		{
			return fmt::format("{:016X}", stamp);
		}

		std::string DescribeCell(const WorldData& data, const WorldCellKey& key)
		{
			const WorldLayer* layer = data.FindLayer(key.Layer);
			return fmt::format("cell ({}, {}, {})", layer ? layer->Name : std::to_string(key.Layer), key.X, key.Z);
		}

		double SquareDistance(const WorldCellSquare& a, const WorldCellSquare& b)
		{
			const double dx = std::max({ 0.0, a.Min.x - b.Max.x, b.Min.x - a.Max.x });
			const double dz = std::max({ 0.0, a.Min.y - b.Max.y, b.Min.y - a.Max.y });
			return std::sqrt(dx * dx + dz * dz);
		}

	}

	// What the cook reads of the persistent scene: its bodies and entity IDs, and the streaming sources' radii.
	struct WorldCookPersistentRead
	{
		uint64_t Bodies = 0;
		std::vector<UUID> EntityIDs;
		std::vector<float> SourceRadii; // Each enabled StreamingSourceComponent's Radius (0: the layers' distances)
		float DistanceScale = 1.0f;
		std::string Error;
	};

	namespace
	{

		void ReadPersistent(AssetManagerBase& assets, AssetHandle persistent, AssetHandle world, const std::function<bool()>& isCancelled,
			WorldCookPersistentRead& out)
		{
			std::string error;
			Scene scene("Persistent");
			if (!WorldCellIO::ReadScene(assets, persistent, scene, isCancelled, &error))
			{
				out.Error = fmt::format("The persistent scene {}", error);
				return;
			}
			WorldCellIO::CellSummary summary = WorldCellIO::SummarizeScene(scene, assets, UUID::Null());
			out.Bodies = summary.Summary.Bodies;
			out.EntityIDs = std::move(summary.EntityIDs);
			const entt::registry& registry = scene.GetRegistry();
			for (const entt::entity handle : registry.view<const StreamingSourceComponent>())
			{
				const StreamingSourceComponent& source = registry.get<StreamingSourceComponent>(handle);
				if (source.Enabled)
					out.SourceRadii.push_back(std::isfinite(source.Radius) ? std::max(source.Radius, 0.0f) : 0.0f);
			}
			for (const entt::entity handle : registry.view<const WorldComponent>())
			{
				const WorldComponent& component = registry.get<WorldComponent>(handle);
				if (component.World == world && std::isfinite(component.DistanceScale))
					out.DistanceScale = std::clamp(component.DistanceScale, 0.1f, 10.0f);
			}
		}

	}

	Scope<WorldCook> WorldCook::Start(const Ref<EditorAssetManager>& assets, AssetHandle world, const Ref<WorldCookCache>& cache, std::string* outError)
	{
		ST_CORE_ASSERT(assets && cache, "A cook needs the project's asset manager and the session's cache");
		auto fail = [outError](std::string message) -> Scope<WorldCook>
		{
			if (outError)
				*outError = std::move(message);
			return nullptr;
		};
		const std::optional<AssetMetadata> metadata = assets->GetMetadata(world);
		if (!metadata || metadata->Type != AssetType::World)
			return fail(fmt::format("{} is not a world of the project", world.ToString()));
		std::vector<uint8_t> bytes;
		std::string error;
		if (!assets->ReadStoredData(world, bytes, &error))
			return fail(fmt::format("The world '{}' cannot be read: {}", metadata->Path, error));
		const Ref<WorldAsset> asset = WorldAsset::Deserialize(bytes, &error);
		if (!asset)
			return fail(fmt::format("The world '{}' is invalid: {}", metadata->Path, error));

		Scope<WorldCook> cook(new WorldCook());
		cook->m_Assets = assets;
		cook->m_Cache = cache;
		cook->m_World = world;
		cook->m_Data = asset->GetData();
		const WorldData& data = cook->m_Data;
		// The jobs read through the cook's reference, never one of their own: the cook waits for them, so that the manager is
		// released on the main thread.
		EditorAssetManager* reader = cook->m_Assets.get();

		// Cells whose stamps changed since the index (or the session's cache) was made are read again.
		for (const WorldCell& cell : data.Cells)
		{
			const uint64_t stamp = assets->GetContentStamp(cell.Scene);
			const WorldCellSummary* indexed = asset->FindSummary(cell.GetKey());
			const auto cached = cache->Cells.find(cell.Scene);
			const bool current = (cached != cache->Cells.end() && cached->second.Stamp == stamp) || (indexed && indexed->Stamp == FormatStamp(stamp));
			if (current && stamp != 0)
				continue;
			Scope<CellJob> job = CreateScope<CellJob>();
			job->Cell = cell;
			job->Stamp = stamp;
			CellJob* target = job.get();
			cook->m_Reads.push_back(std::move(job));
			cook->m_Jobs.push_back(JobSystem::SubmitIO([target, reader, cancelled = &cook->m_Cancelled]()
			{
				if (cancelled->load(std::memory_order_acquire))
					return;
				std::string readError;
				const auto isCancelled = [cancelled]() { return cancelled->load(std::memory_order_acquire); };
				if (std::optional<WorldCellIO::CellSummary> summary = WorldCellIO::ReadCellSummary(*reader, target->Cell.Scene, isCancelled, &readError))
				{
					target->Summary = summary->Summary;
					target->EntityIDs = std::move(summary->EntityIDs);
				}
				else
				{
					target->Error = std::move(readError);
				}
			}));
		}

		// The persistent scene: its bodies, IDs and sources (read every time: it is one document).
		if (data.PersistentScene.IsValid())
		{
			Ref<WorldCookPersistentRead> persistent = CreateRef<WorldCookPersistentRead>();
			cook->m_Jobs.push_back(JobSystem::SubmitIO([persistent, reader, scene = data.PersistentScene, world, cancelled = &cook->m_Cancelled]()
			{
				const auto isCancelled = [cancelled]() { return cancelled->load(std::memory_order_acquire); };
				if (!isCancelled())
					ReadPersistent(*reader, scene, world, isCancelled, *persistent);
			}));
			// Kept by the job and by the poll, which reads it once every job completed.
			cook->m_Persistent = persistent;
		}
		return cook;
	}

	std::optional<WorldCookReport> WorldCook::Run(const Ref<EditorAssetManager>& assets, AssetHandle world, const Ref<WorldCookCache>& cache,
		std::string* outError)
	{
		Scope<WorldCook> cook = Start(assets, world, cache, outError);
		if (!cook)
			return std::nullopt;
		while (!cook->Poll())
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (!cook->Succeeded())
		{
			if (outError)
				*outError = cook->GetError();
			return std::nullopt;
		}
		return cook->GetReport();
	}

	WorldCook::~WorldCook()
	{
		// The jobs write into this cook: they end before it does.
		m_Cancelled.store(true, std::memory_order_release);
		for (const JobHandle& job : m_Jobs)
			job.Wait();
	}

	bool WorldCook::Poll()
	{
		if (m_Finished)
			return true;
		for (const JobHandle& job : m_Jobs)
		{
			if (!job.IsComplete())
				return false;
		}
		m_Jobs.clear();
		if (m_Cancelled.load(std::memory_order_acquire))
			m_Error = "The cook was cancelled";
		else
			Finish();
		m_Finished = true;
		return true;
	}

	void WorldCook::Cancel()
	{
		m_Cancelled.store(true, std::memory_order_release);
	}

	void WorldCook::Finish()
	{
		const WorldData& data = m_Data;
		if (m_Persistent && !m_Persistent->Error.empty())
		{
			m_Error = m_Persistent->Error;
			return;
		}

		// Every cell's summary: read now, cached this session, or still current in the index.
		std::unordered_map<UUID, const CellJob*> reads;
		for (const Scope<CellJob>& job : m_Reads)
			reads.emplace(job->Cell.Scene, job.get());
		const Ref<WorldAsset> previous = WorldAsset::Create(data);
		WorldIndex index;
		index.Cells.reserve(data.Cells.size());
		std::unordered_map<UUID, std::string> owners; // Entity ID -> where it is
		std::vector<std::string> duplicates;
		auto claim = [&](const std::vector<UUID>& ids, const std::string& where)
		{
			for (const UUID id : ids)
			{
				const auto [it, inserted] = owners.emplace(id, where);
				if (!inserted && duplicates.size() < c_NamedDuplicates)
					duplicates.push_back(fmt::format("{} in {} and {}", id.ToString(), it->second, where));
			}
		};
		if (m_Persistent)
			claim(m_Persistent->EntityIDs, "the persistent scene");
		for (const WorldCell& cell : data.Cells)
		{
			const std::string where = DescribeCell(data, cell.GetKey());
			WorldCellSummary summary;
			if (const auto read = reads.find(cell.Scene); read != reads.end())
			{
				const CellJob& job = *read->second;
				if (!job.Summary)
				{
					m_Error = fmt::format("The world's {} cannot be cooked: {}", where, job.Error);
					return;
				}
				summary = *job.Summary;
				summary.Stamp = FormatStamp(job.Stamp);
				claim(job.EntityIDs, where);
				WorldCookCache::Cell& cached = m_Cache->Cells[cell.Scene];
				cached.Stamp = job.Stamp;
				cached.EntityIDs = job.EntityIDs;
				cached.Summary = summary;
				m_Report.CellsRead++;
			}
			else if (const auto cached = m_Cache->Cells.find(cell.Scene); cached != m_Cache->Cells.end() && cached->second.Stamp == m_Assets->GetContentStamp(cell.Scene))
			{
				summary = cached->second.Summary;
				claim(cached->second.EntityIDs, where);
				m_Report.CellsReused++;
			}
			else if (const WorldCellSummary* indexed = previous ? previous->FindSummary(cell.GetKey()) : nullptr)
			{
				// Current in the index, not read this session: its IDs were checked when it was cooked.
				summary = *indexed;
				m_Report.CellsReused++;
			}
			summary.Layer = cell.Layer;
			summary.X = cell.X;
			summary.Z = cell.Z;
			index.Cells.push_back(summary);

			const WorldLayer* layer = data.FindLayer(cell.Layer);
			const WorldCellSquare square = WorldGrid::CellBounds(*layer, data.GridOrigin, cell.X, cell.Z);
			const double farthest = std::max({ std::abs(square.Min.x), std::abs(square.Min.y), std::abs(square.Max.x), std::abs(square.Max.y) });
			if (farthest > WorldData::c_SupportedExtent)
				m_Report.Warnings.push_back(fmt::format("The {} lies {:.1f} km from the origin, beyond the {:.0f} km worlds are supported within (float positions "
					"lose precision there)", where, farthest / 1000.0, WorldData::c_SupportedExtent / 1000.0));
			if (summary.Entities == 0)
				m_Report.Warnings.push_back(fmt::format("The {} is empty", where));
		}
		if (!duplicates.empty())
		{
			std::string list;
			for (const std::string& duplicate : duplicates)
				list += (list.empty() ? "" : "; ") + duplicate;
			m_Error = fmt::format("Entity IDs are used twice (a cell loads only when its IDs are new to the scene): {}{}", list,
				duplicates.size() == c_NamedDuplicates ? "; ..." : "");
			return;
		}

		// The radius each layer streams within: the largest a source uses, else the layer's distance.
		const float scale = m_Persistent ? m_Persistent->DistanceScale : 1.0f;
		m_LayerRadii.clear();
		for (const WorldLayer& layer : data.Layers)
		{
			double radius = static_cast<double>(layer.StreamingDistance) * scale;
			if (m_Persistent && !m_Persistent->SourceRadii.empty())
			{
				radius = 0.0;
				for (const float source : m_Persistent->SourceRadii)
					radius = std::max(radius, source > 0.0f ? static_cast<double>(source) : static_cast<double>(layer.StreamingDistance) * scale);
			}
			m_LayerRadii.push_back(radius);
		}
		m_Report.SourceCount = m_Persistent ? std::max<uint32_t>(1, static_cast<uint32_t>(m_Persistent->SourceRadii.size())) : 1;
		index.PersistentBodies = m_Persistent ? m_Persistent->Bodies : 0;
		index.MaxBodiesInRadius = ComputeMaxBodiesInRadius(data, index.Cells, m_LayerRadii, m_Report.SourceCount, index.PersistentBodies);
		m_Report.RecommendedMaxBodies = index.GetRecommendedMaxBodies();
		PhysicsSettings settings = PhysicsSystem::GetDefaultSettings();
		settings.MaxBodies = std::max(m_Report.RecommendedMaxBodies, 1u);
		m_Report.ReservedBytes = PhysicsWorld::EstimateReservedBytes(PhysicsWorld::ResolveCapacities(settings, settings.MaxBodies));
		m_Report.Index = index;
		m_Data.Index = std::move(index);
	}

	uint64_t WorldCook::ComputeMaxBodiesInRadius(const WorldData& data, std::span<const WorldCellSummary> cells, std::span<const double> layerRadii,
		uint32_t sourceCount, uint64_t persistentBodies)
	{
		std::unordered_map<uint64_t, uint64_t> bodies;
		bodies.reserve(cells.size());
		for (const WorldCellSummary& cell : cells)
			bodies[cell.GetKey().Pack()] += cell.Bodies;

		uint64_t total = 0;
		for (size_t layerIndex = 0; layerIndex < data.Layers.size() && layerIndex < layerRadii.size(); layerIndex++)
		{
			const WorldLayer& layer = data.Layers[layerIndex];
			const double radius = std::max(layerRadii[layerIndex], 0.0);
			const double reach = radius + std::max(static_cast<double>(layer.CellSize) * 0.5, 0.1 * radius);
			std::vector<const WorldCellSummary*> layerCells;
			for (const WorldCellSummary& cell : cells)
			{
				if (cell.Layer == layer.ID)
					layerCells.push_back(&cell);
			}
			const int64_t span = static_cast<int64_t>(std::ceil(reach / static_cast<double>(layer.CellSize))) + 1;
			const bool scanGrid = static_cast<double>(2 * span + 1) * static_cast<double>(2 * span + 1) < static_cast<double>(layerCells.size());
			uint64_t most = 0;
			for (const WorldCellSummary* cell : layerCells)
			{
				const WorldCellSquare square = WorldGrid::CellBounds(layer, data.GridOrigin, cell->X, cell->Z);
				uint64_t sum = 0;
				auto add = [&](int32_t x, int32_t z, uint64_t count)
				{
					if (SquareDistance(square, WorldGrid::CellBounds(layer, data.GridOrigin, x, z)) <= reach)
						sum += count;
				};
				if (scanGrid)
				{
					for (int64_t dx = -span; dx <= span; dx++)
					{
						for (int64_t dz = -span; dz <= span; dz++)
						{
							const int64_t x = cell->X + dx;
							const int64_t z = cell->Z + dz;
							if (std::abs(x) > WorldCellKey::c_MaxCoordinate || std::abs(z) > WorldCellKey::c_MaxCoordinate)
								continue;
							const auto it = bodies.find(WorldCellKey { layer.ID, static_cast<int32_t>(x), static_cast<int32_t>(z) }.Pack());
							if (it != bodies.end())
								add(static_cast<int32_t>(x), static_cast<int32_t>(z), it->second);
						}
					}
				}
				else
				{
					for (const WorldCellSummary* other : layerCells)
						add(other->X, other->Z, other->Bodies);
				}
				most = std::max(most, sum);
			}
			total += most;
		}
		return total * std::max(sourceCount, 1u) + persistentBodies;
	}

}
