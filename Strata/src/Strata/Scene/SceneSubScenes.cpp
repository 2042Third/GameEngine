#include "stpch.h"
#include "Strata/Scene/Scene.h"

#include "Strata/Core/JobSystem.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/EntityPrototype.h"
#include "Strata/Scene/SceneHierarchy.h"
#include "Strata/Scene/SceneLoader.h"
#include "Strata/Scene/SceneWorkBudget.h"
#include "Strata/Scene/SubSceneLoad.h"

#include <algorithm>
#include <chrono>
#include <utility>

// Additive scenes: scene assets (or prepared scenes) merged into a scene over several updates, and removed again.

namespace Strata
{

	namespace
	{

		// Entities created or destroyed between two looks at the clock: a few hundred microseconds of work in Release builds,
		// so a slice stops close to the time limit.
		constexpr size_t c_InstantiationChunk = 512;
		constexpr uint32_t c_UnloadChunk = 512;

	}

	const char* SubSceneStateToString(SubSceneState state)
	{
		switch (state)
		{
			case SubSceneState::Loading:       return "Loading";
			case SubSceneState::Instantiating: return "Instantiating";
			case SubSceneState::Loaded:        return "Loaded";
			case SubSceneState::Failed:        return "Failed";
			case SubSceneState::Unloading:     return "Unloading";
		}
		return "Unknown";
	}

	SubSceneID Scene::LoadSubScene(AssetHandle scene, const SubSceneOptions& options, std::string* outError)
	{
		SceneLoadOptions loadOptions;
		loadOptions.Priority = options.Priority;
		loadOptions.RequiredProgress = options.RequiredProgress;
		Scope<SceneLoadRequest> request = CreateScope<SceneLoadRequest>(SceneLoader::LoadAsync(scene, loadOptions));
		if (request->GetState() == SceneLoadState::Failed)
		{
			if (outError)
				*outError = request->GetError();
			return 0;
		}

		Scope<SubSceneLoad> load = CreateScope<SubSceneLoad>();
		load->Status.ID = m_NextSubSceneID++;
		load->Status.Scene = scene;
		load->Status.State = SubSceneState::Loading;
		load->Request = std::move(request);
		const SubSceneID id = load->Status.ID;
		m_SubScenes.push_back(std::move(load));
		return id;
	}

	SubSceneID Scene::BeginAdditiveLoad(Ref<Scene> prepared, std::string* outError)
	{
		if (!prepared || prepared.get() == this)
		{
			if (outError)
				*outError = prepared ? "A scene cannot be added to itself" : "No scene to add";
			return 0;
		}
		ST_CORE_ASSERT(!prepared->IsRunning(), "Scene::BeginAdditiveLoad: the prepared scene must not run");

		Scope<SubSceneLoad> load = CreateScope<SubSceneLoad>();
		load->Status.ID = m_NextSubSceneID++;
		load->Status.Scene = prepared->GetSourceAsset();
		load->Status.State = SubSceneState::Loading;
		load->Status.Progress = 0.5f;
		// The job owns the prepared scene from now on and drops it once compiled, on its thread.
		load->Compiled = CreateRef<SubSceneLoad::CompileResult>();
		load->CompileJob = JobSystem::Submit([result = load->Compiled, scene = std::move(prepared)]() mutable
		{
			result->Prototype = EntityPrototype::Compile(*scene);
			scene.reset();
		});
		const SubSceneID id = load->Status.ID;
		m_SubScenes.push_back(std::move(load));
		return id;
	}

	bool Scene::UnloadSubScene(SubSceneID id)
	{
		SubSceneLoad* load = FindSubScene(id);
		if (!load)
			return false;
		if (load->Status.State == SubSceneState::Unloading)
			return true;
		// While sub-scenes advance (a failed one's entities go, and their scripts' OnDestroy may unload sub-scenes), the load
		// being advanced must stay: the unload waits for the end of UpdateSubScenes.
		if (m_AdvancingSubScenes)
		{
			if (std::find(m_DeferredSubSceneUnloads.begin(), m_DeferredSubSceneUnloads.end(), id) == m_DeferredSubSceneUnloads.end())
				m_DeferredSubSceneUnloads.push_back(id);
			return true;
		}
		BeginSubSceneUnload(*load);
		RemoveUnloadedSubScenes();
		return true;
	}

