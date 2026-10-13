#pragma once

#include "Strata/Animation/AnimationClip.h"
#include "Strata/Animation/AnimationController.h"
#include "Strata/Animation/AnimatorInstance.h"
#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SceneSystem.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Scene;
	class Skeleton;

	// A parameter of an animator's controller and its value (bools and triggers: 0 or 1).
	struct AnimatorParameterValue
	{
		std::string Name;
		AnimationParameterType Type = AnimationParameterType::Float;
		float Value = 0.0f;
	};

	struct AnimationSystemStats
	{
		uint32_t Animators = 0;        // AnimatorComponents the system follows
		uint32_t ActiveAnimators = 0;  // Of entities active in the hierarchy, at the latest update
		uint32_t Advanced = 0;         // Animators whose time advanced in the latest update
		uint32_t Evaluated = 0;        // Poses computed in the latest update
		uint32_t Culled = 0;           // WhenVisible animators the latest update advanced without computing their pose
		// Poses computed while the scene was paused, of animators a renderer reported with a pose older than their time
		// (OnPausedUpdate), since the scene started.
		uint64_t PausedEvaluations = 0;
		// Animators whose reach (published while WhenVisible) the latest update worked out: only those whose played states or
		// clips, clip assets, culling mode or root motion setting changed.
		uint32_t ReachUpdates = 0;
		uint32_t Waiting = 0;          // Active animators that waited for assets (skeleton, controller, clips) in the latest update
		uint32_t AssetSets = 0;        // Distinct skeleton, controller and clip combinations
		uint32_t Attachments = 0;      // AttachToJointComponents the latest update moved
		uint64_t EventsDispatched = 0; // Animation events since the scene started (each counted once, whatever the listeners)
	};

	// Plays the animators of a running scene: the built-in "Animation" scene system (Play mode only). It updates after the
	// scripts (which set parameters and play states in their OnUpdate) and before physics (root motion moves bodies and
	// characters before the step), in OnUpdate:
	//  1. Animators: every entity with an AnimatorComponent that is active in the hierarchy has an AnimatorInstance (created
	//     when the scene starts or the component is added; reset when the entity is deactivated, started again on
	//     activation with PlayOnStart). Animators with the same skeleton, controller and clip share an asset set: the assets
	//     are requested once per update for all of them, and clip bindings are made once per skeleton and clip.
	//  2. Evaluation, on the job system (JobSystem::ParallelFor over every animator; the jobs read the registry and the asset
	//     sets and write only their own animator's record): an animator whose activity, assets and played clip are as they
	//     were advances its time and state machine (AnimatorInstance::Advance) and computes its pose (Evaluate) right there;
	//     the others (activated, deactivated, given other assets or a clip to play) are brought up to date on the main
	//     thread and then stepped on the job system too. A WhenVisible animator computes its pose only when a renderer drew
	//     a mesh it skins since the previous update (ReportVisible) or when it has none yet; time, transitions, events and
	//     root motion go on while it is unseen. Results do not depend on the number of threads.
	//  3. On the main thread, animator by animator: root motion moves the entity (ApplyRootMotion), the poses' versions
	//     change, attachments (AttachToJointComponent) are placed at their joints, and the events the playheads passed are
	//     dispatched to the listeners (IAnimationEventSource; the scripts' OnAnimationEvent), each animator's in order.
	// Poses are model-space joint transforms (relative to the animator's entity) that renderers read through
	// ISkinningPoses; joints are not entities, so animating adds no transform changes to the scene except root motion and
	// attachments. The system offers ISkinningPoses, IAnimatorControl and IAnimationEventSource while it exists. Main thread
	// only.
	class AnimationSystem final : public SceneSystem, public ISkinningPoses, public IAnimatorControl, public IAnimationEventSource
	{
	public:
		// Animators per job of the evaluation.
		static constexpr uint32_t c_EvaluationBatchSize = 8;

		explicit AnimationSystem(Scene& scene);
		~AnimationSystem() override;

		AnimationSystem(const AnimationSystem&) = delete;
		AnimationSystem& operator=(const AnimationSystem&) = delete;

		// Creates the animators of the scene and starts those with PlayOnStart, before any script's OnCreate (which may
		// play states).
		void OnRuntimeStart() override;
		void OnRuntimeStop() override;
		void OnUpdate(Timestep timestep) override;
		// While the scene is paused no time passes, but an animator that skipped its poses while unseen still shows an older
		// one: those a renderer reported since are posed at their time, so the paused frame shows what the game is at.
		void OnPausedUpdate() override;

		//////////////////////////////////////////////////////////////////////////
		// ISkinningPoses
		//////////////////////////////////////////////////////////////////////////

		bool GetPose(entt::entity animator, SkeletonPose& outPose) const override;
		uint64_t GetPosesVersion() const override { return m_PosesVersion; }
		bool ReportVisible(entt::entity animator) override;

		//////////////////////////////////////////////////////////////////////////
		// IAnimatorControl (see Scene/SceneServices.h)
		//////////////////////////////////////////////////////////////////////////

		bool Play(Entity animator, std::string_view state) override;
		bool PlayClip(Entity animator, AssetHandle clip) override;
		bool CrossFade(Entity animator, std::string_view state, float seconds) override;
		bool SetFloat(Entity animator, std::string_view parameter, float value) override;
		bool SetBool(Entity animator, std::string_view parameter, bool value) override;
		bool SetTrigger(Entity animator, std::string_view parameter) override;
		bool ResetTrigger(Entity animator, std::string_view parameter) override;
		bool SetSpeed(Entity animator, float speed) override;
		std::optional<std::string> GetCurrentState(Entity animator) override;
		float GetNormalizedTime(Entity animator) override;
		bool IsInTransition(Entity animator) override;
		// The state a cross-fade under way comes from (its name; empty for a clip played without one), or nothing while no
		// cross-fade runs: the Inspector shows "<from> -> <to>".
		std::optional<std::string> GetBlendSource(Entity animator) const;
		std::optional<glm::mat4> GetJointTransform(Entity animator, std::string_view joint) override;

		//////////////////////////////////////////////////////////////////////////
		// IAnimationEventSource
		//////////////////////////////////////////////////////////////////////////

		AnimationEventListenerID AddAnimationEventListener(AnimationEventCallback callback) override;
		bool RemoveAnimationEventListener(AnimationEventListenerID id) override;

		// Moves an animator's entity by the root motion of an update (AnimatorComponent::ApplyRootMotion): the move, given in
		// the entity's own space, through its world transform (rotation and scale) into its parent's space, and the turn about
		// its own +Y axis, signaled through the transform contract. This is the one place root motion moves entities. An entity
		// with a character controller (ICharacterControl::AddCharacterMotion accepts it: an enabled, active controller) walks
		// the move in world space at its next fixed step instead, colliding as it walks, and only the turn is written.
		void ApplyRootMotion(Entity animator, const RootMotionDelta& delta);

		//////////////////////////////////////////////////////////////////////////
		// Inspection (editor, tests)
		//////////////////////////////////////////////////////////////////////////

		// The playback of an animator (null for entities without an AnimatorComponent, or inactive ones).
		const AnimatorInstance* FindInstance(Entity animator) const;
		// The parameters of the animator's controller with their values; empty without a controller.
		std::vector<AnimatorParameterValue> GetParameters(Entity animator) const;
		AnimationSystemStats GetStats() const;
	private:
		static constexpr uint32_t c_NoSet = ~0u;

		// The assets animators with the same skeleton, controller and clip share. Held while animators use them (they request
		// them every update anyway), so pointers stay valid between updates for commands and queries.
		struct AssetSet
		{
			AssetHandle SkeletonHandle = UUID::Null();
			AssetHandle ControllerHandle = UUID::Null();
			AssetHandle ClipHandle = UUID::Null();
			uint32_t Users = 0;
			uint64_t ResolvedUpdate = ~0ull; // The update the assets were last requested in (none yet)

			Ref<Skeleton> SkeletonAsset;
			uint64_t SkeletonId = 0;     // Changes with the skeleton object (bindings follow it)
			RootMotionSetup RootMotion;  // Of SkeletonAsset
			Ref<AnimationController> Controller;
			uint64_t ControllerId = 0;   // AnimatorAssets::ControllerId
			bool ControllerFailed = false; // A controller is set but will not load: the animator plays Clip
			// Per controller state (or the one clip of the simple mode): the clip and its binding to the skeleton.
			std::vector<AssetHandle> ClipHandles;
			std::vector<Ref<AnimationClip>> Clips;
			std::vector<AnimationClipBinding> Bindings;
			std::vector<AnimationReach> Reaches; // With the bindings
			std::vector<AnimatorClip> StateClips;
			// Changes whenever StateClips may name other clips or reaches (assets that arrived, went or were bound again).
			uint32_t ReachVersion = 0;
			bool ClipsLoading = false;
			bool Reported = false; // A failed asset was reported (once per set)
		};

		// What an update does with an animator.
		enum class RecordUpdate : uint8_t
		{
			None = 0, // Inactive (or removed): nothing
			Waiting,  // Its assets are loading
			Sync,     // Activity, assets or a played clip changed: the main thread brings the record up to date first
			Step      // Advances (and poses)
		};

		struct AnimatorRecord
		{
			entt::entity Handle = entt::null;
			UUID ID = UUID::Null(); // The entity's (what SkinComponents and AttachToJointComponents name)
			bool Removed = false; // The component went away (the record leaves at the next CompactRecords)
			uint32_t Set = c_NoSet;
			AnimatorInstance Instance;
			bool Active = false;
			std::vector<glm::mat4> Pose; // Model space, per joint of the skeleton
			uint64_t PoseVersion = 0;    // 0: no pose; changes with the pose and with the reach
			// Its time advanced since its pose was computed (a WhenVisible animator unseen): the pose is older than the time.
			bool PoseStale = false;
			uint64_t SeenUpdate = 0;     // The update before which a renderer last drew a mesh it skins
			uint64_t AttachedUpdate = 0; // The update that last placed an attachment of it
			// Published with the pose while the animator may skip poses (SkeletonPose::Reach): where its clips take the joints.
			AABB Reach;
			float ReachScale = 0.0f;
			// What Reach was worked out from (ReachKnown): StepRecord works it out again only when what plays, the set's clips
			// or the settings it depends on changed. A sync of the record forgets it (another set, or a played clip bound again).
			uint32_t ReachPlayRevision = 0; // AnimatorInstance::GetPlayRevision
			uint32_t ReachSetVersion = 0;   // AssetSet::ReachVersion

			// A clip a script played directly, resolved every update.
			AssetHandle PlayedClipHandle = UUID::Null();
			Ref<AnimationClip> PlayedClip;
			AnimationClipBinding PlayedBinding;
			AnimationReach PlayedReach;
			uint64_t PlayedBindingSkeletonId = 0;
			bool PlayedClipLoading = false;

			// The update's work (written by the evaluation jobs).
			RecordUpdate Update = RecordUpdate::None;
			AnimatorSettings Settings;
			bool MayCull = false;      // WhenVisible: it publishes its reach
			bool Evaluate = false;
			bool Evaluated = false;
			bool ReachChanged = false;
			bool ReachComputed = false; // This update worked the reach out (AnimationSystemStats::ReachUpdates)
			bool ReachKnown = false;    // ReachPlayRevision, ReachSetVersion, ReachMayCull and ReachRootMotion hold
			bool ReachMayCull = false;
			bool ReachRootMotion = false;
			std::vector<AnimatorFiredEvent> Events;
		};

		// What an attachment was last given: the local transform aimed at and the values written for it.
		struct AttachmentState
		{
			bool Valid = false;
			glm::mat4 Local = glm::mat4(1.0f);
			glm::vec3 Translation = glm::vec3(0.0f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 Scale = glm::vec3(1.0f);
		};

		struct EventListener
		{
			AnimationEventListenerID ID = 0;
			AnimationEventCallback Callback;
			bool Removed = false;
		};

		struct PendingEvent
		{
			entt::entity Animator = entt::null;
			UUID AnimatorID = UUID::Null();
			std::string Name;
			std::string State;
			float Time = 0.0f;
		};

		void OnAnimatorAdded(entt::registry& registry, entt::entity handle);
		void OnAnimatorRemoved(entt::registry& registry, entt::entity handle);
		void OnAttachmentRemoved(entt::registry& registry, entt::entity handle);
		// SkinComponents created or signaled (read at the next update, ReadPendingSkins), and destroyed: the animators they name
		// (m_SkinUsers).
		void OnSkinChanged(entt::registry& registry, entt::entity handle);
		void OnSkinRemoved(entt::registry& registry, entt::entity handle);
		void ReadPendingSkins();
		void CountSkinUser(entt::entity skin, UUID animator);
		// Whether a SkinComponent names the animator: a renderer can then see it by a mesh (WhenVisible).
		bool HasSkinUsers(UUID animator) const;
		void AddRecord(entt::entity handle);
		// Drops the records of removed animators (AnimatorRecord::Removed) in one pass that keeps the others' order.
		void CompactRecords();
		AnimatorRecord* FindRecord(entt::entity handle);
		const AnimatorRecord* FindRecord(entt::entity handle) const;

		uint32_t AcquireSet(AssetHandle skeleton, AssetHandle controller, AssetHandle clip);
		void ReleaseSet(uint32_t set);
		// Requests the set's assets (once per update) and rebuilds what depends on objects that changed.
		void ResolveSet(AssetSet& set);
		// The played clip of a record against its set's skeleton.
		void ResolvePlayedClip(AnimatorRecord& record, const AssetSet& set);
		AnimatorAssets MakeAssets(const AnimatorRecord& record) const;
		// The assets for a command between updates: the set is resolved first if no update did (an animator a script added
		// or played before the first update), so commands are checked against the loaded controller.
		AnimatorAssets CommandAssets(AnimatorRecord& record);
		// Matches the record to its component: activity, asset set, start. False while the animator is inactive.
		bool SyncRecord(AnimatorRecord& record, const AnimatorComponent& component, bool active);
		// On the job system: what the update does with the record (preparing its step when it can take one). Reads the
		// registry and the asset sets only.
		RecordUpdate ClassifyRecord(AnimatorRecord& record, const entt::registry& registry) const;
		// The settings and work of a step from the component.
		void PrepareStep(AnimatorRecord& record, const AnimatorComponent& component) const;
		// Advances the animator and computes its pose when it is to (thread-safe across records).
		void StepRecord(AnimatorRecord& record, float deltaTime, std::vector<JointPose>& scratch) const;
		bool IsReady(const AnimatorRecord& record) const;
		void PlaceAttachments();
		void DispatchEvents();
	private:
		Scene& m_Scene;
		std::vector<entt::scoped_connection> m_Connections;
		// In the order updates process them (and publish their results in): by entity when the scene starts, then in the
		// order animators were added. Removed ones stay, flagged, until CompactRecords, so the others never move.
		std::vector<AnimatorRecord> m_Records;
		std::unordered_map<entt::entity, uint32_t> m_RecordIndices; // Of the records not removed
		uint32_t m_RemovedRecords = 0; // Flagged records CompactRecords drops

		std::vector<AssetSet> m_Sets;
		std::vector<uint32_t> m_FreeSets;
		std::map<std::array<uint64_t, 3>, uint32_t> m_SetIndices; // By skeleton, controller and clip handle
		uint64_t m_NextObjectId = 1; // SkeletonId and ControllerId

		std::vector<uint32_t> m_SyncQueue; // Records the update's jobs left to the main thread
		std::vector<uint32_t> m_Work;      // Records stepped after the main thread synchronized them
		std::unordered_map<entt::entity, AttachmentState> m_Attachments;
		// The animators SkinComponents name, while the scene runs: per skin entity the animator it counts for, per animator
		// how many name it.
		std::unordered_map<entt::entity, UUID> m_SkinAnimators;
		std::unordered_map<UUID, uint32_t> m_SkinUsers;
		std::vector<entt::entity> m_PendingSkins;
		std::vector<Ref<EventListener>> m_Listeners;
		AnimationEventListenerID m_NextListenerID = 1;
		std::vector<PendingEvent> m_PendingEvents;

		uint64_t m_UpdateIndex = 0;
		uint64_t m_PosesVersion = 1;
		uint64_t m_NextPoseVersion = 1;
		bool m_Running = false;
		AnimationSystemStats m_Stats;
	};

}
