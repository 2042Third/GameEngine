#include "stpch.h"
#include "Strata/Animation/RestPose.h"

#include "Strata/Animation/Skeleton.h"

namespace Strata
{

	void ComputeRestPose(const Skeleton& skeleton, std::vector<glm::mat4>& outModel)
	{
		outModel.resize(skeleton.GetJointCount());
		skeleton.ComputeModelTransforms(skeleton.GetRestPose(), outModel);
	}

}