	void Scene::BeginSubSceneUnload(SubSceneLoad& load)
	{
		SubSceneStatus& status = load.Status;
		if (load.Request)
			load.Request->Cancel();
		load.Request.reset();
		// A compilation still running finishes into its own result, which goes with the job.
		load.Compiled.reset();
		load.CompileJob = JobHandle();
		load.Instantiation.reset();

		// Its top-level entities as they are now leave the game at once: inactive, they leave rendering, the scripts' updates,
		// physics (in one batch) and audio in this frame, before any of them is destroyed.
		std::vector<Entity> roots;
		roots.reserve(load.Roots.size());
		for (const Entity root : load.Roots)
		{
			const SubSceneComponent* tag = root.IsValid() ? m_Registry.try_get<SubSceneComponent>(root.GetHandle()) : nullptr;
			if (tag && tag->SubScene == status.ID)
				roots.push_back(root);
		}
		for (Entity root : roots)
		{
			if (!root.HasComponent<InactiveComponent>())
				root.AddComponent<InactiveComponent>();
		}
		load.Roots = std::move(roots);
		status.State = SubSceneState::Unloading;
		status.DestroyedEntities = 0;
	}

	bool Scene::AdvanceSubSceneUnload(SubSceneLoad& load, uint32_t& budget, std::chrono::steady_clock::time_point deadline)
	{
		// While the scene updates, destruction is deferred to the end of the update: slices run between updates.
		ST_CORE_ASSERT(!m_IsUpdating, "Scene '{}': sub-scene slices run between updates", m_Name);
		std::vector<Entity> tops;
		while (budget > 0 && std::chrono::steady_clock::now() < deadline)
		{
			const size_t count = CollectUnloadSlice(load, std::min(budget, c_UnloadChunk), tops);
			if (count == 0)
				break;
			DestroyEntities(tops);
			load.Status.DestroyedEntities += count;
			budget -= static_cast<uint32_t>(count);
		}
		CollectUnloadSlice(load, 0, tops); // Only refreshes the living top-level entities
		return load.Roots.empty();
	}

	size_t Scene::CollectUnloadSlice(SubSceneLoad& load, size_t limit, std::vector<Entity>& outTops)
	{
		outTops.clear();
		// The top-level entities as they are now: those still of this sub-scene (the game may have destroyed them, or moved
		// them into another).
		std::erase_if(load.Roots, [&](const Entity root)
		{
			const SubSceneComponent* tag = root.IsValid() ? m_Registry.try_get<SubSceneComponent>(root.GetHandle()) : nullptr;
			return !tag || tag->SubScene != load.Status.ID;
		});
		if (limit == 0)
			return 0;

		// Post-order: an entity is collected once its children are, so every collected entity's descendants are collected
		// before it, and the slice destroys exactly what it collected. The tops are kept as a stack: collecting an entity
		// replaces its children's entries (the last ChildCount ones) with its own.
		std::vector<std::pair<entt::entity, entt::entity>>& walk = m_UnloadWalk;
		std::vector<entt::entity> tops;
		size_t collected = 0;
		for (const Entity root : load.Roots)
		{
			walk.clear();
			const HierarchyComponent& rootLinks = m_Registry.get<HierarchyComponent>(root.GetHandle());
			walk.emplace_back(root.GetHandle(), rootLinks.FirstChild);
			while (!walk.empty() && collected < limit)
			{
				auto& [entity, nextChild] = walk.back();
				if (nextChild != entt::null)
				{
					const entt::entity child = nextChild;
					const HierarchyComponent& childLinks = m_Registry.get<HierarchyComponent>(child);
					nextChild = childLinks.NextSibling;
					walk.emplace_back(child, childLinks.FirstChild);
					continue;
				}
				const uint32_t children = m_Registry.get<HierarchyComponent>(entity).ChildCount;
				ST_CORE_ASSERT(tops.size() >= children, "An unload slice collects every child before its parent");
				tops.resize(tops.size() - children);
				tops.push_back(entity);
				collected++;
				walk.pop_back();
			}
			if (collected >= limit)
				break;
		}
		outTops.reserve(tops.size());
		for (const entt::entity top : tops)
			outTops.emplace_back(top, this);
		return collected;
	}

