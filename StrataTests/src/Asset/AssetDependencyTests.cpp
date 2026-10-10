#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Strata/Asset/AssetDependencies.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Asset/EditorAssetManager.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/SceneSerializer.h"
#include "TestHelpers.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Every asset the entities of a scene reference, found by walking the live components through reflection: the values
	// of the asset properties of every registered component, and the script fields of asset type.
	std::set<AssetHandle> WalkSceneReferences(Scene& scene)
	{
		std::set<AssetHandle> references;
		entt::registry& registry = scene.GetRegistry();
		for (const entt::entity entity : registry.view<IDComponent>())
		{
			for (const ComponentInfo* info : ComponentRegistry::GetAll())
			{
				if (!info->Has(registry, entity))
					continue;
				const void* component = info->Get(registry, entity);
				for (const PropertyInfo& property : info->Properties)
				{
					if (property.Type != PropertyType::Asset || HasFlag(property.Flags, PropertyFlags::Transient))
						continue;
					const UUID value = std::get<UUID>(property.GetValue(component));
					if (value.IsValid())
						references.insert(value);
				}
			}
			if (const ScriptComponent* scripts = registry.try_get<ScriptComponent>(entity))
			{
				for (const ScriptEntry& script : scripts->Scripts)
				{
					for (const ScriptFieldValue& field : script.Fields)
					{
						if (field.Type == PropertyType::Asset && std::get<UUID>(field.Value).IsValid())
							references.insert(std::get<UUID>(field.Value));
					}
				}
			}
		}
		return references;
	}

	std::vector<uint8_t> ToBytes(const nlohmann::json& json)
	{
		const std::string text = JsonUtils::Dump(json, 1, '\t');
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// A project of engine-native assets written with chosen handles.
	struct NativeProject
	{
		std::filesystem::path Root;
		std::filesystem::path Assets;
		std::filesystem::path Cache;

		explicit NativeProject(const std::string& name)
			: Root(CreateTemporaryDirectory(name)), Assets(Root / "Assets"), Cache(Root / ".strata" / "Cache")
		{
			REQUIRE(FileSystem::CreateDirectories(Assets));
		}

		void Write(const std::string& relativePath, uint64_t handle, const nlohmann::json& document) const
		{
			const std::filesystem::path path = Assets / FileSystem::FromUTF8(relativePath);
			REQUIRE(FileSystem::CreateDirectories(path.parent_path()));
			REQUIRE(FileSystem::WriteBytes(path, ToBytes(document)));
			std::filesystem::path meta = path;
			meta += ".meta";
			REQUIRE(FileSystem::WriteText(meta, JsonUtils::Dump(nlohmann::json { { "Handle", UUIDToJson(UUID(handle)) } })));
		}

		void WriteMaterial(const std::string& relativePath, uint64_t handle, uint64_t baseColorMap = 0, uint64_t normalMap = 0) const
		{
			MaterialProperties properties;
			properties.BaseColorMap = UUID(baseColorMap);
			properties.NormalMap = UUID(normalMap);
			Write(relativePath, handle, Material::Create(properties)->Serialize());
		}

		// A scene whose entities draw the given meshes with the given materials.
		void WriteScene(const std::string& relativePath, uint64_t handle, const std::vector<std::pair<uint64_t, uint64_t>>& meshMaterials) const
		{
			Scene scene("Scene");
			for (const auto& [mesh, material] : meshMaterials)
			{
				Entity entity = scene.CreateEntity("Drawn");
				MeshRendererComponent& renderer = entity.AddComponent<MeshRendererComponent>();
				renderer.Mesh = UUID(mesh);
				renderer.Material = UUID(material);
			}
			Write(relativePath, handle, SceneSerializer::Serialize(scene));
		}

		Ref<EditorAssetManager> Open() const
		{
			EditorAssetManagerSpecification specification;
			specification.AssetDirectory = Assets;
			specification.CacheDirectory = Cache;
			specification.WatchFiles = false;
			Ref<EditorAssetManager> manager = CreateRef<EditorAssetManager>(specification);
			manager->Scan();
			return manager;
		}
	};

	std::vector<AssetHandle> Handles(std::initializer_list<uint64_t> values)
	{
		std::vector<AssetHandle> handles;
		for (uint64_t value : values)
			handles.push_back(UUID(value));
		return handles;
	}

	std::vector<std::string> Paths(const std::vector<AssetPackSource>& assets)
	{
		std::vector<std::string> paths;
		for (const AssetPackSource& asset : assets)
			paths.push_back(asset.Metadata.Path + (asset.Metadata.SubAssetKey.empty() ? "" : "#" + asset.Metadata.SubAssetKey));
		return paths;
	}

}

