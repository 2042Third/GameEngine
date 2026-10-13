#pragma once

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
	class Scene;
	struct AttachToJointComponent;

	// Poses of animators in a scene that does not play (the editor's edited scene), for viewports and captures: the scene
	// service ISkinningPoses of that scene while the preview exists, so its skinned meshes show the poses asked for, and its
	// attachments (AttachToJointComponent) and the meshes under them are drawn at their joints (GetRenderTransforms). A pose
	// is a state of the animator's controller, a clip, or what the animator plays on start, at a time; nothing in the scene
	// changes (no transforms move, no root motion, no events). Main thread only.
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
		explicit AnimationPreview(Scene& scene);
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
		// component's assets or an asset object (a reload) changed, then where the attachments are drawn. Once per frame,
		// before rendering.
		void Update();
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

		void UpdateEntry(entt::entity handle, Entry& entry);
		void Fail(Entry& entry, std::string error, bool requestInvalid = false);
		// Where an attachment is drawn: its animator's world transform times its joint's pose, while the animator is posed.
		std::optional<glm::mat4> GetAttachmentWorld(Entity entity, const AttachToJointComponent& attachment) const;
		// Computes the render transforms again (GetRenderTransforms); a new list gets a new version.
		void UpdateRenderTransforms();
	private:
		Scene& m_Scene;
		bool m_Registered = false;
		std::unordered_map<entt::entity, Entry> m_Entries;
		uint64_t m_PosesVersion = 1;
		uint64_t m_NextVersion = 1;
		std::vector<JointPose> m_Scratch;
		std::vector<RenderTransform> m_RenderTransforms;
		uint64_t m_RenderTransformsVersion = 1;
	};

}
