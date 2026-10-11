#pragma once

#include "Strata/Animation/Skeleton.h"
#include "Strata/Renderer/Mesh.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace Strata::Tests
{

	// A tube along +Y skinned to a chain of joints: joint j sits at j * JointLength (its bind pose), the tube reaches from 0
	// to Joints * JointLength in rings of Sides vertices, RingsPerJoint rings per joint (and the top ring). A ring between
	// joints j and j + 1 is weighted between them by its height; rings above the last joint follow it.
	struct SkinnedTubeSpecification
	{
		uint32_t Joints = 3;
		uint32_t RingsPerJoint = 4;
		uint32_t Sides = 8;
		float JointLength = 1.0f;
		float Radius = 0.1f;
		AssetHandle Material = UUID::Null();
		// Off: every ring follows only the joint below it (weight 1), so skinning is exact for exact poses.
		bool BlendWeights = true;
	};

	inline uint16_t QuantizeWeight(float weight)
	{
		return static_cast<uint16_t>(std::lround(weight * 65535.0f));
	}

	inline Ref<Mesh> CreateSkinnedTube(const SkinnedTubeSpecification& specification = {})
	{
		std::vector<glm::vec3> positions;
		std::vector<MeshVertexAttributes> attributes;
		std::vector<MeshSkinVertex> skin;
		std::vector<uint32_t> indices;
		const uint32_t rings = specification.Joints * specification.RingsPerJoint + 1;
		for (uint32_t ring = 0; ring < rings; ring++)
		{
			const float height = static_cast<float>(ring) / static_cast<float>(specification.RingsPerJoint); // In joints
			const uint32_t joint = std::min(static_cast<uint32_t>(height), specification.Joints - 1);
			const float fraction = joint + 1 < specification.Joints && specification.BlendWeights ? height - static_cast<float>(joint) : 0.0f;
			for (uint32_t side = 0; side < specification.Sides; side++)
			{
				const float angle = glm::two_pi<float>() * static_cast<float>(side) / static_cast<float>(specification.Sides);
				const glm::vec3 normal(std::cos(angle), 0.0f, -std::sin(angle));
				positions.push_back(glm::vec3(normal.x * specification.Radius, height * specification.JointLength, normal.z * specification.Radius));
				MeshVertexAttributes attribute;
				attribute.Normal = normal;
				attribute.Tangent = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
				attribute.TexCoord = glm::vec2(static_cast<float>(side) / static_cast<float>(specification.Sides), height);
				attributes.push_back(attribute);
				MeshSkinVertex vertex;
				vertex.Joints[0] = static_cast<uint16_t>(joint);
				vertex.Weights[0] = static_cast<uint16_t>(65535 - QuantizeWeight(fraction));
				if (fraction > 0.0f)
				{
					vertex.Joints[1] = static_cast<uint16_t>(joint + 1);
					vertex.Weights[1] = QuantizeWeight(fraction);
				}
				skin.push_back(vertex);
			}
		}
		for (uint32_t ring = 0; ring + 1 < rings; ring++)
		{
			for (uint32_t side = 0; side < specification.Sides; side++)
			{
				// Counter-clockwise seen from outside.
				const uint32_t a = ring * specification.Sides + side;
				const uint32_t b = ring * specification.Sides + (side + 1) % specification.Sides;
				indices.insert(indices.end(), { a, b, b + specification.Sides, a, b + specification.Sides, a + specification.Sides });
			}
		}

		MeshSkinBinding binding;
		for (uint32_t joint = 0; joint < specification.Joints; joint++)
		{
			binding.Joints.push_back(static_cast<uint16_t>(joint));
			binding.InverseBindMatrices.push_back(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -static_cast<float>(joint) * specification.JointLength, 0.0f)));
		}
		Submesh submesh;
		submesh.Name = "Tube";
		submesh.VertexCount = static_cast<uint32_t>(positions.size());
		submesh.LODs = { MeshLOD { 0, static_cast<uint32_t>(indices.size()) } };
		submesh.Material = specification.Material;
		std::string error;
		Ref<Mesh> mesh = Mesh::CreateSkinned(std::move(positions), std::move(attributes), std::move(indices), { submesh }, std::move(skin), std::move(binding), &error);
		REQUIRE_MESSAGE(mesh, error);
		return mesh;
	}

	// The chain the tube is bound to: Joint0 (the root) to Joint<n-1>, each JointLength above its parent.
	inline Ref<Skeleton> CreateChainSkeleton(uint32_t joints, float jointLength = 1.0f)
	{
		std::vector<std::string> names;
		std::vector<JointIndex> parents;
		std::vector<JointPose> rest(joints);
		for (uint32_t joint = 0; joint < joints; joint++)
		{
			names.push_back("Joint" + std::to_string(joint));
			parents.push_back(joint == 0 ? c_NoJoint : static_cast<JointIndex>(joint - 1));
			if (joint > 0)
				rest[joint].Translation = glm::vec3(0.0f, jointLength, 0.0f);
		}
		std::string error;
		Ref<Skeleton> skeleton = Skeleton::Create(std::move(names), std::move(parents), std::move(rest), c_NoJoint, &error);
		REQUIRE_MESSAGE(skeleton, error);
		return skeleton;
	}

	// The skinned position of a vertex (CPU reference): its influences' palette entries blended by weight.
	inline glm::vec3 SkinPosition(const glm::vec3& position, const MeshSkinVertex& vertex, const std::vector<glm::mat4>& palette)
	{
		glm::mat4 blended(0.0f);
		for (int influence = 0; influence < 4; influence++)
		{
			if (vertex.Weights[influence] != 0)
				blended += palette[vertex.Joints[influence]] * (static_cast<float>(vertex.Weights[influence]) / 65535.0f);
		}
		return glm::vec3(blended * glm::vec4(position, 1.0f));
	}

}
