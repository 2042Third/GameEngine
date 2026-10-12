#include "stpch.h"
#include "Strata/Animation/AnimationPreview.h"

#include "Strata/Animation/AnimationClip.h"
#include "Strata/Animation/AnimationController.h"
#include "Strata/Animation/Skeleton.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <cmath>

namespace Strata
{

	namespace
	{

		enum class Arrival : uint8_t
		{
			Ready,
			Loading,
			Failed
		};

		// Requests an asset of a type: here, on its way, or not coming (failed, unknown, another type).
		Arrival RequestAsset(AssetManagerBase& manager, AssetHandle handle, AssetType type, Ref<Asset>& outAsset)
		{
			outAsset = manager.GetAsset(handle);
			if (outAsset)
				return outAsset->GetType() == type ? Arrival::Ready : Arrival::Failed;
			return manager.GetAssetState(handle) == AssetState::Loading ? Arrival::Loading : Arrival::Failed;
		}

	}

	AnimationPreview::AnimationPreview(Scene& scene)
		: m_Scene(scene)
	{
		m_Registered = m_Scene.RegisterService<ISkinningPoses>(*this);
	}

	AnimationPreview::~AnimationPreview()
	{
		if (m_Registered)
			m_Scene.UnregisterService<ISkinningPoses>(*this);
	}

	bool AnimationPreview::Set(Entity animator, const Request& request, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		if (!animator.IsValid() || animator.GetScene() != &m_Scene || !animator.HasComponent<AnimatorComponent>())
			return fail("the entity has no Animator component");
		if (!std::isfinite(request.Time) || request.Time < 0.0f)
			return fail("the time must be finite and not negative");
		Entry& entry = m_Entries[animator.GetHandle()];
		entry.Asked = request;
		entry.Dirty = true;
		// Posed only once this request's pose is computed; an earlier pose keeps showing meanwhile (Version and Pose stay).
		entry.State = Status::Waiting;
		entry.Error.clear();
		return true;
	}

	bool AnimationPreview::Clear(Entity animator)
	{
		auto it = m_Entries.find(animator.GetHandle());
		if (it == m_Entries.end())
			return false;
		if (it->second.Version != 0)
			m_PosesVersion++;
		m_Entries.erase(it);
		return true;
	}

	void AnimationPreview::ClearAll()
	{
		if (!m_Entries.empty())
			m_PosesVersion++;
		m_Entries.clear();
	}

	void AnimationPreview::Fail(Entry& entry, std::string error)
	{
		if (entry.Version != 0)
		{
			entry.Version = 0;
			m_PosesVersion++;
		}
		entry.State = Status::Failed;
		entry.Error = std::move(error);
		entry.Dirty = false;
	}

	void AnimationPreview::Update()
	{
		for (auto& [handle, entry] : m_Entries)
			UpdateEntry(handle, entry);
	}