	void Scene::RemoveUnloadedSubScenes()
	{
		std::vector<Entity> tops;
		std::erase_if(m_SubScenes, [&](const Scope<SubSceneLoad>& load)
		{
			if (load->Status.State != SubSceneState::Unloading)
				return false;
			CollectUnloadSlice(*load, 0, tops); // Only refreshes the living top-level entities
			return load->Roots.empty();
		});
	}

	void Scene::FlushSubSceneUnloads()
	{
		ST_CORE_ASSERT(!m_IsUpdating, "Scene '{}': sub-scene unloads finish between updates", m_Name);
		// By ID: systems hearing the destruction may load or unload sub-scenes.
		std::vector<SubSceneID> unloading;
		for (const Scope<SubSceneLoad>& load : m_SubScenes)
		{
			if (load->Status.State == SubSceneState::Unloading)
				unloading.push_back(load->Status.ID);
		}
		for (const SubSceneID id : unloading)
		{
			if (SubSceneLoad* load = FindSubScene(id))
			{
				uint32_t unlimited = SceneSettings::c_MaxEntitiesDestroyedLimit;
				while (!AdvanceSubSceneUnload(*load, unlimited, std::chrono::steady_clock::time_point::max()))
					unlimited = SceneSettings::c_MaxEntitiesDestroyedLimit;
			}
		}
		RemoveUnloadedSubScenes();
	}

	bool Scene::IsSubSceneLoaded(SubSceneID id) const
	{
		const SubSceneLoad* load = FindSubScene(id);
		return load && load->Status.State == SubSceneState::Loaded;
	}

	const SubSceneStatus* Scene::GetSubSceneStatus(SubSceneID id) const
	{
		const SubSceneLoad* load = FindSubScene(id);
		return load ? &load->Status : nullptr;
	}

	std::vector<SubSceneStatus> Scene::GetSubScenes() const
	{
		std::vector<SubSceneStatus> statuses;
		statuses.reserve(m_SubScenes.size());
		for (const Scope<SubSceneLoad>& load : m_SubScenes)
			statuses.push_back(load->Status);
		return statuses;
	}

	std::vector<Entity> Scene::GetSubSceneRoots(SubSceneID id) const
	{
		std::vector<Entity> roots;
		const SubSceneLoad* load = FindSubScene(id);
		if (!load)
			return roots;
		for (const Entity root : load->Roots)
		{
			const SubSceneComponent* tag = root.IsValid() ? m_Registry.try_get<SubSceneComponent>(root.GetHandle()) : nullptr;
			if (tag && tag->SubScene == id)
				roots.push_back(root);
		}
		return roots;
	}

	bool Scene::HasPendingSubScenes() const
	{
		return std::any_of(m_SubScenes.begin(), m_SubScenes.end(), [](const Scope<SubSceneLoad>& load)
		{
			const SubSceneState state = load->Status.State;
			return state == SubSceneState::Loading || state == SubSceneState::Instantiating || state == SubSceneState::Unloading;
		});
	}

	void Scene::UpdateSubScenes()
	{
		SceneWorkBudget budget = SceneWorkBudget::ForFrame(m_Settings, c_MaxSubSceneMillisecondsPerFrame);
		UpdateSubScenes(budget);
	}