TEST_SUITE("Asset.Dependencies")
{
	TEST_CASE("The feature scene's dependencies are what its entities reference, in the editor and in the exported pack")
	{
		FeatureProject project;
		const Ref<EditorAssetManager>& editor = project.GetAssetManager();
		const AssetHandle sceneHandle = project.GetProject()->GetConfig().StartScene;
		const Ref<Scene> scene = project.LoadStartScene();
		const std::set<AssetHandle> walked = WalkSceneReferences(*scene);

		const std::vector<AssetHandle> dependencies = editor->GetDependencies(sceneHandle);
		CHECK(std::set<AssetHandle>(dependencies.begin(), dependencies.end()) == walked);
		CHECK(dependencies.size() == walked.size()); // Each once

		// The walk is meaningful: the scene references every kind of asset a scene can.
		std::set<AssetType> types;
		for (AssetHandle handle : walked)
			types.insert(editor->GetAssetType(handle));
		for (AssetType type : { AssetType::Mesh, AssetType::Material, AssetType::Texture, AssetType::Prefab, AssetType::AudioClip, AssetType::Font })
		{
			CAPTURE(AssetTypeToString(type));
			CHECK(types.count(type) == 1);
		}

		// Materials reference their textures, the model's meshes their materials and carry their bounds, the prefab what its
		// entities use; the closure reaches the textures through the materials.
		std::set<AssetHandle> closure;
		for (AssetHandle handle : editor->GetDependencyClosure(sceneHandle))
			closure.insert(handle);
		bool meshWithBounds = false;
		for (const AssetMetadata& metadata : editor->GetAllMetadata())
		{
			if (metadata.IsBuiltin())
				continue;
			CAPTURE(metadata.Path);
			CAPTURE(metadata.SubAssetKey);
			if (metadata.Type == AssetType::Material)
			{
				const Ref<Material> material = std::static_pointer_cast<Material>(editor->LoadAssetSync(metadata.Handle));
				REQUIRE(material);
				// Once each, also when a texture serves several maps.
				const std::vector<AssetHandle> textures = material->GetTextureHandles();
				const std::vector<AssetHandle> materialDependencies = editor->GetDependencies(metadata.Handle);
				CHECK(std::set<AssetHandle>(materialDependencies.begin(), materialDependencies.end()) == std::set<AssetHandle>(textures.begin(), textures.end()));
			}
			if (metadata.Type == AssetType::Mesh && metadata.IsSubAsset())
			{
				const std::optional<AABB> bounds = editor->GetBounds(metadata.Handle);
				REQUIRE(bounds.has_value());
				const Ref<Mesh> mesh = std::static_pointer_cast<Mesh>(editor->LoadAssetSync(metadata.Handle));
				REQUIRE(mesh);
				CHECK(bounds->Min == mesh->GetBounds().Min);
				CHECK(bounds->Max == mesh->GetBounds().Max);
				meshWithBounds = true;
				for (const Submesh& submesh : mesh->GetSubmeshes())
				{
					const std::vector<AssetHandle> meshDependencies = editor->GetDependencies(metadata.Handle);
					CHECK(std::find(meshDependencies.begin(), meshDependencies.end(), submesh.Material) != meshDependencies.end());
				}
			}
			if (metadata.Type == AssetType::Prefab)
			{
				const Ref<Prefab> prefab = std::static_pointer_cast<Prefab>(editor->LoadAssetSync(metadata.Handle));
				REQUIRE(prefab);
				std::vector<AssetHandle> expected;
				AssetDependencies::ScanEntities(prefab->GetSnapshot(), expected);
				CHECK(editor->GetDependencies(metadata.Handle) == expected);
			}
			if (metadata.Type == AssetType::Texture)
			{
				CHECK(editor->GetDependencies(metadata.Handle).empty());
				CHECK_FALSE(editor->GetBounds(metadata.Handle).has_value());
			}
		}
		CHECK(meshWithBounds);

		// The exported pack answers the same, for every asset, before anything loads.
		const std::filesystem::path packPath = project.GetDirectory() / "Build" / "Feature.stpak";
		std::string error;
		REQUIRE_MESSAGE(editor->BuildAssetPack(packPath, &error), error);
		const Ref<RuntimeAssetManager> runtime = RuntimeAssetManager::Create(packPath, &error);
		REQUIRE_MESSAGE(runtime, error);
		CHECK(runtime->GetDependencies(sceneHandle) == dependencies);
		for (const AssetMetadata& metadata : editor->GetAllMetadata())
		{
			if (metadata.IsBuiltin())
				continue;
			CAPTURE(metadata.Path);
			CAPTURE(metadata.SubAssetKey);
			CHECK(runtime->GetDependencies(metadata.Handle) == editor->GetDependencies(metadata.Handle));
			const std::optional<AABB> editorBounds = editor->GetBounds(metadata.Handle);
			const std::optional<AABB> runtimeBounds = runtime->GetBounds(metadata.Handle);
			REQUIRE(editorBounds.has_value() == runtimeBounds.has_value());
			if (editorBounds)
			{
				CHECK(editorBounds->Min == runtimeBounds->Min);
				CHECK(editorBounds->Max == runtimeBounds->Max);
			}
			CHECK(runtime->GetAssetState(metadata.Handle) == AssetState::Unloaded);
		}
		CHECK(runtime->GetDependencyClosure(sceneHandle) == editor->GetDependencyClosure(sceneHandle));
	}

	TEST_CASE("Engine-native assets record their references per content and follow their changes")
	{
		NativeProject project("NativeReferences");
		project.WriteMaterial("Materials/Brick.stmat", 0x5001, 0x9001, 0x9002);
		project.WriteMaterial("Materials/Plain.stmat", 0x5002);
		project.WriteScene("Scenes/Main.stscene", 0x5003, { { 0x9101, 0x5001 }, { 0x9101, 0x5002 }, { 0x9102, 0x5001 } });

		std::filesystem::path record;
		{
			Ref<EditorAssetManager> manager = project.Open();
			CHECK(manager->GetDependencies(UUID(0x5001)) == Handles({ 0x9001, 0x9002 }));
			CHECK(manager->GetDependencies(UUID(0x5002)).empty());
			// Each reference once, in the order the scene first names it.
			CHECK(manager->GetDependencies(UUID(0x5003)) == Handles({ 0x9101, 0x5001, 0x5002, 0x9102 }));
			CHECK(manager->GetDependencyClosure(UUID(0x5003)) == Handles({ 0x5003, 0x5001, 0x5002 })); // Only registered assets

			// Saving through the editor records the new references at once.
			MaterialProperties properties;
			properties.EmissiveMap = UUID(0x9003);
			REQUIRE(manager->SaveNativeAsset(UUID(0x5002), ToBytes(Material::Create(properties)->Serialize()), true));
			CHECK(manager->GetDependencies(UUID(0x5002)) == Handles({ 0x9003 }));
			record = project.Cache / (UUID(0x5002).ToString() + ".import");
			REQUIRE(FileSystem::IsRegularFile(record));

			// A caller that serialized the document passes what it references, which is recorded as it is: the document is
			// not parsed again (the editor saving a large scene).
			const std::vector<AssetHandle> given = Handles({ 0x9008 });
			REQUIRE(manager->SaveNativeAsset(UUID(0x5003), ToBytes(SceneSerializer::Serialize(Scene("Main"))), false, nullptr, &given));
			CHECK(manager->GetDependencies(UUID(0x5003)) == given);

			// A file changed on disk (a watcher's report, asset.reimport) is searched on an I/O job, and recorded by the next
			// update; when it changes again meanwhile, the latest search wins.
			project.WriteMaterial("Materials/Brick.stmat", 0x5001, 0x9006);
			REQUIRE(manager->ReimportAsset(UUID(0x5001)));
			CHECK(manager->HasPendingImports());
			project.WriteMaterial("Materials/Brick.stmat", 0x5001, 0x9007);
			REQUIRE(manager->ReimportAsset(UUID(0x5001)));
			manager->WaitForImports();
			CHECK_FALSE(manager->HasPendingImports());
			CHECK(manager->GetDependencies(UUID(0x5001)) == Handles({ 0x9007 }));
			project.WriteMaterial("Materials/Brick.stmat", 0x5001, 0x9001, 0x9002);
			REQUIRE(manager->ReimportAsset(UUID(0x5001)));
			manager->WaitForImports();
		}

		// A record of an unchanged file is used as it is: a planted reference shows that the file was not scanned again.
		std::optional<std::string> text = FileSystem::ReadText(record);
		REQUIRE(text);
		std::optional<nlohmann::json> json = JsonUtils::Parse(*text);
		REQUIRE(json);
		(*json)["References"] = nlohmann::json::array({ UUIDToJson(UUID(0x7777)) });
		REQUIRE(FileSystem::WriteText(record, JsonUtils::Dump(*json, 1, '\t')));
		{
			Ref<EditorAssetManager> manager = project.Open();
			CHECK(manager->GetDependencies(UUID(0x5002)) == Handles({ 0x7777 }));
		}

		// A file changed outside the editor is scanned again.
		project.WriteMaterial("Materials/Plain.stmat", 0x5002, 0x9004);
		{
			Ref<EditorAssetManager> manager = project.Open();
			CHECK(manager->GetDependencies(UUID(0x5002)) == Handles({ 0x9004 }));
		}

		// Stored ranges read only what they ask for.
		Ref<EditorAssetManager> manager = project.Open();
		const std::optional<std::vector<uint8_t>> file = FileSystem::ReadBytes(project.Assets / "Materials" / "Brick.stmat");
		REQUIRE(file);
		std::vector<uint8_t> range;
		std::string error;
		REQUIRE(manager->ReadStoredRange(UUID(0x5001), 5, 20, range, &error));
		CHECK(std::vector<uint8_t>(file->begin() + 5, file->begin() + 25) == range);
		CHECK_FALSE(manager->ReadStoredRange(UUID(0x5001), file->size() - 2, 3, range, &error));
		CHECK(error.find("outside") != std::string::npos);
		CHECK_FALSE(manager->ReadStoredRange(UUID(0x9999), 0, 1, range, &error));
	}

	TEST_CASE("Packs are ordered by first use, and pruning keeps what scenes and AlwaysInclude use")
	{
		NativeProject project("PackPlan");
		project.WriteMaterial("Materials/A.stmat", 0x6001);
		project.WriteMaterial("Materials/B.stmat", 0x6002);
		project.WriteMaterial("Materials/Unused.stmat", 0x6003);
		project.WriteMaterial("Materials/ForScripts.stmat", 0x6004);
		{
			Scene prefabScene("Prefab");
			Entity enemy = prefabScene.CreateEntity("Enemy");
			enemy.AddComponent<MeshRendererComponent>().Material = UUID(0x6004);
			project.Write("Prefabs/Enemy.stprefab", 0x6005, Prefab::CreateFromEntities(prefabScene, { enemy })->Serialize());
		}
		project.WriteScene("Scenes/Level1.stscene", 0x6010, { { 0x0, 0x6001 } });
		project.WriteScene("Scenes/Level2.stscene", 0x6011, { { 0x0, 0x6002 }, { 0x0, 0x6001 } });
		project.WriteScene("Scenes/Menu.stscene", 0x6012, { { 0x0, 0x6002 } });
		Ref<EditorAssetManager> manager = project.Open();

		// Start scene first with what it uses, then the other scenes by path, then the rest by path.
		AssetPackBuildOptions options;
		options.StartScene = UUID(0x6011);
		AssetPackBuildPlan plan;
		std::string error;
		REQUIRE_MESSAGE(manager->PlanAssetPack(options, plan, &error), error);
		CHECK(Paths(plan.Assets) == std::vector<std::string> { "Scenes/Level2.stscene", "Materials/B.stmat", "Materials/A.stmat", "Scenes/Level1.stscene",
			"Scenes/Menu.stscene", "Materials/ForScripts.stmat", "Materials/Unused.stmat", "Prefabs/Enemy.stprefab" });
		CHECK(plan.Excluded.empty());
		CHECK(plan.Assets[0].Dependencies == Handles({ 0x6002, 0x6001 }));

		// Pruned: what the scenes use, and what AlwaysInclude names with what that uses.
		options.Prune = true;
		REQUIRE_MESSAGE(manager->PlanAssetPack(options, plan, &error), error);
		CHECK(Paths(plan.Assets) == std::vector<std::string> { "Scenes/Level2.stscene", "Materials/B.stmat", "Materials/A.stmat", "Scenes/Level1.stscene",
			"Scenes/Menu.stscene" });
		REQUIRE(plan.Excluded.size() == 3);
		options.AlwaysInclude = { "Prefabs/" };
		REQUIRE_MESSAGE(manager->PlanAssetPack(options, plan, &error), error);
		CHECK(Paths(plan.Assets).back() == "Materials/ForScripts.stmat"); // With what the prefab uses
		REQUIRE(plan.Excluded.size() == 1);
		CHECK(plan.Excluded[0].Path == "Materials/Unused.stmat");
		options.AlwaysInclude = { "Materials\\Unused.stmat", "./Prefabs/Enemy.stprefab" };
		REQUIRE_MESSAGE(manager->PlanAssetPack(options, plan, &error), error);
		CHECK(plan.Excluded.size() == 0);
		options.AlwaysInclude = { "Prefabs/Missing.stprefab" };
		CHECK_FALSE(manager->PlanAssetPack(options, plan, &error));
		CHECK(error.find("Prefabs/Missing.stprefab") != std::string::npos);
		options.AlwaysInclude = { "Pref" }; // Not a folder: names have to match whole
		CHECK_FALSE(manager->PlanAssetPack(options, plan, &error));
		CHECK(manager->GetPackPlanCount() == 6);

		// Lists are checked as a pruned plan checks them, without planning.
		CHECK(manager->CheckAlwaysInclude({}, &error));
		CHECK(manager->CheckAlwaysInclude({ "Prefabs/", "Materials\\Unused.stmat", "./Prefabs/Enemy.stprefab", "Prefabs", "Scenes/Menu.stscene" }, &error));
		error.clear();
		CHECK_FALSE(manager->CheckAlwaysInclude({ "Prefabs", "Prefabs/Missing.stprefab", "Pref" }, &error));
		CHECK(error == "Export.AlwaysInclude names 'Prefabs/Missing.stprefab', which is no asset or folder of the project");
		CHECK_FALSE(manager->CheckAlwaysInclude({ "Pref" }, &error));
		CHECK_FALSE(manager->CheckAlwaysInclude({ "" }, &error));
		CHECK_FALSE(manager->CheckAlwaysInclude({ "Materials/A.stmat/More" }, &error)); // Below a file
		CHECK(manager->GetPackPlanCount() == 6);

		// The pruned pack holds the plan, in its order.
		options.AlwaysInclude = { "Prefabs" };
		const std::filesystem::path packPath = project.Root / "Build" / "Game.stpak";
		AssetPackBuildPlan built;
		REQUIRE_MESSAGE(manager->BuildAssetPack(packPath, &error, options, &built), error);
		const Ref<RuntimeAssetManager> runtime = RuntimeAssetManager::Create(packPath, &error);
		REQUIRE_MESSAGE(runtime, error);
		REQUIRE(runtime->GetPack().GetEntries().size() == built.Assets.size());
		for (size_t index = 0; index < built.Assets.size(); index++)
			CHECK(runtime->GetPack().GetEntries()[index].Metadata.Handle == built.Assets[index].Metadata.Handle);
		CHECK_FALSE(runtime->IsHandleValid(UUID(0x6003)));
		CHECK(runtime->IsHandleValid(UUID(0x6004)));
	}

	TEST_CASE("A pack is never written from documents that changed what they reference since its plan")
	{
		NativeProject project("PackPlanChanged");
		project.WriteMaterial("Materials/Used.stmat", 0x6101);
		project.WriteMaterial("Materials/Late.stmat", 0x6102);
		project.WriteScene("Scenes/Main.stscene", 0x6110, { { 0x0, 0x6101 } });
		project.WriteScene("Scenes/Level2.stscene", 0x6111, { { 0x0, 0x6101 } });
		Ref<EditorAssetManager> manager = project.Open();
		AssetPackBuildOptions options;
		options.StartScene = UUID(0x6110);
		options.Prune = true; // Late.stmat is used by no scene: not planned
		AssetPackBuildPlan plan;
		std::string error;
		REQUIRE_MESSAGE(manager->PlanAssetPack(options, plan, &error), error);
		REQUIRE(plan.Excluded.size() == 1);

		// Saved after the plan without new references (another material on the same asset): written as it is now.
		const std::filesystem::path packPath = project.Root / "Build" / "Game.stpak";
		MaterialProperties properties;
		properties.Roughness = 0.25f;
		REQUIRE(manager->SaveNativeAsset(UUID(0x6101), ToBytes(Material::Create(properties)->Serialize()), true));
		REQUIRE_MESSAGE(manager->WriteAssetPack(packPath, plan, {}, nullptr, &error), error);
		const Ref<RuntimeAssetManager> runtime = RuntimeAssetManager::Create(packPath, &error);
		REQUIRE_MESSAGE(runtime, error);
		const Ref<Material> written = std::static_pointer_cast<Material>(runtime->LoadAssetSync(UUID(0x6101)));
		REQUIRE(written);
		CHECK(written->GetProperties().Roughness == 0.25f);

		// A scene saved after the plan with a reference the plan did not record would ship naming an asset the pack lacks:
		// the write fails, naming the scene and what it references now, and leaves no pack behind.
		Scene changed("Level2");
		for (uint64_t material : { 0x6101, 0x6102 })
			changed.CreateEntity("Drawn").AddComponent<MeshRendererComponent>().Material = UUID(material);
		REQUIRE(manager->SaveNativeAsset(UUID(0x6111), ToBytes(SceneSerializer::Serialize(changed)), false));
		const std::filesystem::path failedPath = project.Root / "Build" / "Failed.stpak";
		CHECK_FALSE(manager->WriteAssetPack(failedPath, plan, {}, nullptr, &error));
		INFO(error);
		CHECK(error.find("Scenes/Level2.stscene") != std::string::npos);
		CHECK(error.find("changed after the export started") != std::string::npos);
		CHECK(error.find("Materials/Late.stmat") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(failedPath));

		// A document that cannot be read any more fails the write too, saying so.
		REQUIRE(FileSystem::WriteText(project.Assets / "Scenes" / "Level2.stscene", "{ not a scene"));
		CHECK_FALSE(manager->WriteAssetPack(failedPath, plan, {}, nullptr, &error));
		CHECK(error.find("Scenes/Level2.stscene") != std::string::npos);
		CHECK(error.find("cannot be read as a document") != std::string::npos);
		REQUIRE(manager->SaveNativeAsset(UUID(0x6111), ToBytes(SceneSerializer::Serialize(changed)), false));

		// Planned again, it ships with what it uses.
		REQUIRE_MESSAGE(manager->PlanAssetPack(options, plan, &error), error);
		CHECK(plan.Excluded.empty());
		REQUIRE_MESSAGE(manager->WriteAssetPack(failedPath, plan, {}, nullptr, &error), error);
	}

	TEST_CASE("Documents that reference themselves plan, write and load like any other")
	{
		// A level whose script loads it again and a prefab whose script spawns copies of itself (a splitting asteroid): the
		// document naming itself is no dependency, neither recorded nor when the pack is written.
		NativeProject project("PackSelfReferences");
		auto addScript = [](Entity entity, const std::string& className, const std::string& field, uint64_t asset)
		{
			ScriptEntry script;
			script.ClassName = className;
			script.Fields.push_back(ScriptFieldValue { field, PropertyType::Asset, UUID(asset) });
			entity.AddComponent<ScriptComponent>().Scripts.push_back(std::move(script));
		};
		project.WriteMaterial("Materials/Rock.stmat", 0x6201);
		{
			Scene prefabScene("Asteroid");
			Entity asteroid = prefabScene.CreateEntity("Asteroid");
			asteroid.AddComponent<MeshRendererComponent>().Material = UUID(0x6201);
			addScript(asteroid, "Splitter", "Fragment", 0x6205);
			project.Write("Prefabs/Asteroid.stprefab", 0x6205, Prefab::CreateFromEntities(prefabScene, { asteroid })->Serialize());
		}
		{
			Scene level("Level");
			addScript(level.CreateEntity("Rules"), "Rules", "Restart", 0x6210);
			addScript(level.CreateEntity("Spawner"), "Spawner", "Asteroid", 0x6205);
			project.Write("Scenes/Level.stscene", 0x6210, SceneSerializer::Serialize(level));
		}
		project.WriteScene("Scenes/Menu.stscene", 0x6211, {});
		Ref<EditorAssetManager> manager = project.Open();
		CHECK(manager->GetDependencies(UUID(0x6210)) == Handles({ 0x6205 }));
		CHECK(manager->GetDependencies(UUID(0x6205)) == Handles({ 0x6201 }));

		AssetPackBuildOptions options;
		options.StartScene = UUID(0x6210);
		options.Prune = true;
		AssetPackBuildPlan plan;
		std::string error;
		REQUIRE_MESSAGE(manager->PlanAssetPack(options, plan, &error), error);
		CHECK(Paths(plan.Assets) == std::vector<std::string> { "Scenes/Level.stscene", "Prefabs/Asteroid.stprefab", "Materials/Rock.stmat",
			"Scenes/Menu.stscene" });

		// A scene saved after the plan that now names itself references nothing the plan lacks.
		Scene menu("Menu");
		addScript(menu.CreateEntity("Rules"), "Rules", "Restart", 0x6211);
		REQUIRE(manager->SaveNativeAsset(UUID(0x6211), ToBytes(SceneSerializer::Serialize(menu)), false));
		CHECK(manager->GetDependencies(UUID(0x6211)).empty());

		const std::filesystem::path packPath = project.Root / "Build" / "Game.stpak";
		REQUIRE_MESSAGE(manager->WriteAssetPack(packPath, plan, {}, nullptr, &error), error);
		const Ref<RuntimeAssetManager> runtime = RuntimeAssetManager::Create(packPath, &error);
		REQUIRE_MESSAGE(runtime, error);
		for (uint64_t handle : { 0x6201, 0x6205, 0x6210, 0x6211 })
		{
			CAPTURE(handle);
			CHECK(runtime->GetDependencies(UUID(handle)) == manager->GetDependencies(UUID(handle)));
		}
		CHECK(runtime->GetDependencyClosure(UUID(0x6210)) == Handles({ 0x6210, 0x6205, 0x6201 }));
		const Ref<Prefab> asteroid = std::static_pointer_cast<Prefab>(runtime->LoadAssetSync(UUID(0x6205)));
		REQUIRE(asteroid);
		std::vector<AssetHandle> references;
		AssetDependencies::ScanEntities(asteroid->GetSnapshot(), references);
		CHECK(std::find(references.begin(), references.end(), UUID(0x6205)) != references.end()); // It does name itself
	}

	TEST_CASE("Mounted packs replace the entries of earlier packs and reload what they replace")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("PackMount");
		auto writeMaterials = [](const std::filesystem::path& path, const std::vector<std::pair<uint64_t, float>>& materials)
		{
			std::vector<AssetPackSource> sources;
			std::map<uint64_t, std::vector<uint8_t>> data;
			for (const auto& [handle, roughness] : materials)
			{
				MaterialProperties properties;
				properties.Roughness = roughness;
				properties.BaseColorMap = UUID(handle + 0x100);
				AssetMetadata metadata;
				metadata.Handle = UUID(handle);
				metadata.Type = AssetType::Material;
				metadata.Path = fmt::format("Materials/{}.stmat", handle);
				metadata.Name = metadata.Path;
				sources.push_back(AssetPackSource { metadata, { properties.BaseColorMap }, std::nullopt });
				data[handle] = ToBytes(Material::Create(properties)->Serialize());
			}
			std::string error;
			REQUIRE_MESSAGE(AssetPackWriter::Write(path, sources, [&data](const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string*)
			{
				outData = data.at(static_cast<uint64_t>(metadata.Handle));
				return true;
			}, {}, nullptr, &error), error);
		};
		writeMaterials(directory / "Game.stpak", { { 0x8001, 0.1f }, { 0x8002, 0.2f } });
		writeMaterials(directory / "Patch.stpak", { { 0x8002, 0.9f }, { 0x8003, 0.3f } });

		std::string error;
		const Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(directory / "Game.stpak", &error);
		REQUIRE_MESSAGE(manager, error);
		const Ref<Material> before = std::static_pointer_cast<Material>(manager->LoadAssetSync(UUID(0x8002)));
		REQUIRE(before);
		CHECK(before->GetProperties().Roughness == doctest::Approx(0.2f));
		CHECK(manager->GetDependencies(UUID(0x8002)) == Handles({ 0x8102 }));

		CHECK_FALSE(manager->Mount(directory / "Missing.stpak", &error));
		REQUIRE_MESSAGE(manager->Mount(directory / "Patch.stpak", &error), error);
		CHECK(manager->GetPacks().size() == 2);
		// The replaced asset was loaded: it reloads from the patch (the old object serves until then).
		REQUIRE(manager->WaitForPendingLoads());
		const Ref<Material> after = std::static_pointer_cast<Material>(manager->GetAsset(UUID(0x8002)));
		REQUIRE(after);
		CHECK(after->GetProperties().Roughness == doctest::Approx(0.9f));
		CHECK(std::static_pointer_cast<Material>(manager->LoadAssetSync(UUID(0x8001)))->GetProperties().Roughness == doctest::Approx(0.1f));
		CHECK(std::static_pointer_cast<Material>(manager->LoadAssetSync(UUID(0x8003)))->GetProperties().Roughness == doctest::Approx(0.3f));

		// Ranges come from the pack that provides the asset.
		std::vector<uint8_t> whole;
		std::vector<uint8_t> range;
		const AssetPackEntry& patched = manager->GetPacks()[1]->GetEntries()[0];
		REQUIRE(manager->GetPacks()[1]->ReadData(patched, whole, &error));
		REQUIRE(manager->ReadStoredRange(UUID(0x8002), 3, 10, range, &error));
		CHECK(std::vector<uint8_t>(whole.begin() + 3, whole.begin() + 13) == range);
	}
}

