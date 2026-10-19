#include <doctest/doctest.h>

#include "Engine/ModuleRegistrationHelpers.h"
#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Process.h"
#include "Strata/Engine/BuiltinModules.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/SceneSystem.h"
#include "Strata/Scene/UnknownComponents.h"
#include "TestHelpers.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	struct UnregisteredTestComponent
	{
		int32_t Value = 0;
	};

	size_t CountErrors(uint64_t after, std::string_view text)
	{
		size_t count = 0;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(after))
			count += entry.Level == LogLevel::Error && entry.Message.find(text) != std::string::npos ? 1 : 0;
		return count;
	}

	std::vector<std::string> GetSystemNames()
	{
		std::vector<std::string> names;
		for (const SceneSystemDescriptor& descriptor : SceneSystemRegistry::GetAll())
			names.push_back(descriptor.Name);
		return names;
	}

	// Runs a helper mode of this executable; its output with line ends as '\n' (Windows' console writes "\r\n").
	Process::RunResult RunHelper(std::vector<std::string> arguments)
	{
		ProcessSpecification specification;
		specification.Executable = GetTestExecutablePath();
		specification.Arguments = std::move(arguments);
		Process::RunResult result = Process::Run(specification, std::chrono::milliseconds(60000));
		std::erase(result.Output, '\r');
		return result;
	}

}

