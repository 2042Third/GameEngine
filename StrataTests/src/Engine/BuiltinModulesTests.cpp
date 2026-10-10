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

	Process::RunResult RunHelper(std::vector<std::string> arguments)
	{
		ProcessSpecification specification;
		specification.Executable = GetTestExecutablePath();
		specification.Arguments = std::move(arguments);
		return Process::Run(specification, std::chrono::milliseconds(60000));
	}

}

TEST_SUITE("Engine.Modules")
{
	TEST_CASE("The engine's modules are registered once, before the tests run")
	{
		REQUIRE(Engine::AreBuiltinModulesRegistered());
		CHECK(ComponentRegistry::IsFrozen());
		CHECK_FALSE(ComponentRegistry::IsRegistrationOpen());

		// Every module registered: components (scene), loaders (scene, renderer, audio), importers (pipeline), systems.
		CHECK(ComponentRegistry::Find("Transform") != nullptr);
		CHECK(ComponentRegistry::Find("Script") != nullptr);
		for (AssetType type : { AssetType::Scene, AssetType::Prefab, AssetType::Model, AssetType::Texture, AssetType::Mesh, AssetType::Material, AssetType::Font, AssetType::AudioClip })
		{
			CAPTURE(AssetTypeToString(type));
			CHECK(AssetLoaderRegistry::Find(type) != nullptr);
		}
		CHECK(AssetImporterRegistry::FindByExtension(".gltf") != nullptr);
		const std::vector<std::string> systems = GetSystemNames();
		for (const char* name : { "Scripting", "Physics", "Audio" })
		{
			CAPTURE(name);
			CHECK(std::find(systems.begin(), systems.end(), name) != systems.end());
		}

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
		CHECK(result.Output.find("reloaded: Speed 12.50, Label Rover One") != std::string::npos);
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
		const nlohmann::json saved = SceneSerializer::Serialize(*scene)["Scene"]["Entities"];
		REQUIRE(saved.size() == 1);
		CHECK(saved[0]["Components"]["TestVehicle"] == vehicle);
	}

	TEST_CASE("Using a registry before the modules are registered fails a verify naming Engine::RegisterBuiltinModules")
	{
		for (const char* registry : { "components", "loaders", "importers", "builtin-assets", "systems" })
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
		CHECK(result.Output.find("loaders: 8, systems: Scripting Physics Audio") != std::string::npos);
		CHECK(result.ExitCode == c_VerifyExitCode);
		CHECK(result.Output.find("verify failed: The asset importer registry is used before Engine::RegisterBuiltinModules() registered the asset pipeline") != std::string::npos);
	}

	TEST_CASE("Built-in asset factories are accepted only for built-in handles")
	{
		const uint64_t before = Log::GetBuffer().GetLatestSequence();
		CHECK_FALSE(BuiltinAssets::RegisterFactory(UUID(0x12345678), []() { return Ref<Asset>(); }));
		CHECK(CountErrors(before, "is not a built-in asset") == 1);
		CHECK_FALSE(BuiltinAssets::RegisterFactory(BuiltinAssets::CubeMesh, BuiltinAssetFactory()));
		CHECK(CountErrors(before, "is empty") == 1);
	}
}
