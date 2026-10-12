#include "stpch.h"
#include "Strata/Animation/AnimationSystem.h"

#include "Strata/Animation/AnimationController.h"
#include "Strata/Animation/Skeleton.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Math/Math.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace Strata
{

	namespace
	{

		// The highest speed an animator accepts (AnimatorComponent::Speed's range, AnimationController::c_MaxSpeed).
		constexpr float c_MaxAnimatorSpeed = AnimationController::c_MaxSpeed;

		bool SameMatrix(const glm::mat4& a, const glm::mat4& b)
		{
			return std::memcmp(&a, &b, sizeof(glm::mat4)) == 0;
		}

		// The asset's object of type T (requesting the load), or null; outLoading tells a load in progress from an asset that
		// will not come (failed, unknown, of another type).
		template<typename T>
		Ref<T> RequestAsset(AssetHandle handle, bool& outLoading)
		{
			outLoading = false;
			const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
			if (!manager || !handle.IsValid())
				return nullptr;
			if (Ref<Asset> asset = manager->GetAsset(handle))
				return asset->GetType() == T::GetStaticType() ? std::static_pointer_cast<T>(asset) : nullptr;
			outLoading = manager->GetAssetState(handle) == AssetState::Loading;
			return nullptr;
		}

		std::string DescribeAsset(AssetHandle handle)
		{
			const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
			if (manager)
			{
				if (const std::optional<AssetMetadata> metadata = manager->GetMetadata(handle))
					return metadata->SubAssetKey.empty() ? metadata->Path : fmt::format("{} ({})", metadata->Path, metadata->SubAssetKey);
			}
			return fmt::format("{:016X}", static_cast<uint64_t>(handle));
		}

		std::string DescribeFailure(AssetHandle handle, AssetType expected)
		{
			const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
			if (!manager)
				return "no asset manager is active";
			if (!manager->IsHandleValid(handle))
				return "it is not a known asset";
			if (manager->GetAssetType(handle) != expected)
				return fmt::format("it is not a {}", AssetTypeToString(expected));
			const std::string error = manager->GetAssetError(handle);
			return error.empty() ? std::string("it did not load") : error;
		}

	}

	AnimationSystem::AnimationSystem(Scene& scene)
		: m_Scene(scene)
	{
		m_Scene.RegisterService<ISkinningPoses>(*this);
		m_Scene.RegisterService<IAnimatorControl>(*this);
		m_Scene.RegisterService<IAnimationEventSource>(*this);
		entt::registry& registry = m_Scene.GetRegistry();
		m_Connections.emplace_back(registry.on_construct<AnimatorComponent>().connect<&AnimationSystem::OnAnimatorAdded>(*this));
		m_Connections.emplace_back(registry.on_destroy<AnimatorComponent>().connect<&AnimationSystem::OnAnimatorRemoved>(*this));
		m_Connections.emplace_back(registry.on_destroy<AttachToJointComponent>().connect<&AnimationSystem::OnAttachmentRemoved>(*this));
		m_Connections.emplace_back(registry.on_construct<SkinComponent>().connect<&AnimationSystem::OnSkinChanged>(*this));
		m_Connections.emplace_back(registry.on_update<SkinComponent>().connect<&AnimationSystem::OnSkinChanged>(*this));
		m_Connections.emplace_back(registry.on_destroy<SkinComponent>().connect<&AnimationSystem::OnSkinRemoved>(*this));
	}

	AnimationSystem::~AnimationSystem()
	{
		OnRuntimeStop();
		m_Connections.clear();
		m_Scene.UnregisterService<IAnimationEventSource>(*this);
		m_Scene.UnregisterService<IAnimatorControl>(*this);
		m_Scene.UnregisterService<ISkinningPoses>(*this);
	}

	void AnimationSystem::OnRuntimeStart()
	{
		ST_PROFILE_FUNCTION();
		m_Running = true;
		entt::registry& registry = m_Scene.GetRegistry();
		for (const auto [handle, skin] : registry.view<SkinComponent>().each())
			CountSkinUser(handle, skin.Animator);
		for (const auto [handle, component] : registry.view<AnimatorComponent>().each())
			AddRecord(handle);
		// By entity (the order a scene that was just loaded created them in), whatever order the registry keeps them in;
		// animators added later follow in the order they were added. Results and events go out animator by animator in
		// this order.
		std::sort(m_Records.begin(), m_Records.end(), [](const AnimatorRecord& a, const AnimatorRecord& b)
		{
			return entt::to_entity(a.Handle) < entt::to_entity(b.Handle);
		});
		for (uint32_t index = 0; index < m_Records.size(); index++)
			m_RecordIndices[m_Records[index].Handle] = index;
		// Started before any script's OnCreate, which may play other states.
		for (AnimatorRecord& record : m_Records)
			SyncRecord(record, registry.get<AnimatorComponent>(record.Handle), m_Scene.IsActiveInHierarchy(Entity(record.Handle, &m_Scene)));
	}

	void AnimationSystem::OnRuntimeStop()
	{
		if (!m_Running && m_Records.empty())
			return;
		m_Running = false;
		m_Records.clear();
		m_RecordIndices.clear();
		m_RemovedRecords = 0;
		m_Sets.clear();
		m_FreeSets.clear();
		m_SetIndices.clear();
		m_Attachments.clear();
		m_SkinAnimators.clear();
		m_SkinUsers.clear();
		m_PendingSkins.clear();
		m_PendingEvents.clear();
		m_PosesVersion++;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Records
	////////////////////////////////////////////////////////////////////////////////

	void AnimationSystem::OnAnimatorAdded(entt::registry&, entt::entity handle)
	{
		if (m_Running)
			AddRecord(handle);
	}

	void AnimationSystem::OnAnimatorRemoved(entt::registry&, entt::entity handle)
	{
		auto it = m_RecordIndices.find(handle);
		if (it == m_RecordIndices.end())
			return;
		// Nothing finds the record any more; it leaves the list at the next compaction (CompactRecords), which keeps the
		// others in their order. An update works on the records by index meanwhile, skipping removed ones.
		AnimatorRecord& record = m_Records[it->second];
		m_RecordIndices.erase(it);
		record.Removed = true;
		if (record.PoseVersion != 0)
		{
			record.PoseVersion = 0;
			m_PosesVersion++;
		}
		m_RemovedRecords++;
	}

	void AnimationSystem::OnAttachmentRemoved(entt::registry&, entt::entity handle)
	{
		m_Attachments.erase(handle);
	}

	void AnimationSystem::OnSkinChanged(entt::registry&, entt::entity handle)
	{
		// Read at the next update, so a value written right after AddComponent (without a signal) is seen.
		if (m_Running)
			m_PendingSkins.push_back(handle);
	}

	void AnimationSystem::OnSkinRemoved(entt::registry&, entt::entity handle)
	{
		if (m_Running)
			CountSkinUser(handle, UUID::Null());
	}

	void AnimationSystem::ReadPendingSkins()
	{
		const entt::registry& registry = m_Scene.GetRegistry();
		for (const entt::entity handle : m_PendingSkins)
		{
			const SkinComponent* skin = registry.valid(handle) ? registry.try_get<SkinComponent>(handle) : nullptr;
			CountSkinUser(handle, skin ? skin->Animator : UUID::Null());
		}
		m_PendingSkins.clear();
	}

	void AnimationSystem::CountSkinUser(entt::entity skin, UUID animator)
	{
		// The animator it counted for loses it, the one it names now (if any) gains it.
		if (auto it = m_SkinAnimators.find(skin); it != m_SkinAnimators.end())
		{
			if (it->second == animator)
				return;
			auto users = m_SkinUsers.find(it->second);
			if (users != m_SkinUsers.end() && --users->second == 0)
				m_SkinUsers.erase(users);
			m_SkinAnimators.erase(it);
		}
		if (!animator.IsValid())
			return;
		m_SkinAnimators[skin] = animator;
		m_SkinUsers[animator]++;
	}

	bool AnimationSystem::HasSkinUsers(UUID animator) const
	{
		return m_SkinUsers.contains(animator);
	}

	void AnimationSystem::AddRecord(entt::entity handle)
	{
		if (m_RecordIndices.contains(handle))
			return;
		const AnimatorComponent& component = m_Scene.GetRegistry().get<AnimatorComponent>(handle);
		m_RecordIndices[handle] = static_cast<uint32_t>(m_Records.size());
		AnimatorRecord& record = m_Records.emplace_back();
		record.Handle = handle;
		const IDComponent* id = m_Scene.GetRegistry().try_get<IDComponent>(handle);
		record.ID = id ? id->ID : UUID::Null();
		// Commands work right away (a script that adds the component and plays a state); the first update starts it.
		record.Set = AcquireSet(component.Skeleton, component.Controller, component.Clip);
	}

	void AnimationSystem::CompactRecords()
	{
		if (m_RemovedRecords == 0)
			return;
		// One pass that closes the gaps: the remaining records keep their order (the order updates process them in), and
		// removing many animators costs one pass, not one per animator.
		size_t kept = 0;
		for (size_t index = 0; index < m_Records.size(); index++)
		{
			AnimatorRecord& record = m_Records[index];
			if (record.Removed)
			{
				if (record.Set != c_NoSet)
					ReleaseSet(record.Set);
				continue;
			}
			if (kept != index)
			{
				m_Records[kept] = std::move(record);
				m_RecordIndices[m_Records[kept].Handle] = static_cast<uint32_t>(kept);
			}
			kept++;
		}
		m_Records.erase(m_Records.begin() + static_cast<std::ptrdiff_t>(kept), m_Records.end());
		m_RemovedRecords = 0;
	}

	AnimationSystem::AnimatorRecord* AnimationSystem::FindRecord(entt::entity handle)
	{
		auto it = m_RecordIndices.find(handle);
		return it != m_RecordIndices.end() ? &m_Records[it->second] : nullptr;
	}

	const AnimationSystem::AnimatorRecord* AnimationSystem::FindRecord(entt::entity handle) const
	{
		auto it = m_RecordIndices.find(handle);
		return it != m_RecordIndices.end() ? &m_Records[it->second] : nullptr;
	}

	bool AnimationSystem::SyncRecord(AnimatorRecord& record, const AnimatorComponent& component, bool active)
	{
		auto clearPose = [this](AnimatorRecord& target)
		{
			if (target.PoseVersion != 0)
			{
				target.PoseVersion = 0;
				m_PosesVersion++;
			}
		};
		if (!active)
		{
			if (record.Active)
			{
				// Deactivated: it starts over when it is active again.
				record.Active = false;
				record.Instance.Reset();
				record.PlayedClipHandle = UUID::Null();
				record.PlayedClip.reset();
				clearPose(record);
			}
			return false;
		}

		// The assets follow the component; new ones start the animator over.
		const bool hasSet = record.Set != c_NoSet;
		if (!hasSet || m_Sets[record.Set].SkeletonHandle != component.Skeleton || m_Sets[record.Set].ControllerHandle != component.Controller
			|| m_Sets[record.Set].ClipHandle != component.Clip)
		{
			const bool wasPlaying = record.Instance.IsPlaying();
			const uint32_t set = AcquireSet(component.Skeleton, component.Controller, component.Clip);
			if (hasSet)
				ReleaseSet(record.Set);
			record.Set = set;
			if (hasSet && record.Active)
			{
				record.Instance.Reset();
				record.PlayedClipHandle = UUID::Null();
				record.PlayedClip.reset();
				clearPose(record);
				if (wasPlaying || component.PlayOnStart)
					record.Instance.Start();
			}
		}
		if (!record.Active)
		{
			record.Active = true;
			// A script may have played a state already (after adding the component, or in its OnCreate).
			if (component.PlayOnStart && !record.Instance.IsPlaying())
				record.Instance.Start();
		}
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Asset sets
	////////////////////////////////////////////////////////////////////////////////

	uint32_t AnimationSystem::AcquireSet(AssetHandle skeleton, AssetHandle controller, AssetHandle clip)
	{
		const std::array<uint64_t, 3> key = { static_cast<uint64_t>(skeleton), static_cast<uint64_t>(controller), static_cast<uint64_t>(clip) };
		auto it = m_SetIndices.find(key);
		if (it != m_SetIndices.end())
		{
			m_Sets[it->second].Users++;
			return it->second;
		}
		uint32_t index;
		if (!m_FreeSets.empty())
		{
			index = m_FreeSets.back();
			m_FreeSets.pop_back();
		}
		else
		{
			index = static_cast<uint32_t>(m_Sets.size());
			m_Sets.emplace_back();
		}
		AssetSet& set = m_Sets[index];
		set = AssetSet();
		set.SkeletonHandle = skeleton;
		set.ControllerHandle = controller;
		set.ClipHandle = clip;
		set.Users = 1;
		m_SetIndices[key] = index;
		return index;
	}

	void AnimationSystem::ReleaseSet(uint32_t index)
	{
		AssetSet& set = m_Sets[index];
		ST_CORE_ASSERT(set.Users > 0, "AnimationSystem: asset set {} released more often than acquired", index);
		if (--set.Users > 0)
			return;
		m_SetIndices.erase({ static_cast<uint64_t>(set.SkeletonHandle), static_cast<uint64_t>(set.ControllerHandle), static_cast<uint64_t>(set.ClipHandle) });
		set = AssetSet(); // Drops the assets
		m_FreeSets.push_back(index);
	}

	void AnimationSystem::ResolveSet(AssetSet& set)
	{
		if (set.ResolvedUpdate == m_UpdateIndex)
			return;
		set.ResolvedUpdate = m_UpdateIndex;
		std::vector<std::string> failures;

		// A load in progress keeps the object held (an evicted or reloading asset comes back as a new object).
		bool loading = false;
		bool skeletonChanged = false;
		if (Ref<Skeleton> skeleton = RequestAsset<Skeleton>(set.SkeletonHandle, loading))
		{
			if (skeleton != set.SkeletonAsset)
			{
				set.SkeletonAsset = std::move(skeleton);
				set.SkeletonId = m_NextObjectId++;
				set.RootMotion = RootMotionSetup::Create(*set.SkeletonAsset);
				skeletonChanged = true;
			}
		}
		else if (!loading && set.SkeletonAsset)
		{
			set.SkeletonAsset.reset();
			set.SkeletonId = 0;
			skeletonChanged = true;
		}
		if (!set.SkeletonAsset && !loading && set.SkeletonHandle.IsValid())
			failures.push_back(fmt::format("the skeleton {} did not load ({})", DescribeAsset(set.SkeletonHandle), DescribeFailure(set.SkeletonHandle, AssetType::Skeleton)));

		bool controllerChanged = false;
		if (Ref<AnimationController> controller = RequestAsset<AnimationController>(set.ControllerHandle, loading))
		{
			if (controller != set.Controller)
			{
				set.Controller = std::move(controller);
				set.ControllerId = m_NextObjectId++;
				controllerChanged = true;
			}
			set.ControllerFailed = false;
		}
		else
		{
			set.ControllerFailed = set.ControllerHandle.IsValid() && !loading;
			if (set.ControllerFailed && set.Controller)
			{
				set.Controller.reset();
				set.ControllerId = 0;
				controllerChanged = true;
			}
			if (set.ControllerFailed)
				failures.push_back(fmt::format("the animation controller {} did not load ({}); the animator plays its clip", DescribeAsset(set.ControllerHandle),
					DescribeFailure(set.ControllerHandle, AssetType::AnimationController)));
		}

		// One clip per controller state, or the component's clip.
		if (controllerChanged || set.ClipHandles.empty())
		{
			std::vector<AssetHandle> handles;
			if (set.Controller)
			{
				for (const AnimationState& state : set.Controller->GetStates())
					handles.push_back(state.Clip);
			}
			else
			{
				handles.push_back(set.ClipHandle);
			}
			if (handles != set.ClipHandles)
			{
				set.ClipHandles = std::move(handles);
				set.Clips.assign(set.ClipHandles.size(), nullptr);
				set.Bindings.assign(set.ClipHandles.size(), AnimationClipBinding());
				set.Reaches.assign(set.ClipHandles.size(), AnimationReach());
			}
		}
		set.ClipsLoading = false;
		set.StateClips.assign(set.ClipHandles.size(), AnimatorClip());
		for (size_t index = 0; index < set.ClipHandles.size(); index++)
		{
			const AssetHandle handle = set.ClipHandles[index];
			Ref<AnimationClip> clip = RequestAsset<AnimationClip>(handle, loading);
			bool rebind = skeletonChanged;
			if (clip && clip != set.Clips[index])
			{
				set.Clips[index] = std::move(clip);
				rebind = true;
			}
			else if (!clip && !loading && set.Clips[index])
			{
				set.Clips[index].reset();
			}
			if (!set.Clips[index])
			{
				// Waits for a clip on its way; one that will not come plays the rest pose.
				if (loading)
					set.ClipsLoading = true;
				else if (handle.IsValid())
					failures.push_back(fmt::format("the clip {} did not load ({})", DescribeAsset(handle), DescribeFailure(handle, AssetType::AnimationClip)));
				continue;
			}
			if (!set.SkeletonAsset)
				continue;
			if (rebind)
			{
				set.Bindings[index] = set.Clips[index]->Bind(*set.SkeletonAsset);
				set.Reaches[index] = AnimationReach::Compute(*set.Clips[index], set.Bindings[index], *set.SkeletonAsset, set.RootMotion);
			}
			set.StateClips[index] = AnimatorClip { set.Clips[index].get(), &set.Bindings[index], &set.Reaches[index] };
		}

		if (!failures.empty() && !set.Reported)
		{
			set.Reported = true;
			for (const std::string& failure : failures)
				ST_CORE_WARN("Animator: {}", failure);
		}
	}

	void AnimationSystem::ResolvePlayedClip(AnimatorRecord& record, const AssetSet& set)
	{
		const AssetHandle handle = record.Instance.GetPlayedClip();
		record.PlayedClipLoading = false;
		if (handle != record.PlayedClipHandle)
		{
			record.PlayedClipHandle = handle;
			record.PlayedClip.reset();
			record.PlayedBindingSkeletonId = 0;
		}
		if (!handle.IsValid())
			return;
		bool loading = false;
		Ref<AnimationClip> clip = RequestAsset<AnimationClip>(handle, loading);
		if (clip && clip != record.PlayedClip)
		{
			record.PlayedClip = std::move(clip);
			record.PlayedBindingSkeletonId = 0;
		}
		if (!record.PlayedClip)
		{
			// One that will not come plays the rest pose (PlayClip checked the handle's type).
			record.PlayedClipLoading = loading;
			return;
		}
		if (set.SkeletonAsset && record.PlayedBindingSkeletonId != set.SkeletonId)
		{
			record.PlayedBinding = record.PlayedClip->Bind(*set.SkeletonAsset);
			record.PlayedReach = AnimationReach::Compute(*record.PlayedClip, record.PlayedBinding, *set.SkeletonAsset, set.RootMotion);
			record.PlayedBindingSkeletonId = set.SkeletonId;
		}
	}

	AnimatorAssets AnimationSystem::MakeAssets(const AnimatorRecord& record) const
	{
		AnimatorAssets assets;
		if (record.Set == c_NoSet)
			return assets;
		const AssetSet& set = m_Sets[record.Set];
		assets.SkeletonAsset = set.SkeletonAsset.get();
		assets.RootMotion = set.SkeletonAsset ? &set.RootMotion : nullptr;
		assets.Controller = set.Controller.get();
		assets.ControllerId = set.ControllerId;
		assets.ControllerLoading = !set.Controller && set.ControllerHandle.IsValid() && !set.ControllerFailed;
		assets.StateClips = set.StateClips;
		if (record.PlayedClip && set.SkeletonAsset && record.PlayedBindingSkeletonId == set.SkeletonId)
			assets.PlayedClip = AnimatorClip { record.PlayedClip.get(), &record.PlayedBinding, &record.PlayedReach };
		return assets;
	}

	AnimatorAssets AnimationSystem::CommandAssets(AnimatorRecord& record)
	{
		if (record.Set != c_NoSet)
			ResolveSet(m_Sets[record.Set]);
		return MakeAssets(record);
	}

	bool AnimationSystem::IsReady(const AnimatorRecord& record) const
	{
		if (record.Set == c_NoSet)
			return false;
		const AssetSet& set = m_Sets[record.Set];
		const bool controllerLoading = !set.Controller && set.ControllerHandle.IsValid() && !set.ControllerFailed;
		return set.SkeletonAsset && !controllerLoading && !set.ClipsLoading && !record.PlayedClipLoading;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Update
	////////////////////////////////////////////////////////////////////////////////

	void AnimationSystem::OnUpdate(Timestep timestep)
	{
		ST_PROFILE_FUNCTION();
		m_UpdateIndex++;
		const float deltaTime = timestep;
		AnimationSystemStats& stats = m_Stats;
		stats.ActiveAnimators = 0;
		stats.Advanced = 0;
		stats.Evaluated = 0;
		stats.Culled = 0;
		stats.Waiting = 0;
		stats.Attachments = 0;
		// Animators removed since the previous update leave first; skins created or changed meanwhile are counted.
		CompactRecords();
		ReadPendingSkins();
		// Without animators there is nothing to resolve, step, place or dispatch (records go with their components, events
		// with the update that fired them), so scenes that do not animate pay next to nothing per frame.
		if (m_Records.empty())
			return;

		const entt::registry& registry = m_Scene.GetRegistry();
		// The shared assets first, once per set: requests and bindings (main thread). Sets without users are free slots.
		for (AssetSet& set : m_Sets)
		{
			if (set.Users > 0)
				ResolveSet(set);
		}

		// On the job system, every animator at once: those whose activity, assets and played clip are as they were step right
		// there; the others (activated, deactivated, given other assets or a clip to play) are queued for the main thread. Each
		// job only reads the registry and the asset sets and writes its own records, so the results do not depend on the
		// threads.
		if (m_SyncQueue.size() < m_Records.size())
			m_SyncQueue.resize(m_Records.size());
		std::atomic<uint32_t> queued = 0;
		JobSystem::ParallelFor(static_cast<uint32_t>(m_Records.size()), c_EvaluationBatchSize, [this, &registry, &queued, deltaTime](uint32_t begin, uint32_t end)
		{
			ST_PROFILE_SCOPE("AnimationSystem::Evaluate");
			thread_local std::vector<JointPose> t_Scratch;
			for (uint32_t index = begin; index < end; index++)
			{
				AnimatorRecord& record = m_Records[index];
				record.Update = record.Removed ? RecordUpdate::None : ClassifyRecord(record, registry);
				if (record.Update == RecordUpdate::Sync)
					m_SyncQueue[queued.fetch_add(1, std::memory_order_relaxed)] = index;
				else if (record.Update == RecordUpdate::Step)
					StepRecord(record, deltaTime, t_Scratch);
			}
		});

		// The queued animators, in record order: synchronized with their components here, then stepped on the job system.
		const std::span<uint32_t> synced(m_SyncQueue.data(), queued.load());
		std::sort(synced.begin(), synced.end());
		m_Work.clear();
		for (const uint32_t index : synced)
		{
			AnimatorRecord& record = m_Records[index];
			const AnimatorComponent& component = registry.get<AnimatorComponent>(record.Handle);
			const WorldTransformComponent* world = registry.try_get<WorldTransformComponent>(record.Handle);
			if (!SyncRecord(record, component, world && world->ActiveInHierarchy))
			{
				record.Update = RecordUpdate::None;
				continue;
			}
			ResolveSet(m_Sets[record.Set]);
			ResolvePlayedClip(record, m_Sets[record.Set]);
			if (!IsReady(record))
			{
				record.Update = RecordUpdate::Waiting;
				continue;
			}
			PrepareStep(record, component);
			record.Update = RecordUpdate::Step;
			m_Work.push_back(index);
		}
		JobSystem::ParallelFor(static_cast<uint32_t>(m_Work.size()), c_EvaluationBatchSize, [this, deltaTime](uint32_t begin, uint32_t end)
		{
			ST_PROFILE_SCOPE("AnimationSystem::Evaluate");
			thread_local std::vector<JointPose> t_Scratch;
			for (uint32_t item = begin; item < end; item++)
				StepRecord(m_Records[m_Work[item]], deltaTime, t_Scratch);
		});

		// The results, animator by animator in record order.
		bool posesChanged = false;
		for (uint32_t index = 0; index < m_Records.size(); index++)
		{
			AnimatorRecord& record = m_Records[index];
			if (record.Removed || record.Update == RecordUpdate::None)
				continue;
			stats.ActiveAnimators++;
			if (record.Update != RecordUpdate::Step)
			{
				stats.Waiting++;
				continue;
			}
			stats.Advanced++;
			if (record.Evaluated)
			{
				record.PoseVersion = m_NextPoseVersion++;
				posesChanged = true;
				stats.Evaluated++;
			}
			else
			{
				if (!record.Evaluate)
					stats.Culled++;
				// The pose stays, where its clips take it changed (a transition while unseen).
				if (record.ReachChanged && record.PoseVersion != 0)
				{
					record.PoseVersion = m_NextPoseVersion++;
					posesChanged = true;
				}
			}
			if (!record.Events.empty())
			{
				// Names are copied: listeners may change or remove the animator.
				const AnimationController* controller = m_Sets[record.Set].Controller.get();
				const UUID id = registry.get<IDComponent>(record.Handle).ID;
				for (const AnimatorFiredEvent& fired : record.Events)
				{
					if (!controller || fired.State >= controller->GetStates().size() || fired.Event >= controller->GetStates()[fired.State].Events.size())
						continue;
					const AnimationState& state = controller->GetStates()[fired.State];
					m_PendingEvents.push_back(PendingEvent { record.Handle, id, state.Events[fired.Event].Name, state.Name, fired.Time });
				}
			}
			// Last: the transform's signal reaches listeners, which must not find the record half processed.
			if (record.Settings.ApplyRootMotion)
				ApplyRootMotion(Entity(record.Handle, &m_Scene), record.Instance.GetRootMotion());
		}
		if (posesChanged)
			m_PosesVersion++;

		PlaceAttachments();
		// Animators that listeners of the transforms written above removed.
		CompactRecords();
		DispatchEvents();
	}

	AnimationSystem::RecordUpdate AnimationSystem::ClassifyRecord(AnimatorRecord& record, const entt::registry& registry) const
	{
		const AnimatorComponent& component = registry.get<AnimatorComponent>(record.Handle);
		const WorldTransformComponent* world = registry.try_get<WorldTransformComponent>(record.Handle);
		if (!(world && world->ActiveInHierarchy))
			return record.Active ? RecordUpdate::Sync : RecordUpdate::None;
		if (!record.Active || record.Set == c_NoSet)
			return RecordUpdate::Sync;
		const AssetSet& set = m_Sets[record.Set];
		if (set.SkeletonHandle != component.Skeleton || set.ControllerHandle != component.Controller || set.ClipHandle != component.Clip)
			return RecordUpdate::Sync;
		// A clip played directly that is new, still loading or bound to another version of the skeleton.
		const AssetHandle played = record.Instance.GetPlayedClip();
		if (played != record.PlayedClipHandle || (played.IsValid() && (record.PlayedClipLoading || !record.PlayedClip || record.PlayedBindingSkeletonId != set.SkeletonId)))
			return RecordUpdate::Sync;
		if (!IsReady(record))
			return RecordUpdate::Waiting;
		PrepareStep(record, component);
		return RecordUpdate::Step;
	}

	void AnimationSystem::PrepareStep(AnimatorRecord& record, const AnimatorComponent& component) const
	{
		record.Settings.Speed = std::isfinite(component.Speed) ? std::clamp(component.Speed, -c_MaxAnimatorSpeed, c_MaxAnimatorSpeed) : 0.0f;
		record.Settings.Loop = component.Loop;
		record.Settings.ApplyRootMotion = component.ApplyRootMotion;
		// Unseen WhenVisible animators skip their poses, except for a first one (renderers then have something to draw until
		// they report it). One that skins no mesh but places attachments (a model of rigid parts) has no mesh a renderer could
		// see it by: it poses every update, or its parts would stand still.
		record.MayCull = component.CullingMode == AnimatorCullingMode::WhenVisible;
		const bool seen = record.SeenUpdate != 0 && record.SeenUpdate + 1 >= m_UpdateIndex;
		const bool rigidParts = record.AttachedUpdate != 0 && record.AttachedUpdate + 1 >= m_UpdateIndex && !HasSkinUsers(record.ID);
		record.Evaluate = !record.MayCull || seen || rigidParts || record.PoseVersion == 0;
		record.Evaluated = false;
		record.ReachChanged = false;
		record.Events.clear();
	}

	void AnimationSystem::StepRecord(AnimatorRecord& record, float deltaTime, std::vector<JointPose>& scratch) const
	{
		const AnimatorAssets assets = MakeAssets(record);
		if (!record.Instance.Advance(deltaTime, assets, record.Settings, record.Events))
			return;
		if (record.Evaluate)
			record.Evaluated = record.Instance.Evaluate(assets, record.Settings, scratch, record.Pose);
		// An animator that may skip its poses publishes where its clips take the joints, so renderers find its meshes there
		// whatever pose they show (a change publishes the pose again).
		const AnimatorReach reach = record.MayCull ? record.Instance.GetReach(assets, record.Settings) : AnimatorReach();
		if (std::memcmp(&reach.Bounds, &record.Reach, sizeof(AABB)) != 0 || reach.Scale != record.ReachScale)
		{
			record.Reach = reach.Bounds;
			record.ReachScale = reach.Scale;
			record.ReachChanged = true;
		}
	}

	void AnimationSystem::ApplyRootMotion(Entity animator, const RootMotionDelta& delta)
	{
		if (!animator.IsValid() || animator.GetScene() != &m_Scene || delta.IsZero())
			return;
		// The move is in the entity's space: its world transform takes it to the world (scale included, so a model scaled up
		// travels as far as its feet do), the parent's inverse into the space of the local translation.
		const glm::mat4 world = m_Scene.GetWorldTransform(animator);
		glm::vec3 move = glm::vec3(world * glm::vec4(delta.Translation, 0.0f));
		if (const Entity parent = animator.GetParent())
		{
			const glm::mat4 parentWorld = m_Scene.GetWorldTransform(parent);
			if (!(std::abs(glm::determinant(glm::mat3(parentWorld))) > Scene::c_MinInvertibleDeterminant))
				return;
			move = glm::vec3(glm::inverse(parentWorld) * glm::vec4(move, 0.0f));
		}
		TransformComponent& transform = animator.GetTransform();
		const glm::vec3 translation = transform.Translation + move;
		const glm::quat rotation = glm::normalize(transform.Rotation * glm::angleAxis(delta.Yaw, glm::vec3(0.0f, 1.0f, 0.0f)));
		if (!std::isfinite(translation.x) || !std::isfinite(translation.y) || !std::isfinite(translation.z) || !std::isfinite(rotation.w))
			return;
		transform.Translation = translation;
		transform.Rotation = rotation;
		animator.MarkModified<TransformComponent>();
	}

	void AnimationSystem::PlaceAttachments()
	{
		entt::registry& registry = m_Scene.GetRegistry();
		for (const auto [handle, attachment] : registry.view<AttachToJointComponent>().each())
		{
			Entity entity(handle, &m_Scene);
			if (!m_Scene.IsActiveInHierarchy(entity))
				continue;
			const Entity animator = m_Scene.GetEntityByUUID(attachment.Animator);
			if (!animator || animator == entity)
				continue;
			AnimatorRecord* record = FindRecord(animator.GetHandle());
			if (!record)
				continue;
			record->AttachedUpdate = m_UpdateIndex; // It places parts: posed every update even when WhenVisible (PrepareStep)
			if (!record->Active || record->PoseVersion == 0 || record->Set == c_NoSet)
				continue;
			const Skeleton* skeleton = m_Sets[record->Set].SkeletonAsset.get();
			if (!skeleton)
				continue;
			const JointIndex joint = skeleton->FindJoint(attachment.Joint);
			if (joint == c_NoJoint || joint >= record->Pose.size())
				continue;
			// An entity above its animator would move it along: the transforms would chase each other.
			if (m_Scene.IsDescendantOf(animator, entity))
				continue;
			// The local transform that puts the entity at the joint: the pose itself under the animator (the usual case, which
			// then does not change when the animator moves), else through the parent's world transform.
			glm::mat4 local = record->Pose[joint];
			const Entity parent = entity.GetParent();
			if (parent != animator)
			{
				const glm::mat4 parentWorld = parent ? m_Scene.GetWorldTransform(parent) : glm::mat4(1.0f);
				if (!(std::abs(glm::determinant(glm::mat3(parentWorld))) > Scene::c_MinInvertibleDeterminant))
					continue;
				local = glm::inverse(parentWorld) * m_Scene.GetWorldTransform(animator) * local;
			}
			TransformComponent& transform = entity.GetTransform();
			AttachmentState& state = m_Attachments[handle];
			// The same placement as last time, and nobody moved the entity since: nothing to write, no transform to update.
			if (state.Valid && SameMatrix(local, state.Local) && transform.Translation == state.Translation && transform.Rotation == state.Rotation
				&& transform.Scale == state.Scale)
				continue;
			glm::vec3 translation;
			glm::quat rotation;
			glm::vec3 scale;
			if (!Math::DecomposeTransform(local, translation, rotation, scale))
				continue;
			transform.Translation = translation;
			transform.Rotation = rotation;
			transform.Scale = scale;
			entity.MarkModified<TransformComponent>();
			state.Valid = true;
			state.Local = local;
			state.Translation = translation;
			state.Rotation = rotation;
			state.Scale = scale;
			m_Stats.Attachments++;
		}
	}

	void AnimationSystem::DispatchEvents()
	{
		if (m_PendingEvents.empty())
			return;
		ST_PROFILE_FUNCTION();
		const std::vector<PendingEvent> events = std::move(m_PendingEvents);
		m_PendingEvents.clear();
		m_Stats.EventsDispatched += events.size();
		// Listeners may add or remove listeners while being called; iterate over a snapshot.
		const std::vector<Ref<EventListener>> listeners = m_Listeners;
		for (const PendingEvent& event : events)
		{
			const Entity animator(event.Animator, &m_Scene);
			if (!animator.IsValid())
				continue;
			AnimationEventInfo info;
			info.Animator = animator;
			info.AnimatorID = event.AnimatorID;
			info.Name = event.Name;
			info.State = event.State;
			info.Time = event.Time;
			for (const Ref<EventListener>& listener : listeners)
			{
				if (!listener->Removed)
					listener->Callback(info);
			}
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// ISkinningPoses
	////////////////////////////////////////////////////////////////////////////////

	bool AnimationSystem::GetPose(entt::entity animator, SkeletonPose& outPose) const
	{
		const AnimatorRecord* record = FindRecord(animator);
		if (!record || !record->Active || record->PoseVersion == 0)
			return false;
		outPose.Joints = record->Pose;
		outPose.Version = record->PoseVersion;
		outPose.Reach = record->Reach;
		outPose.ReachScale = record->ReachScale;
		return true;
	}

	void AnimationSystem::ReportVisible(entt::entity animator)
	{
		if (AnimatorRecord* record = FindRecord(animator))
			record->SeenUpdate = m_UpdateIndex;
	}

	////////////////////////////////////////////////////////////////////////////////
	// IAnimatorControl
	////////////////////////////////////////////////////////////////////////////////

	bool AnimationSystem::Play(Entity animator, std::string_view state)
	{
		AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		return record && record->Instance.Play(CommandAssets(*record), state);
	}

	bool AnimationSystem::PlayClip(Entity animator, AssetHandle clip)
	{
		AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		if (!record || !clip.IsValid() || AssetManager::GetAssetType(clip) != AssetType::AnimationClip)
			return false;
		record->Instance.PlayClip(clip);
		return true;
	}

	bool AnimationSystem::CrossFade(Entity animator, std::string_view state, float seconds)
	{
		AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		return record && record->Instance.CrossFade(CommandAssets(*record), state, seconds);
	}

	bool AnimationSystem::SetFloat(Entity animator, std::string_view parameter, float value)
	{
		AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		return record && record->Instance.SetFloat(CommandAssets(*record), parameter, value);
	}

	bool AnimationSystem::SetBool(Entity animator, std::string_view parameter, bool value)
	{
		AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		return record && record->Instance.SetBool(CommandAssets(*record), parameter, value);
	}

	bool AnimationSystem::SetTrigger(Entity animator, std::string_view parameter)
	{
		AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		return record && record->Instance.SetTrigger(CommandAssets(*record), parameter, true);
	}

	bool AnimationSystem::ResetTrigger(Entity animator, std::string_view parameter)
	{
		AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		return record && record->Instance.SetTrigger(CommandAssets(*record), parameter, false);
	}

	bool AnimationSystem::SetSpeed(Entity animator, float speed)
	{
		AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		if (!record || !std::isfinite(speed) || std::abs(speed) > c_MaxAnimatorSpeed)
			return false;
		animator.GetComponent<AnimatorComponent>().Speed = speed;
		record->Settings.Speed = speed;
		return true;
	}

	std::optional<std::string> AnimationSystem::GetCurrentState(Entity animator)
	{
		const AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		if (!record)
			return std::nullopt;
		// A state played while the controller loads is the one the animator is in, as far as callers can tell.
		if (record->Instance.GetPendingState())
			return *record->Instance.GetPendingState();
		const uint32_t state = record->Instance.GetCurrentState();
		const AnimationController* controller = record->Set != c_NoSet ? m_Sets[record->Set].Controller.get() : nullptr;
		if (state == AnimatorInstance::c_ClipState || !controller || state >= controller->GetStates().size())
			return std::string();
		return controller->GetStates()[state].Name;
	}

	float AnimationSystem::GetNormalizedTime(Entity animator)
	{
		const AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		return record ? static_cast<float>(record->Instance.GetNormalizedTime()) : 0.0f;
	}

	bool AnimationSystem::IsInTransition(Entity animator)
	{
		const AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		return record && record->Instance.IsInTransition();
	}

	std::optional<glm::mat4> AnimationSystem::GetJointTransform(Entity animator, std::string_view joint)
	{
		const AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		if (!record || !record->Active || record->PoseVersion == 0 || record->Set == c_NoSet)
			return std::nullopt;
		const Skeleton* skeleton = m_Sets[record->Set].SkeletonAsset.get();
		if (!skeleton)
			return std::nullopt;
		const JointIndex index = skeleton->FindJoint(joint);
		if (index == c_NoJoint || index >= record->Pose.size())
			return std::nullopt;
		return m_Scene.GetWorldTransform(animator) * record->Pose[index];
	}

	////////////////////////////////////////////////////////////////////////////////
	// IAnimationEventSource
	////////////////////////////////////////////////////////////////////////////////

	AnimationEventListenerID AnimationSystem::AddAnimationEventListener(AnimationEventCallback callback)
	{
		if (!callback)
			return 0;
		Ref<EventListener> listener = CreateRef<EventListener>();
		listener->ID = m_NextListenerID++;
		listener->Callback = std::move(callback);
		m_Listeners.push_back(listener);
		return listener->ID;
	}

	bool AnimationSystem::RemoveAnimationEventListener(AnimationEventListenerID id)
	{
		auto it = std::find_if(m_Listeners.begin(), m_Listeners.end(), [id](const Ref<EventListener>& listener) { return listener->ID == id; });
		if (it == m_Listeners.end())
			return false;
		// A dispatch in progress holds its own references; the flag stops it from calling this listener again.
		(*it)->Removed = true;
		m_Listeners.erase(it);
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Inspection
	////////////////////////////////////////////////////////////////////////////////

	const AnimatorInstance* AnimationSystem::FindInstance(Entity animator) const
	{
		const AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		return record && record->Active ? &record->Instance : nullptr;
	}

	std::vector<AnimatorParameterValue> AnimationSystem::GetParameters(Entity animator) const
	{
		std::vector<AnimatorParameterValue> parameters;
		const AnimatorRecord* record = animator.GetScene() == &m_Scene ? FindRecord(animator.GetHandle()) : nullptr;
		if (!record || record->Set == c_NoSet || !m_Sets[record->Set].Controller)
			return parameters;
		// Before the animator bound the controller (it has not played yet) its parameters have their defaults.
		const std::vector<AnimationParameter>& declared = m_Sets[record->Set].Controller->GetParameters();
		const std::span<const float> values = record->Instance.GetParameterValues();
		for (size_t index = 0; index < declared.size(); index++)
		{
			const AnimationParameter& parameter = declared[index];
			float value = parameter.Type == AnimationParameterType::Float ? parameter.DefaultFloat : (parameter.Type == AnimationParameterType::Bool && parameter.DefaultBool ? 1.0f : 0.0f);
			if (values.size() == declared.size())
				value = values[index];
			parameters.push_back(AnimatorParameterValue { parameter.Name, parameter.Type, value });
		}
		return parameters;
	}

	AnimationSystemStats AnimationSystem::GetStats() const
	{
		AnimationSystemStats stats = m_Stats;
		stats.Animators = static_cast<uint32_t>(m_RecordIndices.size());
		stats.AssetSets = static_cast<uint32_t>(m_SetIndices.size());
		return stats;
	}

}
