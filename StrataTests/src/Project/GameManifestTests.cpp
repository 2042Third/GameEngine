#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"
#include "Strata/Project/GameManifest.h"
#include "TestHelpers.h"

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
		for (const char* pack : { "../Other.stpak", "/abs/Game.stpak", "Sub/Game.stpak", "" })
		{
			nlohmann::json json = manifest.ToJson();
			json["Game"]["AssetPack"] = pack;
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

		for (const char* module : { "../GameScripts.dll", "/abs/GameScripts.so", "Scripts/GameScripts.dll" })
		{
			CAPTURE(module);
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
}
