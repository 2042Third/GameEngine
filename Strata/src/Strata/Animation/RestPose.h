#pragma once

#include <glm/glm.hpp>

#include <vector>

namespace Strata
{

	class Skeleton;

	// The pose of a skeleton with nothing playing: each joint's rest transform through its parents', relative to the
	// animator's entity (Skeleton::ComputeModelTransforms of Skeleton::GetRestPose). The joints above the skin (a glTF
	// file's Z_UP node, an exporter's rotated and scaled Armature) are joints of the skeleton, so they orient the pose.
	// AnimatorInstance::Evaluate gives it with nothing playing, and the editor shows it on animators of scenes that do not
	// play (AnimationPreviewSpecification::RestPoses): one function for both, so edit mode and play mode cannot drift apart.
	// `outModel` gets one transform per joint.
	void ComputeRestPose(const Skeleton& skeleton, std::vector<glm::mat4>& outModel);

}