	void AnimationPreview::UpdateEntry(entt::entity handle, Entry& entry)
	{
		const Entity entity(handle, &m_Scene);
		const AnimatorComponent* component = entity.IsValid() ? entity.TryGetComponent<AnimatorComponent>() : nullptr;
		if (!component)
		{
			Fail(entry, "the entity has no Animator component");
			return;
		}
		// What the pose depends on in the component.
		if (component->Skeleton != entry.Skeleton || component->Controller != entry.Controller || component->Clip != entry.ComponentClip
			|| component->Loop != entry.Loop || component->ApplyRootMotion != entry.RootMotion)
		{
			entry.Skeleton = component->Skeleton;
			entry.Controller = component->Controller;
			entry.ComponentClip = component->Clip;
			entry.Loop = component->Loop;
			entry.RootMotion = component->ApplyRootMotion;
			entry.Dirty = true;
		}
		if (entry.State == Status::Failed && !entry.Dirty)
			return;

		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		if (!manager)
		{
			Fail(entry, "no asset manager is active");
			return;
		}
		if (!component->Skeleton.IsValid())
		{
			Fail(entry, "the Animator has no skeleton");
			return;
		}
		bool loading = false;
		Ref<Asset> skeletonAsset;
		switch (RequestAsset(*manager, component->Skeleton, AssetType::Skeleton, skeletonAsset))
		{
			case Arrival::Ready: break;
			case Arrival::Loading: loading = true; break;
			case Arrival::Failed: Fail(entry, "the Animator's skeleton did not load"); return;
		}
		// Without its controller (none, or one that did not load) the animator plays its clip, like in play mode.
		Ref<Asset> controllerAsset;
		if (component->Controller.IsValid() && RequestAsset(*manager, component->Controller, AssetType::AnimationController, controllerAsset) == Arrival::Loading)
			loading = true;
		if (loading)
		{
			// The pose computed before (if any) shows until the assets arrive; it is not the one asked for yet.
			entry.State = Status::Waiting;
			return;
		}
		const Ref<Skeleton> skeleton = std::static_pointer_cast<Skeleton>(skeletonAsset);
		const Ref<AnimationController> controller = controllerAsset ? std::static_pointer_cast<AnimationController>(controllerAsset) : nullptr;

		// The clip the request shows: a named state's, a clip, or what the animator plays on start.
		uint32_t state = AnimatorInstance::c_ClipState;
		AssetHandle clipHandle = UUID::Null();
		if (!entry.Asked.State.empty())
		{
			if (!controller)
			{
				Fail(entry, fmt::format("the Animator has no controller with a state '{}'", entry.Asked.State));
				return;
			}
			state = controller->FindState(entry.Asked.State);
			if (state == ~0u)
			{
				Fail(entry, fmt::format("the controller has no state '{}'", entry.Asked.State));
				return;
			}
			clipHandle = controller->GetStates()[state].Clip;
		}
		else if (entry.Asked.Clip.IsValid())
		{
			clipHandle = entry.Asked.Clip;
		}
		else if (controller)
		{
			state = controller->GetDefaultState();
			clipHandle = controller->GetStates()[state].Clip;
		}
		else
		{
			clipHandle = component->Clip;
		}
		Ref<Asset> clipAsset;
		if (clipHandle.IsValid())
		{
			switch (RequestAsset(*manager, clipHandle, AssetType::AnimationClip, clipAsset))
			{
				case Arrival::Ready: break;
				case Arrival::Loading: entry.State = Status::Waiting; return;
				case Arrival::Failed:
					// A clip asked for by name must exist; a state's clip that did not load shows the rest pose, as in play.
					if (entry.Asked.State.empty() && entry.Asked.Clip.IsValid())
					{
						Fail(entry, fmt::format("asset {} is not an animation clip that loads", clipHandle.ToString()));
						return;
					}
					break;
			}
		}

		// Nothing changed since the pose was computed: no work.
		const std::vector<std::weak_ptr<const Asset>> objects = { skeletonAsset, controllerAsset, clipAsset };
		bool sameObjects = entry.Objects.size() == objects.size();
		for (size_t index = 0; sameObjects && index < objects.size(); index++)
			sameObjects = !entry.Objects[index].owner_before(objects[index]) && !objects[index].owner_before(entry.Objects[index]);
		if (!entry.Dirty && sameObjects && entry.State == Status::Posed)
			return;

		const Ref<AnimationClip> clip = clipAsset ? std::static_pointer_cast<AnimationClip>(clipAsset) : nullptr;
		const RootMotionSetup rootMotion = RootMotionSetup::Create(*skeleton);
		const AnimationClipBinding binding = clip ? clip->Bind(*skeleton) : AnimationClipBinding();
		const AnimatorClip bound = clip ? AnimatorClip { clip.get(), &binding } : AnimatorClip();
		std::vector<AnimatorClip> stateClips(controller ? controller->GetStates().size() : 1);
		AnimatorAssets assets;
		assets.SkeletonAsset = skeleton.get();
		assets.RootMotion = &rootMotion;
		assets.Controller = controller.get();
		assets.ControllerId = controller ? 1 : 0;

		AnimatorInstance instance;
		if (state != AnimatorInstance::c_ClipState)
		{
			stateClips[state] = bound;
			assets.StateClips = stateClips;
			instance.Play(assets, controller->GetStates()[state].Name);
		}
		else if (entry.Asked.Clip.IsValid())
		{
			assets.StateClips = stateClips;
			assets.PlayedClip = bound;
			instance.PlayClip(entry.Asked.Clip);
		}
		else
		{
			stateClips[0] = bound;
			assets.StateClips = stateClips;
			instance.Start();
		}
		AnimatorSettings settings;
		settings.Loop = component->Loop;
		settings.ApplyRootMotion = component->ApplyRootMotion;
		settings.Transitions = false; // The preview holds what it shows
		std::vector<AnimatorFiredEvent> events;
		if (!instance.Advance(entry.Asked.Time, assets, settings, events) || !instance.Evaluate(assets, settings, m_Scratch, entry.Pose))
		{
			Fail(entry, "the pose could not be computed");
			return;
		}
		entry.Objects = objects;
		entry.Version = m_NextVersion++;
		entry.State = Status::Posed;
		entry.Dirty = false;
		entry.Error.clear();
		m_PosesVersion++;
	}

	AnimationPreview::Status AnimationPreview::GetStatus(Entity animator, std::string* outError) const
	{
		auto it = m_Entries.find(animator.GetHandle());
		if (it == m_Entries.end())
			return Status::None;
		if (outError)
			*outError = it->second.Error;
		return it->second.State;
	}

	bool AnimationPreview::GetPose(entt::entity animator, SkeletonPose& outPose) const
	{
		auto it = m_Entries.find(animator);
		if (it == m_Entries.end() || it->second.Version == 0)
			return false;
		outPose.Joints = it->second.Pose;
		outPose.Version = it->second.Version;
		return true;
	}

}