TEST_SUITE("Asset.Groups")
{
	TEST_CASE("A material's textures are requested in the frame its read completes, at its priority")
	{
		ScopedJobSystem jobs(2, 1); // One I/O thread: reads pass the gate in the order they were dispatched
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle first = manager->Add(0x100, MakeUsage(10), 10);
		const AssetHandle second = manager->Add(0x101, MakeUsage(10), 10);
		const AssetHandle material = manager->Add(0x200, MakeUsage(10), 10);
		const AssetHandle textureA = manager->Add(0x201, MakeUsage(10), 10);
		const AssetHandle textureB = manager->Add(0x202, MakeUsage(10), 10);
		const AssetHandle waiting = manager->Add(0x300, MakeUsage(10), 10);
		manager->SetDependencies(material, { textureA, textureB, UUID(0xDEAD) }); // Unknown handles are skipped
		manager->Update();
		const uint64_t frame = manager->GetFrameIndex();

		// Two reads hold the I/O slots (the first at the gate); the material waits at high priority, then another load at
		// normal priority.
		manager->CloseGate();
		manager->RequestLoad(first, AssetPriority::Normal);
		REQUIRE(manager->WaitForWaitingReads(1));
		manager->RequestLoad(material, AssetPriority::High);
		manager->RequestLoad(second, AssetPriority::Normal);
		manager->RequestLoad(waiting, AssetPriority::Normal);
		CHECK(manager->GetStats().QueuedLoads[0] == 0); // The material was dispatched: the first read's slot and one more
		manager->AllowReads(1); // The first read
		REQUIRE(WaitUntil([&]() { return manager->GetReadCount(first) == 1; }));
		manager->OpenGate();
		REQUIRE(WaitUntil([&]() { return manager->GetReadCount(waiting) == 1; }));

		// The textures were read as soon as the material's read completed, before the load that waited longer (the same
		// priority as the material), without an update in between: before the material was finalized.
		CHECK(manager->GetReadFrame(material) == frame);
		CHECK(manager->GetReadFrame(textureA) == frame);
		CHECK(manager->GetReadFrame(textureB) == frame);
		const std::vector<AssetHandle> order = manager->GetReadOrder();
		const auto position = [&order](AssetHandle handle) { return std::find(order.begin(), order.end(), handle) - order.begin(); };
		CHECK(position(material) < position(textureA));
		CHECK(position(textureA) < position(waiting));
		CHECK(position(textureB) < position(waiting));
		CHECK(manager->GetAssetState(material) == AssetState::Loading); // Not finalized yet
		CHECK(manager->GetStats().DependencyPrefetches == 2);

		REQUIRE(manager->WaitForPendingLoads());
		CHECK(manager->GetAssetState(textureA) == AssetState::Ready);
		CHECK(manager->GetAssetState(textureB) == AssetState::Ready);
		CHECK(manager->IsLoadedWithDependencies(material));
		// A prefetch is not a request: the textures were never used.
		for (const AssetResidencyInfo& info : manager->GetResidencyInfo())
		{
			if (info.Handle == textureA || info.Handle == textureB)
				CHECK(info.LastRequestedFrame == 0);
			if (info.Handle == material)
				CHECK(info.LastRequestedFrame == frame);
		}
	}

	TEST_CASE("Prefetched dependencies make no room in the budgets and are evicted first")
	{
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle used = manager->Add(0x100, MakeUsage(50), 50);
		const AssetHandle parent = manager->Add(0x200, MakeUsage(10), 10);
		const AssetHandle prefetched = manager->Add(0x201, MakeUsage(40), 40);
		const AssetHandle newcomer = manager->Add(0x300, MakeUsage(30), 30);
		manager->SetDependencies(parent, { prefetched });
		// Room for 100 bytes of assets beside the built-in ones (memory assets, never evicted), and a window of 100 bytes in
		// flight that prefetches leave free.
		const uint64_t builtin = manager->GetStats().Resident.Cpu;
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.Cpu = builtin + 200;
		budgets.InFlightBytes = 100;
		manager->SetResidencyBudgets(budgets);
		// Past the eviction grace window of the first frames, where never requested counts as recently requested.
		for (int frame = 0; frame < 10; frame++)
			manager->Update();

		// The dependency fits beside what is used: it is prefetched and arrives.
		auto useFor = [&](int frames, std::initializer_list<AssetHandle> handles)
		{
			for (int frame = 0; frame < frames; frame++)
			{
				for (AssetHandle handle : handles)
					manager->GetAsset(handle);
				manager->Update();
			}
		};
		useFor(10, { used, parent });
		REQUIRE(manager->WaitForPendingLoads());
		useFor(10, { used, parent });
		CHECK(manager->GetStats().DependencyPrefetches == 1);
		CHECK(manager->GetAssetState(prefetched) == AssetState::Ready);
		CHECK(manager->GetStats().Resident.Cpu == builtin + 100);

		// Something in use needs room in a smaller budget: the dependency nobody asked for goes first, what is in use stays.
		budgets.Cpu = builtin + 100;
		manager->SetResidencyBudgets(budgets);
		useFor(10, { used, parent, newcomer });
		REQUIRE(manager->WaitForPendingLoads());
		useFor(10, { used, parent, newcomer });
		CHECK(manager->GetAssetState(used) == AssetState::Ready);
		CHECK(manager->GetAssetState(parent) == AssetState::Ready);
		CHECK(manager->GetAssetState(newcomer) == AssetState::Ready);
		CHECK(manager->GetAssetState(prefetched) == AssetState::Unloaded);
		CHECK(manager->GetStats().Resident.Cpu <= builtin + 100);
		CHECK(manager->GetStats().Evictions == 1);
	}

	TEST_CASE("Dependencies are prefetched only into the budgets' headroom beside what is on its way and a window in flight")
	{
		ScopedJobSystem jobs(2, 1); // One I/O thread: reads pass the gate in the order they were dispatched
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle used = manager->Add(0x100, MakeUsage(600), 600);
		const AssetHandle first = manager->Add(0x200, MakeUsage(10), 10);
		const AssetHandle second = manager->Add(0x201, MakeUsage(10), 10);
		const AssetHandle requested = manager->Add(0x202, MakeUsage(100), 100);
		const AssetHandle large = manager->Add(0x300, MakeUsage(300), 300);
		const AssetHandle medium = manager->Add(0x301, MakeUsage(200), 200);
		const AssetHandle unknownSize = manager->Add(0x302, MakeUsage(5));
		const AssetHandle small = manager->Add(0x303, MakeUsage(150), 150);
		manager->SetDependencies(first, { large, medium, unknownSize });
		manager->SetDependencies(second, { small });
		const uint64_t builtin = manager->GetStats().Resident.Cpu;
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.Cpu = builtin + 1400;
		budgets.InFlightBytes = 400; // Kept free for requests: beside it, 1000 bytes
		manager->SetResidencyBudgets(budgets);
		REQUIRE(manager->LoadAssetSync(used));

		// 400 bytes of headroom, of which the loads on their way (the first, and another requested asset) take 110: the 300
		// bytes do not fit (they would without the in-flight window), the 200 do, and a dependency of unknown size counts a
		// megabyte.
		manager->CloseGate();
		manager->RequestLoad(first, AssetPriority::High);
		manager->RequestLoad(requested, AssetPriority::Normal);
		manager->AllowReads(1);
		REQUIRE(WaitUntil([&]() { return manager->GetAssetState(medium) == AssetState::Loading; }));
		CHECK(manager->GetAssetState(large) == AssetState::Unloaded);
		CHECK(manager->GetAssetState(unknownSize) == AssetState::Unloaded);
		CHECK(manager->GetStats().DependencyPrefetches == 1);

		// A prefetch on its way counts like any load until it is finalized: the second's dependency does not fit beside it
		// (the reads complete, but nothing is finalized without an update).
		manager->RequestLoad(second, AssetPriority::High);
		manager->AllowReads(3);
		REQUIRE(WaitUntil([&]() { return manager->GetReadCount(second) == 1 && manager->GetStats().OutstandingReads == 0; }));
		CHECK(manager->GetAssetState(small) == AssetState::Unloaded);
		CHECK(manager->GetStats().DependencyPrefetches == 1);

		manager->OpenGate();
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(manager->GetAssetState(medium) == AssetState::Ready);
		CHECK(manager->GetStats().Resident.Cpu == builtin + 920);
		CHECK(manager->GetStats().Evictions == 0);
	}

	TEST_CASE("Scenes, prefabs and models prefetch nothing: what their entities use loads once it is needed")
	{
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle material = manager->Add(0x200, MakeUsage(10), 10);
		uint64_t next = 0x300;
		for (AssetType type : { AssetType::Scene, AssetType::Prefab, AssetType::Model })
		{
			CAPTURE(AssetTypeToString(type));
			const AssetHandle document = manager->Add(next++, MakeUsage(10), 10, type);
			const AssetHandle used = manager->Add(next++, MakeUsage(10), 10);
			manager->SetDependencies(document, { used });
			manager->RequestLoad(document);
			REQUIRE(manager->WaitForPendingLoads());
			CHECK(manager->GetReadCount(document) == 1); // Read, then refused by the type's loader (its bytes are a fake asset's)
			CHECK(manager->GetAssetState(used) == AssetState::Unloaded);
		}
		CHECK(manager->GetStats().DependencyPrefetches == 0);

		// Other assets prefetch what they reference.
		const AssetHandle texture = manager->Add(0x400, MakeUsage(10), 10);
		manager->SetDependencies(material, { texture });
		manager->RequestLoad(material);
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(manager->GetAssetState(texture) == AssetState::Ready);
		CHECK(manager->GetStats().DependencyPrefetches == 1);
	}

	TEST_CASE("Materials request the textures no prefetch is loading")
	{
		NativeProject project("MaterialTextureRequests");
		for (const auto& [path, handle] : { std::pair<std::string, uint64_t> { "Textures/Albedo.png", 0x5200 }, { "Textures/Normal.png", 0x5201 } })
		{
			REQUIRE(FileSystem::CreateDirectories((project.Assets / "Textures")));
			REQUIRE(FileSystem::WriteBytes(project.Assets / FileSystem::FromUTF8(path), CreateSolidPNG(4, 4, 120, 80, 40)));
			REQUIRE(FileSystem::WriteText(project.Assets / FileSystem::FromUTF8(path + ".meta"), JsonUtils::Dump(nlohmann::json { { "Handle", UUIDToJson(UUID(handle)) } })));
		}
		project.WriteMaterial("Materials/Wall.stmat", 0x5100, 0x5200, 0x5201);
		Ref<EditorAssetManager> manager = project.Open();
		REQUIRE(manager->GetDependencies(UUID(0x5100)) == Handles({ 0x5200, 0x5201 }));
		REQUIRE(manager->IsHandleValid(UUID(0x5200)));
		REQUIRE(manager->IsHandleValid(UUID(0x5201)));

		// A budget without room for prefetches: the material's read prefetches nothing, and the material requests its textures
		// itself when it is finalized, as it does where no dependencies are known.
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.GpuTextures = 1; // No renderer: textures stay in the CPU pool, nothing is evicted
		manager->SetResidencyBudgets(budgets);
		REQUIRE(manager->LoadAssetSync(UUID(0x5100)));
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(manager->GetStats().DependencyPrefetches == 0);
		CHECK(manager->GetAssetState(UUID(0x5200)) == AssetState::Ready);
		CHECK(manager->GetAssetState(UUID(0x5201)) == AssetState::Ready);
	}

	TEST_CASE("A material that arrives only prefetched requests none of its textures; they are prefetched once it is requested")
	{
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle used = manager->Add(0x100, MakeUsage(100), 100);
		const AssetHandle parent = manager->Add(0x200, MakeUsage(10), 10);
		const AssetHandle textureA = manager->Add(0x300, MakeUsage(10'000), 10'000);
		const AssetHandle textureB = manager->Add(0x301, MakeUsage(10'000), 10'000);
		MaterialProperties properties;
		properties.BaseColorMap = textureA;
		properties.NormalMap = textureB;
		const std::vector<uint8_t> document = ToBytes(Material::Create(properties)->Serialize());
		const AssetHandle material = manager->AddStored(0x400, AssetType::Material, document);
		// As the asset pipeline records them: e.g. a mesh's default material, and the material's textures.
		manager->SetDependencies(parent, { material });
		manager->SetDependencies(material, { textureA, textureB });

		// Room for what is used, the parent and the material's prefetch beside a window of 100 bytes in flight, not for the
		// textures: requesting them would evict.
		const uint64_t builtin = manager->GetStats().Resident.Cpu;
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.Cpu = builtin + 100 + 10 + 100 + document.size() + 50;
		budgets.InFlightBytes = 100;
		manager->SetResidencyBudgets(budgets);
		REQUIRE(manager->LoadAssetSync(used));
		// Past the eviction grace window of the first frames, where never requested counts as recently requested.
		for (int frame = 0; frame < 10; frame++)
			manager->Update();

		// The parent's read prefetches the material, which arrives and is published, but asks for none of its textures.
		manager->GetAsset(parent);
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(manager->GetAssetState(parent) == AssetState::Ready);
		CHECK(manager->GetAssetState(material) == AssetState::Ready);
		CHECK(manager->GetStats().DependencyPrefetches == 1);
		for (AssetHandle texture : { textureA, textureB })
		{
			CHECK(manager->GetAssetState(texture) == AssetState::Unloaded);
			CHECK(manager->GetReadCount(texture) == 0);
		}
		CHECK(manager->GetAssetState(used) == AssetState::Ready);
		CHECK(manager->GetStats().Evictions == 0);

		// Requested (as whatever draws with it does), the material's textures are prefetched like a requested material's,
		// into the room the budgets have: still none here, so the material asks for nothing.
		CHECK(manager->GetAsset(material));
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(manager->GetStats().DependencyPrefetches == 1);
		CHECK(manager->GetAssetState(textureA) == AssetState::Unloaded);

		// The next material that arrives only prefetched, with room for its textures once it is requested.
		budgets.Cpu = AssetResidencyBudgets::c_Unlimited;
		manager->SetResidencyBudgets(budgets);
		const AssetHandle otherParent = manager->Add(0x201, MakeUsage(10), 10);
		const AssetHandle otherMaterial = manager->AddStored(0x401, AssetType::Material, document);
		manager->SetDependencies(otherParent, { otherMaterial });
		manager->SetDependencies(otherMaterial, { textureA, textureB });
		manager->GetAsset(otherParent);
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(manager->GetAssetState(otherMaterial) == AssetState::Ready);
		CHECK(manager->GetStats().DependencyPrefetches == 2);
		CHECK(manager->GetAssetState(textureA) == AssetState::Unloaded);
		CHECK(manager->GetAsset(otherMaterial));
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(manager->GetStats().DependencyPrefetches == 4);
		for (AssetHandle texture : { textureA, textureB })
		{
			CHECK(manager->GetAssetState(texture) == AssetState::Ready);
			CHECK(manager->GetReadCount(texture) == 1);
		}
		// Prefetched, not requested: nothing was used, nothing evicted.
		for (const AssetResidencyInfo& info : manager->GetResidencyInfo())
		{
			if (info.Handle == textureA || info.Handle == textureB)
				CHECK(info.LastRequestedFrame == 0);
		}
		CHECK(manager->GetStats().Evictions == 0);
	}

	TEST_CASE("A prefetched dependency that no longer fits when it arrives is dropped unfinalized")
	{
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle parent = manager->Add(0x200, MakeUsage(10), 10);
		// Its stored size fits, what it holds once loaded does not (stored sizes only estimate it).
		const AssetHandle dependency = manager->Add(0x201, MakeUsage(500), 10);
		manager->SetDependencies(parent, { dependency });
		const uint64_t builtin = manager->GetStats().Resident.Cpu;
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.Cpu = builtin + 200;
		budgets.InFlightBytes = 50;
		manager->SetResidencyBudgets(budgets);
		for (int frame = 0; frame < 10; frame++)
			manager->Update();

		for (int frame = 0; frame < 10; frame++)
		{
			manager->GetAsset(parent);
			manager->Update();
			CHECK(manager->GetStats().Resident.Cpu <= budgets.Cpu);
		}
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(manager->GetAssetState(parent) == AssetState::Ready);
		CHECK(manager->GetAssetState(dependency) == AssetState::Unloaded);
		const AssetManagerStats stats = manager->GetStats();
		CHECK(stats.DependencyPrefetches == 1);
		CHECK(stats.DependencyPrefetchesDropped == 1);
		CHECK(stats.Evictions == 0); // Never published, so never evicted
		CHECK(stats.Resident.Cpu == builtin + 10);

		// Requested, it loads and makes room like any load.
		REQUIRE(manager->LoadAssetSync(dependency));
		CHECK(manager->GetAssetState(dependency) == AssetState::Ready);
	}

	TEST_CASE("Prefetches that start, arrive and are dropped keep the statistics' counts of assets per state exact")
	{
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle parent = manager->Add(0x200, MakeUsage(10), 10);
		const AssetHandle arriving = manager->Add(0x201, MakeUsage(20), 20);
		// Its stored size fits, what it holds once loaded does not: dropped when it arrives.
		const AssetHandle dropped = manager->Add(0x202, MakeUsage(500), 10);
		manager->SetDependencies(parent, { arriving, dropped });
		const uint64_t builtin = manager->GetStats().Resident.Cpu;
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.Cpu = builtin + 200;
		budgets.InFlightBytes = 50;
		manager->SetResidencyBudgets(budgets);
		for (int frame = 0; frame < 10; frame++)
			manager->Update();
		REQUIRE(StateCountsMatch(*manager));

		// GetStats counts assets per state as they change (it visits none), so every change a prefetch makes must count.
		for (int frame = 0; frame < 10; frame++)
		{
			manager->GetAsset(parent);
			manager->Update();
			CHECK(StateCountsMatch(*manager));
		}
		REQUIRE(manager->WaitForPendingLoads());
		for (int frame = 0; frame < 3; frame++)
		{
			manager->GetAsset(parent);
			manager->Update();
		}
		CHECK(manager->GetAssetState(parent) == AssetState::Ready);
		CHECK(manager->GetAssetState(arriving) == AssetState::Ready);
		CHECK(manager->GetAssetState(dropped) == AssetState::Unloaded);
		const AssetManagerStats stats = manager->GetStats();
		CHECK(stats.DependencyPrefetches == 2);
		CHECK(stats.DependencyPrefetchesDropped == 1);
		CHECK(stats.LoadingAssets == 0);
		CHECK(StateCountsMatch(*manager));
	}

	TEST_CASE("A group of a model, 10 materials and 20 textures progresses by bytes to completion")
	{
		ScopedJobSystem jobs(2, 1);
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle model = manager->Add(0x1000, MakeUsage(100), 1000);
		std::vector<AssetHandle> materials;
		std::vector<AssetHandle> textures;
		for (uint64_t index = 0; index < 10; index++)
		{
			materials.push_back(manager->Add(0x2000 + index, MakeUsage(10), 200 + index * 10));
			const AssetHandle first = manager->Add(0x3000 + index * 2, MakeUsage(1000), 50'000 + index * 1000);
			const AssetHandle second = manager->Add(0x3001 + index * 2, MakeUsage(1000), 20'000 + index * 1000);
			manager->SetDependencies(materials.back(), { first, second, model }); // Cycles are visited once
			textures.push_back(first);
			textures.push_back(second);
		}
		manager->SetDependencies(model, materials);

		manager->CloseGate();
		AssetGroupRequest group = manager->RequestLoadWithDependencies(model, AssetPriority::High);
		REQUIRE(group.IsValid());
		REQUIRE(group.GetAssets().size() == 31);
		CHECK(group.GetRoot() == model);
		CHECK(std::equal(materials.begin(), materials.end(), group.GetAssets().begin() + 1)); // Breadth first
		for (AssetHandle member : group.GetAssets())
			CHECK(manager->GetPinCount(member) == 1);
		CHECK(group.Progress() == 0.0f);
		CHECK_FALSE(group.IsComplete());

		std::vector<float> progress;
		for (int step = 0; step < 400 && !group.IsComplete(); step++)
		{
			manager->AllowReads(1);
			manager->Update();
			progress.push_back(group.Progress());
			const bool allReady = std::all_of(group.GetAssets().begin(), group.GetAssets().end(),
				[&](AssetHandle member) { return manager->GetAssetState(member) == AssetState::Ready; });
			CHECK(manager->IsLoadedWithDependencies(model) == allReady);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		manager->OpenGate();
		REQUIRE(WaitUntil([&]() { manager->Update(); return group.IsComplete(); }, std::chrono::milliseconds(10000)));
		progress.push_back(group.Progress());
		CHECK(progress.back() == 1.0f);
		CHECK_FALSE(group.HasFailed());
		CHECK(manager->IsLoadedWithDependencies(model));
		for (size_t index = 1; index < progress.size(); index++)
			CHECK(progress[index] >= progress[index - 1]);
		std::set<float> distinct(progress.begin(), progress.end());
		CHECK(distinct.size() > 5);

		// The last texture decides: unloaded, the model is not loaded with its dependencies, and the group keeps its progress.
		manager->UnloadAsset(textures.back());
		CHECK_FALSE(manager->IsLoadedWithDependencies(model));
		CHECK(group.Progress() == 1.0f);

		// Released, the pins go.
		group = AssetGroupRequest();
		for (AssetHandle member : { model, materials[0], textures[0] })
			CHECK(manager->GetPinCount(member) == 0);
		CHECK_FALSE(manager->RequestLoadWithDependencies(UUID(0x9999)).IsValid());
	}

	TEST_CASE("Cancelling a group stops the reads that have not started")
	{
		ScopedJobSystem jobs(2, 1);
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle root = manager->Add(0x1000, MakeUsage(10), 10);
		std::vector<AssetHandle> members;
		for (uint64_t index = 0; index < 10; index++)
			members.push_back(manager->Add(0x2000 + index, MakeUsage(10), 10));
		manager->SetDependencies(root, members);
		const AssetHandle pinnedElsewhere = members[9];

		manager->CloseGate();
		AssetGroupRequest group = manager->RequestLoadWithDependencies(root, AssetPriority::Normal);
		REQUIRE(manager->WaitForWaitingReads(1)); // The root reads; the first member waits for the I/O thread
		AssetPin otherPin = manager->Pin(pinnedElsewhere, AssetPriority::Low);
		const AssetManagerStats before = manager->GetStats();
		group.Cancel();
		manager->OpenGate();
		REQUIRE(manager->WaitForPendingLoads());

		// The root was reading: it finishes, but is not decoded and prefetches nothing. The first member was dispatched and
		// checks before it reads; the queued ones are gone, except the one someone else pins.
		CHECK(manager->GetReadCount(root) == 1);
		uint32_t read = 0;
		for (AssetHandle member : members)
			read += manager->GetReadCount(member);
		CHECK(read == 1);
		CHECK(manager->GetAssetState(root) == AssetState::Unloaded);
		CHECK(manager->GetReadCount(pinnedElsewhere) == 1);
		CHECK(manager->GetAssetState(pinnedElsewhere) == AssetState::Ready);
		CHECK(manager->GetStats().Cancellations - before.Cancellations >= 8);
		for (size_t index = 2; index < 9; index++)
			CHECK(manager->GetAssetState(members[index]) == AssetState::Unloaded);
		for (AssetHandle member : members)
			CHECK(manager->GetPinCount(member) == (member == pinnedElsewhere ? 1u : 0u));
		CHECK(group.IsValid());
	}
}