	void Scene::UpdateSubScenes(SceneWorkBudget& budget)
	{
		if (!HasPendingSubScenes())
			return;
		ST_PROFILE_FUNCTION();
		ST_PROFILE_STAGE("Scene.Load");

		// Creation and destruction together, within the frame's budget.
		const auto deadline = budget.GetDeadlineWithin(budget.SubSceneMilliseconds);
		// By ID: systems hearing the new or destroyed entities may start or stop sub-scene loads, which change the list.
		std::vector<SubSceneID> unloading;
		std::vector<SubSceneID> pending;
		for (const Scope<SubSceneLoad>& load : m_SubScenes)
		{
			const SubSceneState state = load->Status.State;
			if (state == SubSceneState::Unloading)
				unloading.push_back(load->Status.ID);
			else if (state == SubSceneState::Loading || state == SubSceneState::Instantiating)
				pending.push_back(load->Status.ID);
		}
		m_AdvancingSubScenes = true;
		// Unloads first: they free memory, and the IDs a reload of the same scene waits for.
		for (const SubSceneID id : unloading)
		{
			if (SubSceneLoad* load = FindSubScene(id); load && load->Status.State == SubSceneState::Unloading)
				AdvanceSubSceneUnload(*load, budget.EntitiesToDestroy, deadline);
		}
		for (const SubSceneID id : pending)
		{
			if (SubSceneLoad* load = FindSubScene(id))
				AdvanceSubScene(*load, budget.EntitiesToCreate, deadline);
		}
		m_AdvancingSubScenes = false;
		RemoveUnloadedSubScenes();
		const std::vector<SubSceneID> unloads = std::move(m_DeferredSubSceneUnloads);
		m_DeferredSubSceneUnloads.clear();
		for (const SubSceneID id : unloads)
			UnloadSubScene(id);
	}

	void Scene::AdvanceSubScene(SubSceneLoad& load, uint32_t& budget, std::chrono::steady_clock::time_point deadline)
	{
		SubSceneStatus& status = load.Status;
		if (status.State == SubSceneState::Loading && load.Request && !load.Compiled)
		{
			SceneLoadRequest& request = *load.Request;
			request.Update();
			status.Progress = 0.5f * request.GetProgress();
			if (request.GetState() == SceneLoadState::Failed)
			{
				FailSubScene(load, request.GetError());
				return;
			}
			if (request.GetState() != SceneLoadState::Ready)
				return;
			Ref<Scene> prepared = request.TakeScene();
			if (!prepared)
			{
				FailSubScene(load, "The scene was taken by another load");
				return;
			}
			load.Compiled = CreateRef<SubSceneLoad::CompileResult>();
			load.CompileJob = JobSystem::Submit([result = load.Compiled, scene = std::move(prepared)]() mutable
			{
				result->Prototype = EntityPrototype::Compile(*scene);
				scene.reset();
			});
		}
		if (status.State == SubSceneState::Loading)
		{
			if (!load.CompileJob.IsComplete())
				return;
			status.Progress = 0.5f;
			// The same scene loaded again while its previous sub-scene is still being unloaded waits until that one is gone:
			// its entities keep the IDs the new ones would take.
			const bool previousLeaving = status.Scene.IsValid() && std::any_of(m_SubScenes.begin(), m_SubScenes.end(), [&](const Scope<SubSceneLoad>& other)
			{
				return other.get() != &load && other->Status.State == SubSceneState::Unloading && other->Status.Scene == status.Scene;
			});
			if (previousLeaving)
				return;
			StartSubSceneInstantiation(load);
		}
		if (status.State != SubSceneState::Instantiating || !load.Instantiation)
			return;

		// A slice: chunks until the budget is spent, the time is up or every entity exists.
		PrototypeInstantiation& instantiation = *load.Instantiation;
		size_t created = 0;
		while (budget > 0 && !instantiation.IsComplete() && std::chrono::steady_clock::now() < deadline)
		{
			std::string error;
			const size_t count = ContinueInstantiation(instantiation, std::min<size_t>(budget, c_InstantiationChunk), &error);
			if (count == 0)
			{
				FailSubScene(load, error.empty() ? std::string("No entity could be created") : std::move(error));
				return;
			}
			budget -= static_cast<uint32_t>(count);
			created += count;
			// The new top-level entities name their sub-scene, so that unloading finds them wherever the game moved them.
			const std::vector<Entity>& roots = instantiation.GetRoots();
			for (size_t index = load.Roots.size(); index < roots.size(); index++)
			{
				m_Registry.emplace_or_replace<SubSceneComponent>(roots[index].GetHandle(), SubSceneComponent { status.ID, status.Scene });
				load.Roots.push_back(roots[index]);
			}
		}
		if (created > 0)
			status.Slices++;
		status.CreatedEntities = instantiation.GetCreatedCount();
		status.SkippedEntities = instantiation.GetSkippedCount();
		if (!instantiation.IsComplete())
		{
			const float handled = static_cast<float>(instantiation.GetHandledCount());
			status.Progress = 0.5f + 0.5f * handled / static_cast<float>(std::max<size_t>(1, status.TotalEntities));
			return;
		}

		// Every entity exists: from now on the scene requests what they use, so the load's pins go.
		status.State = SubSceneState::Loaded;
		status.Progress = 1.0f;
		load.Instantiation.reset();
		load.Request.reset();
		ST_CORE_TRACE("Scene '{}': sub-scene {} loaded ({} entities in {} slices, {} not created because an ancestor was destroyed meanwhile)", m_Name,
			status.ID, status.CreatedEntities, status.Slices, status.SkippedEntities);
	}

