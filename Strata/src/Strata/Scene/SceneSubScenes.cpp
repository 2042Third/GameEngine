#include "stpch.h"
#include "Strata/Scene/Scene.h"

#include "Strata/Core/JobSystem.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/EntityPrototype.h"
#include "Strata/Scene/SceneLoader.h"
#include "Strata/Scene/SubSceneLoad.h"

#include <algorithm>
#include <chrono>
#include <utility>

// Additive scenes: scene assets (or prepared scenes) merged into a scene over several updates, and removed again.

namespace Strata
{

	namespace
	{

		// Entities created between two looks at the clock: a few hundred microseconds of work in Release builds, so a slice
		// stops close to the time limit.
		constexpr size_t c_InstantiationChunk = 512;

	}

	const char* SubSceneStateToString(SubSceneState state)
	{
		switch (state)
		{
			case SubSceneState::Loading:       return "Loading";
			case SubSceneState::Instantiating: return "Instantiating";
			case SubSceneState::Loaded:        return "Loaded";
			case SubSceneState::Failed:        return "Failed";
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
		auto it = std::find_if(m_SubScenes.begin(), m_SubScenes.end(), [id](const Scope<SubSceneLoad>& load) { return load->Status.ID == id; });
		if (it == m_SubScenes.end())
			return false;
		if (m_AdvancingSubScenes)
		{
			ST_CORE_ASSERT(false, "Scene '{}': sub-scene {} cannot be unloaded from the signals of entities sub-scenes are creating", m_Name, id);
			ST_CORE_ERROR("Scene '{}': sub-scene {} cannot be unloaded from the signals of entities sub-scenes are creating", m_Name, id);
			return false;
		}

		// Taken out of the list first: systems reacting to the destruction may load or unload sub-scenes themselves.
		Scope<SubSceneLoad> load = std::move(*it);
		m_SubScenes.erase(it);
		if (load->Request)
			load->Request->Cancel();
		// A compilation still running finishes into its own result, which goes with the job.
		load->Instantiation.reset();
		DestroySubSceneEntities(*load);
		return true;
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
			return load->Status.State == SubSceneState::Loading || load->Status.State == SubSceneState::Instantiating;
		});
	}

	void Scene::UpdateSubScenes()
	{
		if (!HasPendingSubScenes())
			return;
		ST_PROFILE_FUNCTION();

		const auto deadline = std::chrono::steady_clock::now()
			+ std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(c_MaxInstantiationMillisecondsPerFrame));
		size_t budget = std::clamp<uint32_t>(m_Settings.MaxEntitiesInstantiatedPerFrame, 1, SceneSettings::c_MaxEntitiesInstantiatedLimit);
		// By ID: systems hearing the new entities may start sub-scene loads, which grow the list.
		std::vector<SubSceneID> pending;
		for (const Scope<SubSceneLoad>& load : m_SubScenes)
		{
			if (load->Status.State == SubSceneState::Loading || load->Status.State == SubSceneState::Instantiating)
				pending.push_back(load->Status.ID);
		}
		m_AdvancingSubScenes = true;
		for (const SubSceneID id : pending)
		{
			if (SubSceneLoad* load = FindSubScene(id))
				AdvanceSubScene(*load, budget, deadline);
		}
		m_AdvancingSubScenes = false;
	}

	void Scene::AdvanceSubScene(SubSceneLoad& load, size_t& budget, std::chrono::steady_clock::time_point deadline)
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
			const size_t count = ContinueInstantiation(instantiation, std::min(budget, c_InstantiationChunk), &error);
			if (count == 0)
			{
				FailSubScene(load, error.empty() ? std::string("No entity could be created") : std::move(error));
				return;
			}
			budget -= count;
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
		if (!instantiation.IsComplete())
		{
			status.Progress = 0.5f + 0.5f * static_cast<float>(status.CreatedEntities) / static_cast<float>(std::max<size_t>(1, status.TotalEntities));
			return;
		}

		// Every entity exists: from now on the scene requests what they use, so the load's pins go.
		status.State = SubSceneState::Loaded;
		status.Progress = 1.0f;
		load.Instantiation.reset();
		load.Request.reset();
		ST_CORE_TRACE("Scene '{}': sub-scene {} loaded ({} entities in {} slices)", m_Name, status.ID, status.TotalEntities, status.Slices);
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
