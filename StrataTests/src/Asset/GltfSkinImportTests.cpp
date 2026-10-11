#include <doctest/doctest.h>

#include "Animation/SkinnedGltf.h"
#include "Strata/Animation/AnimationClip.h"
#include "Strata/Animation/Skeleton.h"
#include "Strata/Asset/AssetDependencies.h"
#include "Strata/Asset/GltfImporter.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "TestHelpers.h"

#include <glm/gtc/epsilon.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	struct SkinImport
	{
		bool Success = false;
		std::string Error;
		ImportResult Result;
		AssetHandle Handle = UUID(0x5EED5EED5EEDull);

		const ImportedSubAsset* Find(std::string_view key) const
		{
			auto it = std::find_if(Result.SubAssets.begin(), Result.SubAssets.end(), [key](const ImportedSubAsset& subAsset) { return subAsset.Key == key; });
			return it != Result.SubAssets.end() ? &*it : nullptr;
		}
	};

	SkinImport ImportModel(const std::filesystem::path& assetDirectory, const std::string& name, const std::vector<uint8_t>& file)
	{
		const std::filesystem::path path = assetDirectory / FileSystem::FromUTF8(name);
		REQUIRE(FileSystem::WriteBytes(path, file));
		GltfImporter importer;
		SkinImport output;
		ImportContext context;
		context.Handle = output.Handle;
		context.SourcePath = path;
		context.AssetDirectory = assetDirectory;
		context.Settings = importer.GetDefaultSettings(path);
		output.Success = importer.Import(context, output.Result, &output.Error);
		return output;
	}

	bool HasWarningContaining(const ImportResult& result, std::string_view text)
	{
		return std::any_of(result.Warnings.begin(), result.Warnings.end(), [text](const std::string& warning) { return warning.find(text) != std::string::npos; });
	}

	Entity FindByName(Entity parent, std::string_view name)
	{
		for (Entity child : parent.GetChildren())
		{
			if (child.GetName() == name)
				return child;
			if (Entity found = FindByName(child, name))
				return found;
		}
		return {};
	}

}

