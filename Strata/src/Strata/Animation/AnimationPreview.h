#pragma once

#include "Strata/Animation/AnimationEditPose.h"
#include "Strata/Animation/AnimatorInstance.h"
#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneServices.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Strata
{

	class AnimationController;

	class Asset;
	class AssetManagerBase;
	class Scene;
	struct AnimatorComponent;
	struct AttachToJointComponent;

	struct AnimationPreviewSpecification
	{
		// Every animator of the scene that no request poses shows its skeleton's rest pose (ComputeRestPose), so skinned
		// meshes stand as their skeleton holds them, oriented by the joints above the skin (a Z_UP node, a rotated and scaled
		// Armature), instead of lying in their mesh's bind space. The editor's edited scene and its thumbnail scenes ask for
		// it.
		bool RestPoses = false;
		// What those animators show (AnimationPreview::SetEditPose changes it): the rest pose, or the first frame of what they
		// play on start (the editor's ViewportSettings::EditPose).
		AnimationEditPose EditPose = AnimationEditPose::Rest;
	};

	// What the edit poses cost (tests check that idle updates add nothing).
	struct AnimationPreviewStats
	{
		uint64_t RestPosesComputed = 0; // Edit poses: once per skeleton (and clip), again when a source changed (a reimport)
		uint64_t AssetRequests = 0;     // Skeletons, controllers and clips requested for edit poses
		uint64_t AnimatorsExamined = 0; // Animators looked at for edit poses: added, removed or their component signaled
	};

	// Poses of animators in a scene that does not play (the editor's edited scene, thumbnails), for viewports and captures:
	// the scene service ISkinningPoses of that scene while the preview exists, so its skinned meshes show the poses asked
	// for, and its attachments (AttachToJointComponent) and the meshes under them are drawn at their joints
	// (GetRenderTransforms). A pose is a state of the animator's controller, a clip, or what the animator plays on start,
	// at a time; with AnimationPreviewSpecification::RestPoses the other animators show their edit pose: their skeleton's
	// rest pose, or the first frame of what they play on start (AnimationEditPose). Nothing in the scene changes (no
	// transforms move, no root motion, no events). Main thread only.
	//
	// Edit poses cost nothing while nothing changes: one per skeleton (and controller and clip), shared by its animators,
	// computed again only when one of their assets' sources changes (a reimport; an eviction keeps the matrices). Animators
	// are found through their component's signals, so an update looks only at animators that were added, removed or
	// changed.
	class AnimationPreview final : public ISkinningPoses
	{
	public:
		struct Request
		{
			std::string State;                // A state of the animator's controller; empty: Clip, or what it plays on start
			AssetHandle Clip = UUID::Null();  // An animation clip, when no state is named
			float Time = 0.0f;                // Seconds into the state or clip (loops follow the state or the component)
		};

		enum class Status : uint8_t
		{
			None = 0, // No request for the entity
			Waiting,  // The latest request's pose is not computed yet (its assets are loading); an earlier pose may show meanwhile
			Posed,    // The pose of the latest request is the one shown
			Failed    // An asset did not load, or the request names what the animator does not have (GetStatus says why)
		};

		// Offers ISkinningPoses on `scene` (which must not have a provider: scenes that play have their animation system).
		explicit AnimationPreview(Scene& scene, const AnimationPreviewSpecification& specification = {});
		~AnimationPreview() override;

		AnimationPreview(const AnimationPreview&) = delete;
		AnimationPreview& operator=(const AnimationPreview&) = delete;

		// Whether the scene took the preview as its ISkinningPoses (false when another provider was there first).
		bool IsRegistered() const { return m_Registered; }

		// Poses the animator on `animator` as asked (replacing an earlier request for it) from the next Update on. False, with
		// the reason, for an entity without an AnimatorComponent and for a negative or non-finite time.
		bool Set(Entity animator, const Request& request, std::string* outError = nullptr);
		bool Clear(Entity animator);
		void ClearAll();
		size_t GetCount() const { return m_Entries.size(); }
		// The animators previewed now with what was asked of them (the viewport names them, so that a preview is never taken
		// for scene data).
		std::vector<std::pair<entt::entity, Request>> GetRequests() const;

		// Requests the assets of every preview and computes the poses whose assets arrived, again when the request, the
		// component's assets or an asset object (a reload) changed, then where the attachments are drawn; with rest poses,
		// also the rest poses of skeletons that arrived or changed. Once per frame, before rendering.
		void Update();
		// What animators without a request show (with RestPoses): their skeleton's rest pose, or the first frame of what they
		// play on start. A change looks at every animator again, once; poses already computed for the other choice are let go
		// as their animators leave them.
		void SetEditPose(AnimationEditPose pose);
		AnimationEditPose GetEditPose() const { return m_Specification.EditPose; }
		// Whether an animator waits for its skeleton (or clip) to show its edit pose (thumbnails wait for it like for their
		// meshes).
		bool HasWaitingRestPoses() const { return m_WaitingRestPoses > 0 || !m_DirtyAnimators.empty(); }
		const AnimationPreviewStats& GetStats() const { return m_Stats; }
		// `outRequestInvalid`: a failure because the request names what the animator does not have (a state its controller
		// lacks), rather than because an asset did not load.
		Status GetStatus(Entity animator, std::string* outError = nullptr, bool* outRequestInvalid = nullptr) const;
		// The states of a controller as a list for messages ("Idle, Run").
		static std::string DescribeStates(const AnimationController& controller);

		bool GetPose(entt::entity animator, SkeletonPose& outPose) const override;
		uint64_t GetPosesVersion() const override { return m_PosesVersion; }
		bool ReportVisible(entt::entity) override { return false; } // Previews show the pose of their time
		// The mesh renderers of the posed animators' attachments, and of what is under them, at the attachments' joints: each
		// moved by the change from where the scene has its attachment to the animator's world transform times the joint's pose.
		std::span<const RenderTransform> GetRenderTransforms() const override { return m_RenderTransforms; }
		uint64_t GetRenderTransformsVersion() const override { return m_RenderTransformsVersion; }
	private:
		struct Entry
		{
			Request Asked;
			Status State = Status::Waiting;
			std::string Error;
			bool RequestInvalid = false; // The failure is the request's (GetStatus)
			bool Dirty = true;
			// What the pose was computed from: the component's assets and settings, and the asset objects.
			AssetHandle Skeleton = UUID::Null();
			AssetHandle Controller = UUID::Null();
			AssetHandle ComponentClip = UUID::Null();
			bool Loop = true;
			bool RootMotion = false;
			std::vector<std::weak_ptr<const Asset>> Objects;
			std::vector<glm::mat4> Pose;
			uint64_t Version = 0;
		};

		// What an animator without a request shows, shared by every animator with the same key: its skeleton's rest pose
		// (Controller and Clip null), or with the clip-start edit pose the first frame of what it plays on start (its
		// controller, else its clip; root motion changes how the root joint is posed).
		struct EditPoseKey
		{
			AssetHandle Skeleton = UUID::Null();
			AssetHandle Controller = UUID::Null();
			AssetHandle Clip = UUID::Null();
			bool RootMotion = false;
			bool operator==(const EditPoseKey&) const = default;
		};
		struct EditPoseKeyHash
		{
			size_t operator()(const EditPoseKey& key) const;
		};

		struct RestPose
		{
			enum class State : uint8_t
			{
				Waiting, // Its assets are loading (a pose computed before may show meanwhile)
				Ready,
				Failed   // Its skeleton did not load
			};
			State Status = State::Waiting;
			std::vector<glm::mat4> Model; // Kept while the assets are evicted: data, not held assets
			uint64_t Version = 0;         // 0 until computed
			// The assets it was computed from and their AssetSourceRevisions::Loaded: a reimport of any poses it again.
			std::vector<std::pair<AssetHandle, uint64_t>> Sources;
			// The skeleton's joint names, for attachments, when the pose came from a clip (rest poses place none: the importer
			// put attachments at the rest pose already).
			std::vector<std::string> ClipJoints;
			uint32_t Animators = 0;       // Animators that show it
		};

		// The scene's AnimatorComponent signals (added, changed, removing): the animator is looked at in the next update.
		void OnAnimatorChanged(entt::registry& registry, entt::entity entity);
		// Attachments and mesh renderers added, changed or removing: where attachments are drawn may change.
		void OnAttachmentsChanged(entt::registry& registry, entt::entity entity);
		void UpdateRestPoses(AssetManagerBase& manager);
		EditPoseKey MakeEditPoseKey(const AnimatorComponent& component) const;
		void ExamineAnimator(entt::entity handle);
		void SetRestPoseState(RestPose& pose, RestPose::State state);
		// A pose whose skeleton did not load (or could not be evaluated), with the sources a change of which tries again.
		void FailRestPose(RestPose& pose, std::vector<std::pair<AssetHandle, uint64_t>> sources);
		void ReleaseRestPose(const EditPoseKey& key);
		void UpdateEntry(AssetManagerBase* manager, entt::entity handle, Entry& entry);
		void Fail(Entry& entry, std::string error, bool requestInvalid = false);
		// Where an attachment is drawn: its animator's world transform times its joint's pose, while the animator is posed.
		std::optional<glm::mat4> GetAttachmentWorld(Entity entity, const AttachToJointComponent& attachment) const;
		// Computes the render transforms again (GetRenderTransforms) when what they follow changed; a new list gets a new
		// version.
		void UpdateRenderTransforms();
	private:
		Scene& m_Scene;
		AnimationPreviewSpecification m_Specification;
		bool m_Registered = false;
		std::unordered_map<entt::entity, Entry> m_Entries;
		uint64_t m_PosesVersion = 1;
		uint64_t m_NextVersion = 1;
		std::vector<JointPose> m_Scratch;
		std::vector<RenderTransform> m_RenderTransforms;
		uint64_t m_RenderTransformsVersion = 1;
		// What the render transforms were computed from (UpdateRenderTransforms).
		uint64_t m_RequestsRevision = 1;
		uint64_t m_AttachmentsRevision = 1;
		struct RenderInputs
		{
			uint64_t Poses = 0;
			uint64_t Transforms = 0;
			uint64_t Hierarchy = 0;
			uint64_t Requests = 0;
			uint64_t Attachments = 0;
			bool operator==(const RenderInputs&) const = default;
		};
		RenderInputs m_RenderInputs;

		// Edit poses (AnimationPreviewSpecification::RestPoses).
		std::unordered_map<EditPoseKey, RestPose, EditPoseKeyHash> m_RestPoses;
		std::unordered_map<entt::entity, EditPoseKey> m_RestAnimators; // Animator -> the pose it shows
		std::vector<entt::entity> m_DirtyAnimators;
		uint32_t m_WaitingRestPoses = 0;
		uint64_t m_ContentVersion = 0; // Of the asset manager when the skeletons' sources were last checked
		std::vector<AssetHandle> m_ContentChanges;
		std::vector<entt::connection> m_Connections;
		AnimationPreviewStats m_Stats;
	};

}
