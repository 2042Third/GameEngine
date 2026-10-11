#pragma once

#include "Strata/Animation/AnimatorInstance.h"
#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneServices.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Asset;
	class Scene;

	// Poses of animators in a scene that does not play (the editor's edited scene), for viewports and captures: the scene
	// service ISkinningPoses of that scene while the preview exists, so its skinned meshes show the poses asked for. A pose
	// is a state of the animator's controller, a clip, or what the animator plays on start, at a time; nothing in the scene
	// changes (no transforms move, no root motion, no events, no attachments). Main thread only.
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
			Waiting,  // Its assets are still loading
			Posed,
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

		// Requests the assets of every preview and computes the poses whose assets arrived, again when the request, the
		// component's assets or an asset object (a reload) changed. Once per frame, before rendering.
		void Update();
		Status GetStatus(Entity animator, std::string* outError = nullptr) const;

		bool GetPose(entt::entity animator, SkeletonPose& outPose) const override;
		uint64_t GetPosesVersion() const override { return m_PosesVersion; }
		void ReportVisible(entt::entity) override {}
	private:
		struct Entry
		{
			Request Asked;
			Status State = Status::Waiting;
			std::string Error;
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
		void Fail(Entry& entry, std::string error);
	private:
		Scene& m_Scene;
		bool m_Registered = false;
		std::unordered_map<entt::entity, Entry> m_Entries;
		uint64_t m_PosesVersion = 1;
		uint64_t m_NextVersion = 1;
		std::vector<JointPose> m_Scratch;
	};

}
