#pragma once

#include "Strata/Scene/SceneServices.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <unordered_map>
#include <vector>

namespace Strata::Tests
{

	// Poses that tests set directly, standing in for the animation system (Scene::RegisterService<ISkinningPoses>).
	class TestSkinningPoses final : public ISkinningPoses
	{
	public:
		std::unordered_map<entt::entity, std::vector<glm::mat4>> Poses;
		uint64_t Version = 1; // Bump after changing Poses
		std::vector<entt::entity> Reported;

		bool GetPose(entt::entity animator, SkeletonPose& outPose) const override
		{
			auto it = Poses.find(animator);
			if (it == Poses.end())
				return false;
			outPose.Joints = it->second;
			outPose.Version = Version;
			return true;
		}

		uint64_t GetPosesVersion() const override { return Version; }

		void ReportVisible(entt::entity animator) override { Reported.push_back(animator); }
	};

	// A quarter turn about +Z, exact (x -> y, y -> -x).
	inline glm::mat4 QuarterTurnZ()
	{
		glm::mat4 matrix(0.0f);
		matrix[0] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
		matrix[1] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
		matrix[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
		matrix[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		return matrix;
	}

	// The model-space joints of a chain (each joint jointLength above its parent) whose joint `bent` turns a quarter about
	// +Z (the joints above it follow).
	inline std::vector<glm::mat4> BentChain(uint32_t joints, uint32_t bent, float jointLength = 1.0f)
	{
		std::vector<glm::mat4> model(joints);
		for (uint32_t joint = 0; joint < joints; joint++)
		{
			glm::mat4 local = joint == 0 ? glm::mat4(1.0f) : glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, jointLength, 0.0f));
			if (joint == bent)
				local = local * QuarterTurnZ();
			model[joint] = joint == 0 ? local : model[joint - 1] * local;
		}
		return model;
	}

}
