#include <doctest/doctest.h>

#include "Animation/SkinnedMeshTestUtils.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"

#include <cstring>
#include <limits>

using namespace Strata;

TEST_SUITE("Renderer.Mesh")
{
	TEST_CASE("Skinned meshes round trip and know each joint's bind-pose extent")
	{
		Ref<Mesh> source = Tests::CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 2, .Sides = 6 });
		REQUIRE(source->IsSkinned());
		const MeshSkinBinding& binding = *source->GetSkinBinding();
		REQUIRE(binding.JointBounds.size() == 3);
		// Joint 0 influences the rings from 0 to just below 1, joint 1 those from 0.5 to 2, joint 2 the top two.
		CHECK(binding.JointBounds[0].Min.y == 0.0f);
		CHECK(binding.JointBounds[0].Max.y == doctest::Approx(0.5f));
		CHECK(binding.JointBounds[1].Min.y == doctest::Approx(0.5f));
		CHECK(binding.JointBounds[2].Max.y == doctest::Approx(3.0f));

		const std::vector<uint8_t> cooked = source->Serialize();
		std::string error;
		Ref<Mesh> loaded = Mesh::Deserialize(cooked, &error);
		REQUIRE_MESSAGE(loaded, error);
		REQUIRE(loaded->IsSkinned());
		CHECK(loaded->GetPositions() == source->GetPositions());
		REQUIRE(loaded->GetSkinVertices().size() == source->GetSkinVertices().size());
		CHECK(std::memcmp(loaded->GetSkinVertices().data(), source->GetSkinVertices().data(), source->GetSkinVertices().size() * sizeof(MeshSkinVertex)) == 0);
		CHECK(loaded->GetSkinBinding()->Joints == binding.Joints);
		CHECK(loaded->GetSkinBinding()->InverseBindMatrices == binding.InverseBindMatrices);
		for (size_t joint = 0; joint < binding.JointBounds.size(); joint++)
		{
			CHECK(loaded->GetSkinBinding()->JointBounds[joint].Min == binding.JointBounds[joint].Min);
			CHECK(loaded->GetSkinBinding()->JointBounds[joint].Max == binding.JointBounds[joint].Max);
		}
		CHECK(loaded->Serialize() == cooked);

		// Meshes without a skin hold nothing of one.
		Ref<Mesh> cube = MeshFactory::CreateCube();
		CHECK_FALSE(cube->IsSkinned());
		CHECK(cube->GetSkinVertices().empty());
	}

	TEST_CASE("Cooked meshes of version 1 still load")
	{
		Ref<Mesh> cube = MeshFactory::CreateSphere(0.5f, 16, 8);
		std::vector<uint8_t> cooked = cube->Serialize();
		// Version 1 had the same layout without flags (its reserved word was 0).
		const uint32_t version = 1;
		std::memcpy(cooked.data() + 4, &version, sizeof(version));
		std::string error;
		Ref<Mesh> loaded = Mesh::Deserialize(cooked, &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK_FALSE(loaded->IsSkinned());
		CHECK(loaded->GetPositions() == cube->GetPositions());
		CHECK(loaded->GetIndices() == cube->GetIndices());

		// A version 1 mesh with the skin flag is corrupt.
		const uint32_t skinned = 1;
		std::memcpy(cooked.data() + 12, &skinned, sizeof(skinned));
		CHECK_FALSE(Mesh::Deserialize(cooked));
	}

	TEST_CASE("Skinned mesh loading rejects truncated and corrupt data")
	{
		const std::vector<uint8_t> cooked = Tests::CreateSkinnedTube({ .Joints = 2, .RingsPerJoint = 1, .Sides = 3 })->Serialize();
		CHECK_FALSE(Mesh::Deserialize({}));
		for (size_t length = 0; length < cooked.size(); length++)
		{
			CAPTURE(length);
			CHECK_FALSE(Mesh::Deserialize(std::span<const uint8_t>(cooked.data(), length)));
		}
		std::vector<uint8_t> trailing = cooked;
		trailing.push_back(0);
		CHECK_FALSE(Mesh::Deserialize(trailing));

		// Any single corrupted byte is rejected or loads a mesh whose skin passes validation.
		for (size_t offset = 0; offset < cooked.size(); offset++)
		{
			for (uint8_t pattern : { uint8_t(0x5A), uint8_t(0xFF), uint8_t(0x01) })
			{
				std::vector<uint8_t> corrupted = cooked;
				corrupted[offset] ^= pattern;
				Ref<Mesh> mesh = Mesh::Deserialize(corrupted);
				if (!mesh || !mesh->IsSkinned())
					continue;
				const MeshSkinBinding& binding = *mesh->GetSkinBinding();
				for (const MeshSkinVertex& vertex : mesh->GetSkinVertices())
				{
					uint32_t sum = 0;
					for (int influence = 0; influence < 4; influence++)
					{
						sum += vertex.Weights[influence];
						if (vertex.Weights[influence] != 0)
							CHECK(vertex.Joints[influence] < binding.Joints.size());
					}
					CHECK(sum == 65535);
				}
			}
		}
	}

	TEST_CASE("Skinned mesh creation validates the skin")
	{
		Ref<Mesh> tube = Tests::CreateSkinnedTube({ .Joints = 2, .RingsPerJoint = 1, .Sides = 3 });
		auto attempt = [&](std::vector<MeshSkinVertex> skin, MeshSkinBinding binding)
		{
			std::string error;
			Ref<Mesh> mesh = Mesh::CreateSkinned(tube->GetPositions(), tube->GetAttributes(), tube->GetIndices(), tube->GetSubmeshes(), std::move(skin), std::move(binding), &error);
			CAPTURE(error);
			CHECK((mesh != nullptr) == error.empty());
			return mesh != nullptr;
		};
		const std::vector<MeshSkinVertex> skin = tube->GetSkinVertices();
		const MeshSkinBinding binding = *tube->GetSkinBinding();
		CHECK(attempt(skin, binding));

		MeshSkinBinding noJoints;
		CHECK_FALSE(attempt(skin, noJoints));
		MeshSkinBinding missingMatrix = binding;
		missingMatrix.InverseBindMatrices.pop_back();
		CHECK_FALSE(attempt(skin, missingMatrix));
		MeshSkinBinding projective = binding;
		projective.InverseBindMatrices[0][0][3] = 0.5f;
		CHECK_FALSE(attempt(skin, projective));
		MeshSkinBinding infinite = binding;
		infinite.InverseBindMatrices[1][3][0] = std::numeric_limits<float>::infinity();
		CHECK_FALSE(attempt(skin, infinite));

		std::vector<MeshSkinVertex> shortSkin = skin;
		shortSkin.pop_back();
		CHECK_FALSE(attempt(shortSkin, binding));
		std::vector<MeshSkinVertex> outside = skin;
		outside[0].Joints[0] = 2;
		CHECK_FALSE(attempt(outside, binding));
		std::vector<MeshSkinVertex> unbalanced = skin;
		unbalanced[0].Weights[0] = 1000;
		CHECK_FALSE(attempt(unbalanced, binding));
		// A zero weight may name any joint.
		std::vector<MeshSkinVertex> unused = skin;
		unused[0].Joints[3] = 999;
		unused[0].Weights[3] = 0;
		CHECK(attempt(unused, binding));

		// The same geometry without a skin is a mesh of its own.
		CHECK(Mesh::Create(tube->GetPositions(), tube->GetAttributes(), tube->GetIndices(), tube->GetSubmeshes()) != nullptr);
	}

	TEST_CASE("Seam tangents weld the skin stream with the other streams")
	{
		// A quad whose two triangles share an edge: the shared corners keep one vertex each only where their skin matches.
		std::vector<glm::vec3> positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } };
		std::vector<MeshVertexAttributes> attributes(4);
		attributes[0].TexCoord = { 0, 1 };
		attributes[1].TexCoord = { 1, 1 };
		attributes[2].TexCoord = { 1, 0 };
		attributes[3].TexCoord = { 0, 0 };
		for (MeshVertexAttributes& attribute : attributes)
			attribute.Normal = { 0, 0, 1 };
		std::vector<uint32_t> indices = { 0, 1, 2, 0, 2, 3 };
		std::vector<MeshSkinVertex> skin(4);
		for (uint16_t vertex = 0; vertex < 4; vertex++)
			skin[vertex].Joints[0] = vertex;
		REQUIRE(MeshUtils::GenerateSeamTangents(positions, attributes, indices, false, &skin));
		REQUIRE(skin.size() == positions.size());
		CHECK(positions.size() == 4);
		for (size_t corner = 0; corner < indices.size(); corner++)
		{
			// Every vertex keeps the skin of the corner it came from.
			const glm::vec3& position = positions[indices[corner]];
			const uint16_t expected = position == glm::vec3(0, 0, 0) ? 0 : position == glm::vec3(1, 0, 0) ? 1 : position == glm::vec3(1, 1, 0) ? 2 : 3;
			CHECK(skin[indices[corner]].Joints[0] == expected);
		}
		std::vector<MeshSkinVertex> wrongLength(3);
		CHECK_FALSE(MeshUtils::GenerateSeamTangents(positions, attributes, indices, false, &wrongLength));
	}
}