	void Scene::StartSubSceneInstantiation(SubSceneLoad& load)
	{
		SubSceneStatus& status = load.Status;
		const Ref<EntityPrototype> prototype = load.Compiled ? load.Compiled->Prototype : nullptr;
		load.Compiled.reset();
		load.CompileJob = JobHandle();
		if (!prototype)
		{
			FailSubScene(load, "Its entities could not be compiled");
			return;
		}

		// The entities keep their IDs: one the scene has already fails the load before anything is created.
		InstantiationOptions options;
		options.KeepIDs = true;
		load.Instantiation = CreateScope<PrototypeInstantiation>();
		std::string error;
		if (!BeginInstantiation(prototype, options, *load.Instantiation, &error))
		{
			FailSubScene(load, std::move(error));
			return;
		}
		status.TotalEntities = prototype->GetEntityCount();
		status.State = SubSceneState::Instantiating;
	}

	void Scene::FailSubScene(SubSceneLoad& load, std::string error)
	{
		SubSceneStatus& status = load.Status;
		ST_CORE_ERROR("Scene '{}': sub-scene {}{} failed: {}", m_Name, status.ID,
			status.Scene.IsValid() ? fmt::format(" (scene {})", status.Scene.ToString()) : std::string(), error);
		status.State = SubSceneState::Failed;
		status.Error = std::move(error);
		status.Progress = 0.0f;
		if (load.Request)
			load.Request->Cancel();
		load.Request.reset();
		load.Compiled.reset();
		load.CompileJob = JobHandle();
		load.Instantiation.reset();
		DestroySubSceneEntities(load);
		status.CreatedEntities = 0;
		status.SkippedEntities = 0;
	}

	void Scene::DestroySubSceneEntities(SubSceneLoad& load)
	{
		std::vector<Entity> roots;
		roots.reserve(load.Roots.size());
		for (const Entity root : load.Roots)
		{
			const SubSceneComponent* tag = root.IsValid() ? m_Registry.try_get<SubSceneComponent>(root.GetHandle()) : nullptr;
			if (tag && tag->SubScene == load.Status.ID)
				roots.push_back(root);
		}
		load.Roots.clear();
		DestroyEntities(roots);
	}

	SubSceneLoad* Scene::FindSubScene(SubSceneID id) const
	{
		for (const Scope<SubSceneLoad>& load : m_SubScenes)
		{
			if (load->Status.ID == id)
				return load.get();
		}
		return nullptr;
	}

}
