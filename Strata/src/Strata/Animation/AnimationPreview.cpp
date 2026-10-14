#include "stpch.h"
#include "Strata/Animation/AnimationPreview.h"

#include "Strata/Animation/AnimationClip.h"
#include "Strata/Animation/AnimationController.h"
#include "Strata/Animation/RestPose.h"
#include "Strata/Animation/Skeleton.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <cmath>
#include <algorithm>
#include <cstring>

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

	AnimationPreview::AnimationPreview(Scene& scene, const AnimationPreviewSpecification& specification)
		: m_Scene(scene), m_Specification(specification)
	{
		m_Registered = m_Scene.RegisterService<ISkinningPoses>(*this);
		entt::registry& registry = m_Scene.GetRegistry();
		m_Connections.push_back(registry.on_construct<AttachToJointComponent>().connect<&AnimationPreview::OnAttachmentsChanged>(*this));
		m_Connections.push_back(registry.on_update<AttachToJointComponent>().connect<&AnimationPreview::OnAttachmentsChanged>(*this));
		m_Connections.push_back(registry.on_destroy<AttachToJointComponent>().connect<&AnimationPreview::OnAttachmentsChanged>(*this));
		m_Connections.push_back(registry.on_construct<MeshRendererComponent>().connect<&AnimationPreview::OnAttachmentsChanged>(*this));
		m_Connections.push_back(registry.on_destroy<MeshRendererComponent>().connect<&AnimationPreview::OnAttachmentsChanged>(*this));
		if (!m_Specification.RestPoses)
			return;
		m_Connections.push_back(registry.on_construct<AnimatorComponent>().connect<&AnimationPreview::OnAnimatorChanged>(*this));
		m_Connections.push_back(registry.on_update<AnimatorComponent>().connect<&AnimationPreview::OnAnimatorChanged>(*this));
		m_Connections.push_back(registry.on_destroy<AnimatorComponent>().connect<&AnimationPreview::OnAnimatorChanged>(*this));
		// The animators the scene has already; later ones arrive through the signals.
		for (const entt::entity handle : registry.view<AnimatorComponent>())
			m_DirtyAnimators.push_back(handle);
	}

	AnimationPreview::~AnimationPreview()
	{
		for (entt::connection& connection : m_Connections)
			connection.release();
		if (m_Registered)
			m_Scene.UnregisterService<ISkinningPoses>(*this);
	}

	void AnimationPreview::OnAnimatorChanged(entt::registry&, entt::entity entity)
	{
		m_DirtyAnimators.push_back(entity);
	}

	void AnimationPreview::OnAttachmentsChanged(entt::registry&, entt::entity)
	{
		m_AttachmentsRevision++;
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
		m_RequestsRevision++;
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
		m_RequestsRevision++;
		UpdateRenderTransforms();
		return true;
	}

	void AnimationPreview::ClearAll()
	{
		if (!m_Entries.empty())
		{
			m_PosesVersion++;
			m_RequestsRevision++;
		}
		m_Entries.clear();
		UpdateRenderTransforms();
	}

	void AnimationPreview::Fail(Entry& entry, std::string error, bool requestInvalid)
	{
		if (entry.Version != 0)
		{
			entry.Version = 0;
			m_PosesVersion++;
		}
		entry.State = Status::Failed;
		entry.Error = std::move(error);
		entry.RequestInvalid = requestInvalid;
		entry.Dirty = false;
	}

	std::vector<std::pair<entt::entity, AnimationPreview::Request>> AnimationPreview::GetRequests() const
	{
		std::vector<std::pair<entt::entity, Request>> requests;
		requests.reserve(m_Entries.size());
		for (const auto& [handle, entry] : m_Entries)
			requests.emplace_back(handle, entry.Asked);
		// In a fixed order: entity handles, not the map's.
		std::sort(requests.begin(), requests.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		return requests;
	}

	std::string AnimationPreview::DescribeStates(const AnimationController& controller)
	{
		std::string names;
		for (const AnimationState& state : controller.GetStates())
		{
			if (!names.empty())
				names += ", ";
			names += state.Name;
		}
		return names.empty() ? std::string("none") : names;
	}

	void AnimationPreview::Update()
	{
		AssetManagerBase* manager = m_Scene.GetAssetManager();
		if (m_Specification.RestPoses && manager)
			UpdateRestPoses(*manager);
		for (auto& [handle, entry] : m_Entries)
			UpdateEntry(manager, handle, entry);
		UpdateRenderTransforms();
	}

	void AnimationPreview::SetRestPoseState(RestPose& pose, RestPose::State state)
	{
		if (pose.Status == RestPose::State::Waiting && state != RestPose::State::Waiting)
			m_WaitingRestPoses--;
		else if (pose.Status != RestPose::State::Waiting && state == RestPose::State::Waiting)
			m_WaitingRestPoses++;
		pose.Status = state;
	}

	void AnimationPreview::ReleaseRestPose(AssetHandle skeleton)
	{
		auto it = m_RestPoses.find(skeleton);
		if (it == m_RestPoses.end() || --it->second.Animators > 0)
			return;
		SetRestPoseState(it->second, RestPose::State::Ready); // Counts as waiting no more
		m_RestPoses.erase(it);
	}

	void AnimationPreview::ExamineAnimator(entt::entity handle)
	{
		m_Stats.AnimatorsExamined++;
		const entt::registry& registry = m_Scene.GetRegistry();
		const AnimatorComponent* component = registry.valid(handle) ? registry.try_get<AnimatorComponent>(handle) : nullptr;
		const AssetHandle skeleton = component ? component->Skeleton : AssetHandle(UUID::Null());
		auto it = m_RestAnimators.find(handle);
		const AssetHandle previous = it != m_RestAnimators.end() ? it->second : AssetHandle(UUID::Null());
		if (previous == skeleton)
			return;
		// What the animator shows changes (a request still wins over it).
		m_PosesVersion++;
		if (previous.IsValid())
		{
			m_RestAnimators.erase(it);
			ReleaseRestPose(previous);
		}
		if (!skeleton.IsValid())
			return;
		m_RestAnimators.emplace(handle, skeleton);
		auto [pose, added] = m_RestPoses.try_emplace(skeleton);
		pose->second.Animators++;
		if (added)
			m_WaitingRestPoses++; // A new entry starts out waiting for its skeleton
	}

	void AnimationPreview::UpdateRestPoses(AssetManagerBase& manager)
	{
		// Animators that were added, removed or changed since the last update; each once.
		if (!m_DirtyAnimators.empty())
		{
			std::sort(m_DirtyAnimators.begin(), m_DirtyAnimators.end());
			m_DirtyAnimators.erase(std::unique(m_DirtyAnimators.begin(), m_DirtyAnimators.end()), m_DirtyAnimators.end());
			for (const entt::entity handle : m_DirtyAnimators)
				ExamineAnimator(handle);
			m_DirtyAnimators.clear();
		}

		// Skeletons whose source changed (a reimport) are posed again; loads and evictions change nothing.
		const uint64_t contentVersion = manager.GetContentVersion();
		if (contentVersion != m_ContentVersion && !m_RestPoses.empty())
		{
			m_ContentChanges.clear();
			const bool known = manager.GetContentChanges(m_ContentVersion, m_ContentChanges);
			auto check = [this, &manager](AssetHandle handle, RestPose& pose)
			{
				if (pose.Status == RestPose::State::Waiting || manager.GetSourceRevisions(handle).Current == pose.SourceRevision)
					return;
				SetRestPoseState(pose, RestPose::State::Waiting);
			};
			if (known)
			{
				for (const AssetHandle handle : m_ContentChanges)
				{
					if (auto it = m_RestPoses.find(handle); it != m_RestPoses.end())
						check(handle, it->second);
				}
			}
			else
			{
				for (auto& [handle, pose] : m_RestPoses)
					check(handle, pose);
			}
		}
		m_ContentVersion = contentVersion;

		if (m_WaitingRestPoses == 0)
			return;
		for (auto& [handle, pose] : m_RestPoses)
		{
			if (pose.Status != RestPose::State::Waiting)
				continue;
			m_Stats.AssetRequests++;
			const Ref<Asset> asset = manager.GetAsset(handle);
			const AssetSourceRevisions revisions = manager.GetSourceRevisions(handle);
			if (!asset || revisions.Loaded != revisions.Current)
			{
				// Still loading, or the previous version is loaded while the reimported one loads.
				if (!asset && manager.GetAssetState(handle) != AssetState::Loading)
				{
					SetRestPoseState(pose, RestPose::State::Failed);
					if (pose.Version != 0)
					{
						pose.Model.clear();
						pose.Version = 0;
						m_PosesVersion++;
					}
				}
				continue;
			}
			if (asset->GetType() != AssetType::Skeleton)
			{
				SetRestPoseState(pose, RestPose::State::Failed);
				continue;
			}
			ComputeRestPose(static_cast<const Skeleton&>(*asset), pose.Model);
			pose.Version = m_NextVersion++;
			pose.SourceRevision = revisions.Loaded;
			SetRestPoseState(pose, RestPose::State::Ready);
			m_Stats.RestPosesComputed++;
			m_PosesVersion++;
		}
	}

	std::optional<glm::mat4> AnimationPreview::GetAttachmentWorld(Entity entity, const AttachToJointComponent& attachment) const
	{
		const Entity animator = m_Scene.GetEntityByUUID(attachment.Animator);
		// An entity above its animator would carry the animator along (as in play, it is not placed).
		if (!animator || animator == entity || m_Scene.IsDescendantOf(animator, entity))
			return std::nullopt;
		auto it = m_Entries.find(animator.GetHandle());
		if (it == m_Entries.end() || it->second.Version == 0 || it->second.Objects.empty())
			return std::nullopt;
		// The skeleton the pose was computed with (requested every update, so it is loaded while the pose shows).
		const std::shared_ptr<const Asset> skeleton = it->second.Objects[0].lock();
		if (!skeleton || skeleton->GetType() != AssetType::Skeleton)
			return std::nullopt;
		const JointIndex joint = static_cast<const Skeleton&>(*skeleton).FindJoint(attachment.Joint);
		if (joint == c_NoJoint || joint >= it->second.Pose.size())
			return std::nullopt;
		return m_Scene.GetWorldTransform(animator) * it->second.Pose[joint];
	}

	void AnimationPreview::UpdateRenderTransforms()
	{
		// Attachments are placed by requests only (the importer put them at the rest pose already), so nothing to do unless
		// the requests, their poses or what the attachments hang from changed.
		const RenderInputs inputs { m_PosesVersion, m_Scene.GetTransformsVersion(), m_Scene.GetHierarchyVersion(), m_RequestsRevision,
			m_AttachmentsRevision };
		if (inputs == m_RenderInputs)
			return;
		m_RenderInputs = inputs;
		std::vector<RenderTransform> transforms;
		bool posed = false;
		for (const auto& [handle, entry] : m_Entries)
			posed = posed || entry.Version != 0;
		if (posed)
		{
			const entt::registry& registry = m_Scene.GetRegistry();
			std::vector<Entity> stack;
			for (const auto [handle, attachment] : registry.view<AttachToJointComponent>().each())
			{
				const Entity entity(handle, &m_Scene);
				if (!m_Scene.IsActiveInHierarchy(entity))
					continue;
				const std::optional<glm::mat4> world = GetAttachmentWorld(entity, attachment);
				if (!world)
					continue;
				const glm::mat4 sceneWorld = m_Scene.GetWorldTransform(entity);
				if (!(std::abs(glm::determinant(glm::mat3(sceneWorld))) > Scene::c_MinInvertibleDeterminant))
					continue;
				// The attachment and what is under it move together: by the change from where the scene has the attachment to
				// its joint. An attachment below it that is placed itself starts its own subtree.
				const glm::mat4 change = *world * glm::inverse(sceneWorld);
				stack.assign(1, entity);
				while (!stack.empty())
				{
					const Entity current = stack.back();
					stack.pop_back();
					if (current != entity)
					{
						const AttachToJointComponent* inner = current.TryGetComponent<AttachToJointComponent>();
						if (inner && m_Scene.IsActiveInHierarchy(current) && GetAttachmentWorld(current, *inner))
							continue;
					}
					if (current.HasComponent<MeshRendererComponent>())
						transforms.push_back(RenderTransform { current.GetHandle(), current == entity ? *world : change * m_Scene.GetWorldTransform(current) });
					for (const Entity child : current.GetChildren())
						stack.push_back(child);
				}
			}
		}

		bool same = transforms.size() == m_RenderTransforms.size();
		for (size_t index = 0; same && index < transforms.size(); index++)
		{
			same = transforms[index].Entity == m_RenderTransforms[index].Entity
				&& std::memcmp(&transforms[index].World, &m_RenderTransforms[index].World, sizeof(glm::mat4)) == 0;
		}
		if (same)
			return;
		m_RenderTransforms = std::move(transforms);
		m_RenderTransformsVersion++;
	}

	void AnimationPreview::UpdateEntry(AssetManagerBase* manager, entt::entity handle, Entry& entry)
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
				Fail(entry, fmt::format("the Animator has no controller with a state '{}'", entry.Asked.State), true);
				return;
			}
			state = controller->FindState(entry.Asked.State);
			if (state == ~0u)
			{
				Fail(entry, fmt::format("the controller has no state '{}' (its states: {})", entry.Asked.State, DescribeStates(*controller)), true);
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

	AnimationPreview::Status AnimationPreview::GetStatus(Entity animator, std::string* outError, bool* outRequestInvalid) const
	{
		auto it = m_Entries.find(animator.GetHandle());
		if (it == m_Entries.end())
			return Status::None;
		if (outError)
			*outError = it->second.Error;
		if (outRequestInvalid)
			*outRequestInvalid = it->second.State == Status::Failed && it->second.RequestInvalid;
		return it->second.State;
	}

	bool AnimationPreview::GetPose(entt::entity animator, SkeletonPose& outPose) const
	{
		// A request's pose wins; without one (or before its first pose), the skeleton's rest pose.
		if (auto it = m_Entries.find(animator); it != m_Entries.end() && it->second.Version != 0)
		{
			outPose.Joints = it->second.Pose;
			outPose.Version = it->second.Version;
			return true;
		}
		const auto animatorIt = m_RestAnimators.find(animator);
		if (animatorIt == m_RestAnimators.end())
			return false;
		const auto pose = m_RestPoses.find(animatorIt->second);
		if (pose == m_RestPoses.end() || pose->second.Version == 0)
			return false;
		outPose.Joints = pose->second.Model;
		outPose.Version = pose->second.Version;
		return true;
	}

}
