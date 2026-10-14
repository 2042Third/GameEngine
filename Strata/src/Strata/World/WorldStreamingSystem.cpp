#include "stpch.h"
#include "Strata/World/WorldStreamingSystem.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneWorkBudget.h"
#include "Strata/World/WorldAsset.h"
#include "Strata/World/WorldComponents.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace Strata
{

	namespace
	{

		// What the World module's preparation keeps on a loaded scene while it loads its start cells
		// (Scene::SetPreparedState): the streamer that loads them, dropped (leaving the cells) once they are in.
		struct PreparedWorldStart
		{
			Scope<WorldStreamer> Streamer;
			bool LayersChosen = false; // Only the LoadBeforeStart layers are enabled
		};

		bool HasWorldComponents(const Scene& scene)
		{
			const auto* storage = scene.GetRegistry().storage<WorldComponent>();
			return storage && !storage->empty();
		}

		std::string_view Trim(std::string_view text)
		{
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
				text.remove_prefix(1);
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
				text.remove_suffix(1);
			return text;
		}

		// The layers a source's comma-separated list names, as a mask of the world's layer indexes; every layer for an empty
		// list (or before the world is loaded). Names the world does not have are left out, warned about once each.
		uint32_t ParseLayerMask(const WorldData* data, std::string_view list, std::set<std::string>* warnedLayers)
		{
			if (!data || Trim(list).empty())
				return ~0u;
			uint32_t mask = 0;
			while (!list.empty())
			{
				const size_t comma = list.find(',');
				const std::string_view name = Trim(list.substr(0, comma));
				list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
				if (name.empty())
					continue;
				const auto layer = std::find_if(data->Layers.begin(), data->Layers.end(), [&](const WorldLayer& item) { return item.Name == name; });
				if (layer != data->Layers.end())
				{
					mask |= 1u << static_cast<uint32_t>(layer - data->Layers.begin());
				}
				else if (warnedLayers && warnedLayers->emplace(name).second)
				{
					ST_CORE_WARN("A streaming source names the layer '{}', which world '{}' does not have", name, data->Name);
				}
			}
			return mask;
		}

	}

	WorldStreamingSystem::WorldStreamingSystem(Scene& scene)
		: m_Scene(scene)
	{
		m_Scene.RegisterService<IWorldStreaming>(*this);
	}

	WorldStreamingSystem::~WorldStreamingSystem()
	{
		m_Scene.UnregisterService<IWorldStreaming>(*this);
	}

	Entity WorldStreamingSystem::FindWorldEntity(Scene& scene)
	{
		Entity found;
		const entt::registry& registry = scene.GetRegistry();
		for (const entt::entity handle : registry.view<const WorldComponent>())
		{
			const Entity entity(handle, &scene);
			if (!scene.IsActiveInHierarchy(entity))
				continue;
			if (!found || scene.CompareHierarchyOrder(entity, found) < 0)
				found = entity;
		}
		return found;
	}

	void WorldStreamingSystem::GatherSources(Scene& scene, const WorldStreamer& streamer, std::vector<WorldStreamingSource>& outSources, std::set<std::string>* warnedLayers)
	{
		outSources.clear();
		const Ref<WorldAsset> world = streamer.GetWorldAsset();
		const WorldData* data = world ? &world->GetData() : nullptr;
		const entt::registry& registry = scene.GetRegistry();
		if (const auto* storage = registry.storage<StreamingSourceComponent>(); storage && !storage->empty())
		{
			for (const entt::entity handle : static_cast<const entt::sparse_set&>(*storage))
			{
				const StreamingSourceComponent& component = registry.get<StreamingSourceComponent>(handle);
				const Entity entity(handle, &scene);
				if (!component.Enabled || !scene.IsActiveInHierarchy(entity))
					continue;
				const glm::mat4 transform = scene.GetWorldTransform(entity);
				WorldStreamingSource source;
				source.Position = glm::vec3(transform[3]);
				source.Forward = -glm::vec3(transform[2]);
				source.Radius = std::isfinite(component.Radius) ? std::clamp(component.Radius, 0.0f, 1'000'000.0f) : 0.0f;
				source.Priority = std::clamp(component.Priority, -100, 100);
				source.ViewBias = std::isfinite(component.ViewBias) ? std::clamp(component.ViewBias, 0.0f, 1.0f) : WorldStreamer::c_DefaultViewBias;
				source.LayerMask = ParseLayerMask(data, component.Layers, warnedLayers);
				outSources.push_back(source);
			}
		}
		// Without an active source, the primary camera streams.
		if (outSources.empty())
		{
			if (const Entity camera = scene.GetPrimaryCameraEntity())
			{
				const glm::mat4 transform = scene.GetWorldTransform(camera);
				WorldStreamingSource source;
				source.Position = glm::vec3(transform[3]);
				source.Forward = -glm::vec3(transform[2]);
				outSources.push_back(source);
			}
		}
	}

	bool WorldStreamingSystem::AreStartCellsDone(const WorldStreamer& streamer)
	{
		const Ref<WorldAsset> world = streamer.GetWorldAsset();
		if (!world || !streamer.HasEvaluated())
			return false;
		const WorldData& data = world->GetData();
		std::vector<WorldCellState> states;
		streamer.GetCellStates(states);
		for (const WorldCellState& state : states)
		{
			const WorldLayer* layer = data.FindLayer(state.Key.Layer);
			if (!state.Wanted || !layer || !layer->LoadBeforeStart || streamer.IsLayerEnabled(layer->Name) != true)
				continue;
			if (state.Status != WorldCellStatus::Loaded && state.Status != WorldCellStatus::Failed)
				return false;
		}
		return true;
	}

	void WorldStreamingSystem::SyncWorld()
	{
		// Scenes without a world cost this check alone.
		const Entity world = HasWorldComponents(m_Scene) ? FindWorldEntity(m_Scene) : Entity();
		const WorldComponent* component = world ? &world.GetComponent<WorldComponent>() : nullptr;
		if (!component || !component->World.IsValid())
		{
			if (m_Streamer)
			{
				// The scene stopped being a world's persistent scene: its cells go.
				m_Streamer->UnloadAll();
				m_Streamer.reset();
			}
			return;
		}
		if (!m_Streamer)
			m_Streamer = CreateScope<WorldStreamer>(m_Scene);
		m_Streamer->SetWorld(component->World, component->DistanceScale);
	}

	void WorldStreamingSystem::OnRuntimeStart()
	{
		SyncWorld();
		// The cells a prepared start loaded (or the editor adopted into a play copy) are streamed like any.
		if (m_Streamer)
			m_Streamer->AdoptLoadedCells();
	}

	void WorldStreamingSystem::OnRuntimeStop()
	{
		// The scene stops: what it holds goes with it, the cells included.
		m_Streamer.reset();
		m_Requests.clear();
	}

	void WorldStreamingSystem::OnUpdate(Timestep)
	{
		SyncWorld();
		if (!m_Streamer)
			return;
		GatherSources(m_Scene, *m_Streamer, m_Sources, &m_WarnedLayers);
		for (const auto& [id, source] : m_Requests)
			m_Sources.push_back(source);
		m_Streamer->SetSources(m_Sources);
		m_Streamer->Update();
	}

	uint64_t WorldStreamingSystem::RequestCellsAround(const glm::vec3& position, float radius)
	{
		if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) || !std::isfinite(radius) || radius < 0.0f)
			return 0;
		WorldStreamingSource source;
		source.Position = position;
		source.Forward = glm::vec3(0.0f);
		// A radius of 0 asks for the cells at the position (a source's 0 would mean each layer's distance).
		source.Radius = std::max(radius, 0.001f);
		source.Priority = c_RequestPriority;
		source.ViewBias = 0.0f;
		const uint64_t id = m_NextRequest++;
		m_Requests.emplace(id, source);
		return id;
	}

	bool WorldStreamingSystem::ReleaseCells(uint64_t request)
	{
		return m_Requests.erase(request) > 0;
	}

	bool WorldStreamingSystem::IsLoadedAround(const glm::vec3& position, float radius) const
	{
		return m_Streamer && m_Streamer->IsLoadedAround(position, radius);
	}

	bool WorldStreamingSystem::SetLayerEnabled(std::string_view layer, bool enabled)
	{
		return m_Streamer && m_Streamer->SetLayerEnabled(layer, enabled);
	}

	std::optional<bool> WorldStreamingSystem::IsLayerEnabled(std::string_view layer) const
	{
		return m_Streamer ? m_Streamer->IsLayerEnabled(layer) : std::nullopt;
	}

	WorldStreamingStats WorldStreamingSystem::GetStats() const
	{
		return m_Streamer ? m_Streamer->GetStats() : WorldStreamingStats();
	}

	bool WorldStreamingSystem::WaitForStartCells(std::chrono::milliseconds timeout)
	{
		SyncWorld();
		AssetManagerBase* assets = m_Scene.GetAssetManager();
		if (!m_Streamer || !assets)
			return false;
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline)
		{
			assets->Update();
			m_Scene.UpdateSubScenes();
			OnUpdate(Timestep(0.0f));
			if (!m_Streamer)
				return false;
			if (AreStartCellsDone(*m_Streamer))
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

	bool WorldStreamingSystem::PrepareLoadedScene(Scene& scene, SceneWorkBudget& budget)
	{
		if (!HasWorldComponents(scene))
			return true;
		ST_PROFILE_FUNCTION();
		const Entity world = FindWorldEntity(scene);
		const WorldComponent* component = world ? &world.GetComponent<WorldComponent>() : nullptr;
		AssetManagerBase* assets = scene.GetAssetManager();
		if (!component || !component->World.IsValid() || !assets)
			return true;

		PreparedWorldStart* prepared = scene.GetPreparedState<PreparedWorldStart>();
		if (!prepared)
		{
			Scope<PreparedWorldStart> state = CreateScope<PreparedWorldStart>();
			state->Streamer = CreateScope<WorldStreamer>(scene);
			state->Streamer->SetWorld(component->World, component->DistanceScale);
			prepared = &scene.SetPreparedState(std::move(state));
		}
		WorldStreamer& streamer = *prepared->Streamer;
		// The World asset loads with the scene (its WorldComponent references it): one that failed starts the world without
		// prepared cells, streaming them over its first frames.
		if (assets->GetAssetState(component->World) == AssetState::Failed)
		{
			scene.TakePreparedState<PreparedWorldStart>();
			return true;
		}

		std::vector<WorldStreamingSource> sources;
		GatherSources(scene, streamer, sources, nullptr);
		streamer.SetSources(sources);
		streamer.Update();
		const Ref<WorldAsset> asset = streamer.GetWorldAsset();
		if (!asset)
			return false;
		if (!prepared->LayersChosen)
		{
			for (const WorldLayer& layer : asset->GetData().Layers)
			{
				if (!layer.LoadBeforeStart)
					streamer.SetLayerEnabled(layer.Name, false);
			}
			prepared->LayersChosen = true;
			// A game never starts with less physics room than its world's cook found it needs: the world physics' hook
			// creates next is sized for it (as an export of the cooked capacity would have saved it).
			if (const std::optional<WorldIndex>& index = asset->GetData().Index)
			{
				PhysicsCapacitySettings& capacity = scene.GetSettings().PhysicsCapacity;
				capacity.MaxBodies = std::max(capacity.MaxBodies, index->GetRecommendedMaxBodies());
			}
			streamer.Update();
		}
		// A slice of the cells' entities within the frame's budget (their loads advance with the asset manager).
		scene.UpdateSubScenes(budget);
		streamer.Update();
		if (!AreStartCellsDone(streamer))
			return false;
		// The cells stay; the system that starts with the scene adopts them.
		scene.TakePreparedState<PreparedWorldStart>();
		return true;
	}

}
