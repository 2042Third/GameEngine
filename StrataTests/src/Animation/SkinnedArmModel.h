#pragma once

#include "Animation/SkinnedGltf.h"
#include "Strata/Animation/AnimationClip.h"
#include "Strata/Animation/Skeleton.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/GltfImporter.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Scene/Prefab.h"
#include "TestHelpers.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace Strata::Tests
{

	// The skinned arm (SkinnedGltf.h) imported by the glTF importer, its sub-assets (mesh, skeleton, clips, materials)
	// added to an asset manager as memory assets under the handles the model's entities name.
	struct ImportedArm
	{
		AssetHandle Handle = UUID::Null();
		Ref<Model> ArmModel;
		AssetHandle Mesh = UUID::Null();
		AssetHandle Skeleton = UUID::Null();
		ImportResult Result;
	};

	// `settings` overrides the importer's defaults (e.g. {"Scale": 0.01}). `handle` is the model's handle: arms imported
	// with different handles have sub-assets of their own.
	inline ImportedArm ImportSkinnedArm(AssetManagerBase& manager, const SkinnedArmOptions& options, uint64_t handle,
		const nlohmann::json& settings = nlohmann::json::object())
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("SkinnedArm");
		const std::filesystem::path path = directory / "Arm.gltf";
		REQUIRE(FileSystem::WriteBytes(path, CreateSkinnedArmGltf(options)));
		GltfImporter importer;
		ImportContext context;
		context.Handle = UUID(handle);
		context.SourcePath = path;
		context.AssetDirectory = directory;
		context.Settings = importer.GetDefaultSettings(path);
		for (const auto& [key, value] : settings.items())
			context.Settings[key] = value;
		ImportedArm arm;
		arm.Handle = context.Handle;
		std::string error;
		REQUIRE_MESSAGE(importer.Import(context, arm.Result, &error), error);
		for (const ImportedSubAsset& subAsset : arm.Result.SubAssets)
		{
			Ref<Asset> asset;
			switch (subAsset.Type)
			{
				case AssetType::Mesh: asset = Mesh::Deserialize(subAsset.Data, &error); break;
				case AssetType::Skeleton: asset = Skeleton::Deserialize(subAsset.Data, &error); break;
				case AssetType::AnimationClip: asset = AnimationClip::Deserialize(subAsset.Data, &error); break;
				case AssetType::Material: asset = Material::Deserialize(subAsset.Data, &error); break;
				default: FAIL("the arm imports no ", AssetTypeToString(subAsset.Type), " sub-assets");
			}
			REQUIRE_MESSAGE(asset, error);
			AssetMetadata metadata;
			metadata.Handle = DeriveSubAssetHandle(arm.Handle, subAsset.Key);
			metadata.Name = subAsset.Name;
			manager.AddMemoryAsset(asset, metadata);
			if (subAsset.Type == AssetType::Mesh)
				arm.Mesh = metadata.Handle;
			else if (subAsset.Type == AssetType::Skeleton)
				arm.Skeleton = metadata.Handle;
		}
		arm.ArmModel = Model::Deserialize(arm.Result.Data, &error);
		REQUIRE_MESSAGE(arm.ArmModel, error);
		return arm;
	}

}