TEST_SUITE("Asset.Gltf")
{
	TEST_CASE("Skinned glTF files import a skeleton, clips and a skinned mesh without skipping anything")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfSkin");
		const SkinImport output = ImportModel(assets, "Arm.gltf", Tests::CreateSkinnedArmGltf());
		REQUIRE_MESSAGE(output.Success, output.Error);
		CAPTURE(output.Result.Warnings);
		CHECK(output.Result.Warnings.empty());
		CHECK_FALSE(HasWarningContaining(output.Result, "skins"));
		CHECK_FALSE(HasWarningContaining(output.Result, "animations"));
		CHECK_FALSE(HasWarningContaining(output.Result, "skinning"));

		// The skeleton: the skin's joints and their ancestor "Armature", parents first.
		const ImportedSubAsset* skeletonData = output.Find("Skeleton");
		REQUIRE(skeletonData);
		CHECK(skeletonData->Type == AssetType::Skeleton);
		std::string error;
		Ref<Skeleton> skeleton = Skeleton::Deserialize(skeletonData->Data, &error);
		REQUIRE_MESSAGE(skeleton, error);
		CHECK(skeleton->GetJointNames() == std::vector<std::string> { "Armature", "Shoulder", "Elbow", "Wrist" });
		CHECK(skeleton->GetParents() == std::vector<JointIndex> { c_NoJoint, 0, 1, 2 });
		CHECK(skeleton->GetRestPose()[2].Translation == glm::vec3(0.0f, 1.0f, 0.0f));
		// Root motion follows the topmost joint of the skin.
		CHECK(skeleton->GetRootMotionJoint() == 1);

		// The mesh: one skin vertex per vertex, bound to the skin's joints in the skeleton with its inverse bind matrices.
		const ImportedSubAsset* meshData = output.Find("Mesh/0");
		REQUIRE(meshData);
		Ref<Mesh> mesh = Mesh::Deserialize(meshData->Data, &error);
		REQUIRE_MESSAGE(mesh, error);
		REQUIRE(mesh->IsSkinned());
		const MeshSkinBinding& binding = *mesh->GetSkinBinding();
		CHECK(binding.Joints == std::vector<uint16_t> { 1, 2, 3 });
		CHECK(binding.InverseBindMatrices[1][3] == glm::vec4(0.0f, -1.0f, 0.0f, 1.0f));
		REQUIRE(mesh->GetSkinVertices().size() == mesh->GetPositions().size());
		for (size_t vertex = 0; vertex < mesh->GetPositions().size(); vertex++)
		{
			const MeshSkinVertex& skin = mesh->GetSkinVertices()[vertex];
			const float y = mesh->GetPositions()[vertex].y;
			CAPTURE(y);
			CHECK(skin.Weights[0] + skin.Weights[1] + skin.Weights[2] + skin.Weights[3] == 65535);
			if (y < 0.7f)
				CHECK((skin.Joints[0] == 0 && skin.Weights[0] == 65535));
			else if (y > 1.9f)
				CHECK((skin.Joints[0] == 2 && skin.Weights[0] == 65535));
			else if (y > 0.9f && y < 1.1f)
				CHECK(std::abs(static_cast<int>(skin.Weights[0]) - static_cast<int>(skin.Weights[1])) <= 300); // 128 and 127 of 255
		}
		// Each joint's extent covers the rings it moves.
		CHECK(binding.JointBounds[0].Max.y == doctest::Approx(1.25f));
		CHECK(binding.JointBounds[2].Min.y == doctest::Approx(2.0f));

		// The clips, reduced: the wave's 31 linear keys lie on two arcs.
		const ImportedSubAsset* waveData = output.Find("Animation/Wave");
		const ImportedSubAsset* reachData = output.Find("Animation/Reach");
		REQUIRE(waveData);
		REQUIRE(reachData);
		CHECK(waveData->Type == AssetType::AnimationClip);
		Ref<AnimationClip> wave = AnimationClip::Deserialize(waveData->Data, &error);
		REQUIRE_MESSAGE(wave, error);
		CHECK(wave->GetName() == "Wave");
		CHECK(wave->GetDuration() == doctest::Approx(1.0f));
		REQUIRE(wave->GetChannels().size() == 1);
		CHECK(wave->GetChannels()[0].KeyCount == 3);
		CHECK(wave->GetTargetNames()[0] == "Elbow");
		Ref<AnimationClip> reach = AnimationClip::Deserialize(reachData->Data, &error);
		REQUIRE_MESSAGE(reach, error);
		CHECK(reach->GetDuration() == doctest::Approx(2.0f));
		REQUIRE(reach->GetChannels().size() == 2);
		CHECK(reach->GetChannels()[0].Interpolation == AnimationInterpolation::Step);
		CHECK(reach->GetChannels()[1].Interpolation == AnimationInterpolation::CubicSpline);

		// Sampled on the skeleton, the wave bends the elbow 90 degrees halfway.
		const AnimationClipBinding clipBinding = wave->Bind(*skeleton);
		std::vector<JointPose> pose = skeleton->GetRestPose();
		wave->Sample(clipBinding, 0.5f, pose);
		CHECK(glm::degrees(glm::angle(pose[2].Rotation)) == doctest::Approx(90.0f).epsilon(1e-3));

		// The model draws the skinned mesh under its root, where glTF puts it: the drawing node's transform is ignored.
		Ref<Model> model = Model::Deserialize(output.Result.Data, &error);
		REQUIRE_MESSAGE(model, error);
		Scene scene;
		const std::vector<Entity> roots = model->Instantiate(scene);
		REQUIRE(roots.size() == 1);
		Entity arm = FindByName(roots[0], "Arm");
		REQUIRE(arm);
		CHECK(arm.GetParent() == roots[0]);
		CHECK(arm.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
		CHECK(arm.GetComponent<MeshRendererComponent>().Mesh == DeriveSubAssetHandle(output.Handle, "Mesh/0"));

		// The root is the model's animator, playing the first animation; the skinned mesh follows its pose.
		REQUIRE(arm.HasComponent<SkinComponent>());
		CHECK(arm.GetComponent<SkinComponent>().Animator == roots[0].GetUUID());
		REQUIRE(roots[0].HasComponent<AnimatorComponent>());
		const AnimatorComponent& animator = roots[0].GetComponent<AnimatorComponent>();
		CHECK(animator.Skeleton == DeriveSubAssetHandle(output.Handle, "Skeleton"));
		CHECK(animator.Clip == DeriveSubAssetHandle(output.Handle, "Animation/Wave"));
		CHECK(animator.Controller == UUID::Null());
		CHECK(animator.PlayOnStart);
		// Joints without content are only part of the pose: they are no entities.
		for (const char* joint : { "Armature", "Shoulder", "Elbow", "Wrist" })
		{
			CAPTURE(joint);
			CHECK_FALSE(FindByName(roots[0], joint));
		}
		CHECK(roots[0].GetChildren().size() == 1);
		// The model references its skeleton and clip through the animator.
		std::vector<AssetHandle> references;
		REQUIRE(AssetDependencies::Scan(AssetType::Model, output.Result.Data, references));
		CHECK(std::find(references.begin(), references.end(), animator.Skeleton) != references.end());
		CHECK(std::find(references.begin(), references.end(), animator.Clip) != references.end());
	}

	TEST_CASE("Joints that carry meshes or other nodes become entities that follow their joint")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfSkinAttachments");
		nlohmann::json document = Tests::CreateSkinnedArmDocument();
		// A sword in the hand: a node below the wrist drawing the mesh rigidly.
		document["nodes"].push_back({ { "name", "Sword" }, { "mesh", 0 }, { "translation", { 0.0, 0.5, 0.0 } } });
		document["nodes"][4]["children"] = { document["nodes"].size() - 1 };
		// A propeller at the top level that the wave's sampler spins: animated, so a joint of the skeleton.
		document["nodes"].push_back({ { "name", "Propeller" }, { "mesh", 0 }, { "translation", { 2.0, 0.0, 0.0 } } });
		document["scenes"][0]["nodes"].push_back(document["nodes"].size() - 1);
		document["animations"][0]["channels"].push_back({ { "sampler", 0 }, { "target", { { "node", document["nodes"].size() - 1 }, { "path", "rotation" } } } });
		const std::string text = JsonUtils::Dump(document);
		const SkinImport output = ImportModel(assets, "Attachments.gltf", std::vector<uint8_t>(text.begin(), text.end()));
		REQUIRE_MESSAGE(output.Success, output.Error);
		CHECK(output.Result.Warnings.empty());

		const ImportedSubAsset* skeletonData = output.Find("Skeleton");
		REQUIRE(skeletonData);
		std::string error;
		Ref<Skeleton> skeleton = Skeleton::Deserialize(skeletonData->Data, &error);
		REQUIRE_MESSAGE(skeleton, error);
		CHECK(skeleton->FindJoint("Propeller") != c_NoJoint);
		CHECK(skeleton->FindJoint("Sword") == c_NoJoint);

		Ref<Model> model = Model::Deserialize(output.Result.Data, &error);
		REQUIRE_MESSAGE(model, error);
		Scene scene;
		const std::vector<Entity> roots = model->Instantiate(scene);
		REQUIRE(roots.size() == 1);
		const UUID root = roots[0].GetUUID();

		// The wrist carries the sword, so it is an entity under the root at the wrist's rest pose.
		Entity wrist = FindByName(roots[0], "Wrist");
		REQUIRE(wrist);
		CHECK(wrist.GetParent() == roots[0]);
		REQUIRE(wrist.HasComponent<AttachToJointComponent>());
		CHECK(wrist.GetComponent<AttachToJointComponent>().Animator == root);
		CHECK(wrist.GetComponent<AttachToJointComponent>().Joint == "Wrist");
		CHECK(glm::all(glm::epsilonEqual(wrist.GetComponent<TransformComponent>().Translation, glm::vec3(0.0f, 2.0f, 0.0f), 1e-6f)));
		CHECK_FALSE(wrist.HasComponent<MeshRendererComponent>());
		Entity sword = FindByName(roots[0], "Sword");
		REQUIRE(sword);
		CHECK(sword.GetParent() == wrist);
		CHECK(sword.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 0.5f, 0.0f));
		CHECK(sword.GetComponent<MeshRendererComponent>().Mesh == DeriveSubAssetHandle(output.Handle, "Mesh/0/Rigid"));
		CHECK_FALSE(sword.HasComponent<AttachToJointComponent>());

		// The propeller draws a mesh and is animated: it follows its own joint.
		Entity propeller = FindByName(roots[0], "Propeller");
		REQUIRE(propeller);
		CHECK(propeller.GetParent() == roots[0]);
		CHECK(propeller.GetComponent<AttachToJointComponent>().Joint == "Propeller");
		CHECK(propeller.GetComponent<TransformComponent>().Translation == glm::vec3(2.0f, 0.0f, 0.0f));
		CHECK(propeller.HasComponent<MeshRendererComponent>());
		for (const char* joint : { "Armature", "Shoulder", "Elbow" })
		{
			CAPTURE(joint);
			CHECK_FALSE(FindByName(roots[0], joint));
		}
	}

	TEST_CASE("Skeleton, clip and mesh handles and data are stable across re-imports")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfSkinStable");
		const SkinImport first = ImportModel(assets, "Arm.gltf", Tests::CreateSkinnedArmGltf());
		const SkinImport second = ImportModel(assets, "Arm.gltf", Tests::CreateSkinnedArmGltf());
		REQUIRE(first.Success);
		REQUIRE(second.Success);
		REQUIRE(first.Result.SubAssets.size() == second.Result.SubAssets.size());
		for (size_t index = 0; index < first.Result.SubAssets.size(); index++)
		{
			CAPTURE(first.Result.SubAssets[index].Key);
			CHECK(first.Result.SubAssets[index].Key == second.Result.SubAssets[index].Key);
			CHECK(first.Result.SubAssets[index].Data == second.Result.SubAssets[index].Data);
		}
		CHECK(first.Result.Data == second.Result.Data);
		CHECK(DeriveSubAssetHandle(first.Handle, "Skeleton") == DeriveSubAssetHandle(second.Handle, "Skeleton"));
		CHECK(DeriveSubAssetHandle(first.Handle, "Animation/Wave") != DeriveSubAssetHandle(first.Handle, "Animation/Reach"));
	}

	TEST_CASE("Skins without inverse bind matrices, unnamed animations and meshes drawn with and without a skin import")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfSkinVariants");
		nlohmann::json document = Tests::CreateSkinnedArmDocument({ .InverseBindMatrices = false });
		// The same mesh drawn rigidly by another node, and the animations without names.
		document["nodes"].push_back({ { "name", "Statue" }, { "mesh", 0 }, { "translation", { 3.0, 0.0, 0.0 } } });
		document["scenes"][0]["nodes"].push_back(document["nodes"].size() - 1);
		document["animations"][0].erase("name");
		document["animations"][1]["name"] = "";
		const std::string text = JsonUtils::Dump(document);
		const SkinImport output = ImportModel(assets, "Variants.gltf", std::vector<uint8_t>(text.begin(), text.end()));
		REQUIRE_MESSAGE(output.Success, output.Error);

		const ImportedSubAsset* skinned = output.Find("Mesh/0");
		const ImportedSubAsset* rigid = output.Find("Mesh/0/Rigid");
		REQUIRE(skinned);
		REQUIRE(rigid);
		std::string error;
		Ref<Mesh> skinnedMesh = Mesh::Deserialize(skinned->Data, &error);
		Ref<Mesh> rigidMesh = Mesh::Deserialize(rigid->Data, &error);
		REQUIRE(skinnedMesh);
		REQUIRE(rigidMesh);
		CHECK(skinnedMesh->IsSkinned());
		CHECK_FALSE(rigidMesh->IsSkinned());
		// Without inverse bind matrices they are identities.
		CHECK(skinnedMesh->GetSkinBinding()->InverseBindMatrices[2] == glm::mat4(1.0f));
		CHECK(output.Find("Animation/0"));
		CHECK(output.Find("Animation/1"));

		Ref<Model> model = Model::Deserialize(output.Result.Data, &error);
		REQUIRE(model);
		Scene scene;
		const std::vector<Entity> roots = model->Instantiate(scene);
		Entity statue = FindByName(roots[0], "Statue");
		REQUIRE(statue);
		CHECK(statue.GetComponent<MeshRendererComponent>().Mesh == DeriveSubAssetHandle(output.Handle, "Mesh/0/Rigid"));
		CHECK(statue.GetComponent<TransformComponent>().Translation == glm::vec3(3.0f, 0.0f, 0.0f));
	}

	TEST_CASE("Broken skins and animation channels are skipped with a warning")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfSkinBroken");
		nlohmann::json document = Tests::CreateSkinnedArmDocument();
		// A morph target weight channel is skipped (the rest of the animation imports). The mesh gets a morph target (the
		// normals stand in for its displacements) and the wave a sampler of its weight (its times stand in for the values).
		nlohmann::json& primitive = document["meshes"][0]["primitives"][0];
		primitive["targets"] = nlohmann::json::array({ { { "POSITION", primitive["attributes"]["NORMAL"] } } });
		nlohmann::json& wave = document["animations"][0];
		const nlohmann::json times = wave["samplers"][0]["input"];
		wave["samplers"].push_back({ { "input", times }, { "output", times }, { "interpolation", "LINEAR" } });
		wave["channels"].push_back({ { "sampler", 1 }, { "target", { { "node", 0 }, { "path", "weights" } } } });
		const std::string text = JsonUtils::Dump(document);
		const SkinImport output = ImportModel(assets, "Morph.gltf", std::vector<uint8_t>(text.begin(), text.end()));
		REQUIRE_MESSAGE(output.Success, output.Error);
		CHECK(HasWarningContaining(output.Result, "morph target animation"));
		CHECK(output.Find("Animation/Wave"));

		// Unreadable inverse bind matrices leave the mesh rigid.
		nlohmann::json projective = Tests::CreateSkinnedArmDocument();
		const size_t matrices = projective["skins"][0]["inverseBindMatrices"].get<size_t>();
		projective["accessors"][matrices]["count"] = 2; // Fewer matrices than joints
		const std::string projectiveText = JsonUtils::Dump(projective);
		const SkinImport unreadable = ImportModel(assets, "Unreadable.gltf", std::vector<uint8_t>(projectiveText.begin(), projectiveText.end()));
		if (unreadable.Success)
		{
			CHECK(HasWarningContaining(unreadable.Result, "inverse bind matrices"));
			const ImportedSubAsset* mesh = unreadable.Find("Mesh/0");
			REQUIRE(mesh);
			std::string error;
			Ref<Mesh> loaded = Mesh::Deserialize(mesh->Data, &error);
			REQUIRE(loaded);
			CHECK_FALSE(loaded->IsSkinned());
		}
		else
		{
			// cgltf's validation may refuse the file outright, which is fine too.
			CHECK_FALSE(unreadable.Error.empty());
		}
	}
}
