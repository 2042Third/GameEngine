#pragma once

#include "Strata/Animation/AnimationTypes.h"
#include "Strata/Asset/Asset.h"

#include <glm/glm.hpp>

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Strata
{

	// The joints an animator poses: their names (unique, never empty), their parents (every parent comes before its
	// children: joints are in topological order) and the rest pose, each joint's transform relative to its parent (root
	// joints: relative to the animator's entity). An imported model has one (the sub-asset "Skeleton"): the joints of all
	// its skins and the nodes its animations move, with their ancestors, so one animator plays the whole model.
	//
	// Clips bind to joints by name (AnimationClip), so clips of another model whose joints carry the same names play on
	// this skeleton too. The inverse bind matrices are not part of the skeleton: they belong to how a mesh was bound to it
	// (Mesh's skin binding, one per glTF skin), as two skins may bind the same joint in different poses.
	//
	// Cooked format ("STSK"): a header, then per joint its parent and rest pose, then the names. Deserialize validates
	// everything (counts, parent order, finite poses, unique names).
	class Skeleton : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::Skeleton; }
		AssetType GetType() const override { return GetStaticType(); }

		static constexpr uint32_t c_CookedVersion = 1;
		// Joint indices are 16 bits wide in meshes and clips (c_NoJoint is reserved).
		static constexpr uint32_t c_MaxJoints = 4096;
		static constexpr uint32_t c_MaxNameLength = 1024;

		// Validates and normalizes (rotations to unit length); null with the reason for invalid input. rootMotionJoint is
		// the joint whose motion root motion extracts (c_NoJoint: the first root joint).
		static Ref<Skeleton> Create(std::vector<std::string> names, std::vector<JointIndex> parents, std::vector<JointPose> restPose,
			JointIndex rootMotionJoint = c_NoJoint, std::string* outError = nullptr);

		std::vector<uint8_t> Serialize() const;
		static Ref<Skeleton> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);

		// The name index refers into the skeleton's own names.
		Skeleton(const Skeleton&) = delete;
		Skeleton& operator=(const Skeleton&) = delete;

		AssetMemoryUsage GetMemoryUsage() const override;

		uint32_t GetJointCount() const { return static_cast<uint32_t>(m_Names.size()); }
		const std::vector<std::string>& GetJointNames() const { return m_Names; }
		const std::string& GetJointName(JointIndex joint) const { return m_Names[joint]; }
		const std::vector<JointIndex>& GetParents() const { return m_Parents; }
		JointIndex GetParent(JointIndex joint) const { return m_Parents[joint]; }
		const std::vector<JointPose>& GetRestPose() const { return m_RestPose; }
		// The joint whose motion root motion moves the animator's entity by.
		JointIndex GetRootMotionJoint() const { return m_RootMotionJoint; }
		// The joint with this name, or c_NoJoint (exact, case-sensitive match).
		JointIndex FindJoint(std::string_view name) const;

		// Model-space transforms (relative to the animator's entity) of a pose of this skeleton's joints: each joint's
		// parent's transform times its own. `local` and `outModel` have one entry per joint.
		void ComputeModelTransforms(std::span<const JointPose> local, std::span<glm::mat4> outModel) const;
	private:
		Skeleton() = default;
	private:
		std::vector<std::string> m_Names;
		std::vector<JointIndex> m_Parents;
		std::vector<JointPose> m_RestPose;
		JointIndex m_RootMotionJoint = 0;
		std::unordered_map<std::string_view, JointIndex> m_JointsByName; // Views into m_Names
	};

}