TEST_SUITE("Engine.Modules")
{
	TEST_CASE("The engine's modules are registered once, before the tests run")
	{
		REQUIRE(Engine::AreBuiltinModulesRegistered());
		CHECK(ComponentRegistry::IsFrozen());
		CHECK_FALSE(ComponentRegistry::IsRegistrationOpen());

		// Every module of the build registered: components (scene), loaders (scene, renderer, audio, animation), importers
		// (pipeline), systems.
		CHECK(ComponentRegistry::Find("Transform") != nullptr);
		CHECK(ComponentRegistry::Find("Script") != nullptr);
		CHECK((ComponentRegistry::Find("RigidBody") != nullptr) == ST_WITH_PHYSICS);
		CHECK((ComponentRegistry::Find("AudioSource") != nullptr) == ST_WITH_AUDIO);
		CHECK((ComponentRegistry::Find("Animator") != nullptr) == ST_WITH_ANIMATION);
		for (AssetType type : { AssetType::Scene, AssetType::Prefab, AssetType::Model, AssetType::Texture, AssetType::Mesh, AssetType::Material, AssetType::Font })
		{
			CAPTURE(AssetTypeToString(type));
			CHECK(AssetLoaderRegistry::Find(type) != nullptr);
		}
		CHECK((AssetLoaderRegistry::Find(AssetType::AudioClip) != nullptr) == ST_WITH_AUDIO);
		CHECK((AssetLoaderRegistry::Find(AssetType::AnimationClip) != nullptr) == ST_WITH_ANIMATION);
		CHECK(AssetImporterRegistry::FindByExtension(".gltf") != nullptr);
		const std::vector<std::string> systems = GetSystemNames();
		const auto hasSystem = [&systems](const char* name) { return std::find(systems.begin(), systems.end(), name) != systems.end(); };
		CHECK(hasSystem("Scripting"));
		CHECK(hasSystem("Physics") == ST_WITH_PHYSICS);
		CHECK(hasSystem("Audio") == ST_WITH_AUDIO);
		CHECK(hasSystem("Animation") == ST_WITH_ANIMATION);

		// A second call is a no-op; registrations it carries are reported, not run.
		const size_t components = ComponentRegistry::GetAll().size();
		const size_t importers = AssetImporterRegistry::GetAll().size();
		const uint64_t before = Log::GetBuffer().GetLatestSequence();
		bool extraRan = false;
		bool pipelineRan = false;
		Engine::ModuleRegistrationOptions options;
		options.AssetPipeline = [&]() { pipelineRan = true; };
		options.Extra.push_back([&]() { extraRan = true; });
		Engine::RegisterBuiltinModules(options);
		Engine::RegisterBuiltinModules();
		CHECK_FALSE(extraRan);
		CHECK_FALSE(pipelineRan);
		CHECK(CountErrors(before, "extra registrations, which are ignored") == 1);
		CHECK(CountErrors(before, "called again with the asset pipeline, which is ignored") == 1);
		CHECK(ComponentRegistry::GetAll().size() == components);
		CHECK(AssetImporterRegistry::GetAll().size() == importers);
		CHECK(GetSystemNames() == systems);
	}

	TEST_CASE("Registering a component after the registry froze is refused with an error, without aborting")
	{
		const size_t components = ComponentRegistry::GetAll().size();
		const uint64_t before = Log::GetBuffer().GetLatestSequence();
		auto builder = ComponentRegistry::Register<UnregisteredTestComponent>("LateTestComponent");
		builder.Category("Tests").Property("Value", &UnregisteredTestComponent::Value);
		CHECK_FALSE(builder.IsRegistered());
		CHECK_FALSE(static_cast<bool>(builder));
		CHECK(ComponentRegistry::Find("LateTestComponent") == nullptr);
		CHECK(ComponentRegistry::Find<UnregisteredTestComponent>() == nullptr);
		CHECK(ComponentRegistry::GetAll().size() == components);
		CHECK(CountErrors(before, "'LateTestComponent' is not registered: the component registry is frozen") == 1);
	}

	TEST_CASE("A game registers its components through ModuleRegistrationOptions::Extra")
	{
		// In a child process: registration happens once per process, and the registry is frozen in this one.
		const std::filesystem::path directory = CreateTemporaryDirectory("CustomComponent");
		const Process::RunResult result = RunHelper({ "--strata-test-helper=custom-component", FileSystem::ToUTF8(directory) });
		INFO("Output: ", result.Output);
		REQUIRE(result.Started);
		CHECK_FALSE(result.TimedOut);
		CHECK(result.ExitCode == 0);
		CHECK(result.Output.find("registered: TestVehicle") != std::string::npos);
		CHECK(result.Output.find("requirements: TestVehicle") != std::string::npos);
		CHECK(result.Output.find("reloaded: Speed 12.50, Label Rover One") != std::string::npos);
		// Its extra data's references are remapped in copies, compared with prefabs and scanned (ComponentInfo::ExtraReferences).
		CHECK(result.Output.find("extra references: remapped, compared, scanned") != std::string::npos);
		// The renderer draws its gizmo with the colliders (ComponentInfo::Gizmo).
		CHECK(result.Output.find("gizmos: a box per vehicle") != std::string::npos);
		CHECK(result.Output.find("late registration refused: Component 'Late' is not registered: the component registry is frozen") != std::string::npos);

		// This process lacks the game's component: its scene still loads, keeps the component and saves it unchanged.
		REQUIRE(ComponentRegistry::Find("TestVehicle") == nullptr);
		std::string error;
		const Ref<Scene> scene = SceneSerializer::LoadFromFile(directory / "Garage.stscene", &error);
		REQUIRE(scene);
		Entity rover = scene->FindEntityByName("Rover");
		REQUIRE(rover);
		REQUIRE(rover.HasComponent<UnknownComponentsComponent>());
		const nlohmann::json vehicle = rover.GetComponent<UnknownComponentsComponent>().Components.value("TestVehicle", nlohmann::json());
		REQUIRE(vehicle.is_object());
		CHECK(vehicle["Speed"] == 12.5);
		CHECK(vehicle["Label"] == "Rover One");
		CHECK(vehicle["Passengers"].is_array());
		const nlohmann::json saved = SceneSerializer::Serialize(*scene)["Scene"]["Entities"];
		REQUIRE(saved.size() == 2); // The rover and its driver
		nlohmann::json savedVehicle;
		for (const nlohmann::json& entity : saved)
		{
			if (entity["Components"].contains("TestVehicle"))
				savedVehicle = entity["Components"]["TestVehicle"];
		}
		CHECK(savedVehicle == vehicle);
	}

	TEST_CASE("Using a registry before the modules are registered fails a verify naming Engine::RegisterBuiltinModules")
	{
		for (const char* registry : { "components", "loaders", "importers", "builtin-assets", "systems", "mesh-geometry" })
		{
			const Process::RunResult result = RunHelper({ "--strata-test-helper=unregistered-registry", registry });
			INFO("Registry ", registry, ", output: ", result.Output);
			REQUIRE(result.Started);
			CHECK_FALSE(result.TimedOut);
			CHECK(result.ExitCode == c_VerifyExitCode);
			CHECK(result.Output.find("verify failed:") != std::string::npos);
			// The importer registry belongs to the asset pipeline, which programs hand in.
			const std::string_view expected = std::string_view(registry) == "importers"
				? "before Engine::RegisterBuiltinModules() registered the asset pipeline (ModuleRegistrationOptions::AssetPipeline"
				: "before Engine::RegisterBuiltinModules() registered the engine's modules";
			CHECK(result.Output.find(expected) != std::string::npos);
		}
	}

	TEST_CASE("Without the asset pipeline every module registers, and the importer registry stays closed")
	{
		// A shipped game's registration (no options), in a child process: registration happens once per process.
		const Process::RunResult result = RunHelper({ "--strata-test-helper=no-asset-pipeline" });
		INFO("Output: ", result.Output);
		REQUIRE(result.Started);
		CHECK_FALSE(result.TimedOut);
		// The loaders and systems of the modules the build has: 11 and five in a full build.
		const int loaders = 7 + (ST_WITH_AUDIO ? 1 : 0) + (ST_WITH_ANIMATION ? 3 : 0);
		std::string systems = " UI Scripting";
		systems += ST_WITH_ANIMATION ? " Animation" : "";
		systems += ST_WITH_PHYSICS ? " Physics" : "";
		systems += ST_WITH_AUDIO ? " Audio" : "";
		CHECK(result.Output.find("loaders: " + std::to_string(loaders) + ", systems:" + systems + "\n") != std::string::npos);
		CHECK(result.ExitCode == c_VerifyExitCode);
		CHECK(result.Output.find("verify failed: The asset importer registry is used before Engine::RegisterBuiltinModules() registered the asset pipeline") != std::string::npos);
	}

	TEST_CASE("The composition root registers the engine's modules in its table's order")
	{
		std::vector<std::string> expected = { "Scene", "Renderer" };
#if ST_WITH_PHYSICS
		expected.push_back("Physics");
#endif
#if ST_WITH_AUDIO
		expected.push_back("Audio");
#endif
#if ST_WITH_ANIMATION
		expected.push_back("Animation");
#endif
		expected.push_back("Scripting");
		expected.push_back("GameUI");
		CHECK(Engine::GetBuiltinModuleNames() == expected);
		CHECK(Engine::GetRegisteredModules() == expected);
	}

#if ST_TESTS_HAVE_EVERY_MODULE
	TEST_CASE("Without the physics module the others register, its components stay unknown and scripts are told")
	{
		// In a child process: registration happens once per process.
		const std::filesystem::path scene = FileSystem::FromUTF8(STRATA_FEATURE_TEST_DIR) / "Assets" / "Scenes" / "Feature.stscene";
		const Process::RunResult result = RunHelper({ "--strata-test-helper=register-without", "Physics", FileSystem::ToUTF8(scene) });
		INFO("Output: ", result.Output);
		REQUIRE(result.Started);
		CHECK_FALSE(result.TimedOut);
		CHECK(result.ExitCode == 0);
		CHECK(result.Output.find("modules: Scene Renderer Audio Animation Scripting GameUI\n") != std::string::npos);
		// The others' soft constraints order them as in a full engine.
		CHECK(result.Output.find("systems: UI Scripting Animation Audio\n") != std::string::npos);
		// The feature scene's physics components stay unknown components and save back unchanged.
		const size_t unknown = result.Output.find("unknown components: ");
		REQUIRE(unknown != std::string::npos);
		const std::string names = result.Output.substr(unknown, result.Output.find('\n', unknown) - unknown);
		for (const char* name : { "RigidBody", "BoxCollider", "SphereCollider", "CapsuleCollider", "MeshCollider", "CharacterController" })
		{
			CAPTURE(name);
			CHECK(names.find(name) != std::string::npos);
		}
		CHECK(names.find("Transform") == std::string::npos);
		CHECK(names.find("AudioSource") == std::string::npos);
		const size_t saved = result.Output.find("saved unchanged: ");
		REQUIRE(saved != std::string::npos);
		size_t unchanged = 0;
		size_t total = 0;
		REQUIRE(std::sscanf(result.Output.c_str() + saved, "saved unchanged: %zu of %zu", &unchanged, &total) == 2);
		CHECK(total >= 6);
		CHECK(unchanged == total);
		// The API test module's physics calls fail and report that the engine has no physics module; nothing crashes.
		CHECK(result.Output.find("scripts ran: yes, failed check: (none), faulted: no") != std::string::npos);
		CHECK(result.Output.find("reported: 0") == std::string::npos);
		CHECK(result.Output.find("reported: ") != std::string::npos);
	}
#endif

#if ST_TESTS_HAVE_EVERY_MODULE
	TEST_CASE("Excluding the scene, the renderer or a module that does not exist is refused")
	{
		const std::filesystem::path scene = FileSystem::FromUTF8(STRATA_FEATURE_TEST_DIR) / "Assets" / "Scenes" / "FeatureSub.stscene";
		const Process::RunResult result = RunHelper({ "--strata-test-helper=register-without", "Scene,Renderer,Teleportation,Audio", FileSystem::ToUTF8(scene) });
		INFO("Output: ", result.Output);
		REQUIRE(result.Started);
		CHECK_FALSE(result.TimedOut);
		CHECK(result.ExitCode == 0);
		CHECK(result.Output.find("the Scene module cannot be excluded (every engine has it); it is registered") != std::string::npos);
		CHECK(result.Output.find("the Renderer module cannot be excluded (every engine has it); it is registered") != std::string::npos);
		CHECK(result.Output.find("'Teleportation' is no engine module of this build and cannot be excluded") != std::string::npos);
		CHECK(result.Output.find("modules: Scene Renderer Physics Animation Scripting GameUI\n") != std::string::npos);
		CHECK(result.Output.find("systems: UI Scripting Animation Physics\n") != std::string::npos);
	}
#endif

#if ST_TESTS_HAVE_EVERY_MODULE
	TEST_CASE("Without the scripting module the game UI module registers and orders its system")
	{
		// The UI system runs before the scripts only when the engine has them (BeforeIfPresent): the game UI works without.
		const std::filesystem::path scene = FileSystem::FromUTF8(STRATA_FEATURE_TEST_DIR) / "Assets" / "Scenes" / "FeatureSub.stscene";
		const Process::RunResult result = RunHelper({ "--strata-test-helper=register-without", "Scripting", FileSystem::ToUTF8(scene) });
		INFO("Output: ", result.Output);
		REQUIRE(result.Started);
		CHECK_FALSE(result.TimedOut);
		CHECK(result.ExitCode == 0);
		CHECK(result.Output.find("modules: Scene Renderer Physics Audio Animation GameUI\n") != std::string::npos);
		CHECK(result.Output.find("systems: Animation Physics Audio UI\n") != std::string::npos);
	}
#endif

	TEST_CASE("Built-in asset factories are accepted only for built-in handles")
	{
		const uint64_t before = Log::GetBuffer().GetLatestSequence();
		CHECK_FALSE(BuiltinAssets::RegisterFactory(UUID(0x12345678), []() { return Ref<Asset>(); }));
		CHECK(CountErrors(before, "is not a built-in asset") == 1);
		CHECK_FALSE(BuiltinAssets::RegisterFactory(BuiltinAssets::CubeMesh, BuiltinAssetFactory()));
		CHECK(CountErrors(before, "is empty") == 1);
	}
}
