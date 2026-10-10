#include <doctest/doctest.h>

#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Project/GameManifest.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Runtime/GameRuntime.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/SceneSerializer.h"
#include "TestHelpers.h"

#include <map>

using namespace Strata;

TEST_SUITE("Project.GameManifest")
{
	TEST_CASE("Game manifests round trip and validate their fields")
	{
		GameManifest manifest;
		manifest.Name = "Tetris";
		manifest.AssetPack = "Tetris.stpak";
		manifest.StartScene = UUID(0x1234567890ABCDEFull);
		manifest.WindowWidth = 800;
		manifest.WindowHeight = 600;
		manifest.Fullscreen = true;
		manifest.VSync = false;

		std::string error;
		std::optional<GameManifest> loaded = GameManifest::FromJson(manifest.ToJson(), &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->Name == "Tetris");
		CHECK(loaded->AssetPack == "Tetris.stpak");
		CHECK(loaded->StartScene == manifest.StartScene);
		CHECK(loaded->WindowWidth == 800);
		CHECK(loaded->WindowHeight == 600);
		CHECK(loaded->Fullscreen);
		CHECK_FALSE(loaded->VSync);

		// The pack must be a file next to the manifest; a scene handle is required.
		for (const char* pack : { "../Other.stpak", "/abs/Game.stpak", "Sub/Game.stpak", "", ".", ".." })
		{
			nlohmann::json json = manifest.ToJson();
			json["Game"]["AssetPack"] = pack;
			INFO("pack: ", pack);
			CHECK_FALSE(GameManifest::FromJson(json).has_value());
		}
		nlohmann::json noScene = manifest.ToJson();
		noScene["Game"].erase("StartScene");
		CHECK_FALSE(GameManifest::FromJson(noScene).has_value());
		nlohmann::json wrongFormat = manifest.ToJson();
		wrongFormat["Strata"]["Format"] = "Scene";
		CHECK_FALSE(GameManifest::FromJson(wrongFormat).has_value());
		nlohmann::json hugeWindow = manifest.ToJson();
		hugeWindow["Game"]["Window"]["Width"] = 1000000;
		CHECK(GameManifest::FromJson(hugeWindow)->WindowWidth == 16384);
	}

	TEST_CASE("Game manifests name the script module next to them")
	{
		GameManifest manifest;
		manifest.AssetPack = "Game.stpak";
		manifest.StartScene = UUID(0x42);
		manifest.ScriptModule = "GameScripts.dll";

		std::string error;
		std::optional<GameManifest> loaded = GameManifest::FromJson(manifest.ToJson(), &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->ScriptModule == "GameScripts.dll");
		CHECK(manifest.ToJson()["Strata"]["Version"] == GameManifest::c_FormatVersion);

		// No module: a game without scripts.
		manifest.ScriptModule.clear();
		loaded = GameManifest::FromJson(manifest.ToJson(), &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->ScriptModule.empty());

		for (const char* module : { "../GameScripts.dll", "/abs/GameScripts.so", "Scripts/GameScripts.dll", ".", ".." })
		{
			INFO("module: ", module);
			nlohmann::json json = manifest.ToJson();
			json["Game"]["ScriptModule"] = module;
			CHECK_FALSE(GameManifest::FromJson(json, &error).has_value());
			CHECK(error.find("ScriptModule") != std::string::npos);
		}

		// Version 1 manifests (written before games had scripts) still load; newer versions are refused.
		nlohmann::json version1 = manifest.ToJson();
		version1["Strata"]["Version"] = 1;
		version1["Game"].erase("ScriptModule");
		loaded = GameManifest::FromJson(version1, &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->ScriptModule.empty());
		nlohmann::json future = manifest.ToJson();
		future["Strata"]["Version"] = GameManifest::c_FormatVersion + 1;
		CHECK_FALSE(GameManifest::FromJson(future).has_value());
	}

	TEST_CASE("The runtime finds the manifest next to its executable")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("GameManifestFind");
		GameManifest manifest;
		manifest.AssetPack = "Game.stpak";
		manifest.StartScene = UUID(0x42);
		CHECK(GameManifest::FindForExecutable(directory / "Game.exe").empty());

		REQUIRE(manifest.Save(directory / "Other.stgame"));
		CHECK(GameManifest::FindForExecutable(directory / "Game.exe") == directory / "Other.stgame"); // The only one

		REQUIRE(manifest.Save(directory / "Third.stgame"));
		CHECK(GameManifest::FindForExecutable(directory / "Game.exe").empty()); // Ambiguous

		REQUIRE(manifest.Save(directory / "Game.stgame"));
		CHECK(GameManifest::FindForExecutable(directory / "Game.exe") == directory / "Game.stgame"); // Named after the executable
		std::string error;
		CHECK(GameManifest::Load(directory / "Game.stgame", &error).has_value());
		CHECK_FALSE(GameManifest::Load(directory / "Missing.stgame", &error).has_value());
	}

	TEST_CASE("Game manifests list patch packs next to them, and version 2 manifests load without")
	{
		GameManifest manifest;
		manifest.AssetPack = "Game.stpak";
		manifest.StartScene = UUID(0x42);
		manifest.PatchPacks = { "Patch1.stpak", "Patch2.stpak" };
		std::string error;
		std::optional<GameManifest> loaded = GameManifest::FromJson(manifest.ToJson(), &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->PatchPacks == manifest.PatchPacks);

		nlohmann::json version2 = manifest.ToJson();
		version2["Strata"]["Version"] = 2;
		version2["Game"].erase("PatchPacks");
		loaded = GameManifest::FromJson(version2, &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->PatchPacks.empty());

		for (const nlohmann::json& patches : { nlohmann::json("Patch.stpak"), nlohmann::json::array({ "../Patch.stpak" }), nlohmann::json::array({ 3 }),
			nlohmann::json::array({ "Sub/Patch.stpak" }), nlohmann::json::array({ "" }) })
		{
			INFO("patches: ", patches.dump());
			nlohmann::json json = manifest.ToJson();
			json["Game"]["PatchPacks"] = patches;
			CHECK_FALSE(GameManifest::FromJson(json, &error).has_value());
			CHECK(error.find("PatchPacks") != std::string::npos);
		}
	}

	TEST_CASE("Games mount the patch packs their manifest lists over their pack")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("GameManifestPatches");
		const AssetHandle sceneHandle = UUID(0x9100);
		const AssetHandle materialHandle = UUID(0x9101);
		Scene scene("Patched");
		scene.CreateEntity("Box").AddComponent<MeshRendererComponent>().Material = materialHandle;
		const std::string sceneText = JsonUtils::Dump(SceneSerializer::Serialize(scene));
		auto materialBytes = [](float roughness)
		{
			MaterialProperties properties;
			properties.Roughness = roughness;
			const std::string text = JsonUtils::Dump(Material::Create(properties)->Serialize());
			return std::vector<uint8_t>(text.begin(), text.end());
		};
		auto writePack = [](const std::filesystem::path& path, const std::map<uint64_t, std::pair<AssetType, std::vector<uint8_t>>>& assets)
		{
			std::vector<AssetMetadata> metadata;
			for (const auto& [handle, asset] : assets)
			{
				AssetMetadata entry;
				entry.Handle = UUID(handle);
				entry.Type = asset.first;
				entry.Path = std::to_string(handle);
				entry.Name = entry.Path;
				metadata.push_back(entry);
			}
			std::string writeError;
			REQUIRE_MESSAGE(AssetPackWriter::Write(path, metadata, [&assets](const AssetMetadata& asset, std::vector<uint8_t>& outData, std::string*)
			{
				outData = assets.at(static_cast<uint64_t>(asset.Handle)).second;
				return true;
			}, &writeError), writeError);
		};
		writePack(directory / "Game.stpak", { { 0x9100, { AssetType::Scene, std::vector<uint8_t>(sceneText.begin(), sceneText.end()) } },
			{ 0x9101, { AssetType::Material, materialBytes(0.1f) } } });
		writePack(directory / "Patch.stpak", { { 0x9101, { AssetType::Material, materialBytes(0.7f) } } });

		GameManifest manifest;
		manifest.Name = "Patched";
		manifest.AssetPack = "Game.stpak";
		manifest.StartScene = sceneHandle;
		manifest.PatchPacks = { "Patch.stpak" };
		REQUIRE(manifest.Save(directory / "Patched.stgame"));
		std::string error;
		{
			Scope<GameRuntime> game = GameRuntime::Create(directory / "Patched.stgame", &error);
			REQUIRE_MESSAGE(game, error);
			CHECK(game->GetAssetManager()->GetPacks().size() == 2);
			const Ref<Material> material = std::static_pointer_cast<Material>(game->GetAssetManager()->LoadAssetSync(materialHandle));
			REQUIRE(material);
			CHECK(material->GetProperties().Roughness == doctest::Approx(0.7f));
		}

		// A patch the manifest names must exist: the game would otherwise run with what it was meant to replace.
		manifest.PatchPacks = { "Missing.stpak" };
		REQUIRE(manifest.Save(directory / "Broken.stgame"));
		CHECK_FALSE(GameRuntime::Create(directory / "Broken.stgame", &error));
		CHECK(error.find("Missing.stpak") != std::string::npos);
	}
}
