#include "stpch.h"
#include "Strata/World/WorldStreamer.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Scene/Scene.h"
#include "Strata/World/WorldAsset.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Strata
{

	namespace
	{

		// The squares of a layer's grid that lie within `radius` of a point on the ground plane, by their corner cells.
		struct SquareRange
		{
			WorldCellKey Min;
			WorldCellKey Max;

			double GetCount() const
			{
				return (static_cast<double>(Max.X) - Min.X + 1.0) * (static_cast<double>(Max.Z) - Min.Z + 1.0);
			}
		};

		SquareRange GetSquaresAround(const WorldLayer& layer, const glm::dvec2& origin, const glm::dvec3& position, double radius)
		{
			return SquareRange { WorldGrid::CellAt(layer, origin, position - glm::dvec3(radius, 0.0, radius)),
				WorldGrid::CellAt(layer, origin, position + glm::dvec3(radius, 0.0, radius)) };
		}

		// Calls `visit` with the index (into WorldData::Cells) of every existing cell of the layer whose square may lie within
		// `radius`: the grid's squares around the point, or the layer's cell list when that is shorter.
		template<typename Visit>
		void ForEachCellAround(const WorldAsset& world, const WorldLayer& layer, std::span<const int32_t> layerCells, const glm::dvec3& position, double radius,
			Visit&& visit)
		{
			const WorldData& data = world.GetData();
			const SquareRange range = GetSquaresAround(layer, data.GridOrigin, position, radius);
			if (range.GetCount() > static_cast<double>(layerCells.size()))
			{
				for (const int32_t index : layerCells)
					visit(index);
				return;
			}
			for (int32_t x = range.Min.X; x <= range.Max.X; x++)
			{
				for (int32_t z = range.Min.Z; z <= range.Max.Z; z++)
				{
					const int32_t index = world.FindCell(WorldCellKey { layer.ID, x, z });
					if (index >= 0)
						visit(index);
				}
			}
		}

		bool IsFiniteVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

	}

	const char* WorldCellStatusToString(WorldCellStatus status)
	{
		switch (status)
		{
			case WorldCellStatus::Unloaded:  return "Unloaded";
			case WorldCellStatus::Loading:   return "Loading";
			case WorldCellStatus::Loaded:    return "Loaded";
			case WorldCellStatus::Unloading: return "Unloading";
			case WorldCellStatus::Failed:    return "Failed";
		}
		return "Unloaded";
	}

	WorldStreamer::WorldStreamer(Scene& scene)
		: m_Scene(scene)
	{
	}

	WorldStreamer::~WorldStreamer() = default;

	void WorldStreamer::SetWorld(AssetHandle world, float distanceScale)
	{
		const float scale = std::isfinite(distanceScale) ? std::clamp(distanceScale, 0.1f, 10.0f) : 1.0f;
		if (world != m_World)
		{
			UnloadAll();
			m_World = world;
			m_SeenWorld.reset();
			m_LayerCells.clear();
			m_LayerEnabled.clear();
			m_HasEvaluated = false;
			m_Dirty = true;
		}
		if (scale != m_DistanceScale)
		{
			m_DistanceScale = scale;
			m_Dirty = true;
		}
	}

	Ref<WorldAsset> WorldStreamer::GetWorldAsset() const
	{
		AssetManagerBase* assets = m_Scene.GetAssetManager();
		return assets && m_World.IsValid() ? assets->PeekLoadedAsset<WorldAsset>(m_World) : nullptr;
	}

	void WorldStreamer::SetSources(std::span<const WorldStreamingSource> sources)
	{
		m_Sources.assign(sources.begin(), sources.end());
		// Sources that are no number cannot stream anything.
		std::erase_if(m_Sources, [](const WorldStreamingSource& source) { return !IsFiniteVector(source.Position) || !std::isfinite(source.Radius); });
	}

	bool WorldStreamer::SetLayerEnabled(std::string_view layer, bool enabled)
	{
		if (const Ref<WorldAsset> world = m_SeenWorld.lock())
		{
			const std::vector<WorldLayer>& layers = world->GetData().Layers;
			const auto found = std::find_if(layers.begin(), layers.end(), [&](const WorldLayer& item) { return item.Name == layer; });
			if (found == layers.end())
				return false;
			const size_t index = static_cast<size_t>(found - layers.begin());
			if (m_LayerEnabled[index] != enabled)
			{
				m_LayerEnabled[index] = enabled;
				m_Dirty = true;
			}
		}
		const auto existing = std::find_if(m_LayerOverrides.begin(), m_LayerOverrides.end(), [&](const auto& item) { return item.first == layer; });
		if (existing != m_LayerOverrides.end())
			existing->second = enabled;
		else
			m_LayerOverrides.emplace_back(std::string(layer), enabled);
		return true;
	}

	std::optional<bool> WorldStreamer::IsLayerEnabled(std::string_view layer) const
	{
		if (const Ref<WorldAsset> world = m_SeenWorld.lock())
		{
			const std::vector<WorldLayer>& layers = world->GetData().Layers;
			const auto found = std::find_if(layers.begin(), layers.end(), [&](const WorldLayer& item) { return item.Name == layer; });
			if (found == layers.end())
				return std::nullopt;
			return static_cast<bool>(m_LayerEnabled[static_cast<size_t>(found - layers.begin())]);
		}
		const auto existing = std::find_if(m_LayerOverrides.begin(), m_LayerOverrides.end(), [&](const auto& item) { return item.first == layer; });
		if (existing != m_LayerOverrides.end())
			return existing->second;
		return std::nullopt;
	}

	void WorldStreamer::PinCell(const WorldCellKey& key)
	{
		if (std::abs(key.X) > WorldCellKey::c_MaxCoordinate || std::abs(key.Z) > WorldCellKey::c_MaxCoordinate)
			return;
		if (m_Pins[key.Pack()]++ == 0)
			m_Dirty = true;
	}

	void WorldStreamer::UnpinCell(const WorldCellKey& key)
	{
		if (std::abs(key.X) > WorldCellKey::c_MaxCoordinate || std::abs(key.Z) > WorldCellKey::c_MaxCoordinate)
			return;
		const auto it = m_Pins.find(key.Pack());
		if (it == m_Pins.end())
			return;
		if (--it->second == 0)
		{
			m_Pins.erase(it);
			m_Dirty = true;
		}
	}

	bool WorldStreamer::IsPinned(const WorldCellKey& key) const
	{
		if (std::abs(key.X) > WorldCellKey::c_MaxCoordinate || std::abs(key.Z) > WorldCellKey::c_MaxCoordinate)
			return false;
		return m_Pins.contains(key.Pack());
	}

	void WorldStreamer::AdoptLoadedCells()
	{
		if (const Ref<WorldAsset> world = GetWorldAsset())
		{
			RefreshWorld(world);
			AdoptCells(*world);
		}
		else
		{
			m_AdoptPending = true;
		}
	}

	void WorldStreamer::AdoptCells(const WorldAsset& world)
	{
		m_AdoptPending = false;
		for (const WorldCell& cell : world.GetData().Cells)
		{
			const uint64_t key = cell.GetKey().Pack();
			if (m_Cells.contains(key))
				continue;
			const SubSceneID id = m_Scene.FindSubSceneBySource(cell.Scene);
			const SubSceneStatus* status = id != 0 ? m_Scene.GetSubSceneStatus(id) : nullptr;
			if (!status || status->State == SubSceneState::Unloading || status->State == SubSceneState::Failed)
				continue;
			CellRecord record;
			record.SubScene = id;
			record.Scene = cell.Scene;
			record.CreatedSeen = status->CreatedEntities;
			record.Status = status->State == SubSceneState::Loaded ? WorldCellStatus::Loaded : WorldCellStatus::Loading;
			if (record.Status == WorldCellStatus::Loading)
				m_InFlight.push_back(key);
			m_Cells.emplace(key, std::move(record));
			m_Stats.StateVersion++;
		}
		m_Dirty = true;
	}

	void WorldStreamer::Update()
	{
		ST_PROFILE_FUNCTION();
		m_Decisions.clear();
		m_Stats.EntitiesCreatedLastUpdate = 0;
		m_Stats.EntitiesDestroyedLastUpdate = 0;
		m_Stats.EvaluatedCells = 0;
		AssetManagerBase* assets = m_Scene.GetAssetManager();
		if (!assets || !m_World.IsValid())
			return;

		// Requested every update: the World asset stays resident while it streams, without a reference held across frames.
		const Ref<WorldAsset> world = assets->GetAsset<WorldAsset>(m_World, AssetPriority::High);
		if (PollCellsInFlight())
			m_Dirty = true;
		if (!world)
			return;
		if (RefreshWorld(world))
			m_Dirty = true;
		if (RetryChangedFailures())
			m_Dirty = true;
		if (!m_Dirty && !SourcesMoved())
			return;
		Evaluate(*world);
		m_Dirty = false;
	}

	bool WorldStreamer::RefreshWorld(const Ref<WorldAsset>& world)
	{
		if (m_SeenWorld.lock() == world)
			return false;
		m_SeenWorld = world;
		const WorldData& data = world->GetData();
		m_LayerCells.assign(data.Layers.size(), {});
		m_LayerEnabled.assign(data.Layers.size(), true);
		for (size_t index = 0; index < data.Layers.size(); index++)
		{
			const WorldLayer& layer = data.Layers[index];
			bool enabled = layer.EnabledByDefault;
			for (const auto& [name, value] : m_LayerOverrides)
			{
				if (name == layer.Name)
					enabled = value;
			}
			m_LayerEnabled[index] = enabled;
		}
		for (size_t index = 0; index < data.Cells.size(); index++)
		{
			const auto layer = std::find_if(data.Layers.begin(), data.Layers.end(), [&](const WorldLayer& item) { return item.ID == data.Cells[index].Layer; });
			if (layer != data.Layers.end())
				m_LayerCells[static_cast<size_t>(layer - data.Layers.begin())].push_back(static_cast<int32_t>(index));
		}
		m_Stats.StateVersion++;
		if (m_AdoptPending)
			AdoptCells(*world);
		return true;
	}

	bool WorldStreamer::PollCellsInFlight()
	{
		bool finished = false;
		for (size_t position = 0; position < m_InFlight.size();)
		{
			const uint64_t key = m_InFlight[position];
			const auto it = m_Cells.find(key);
			if (it == m_Cells.end() || (it->second.Status != WorldCellStatus::Loading && it->second.Status != WorldCellStatus::Unloading))
			{
				m_InFlight[position] = m_InFlight.back();
				m_InFlight.pop_back();
				continue;
			}
			CellRecord& record = it->second;
			const SubSceneStatus* status = m_Scene.GetSubSceneStatus(record.SubScene);
			bool done = false;
			if (record.Status == WorldCellStatus::Unloading)
			{
				if (!status)
				{
					// Its last entities went in the last update.
					if (record.EntitiesAtUnload > record.DestroyedSeen)
						m_Stats.EntitiesDestroyedLastUpdate += static_cast<uint32_t>(record.EntitiesAtUnload - record.DestroyedSeen);
					m_Cells.erase(it);
					done = true;
				}
				else if (status->State == SubSceneState::Unloading)
				{
					m_Stats.EntitiesDestroyedLastUpdate += static_cast<uint32_t>(status->DestroyedEntities - std::min(status->DestroyedEntities, record.DestroyedSeen));
					record.DestroyedSeen = status->DestroyedEntities;
				}
			}
			else if (!status)
			{
				// Unloaded by someone else to its last entity.
				m_Cells.erase(it);
				done = true;
			}
			else
			{
				m_Stats.EntitiesCreatedLastUpdate += static_cast<uint32_t>(status->CreatedEntities - std::min(status->CreatedEntities, record.CreatedSeen));
				record.CreatedSeen = status->CreatedEntities;
				if (status->State == SubSceneState::Loaded)
				{
					record.Status = WorldCellStatus::Loaded;
					done = true;
				}
				else if (status->State == SubSceneState::Failed)
				{
					// Kept as failed (with its error) until its scene changes; the failed sub-scene goes.
					record.Status = WorldCellStatus::Failed;
					record.Error = status->Error;
					m_FailedCells++;
					m_Scene.UnloadSubScene(record.SubScene);
					record.SubScene = 0;
					if (AssetManagerBase* assets = m_Scene.GetAssetManager())
						record.FailedRevision = assets->GetSourceRevisions(record.Scene).Current;
					done = true;
				}
				else if (status->State == SubSceneState::Unloading)
				{
					// Someone else unloads it: it goes like one the streamer unloads.
					record.Status = WorldCellStatus::Unloading;
					record.EntitiesAtUnload = status->CreatedEntities;
					record.DestroyedSeen = status->DestroyedEntities;
					m_Stats.StateVersion++;
				}
			}
			if (done)
			{
				m_Stats.StateVersion++;
				finished = true;
				m_InFlight[position] = m_InFlight.back();
				m_InFlight.pop_back();
				continue;
			}
			position++;
		}
		return finished;
	}

	bool WorldStreamer::RetryChangedFailures()
	{
		// A changed scene publishes a content change (ReloadAsset): only then are the failed cells' sources compared.
		AssetManagerBase* assets = m_Scene.GetAssetManager();
		const uint64_t version = assets->GetContentVersion();
		if (m_FailedCells == 0 || version == m_FailureVersion)
			return false;
		m_FailureVersion = version;
		bool retry = false;
		for (auto it = m_Cells.begin(); it != m_Cells.end();)
		{
			if (it->second.Status == WorldCellStatus::Failed && assets->GetSourceRevisions(it->second.Scene).Current != it->second.FailedRevision)
			{
				it = m_Cells.erase(it);
				m_FailedCells--;
				m_Stats.StateVersion++;
				retry = true;
				continue;
			}
			++it;
		}
		return retry;
	}

	bool WorldStreamer::SourcesMoved() const
	{
		if (m_Sources.size() != m_EvaluatedSources.size())
			return true;
		for (size_t index = 0; index < m_Sources.size(); index++)
		{
			const WorldStreamingSource& source = m_Sources[index];
			const WorldStreamingSource& evaluated = m_EvaluatedSources[index];
			// The facing only orders loads: it is read whenever an evaluation runs.
			if (glm::distance(source.Position, evaluated.Position) > c_SourceEpsilon || source.Radius != evaluated.Radius || source.LayerMask != evaluated.LayerMask
				|| source.Priority != evaluated.Priority || source.ViewBias != evaluated.ViewBias)
				return true;
		}
		return false;
	}

	bool WorldStreamer::IsLayerOn(size_t layerIndex) const
	{
		return layerIndex < m_LayerEnabled.size() && m_LayerEnabled[layerIndex];
	}

	float WorldStreamer::GetRadius(const WorldStreamingSource& source, size_t layerIndex) const
	{
		const Ref<WorldAsset> world = m_SeenWorld.lock();
		if (source.Radius > 0.0f || !world)
			return std::max(source.Radius, 0.0f);
		return world->GetData().Layers[layerIndex].StreamingDistance * m_DistanceScale;
	}

	void WorldStreamer::Evaluate(const WorldAsset& world)
	{
		ST_PROFILE_FUNCTION();
		const WorldData& data = world.GetData();
		m_HasEvaluated = true;
		m_EvaluatedSources = m_Sources;
		m_Candidates.clear();
		m_CandidateIndex.clear();
		m_Keep.clear();
		uint32_t evaluated = 0;

		auto want = [this](uint64_t key, int32_t priority, double distance, bool high)
		{
			const auto [it, inserted] = m_CandidateIndex.try_emplace(key, m_Candidates.size());
			if (inserted)
			{
				m_Candidates.push_back(Candidate { key, priority, distance, high });
				return;
			}
			Candidate& candidate = m_Candidates[it->second];
			if (priority > candidate.Priority || (priority == candidate.Priority && distance < candidate.Distance))
			{
				candidate.Priority = priority;
				candidate.Distance = distance;
			}
			candidate.High = candidate.High || high;
		};

		for (const WorldStreamingSource& source : m_Sources)
		{
			const glm::dvec3 position(source.Position);
			glm::dvec2 forward(source.Forward.x, source.Forward.z);
			const double forwardLength = glm::length(forward);
			forward = forwardLength > 1e-6 ? forward / forwardLength : glm::dvec2(0.0);
			const double viewBias = std::isfinite(source.ViewBias) ? std::clamp(static_cast<double>(source.ViewBias), 0.0, 1.0) : 0.0;
			for (size_t layerIndex = 0; layerIndex < data.Layers.size() && layerIndex < 32; layerIndex++)
			{
				if (!IsLayerOn(layerIndex) || (source.LayerMask & (1u << layerIndex)) == 0)
					continue;
				const WorldLayer& layer = data.Layers[layerIndex];
				const double radius = GetRadius(source, layerIndex);
				const double hysteresis = std::max(static_cast<double>(layer.CellSize) * 0.5, 0.1 * radius);
				const double keepRadius = radius + hysteresis;
				ForEachCellAround(world, layer, m_LayerCells[layerIndex], position, keepRadius, [&](int32_t index)
				{
					const WorldCell& cell = data.Cells[static_cast<size_t>(index)];
					const WorldCellSquare square = WorldGrid::CellBounds(layer, data.GridOrigin, cell.X, cell.Z);
					const double distance = WorldGrid::Distance(square, position);
					evaluated++;
					if (distance > keepRadius)
						return;
					const uint64_t key = cell.GetKey().Pack();
					m_Keep.insert(key);
					if (distance > radius)
						return;
					// Cells ahead of the source rank as if a share of the radius nearer.
					const glm::dvec2 toCell = (square.Min + square.Max) * 0.5 - glm::dvec2(position.x, position.z);
					const double toCellLength = glm::length(toCell);
					const double ahead = toCellLength > 1e-6 ? std::max(0.0, glm::dot(toCell / toCellLength, forward)) : 0.0;
					want(key, source.Priority, distance - viewBias * radius * ahead, distance <= radius * 0.5);
				});
			}
		}
		// Pinned cells are always wanted, before any source's.
		for (const auto& [key, count] : m_Pins)
		{
			if (world.FindCell(WorldCellKey::Unpack(key)) < 0)
				continue;
			m_Keep.insert(key);
			want(key, std::numeric_limits<int32_t>::max(), 0.0, true);
		}
		m_Stats.EvaluatedCells = evaluated;
		m_Wanted.clear();
		for (const Candidate& candidate : m_Candidates)
			m_Wanted.insert(candidate.Key);

		// Unloads and cancellations first, in key order: cells left behind, of disabled layers, or gone from the world, and
		// loaded ones someone else unloaded meanwhile.
		m_Keys.clear();
		for (auto& [key, record] : m_Cells)
		{
			if (record.Status == WorldCellStatus::Loaded)
			{
				const SubSceneStatus* status = m_Scene.GetSubSceneStatus(record.SubScene);
				if (!status || status->State != SubSceneState::Loaded)
				{
					m_Keys.push_back(key);
					continue;
				}
			}
			if ((record.Status == WorldCellStatus::Loading || record.Status == WorldCellStatus::Loaded) && !m_Keep.contains(key))
				m_Keys.push_back(key);
		}
		std::sort(m_Keys.begin(), m_Keys.end());
		for (const uint64_t key : m_Keys)
		{
			CellRecord& record = m_Cells.at(key);
			const SubSceneStatus* status = m_Scene.GetSubSceneStatus(record.SubScene);
			if (record.Status == WorldCellStatus::Loaded && (!status || status->State != SubSceneState::Loaded))
			{
				// Gone or going without the streamer: follow it.
				if (status && status->State == SubSceneState::Unloading)
				{
					record.Status = WorldCellStatus::Unloading;
					record.EntitiesAtUnload = status->CreatedEntities;
					record.DestroyedSeen = status->DestroyedEntities;
					m_InFlight.push_back(key);
				}
				else
				{
					m_Cells.erase(key);
				}
				m_Stats.StateVersion++;
				continue;
			}
			Unload(key, record);
		}

		// Loads, best ranked first, while fewer than c_MaxConcurrentCellLoads load.
		uint32_t loading = 0;
		for (const auto& [key, record] : m_Cells)
			loading += record.Status == WorldCellStatus::Loading ? 1 : 0;
		std::sort(m_Candidates.begin(), m_Candidates.end(), [](const Candidate& a, const Candidate& b)
		{
			if (a.Priority != b.Priority)
				return a.Priority > b.Priority;
			if (a.Distance != b.Distance)
				return a.Distance < b.Distance;
			return a.Key < b.Key;
		});
		size_t starting = 0;
		std::vector<Candidate>& candidates = m_Candidates;
		for (size_t index = 0; index < candidates.size() && loading + starting < c_MaxConcurrentCellLoads; index++)
		{
			if (m_Cells.contains(candidates[index].Key))
				continue;
			// Moved to the front, in rank order: the cells this update starts.
			std::swap(candidates[starting], candidates[index]);
			starting++;
		}
		if (starting == 0)
			return;
		std::sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(starting), [](const Candidate& a, const Candidate& b)
		{
			if (a.Priority != b.Priority)
				return a.Priority > b.Priority;
			if (a.Distance != b.Distance)
				return a.Distance < b.Distance;
			return a.Key < b.Key;
		});
		ReserveBodies(world, std::span<const Candidate>(candidates.data(), starting));
		for (size_t index = 0; index < starting; index++)
			StartLoad(world, candidates[index]);
	}

	void WorldStreamer::ReserveBodies(const WorldAsset& world, std::span<const Candidate> starting)
	{
		const WorldData& data = world.GetData();
		if (!data.Index)
			return;
		IPhysicsCapacity* capacity = m_Scene.GetService<IPhysicsCapacity>();
		if (!capacity)
			return;
		// What the persistent scene, the cells still holding bodies and the cells about to load need together.
		uint64_t bodies = data.Index->PersistentBodies;
		for (const auto& [key, record] : m_Cells)
		{
			if (record.Status == WorldCellStatus::Failed)
				continue;
			if (const WorldCellSummary* summary = world.FindSummary(WorldCellKey::Unpack(key)))
				bodies += summary->Bodies;
		}
		for (const Candidate& candidate : starting)
		{
			if (const WorldCellSummary* summary = world.FindSummary(WorldCellKey::Unpack(candidate.Key)))
				bodies += summary->Bodies;
		}
		capacity->Reserve(static_cast<uint32_t>(std::min<uint64_t>(bodies, std::numeric_limits<uint32_t>::max())));
	}

	void WorldStreamer::StartLoad(const WorldAsset& world, const Candidate& candidate)
	{
		const int32_t index = world.FindCell(WorldCellKey::Unpack(candidate.Key));
		if (index < 0)
			return;
		const WorldCell& cell = world.GetData().Cells[static_cast<size_t>(index)];
		SubSceneOptions options;
		options.Priority = candidate.High ? AssetPriority::High : AssetPriority::Normal;
		options.Reload = m_ReloadCells;
		std::string error;
		const SubSceneID id = m_Scene.LoadSubScene(cell.Scene, options, &error);
		CellRecord record;
		record.Scene = cell.Scene;
		if (id == 0)
		{
			record.Status = WorldCellStatus::Failed;
			record.Error = std::move(error);
			m_FailedCells++;
			if (AssetManagerBase* assets = m_Scene.GetAssetManager())
				record.FailedRevision = assets->GetSourceRevisions(cell.Scene).Current;
		}
		else
		{
			record.Status = WorldCellStatus::Loading;
			record.SubScene = id;
			m_InFlight.push_back(candidate.Key);
		}
		m_Cells[candidate.Key] = std::move(record);
		m_Decisions.push_back(WorldStreamingDecision { WorldStreamingDecision::Action::Load, WorldCellKey::Unpack(candidate.Key) });
		m_Stats.LoadsStarted++;
		m_Stats.StateVersion++;
	}

	void WorldStreamer::Unload(uint64_t key, CellRecord& record)
	{
		const SubSceneStatus* status = m_Scene.GetSubSceneStatus(record.SubScene);
		record.EntitiesAtUnload = status ? status->CreatedEntities : 0;
		record.DestroyedSeen = 0;
		m_Scene.UnloadSubScene(record.SubScene);
		if (record.Status == WorldCellStatus::Loaded)
			m_InFlight.push_back(key);
		record.Status = WorldCellStatus::Unloading;
		m_Decisions.push_back(WorldStreamingDecision { WorldStreamingDecision::Action::Unload, WorldCellKey::Unpack(key) });
		m_Stats.UnloadsStarted++;
		m_Stats.StateVersion++;
	}

	void WorldStreamer::UnloadAll()
	{
		for (auto& [key, record] : m_Cells)
		{
			if (record.Status == WorldCellStatus::Loading || record.Status == WorldCellStatus::Loaded)
				m_Scene.UnloadSubScene(record.SubScene);
		}
		m_Cells.clear();
		m_InFlight.clear();
		m_Wanted.clear();
		m_FailedCells = 0;
		m_Stats.StateVersion++;
		m_Dirty = true;
	}

	bool WorldStreamer::IsLoadedAround(const glm::vec3& position, float radius) const
	{
		const Ref<WorldAsset> world = m_SeenWorld.lock();
		if (!world || !IsFiniteVector(position) || !std::isfinite(radius))
			return false;
		const WorldData& data = world->GetData();
		const glm::dvec3 point(position);
		const double reach = std::max(static_cast<double>(radius), 0.0);
		bool loaded = true;
		for (size_t layerIndex = 0; layerIndex < data.Layers.size() && loaded; layerIndex++)
		{
			if (!IsLayerOn(layerIndex))
				continue;
			const WorldLayer& layer = data.Layers[layerIndex];
			ForEachCellAround(*world, layer, m_LayerCells[layerIndex], point, reach, [&](int32_t index)
			{
				const WorldCell& cell = data.Cells[static_cast<size_t>(index)];
				if (!loaded || WorldGrid::Distance(WorldGrid::CellBounds(layer, data.GridOrigin, cell.X, cell.Z), point) > reach)
					return;
				const auto it = m_Cells.find(cell.GetKey().Pack());
				loaded = it != m_Cells.end() && it->second.Status == WorldCellStatus::Loaded;
			});
		}
		return loaded;
	}

	WorldStreamingStats WorldStreamer::GetStats() const
	{
		WorldStreamingStats stats = m_Stats;
		stats.CellsWanted = static_cast<uint32_t>(m_Wanted.size());
		stats.CellsPinned = static_cast<uint32_t>(m_Pins.size());
		stats.CellsLoading = 0;
		stats.CellsLoaded = 0;
		stats.CellsUnloading = 0;
		stats.CellsFailed = 0;
		for (const auto& [key, record] : m_Cells)
		{
			switch (record.Status)
			{
				case WorldCellStatus::Loading:   stats.CellsLoading++; break;
				case WorldCellStatus::Loaded:    stats.CellsLoaded++; break;
				case WorldCellStatus::Unloading: stats.CellsUnloading++; break;
				case WorldCellStatus::Failed:    stats.CellsFailed++; break;
				case WorldCellStatus::Unloaded:  break;
			}
		}
		return stats;
	}

	void WorldStreamer::GetCellStates(std::vector<WorldCellState>& outStates) const
	{
		outStates.clear();
		const Ref<WorldAsset> world = m_SeenWorld.lock();
		if (!world)
			return;
		outStates.reserve(world->GetData().Cells.size());
		for (const WorldCell& cell : world->GetData().Cells)
		{
			WorldCellState state;
			state.Key = cell.GetKey();
			state.Scene = cell.Scene;
			const uint64_t key = state.Key.Pack();
			state.Wanted = m_Wanted.contains(key);
			state.Pinned = m_Pins.contains(key);
			if (const auto it = m_Cells.find(key); it != m_Cells.end())
			{
				const CellRecord& record = it->second;
				state.Status = record.Status;
				state.SubScene = record.SubScene;
				state.Error = record.Error;
				if (const SubSceneStatus* status = record.SubScene != 0 ? m_Scene.GetSubSceneStatus(record.SubScene) : nullptr)
					state.Entities = record.Status == WorldCellStatus::Unloading ? status->CreatedEntities - std::min(status->CreatedEntities, status->DestroyedEntities) : status->CreatedEntities;
			}
			outStates.push_back(std::move(state));
		}
	}

	SubSceneID WorldStreamer::GetCellSubScene(const WorldCellKey& key) const
	{
		if (std::abs(key.X) > WorldCellKey::c_MaxCoordinate || std::abs(key.Z) > WorldCellKey::c_MaxCoordinate)
			return 0;
		const auto it = m_Cells.find(key.Pack());
		return it != m_Cells.end() && it->second.Status != WorldCellStatus::Failed ? it->second.SubScene : 0;
	}

}
