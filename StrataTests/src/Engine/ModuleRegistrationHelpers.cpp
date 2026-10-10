#include "Engine/ModuleRegistrationHelpers.h"

#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/Assert.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Engine/BuiltinModules.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/SceneSystem.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace Strata::Tests
{

	namespace
	{

		// A component of a game module: registered through ModuleRegistrationOptions::Extra.
		struct TestVehicleComponent
		{
			float Speed = 0.0f;
			std::string Label;
		};

		struct LateComponent
		{
			int32_t Value = 0;
		};

		int Fail(const std::string& message)
		{
			std::printf("FAILED: %s\n", message.c_str());
			std::fflush(stdout);
			return 1;
		}

		// The newest error entry after `after` that contains `text`, or an empty string.
		std::string FindLoggedError(uint64_t after, std::string_view text)
		{
			std::string found;
			for (const LogEntry& entry : Log::GetBuffer().GetEntries(after))
			{
				if (entry.Level == LogLevel::Error && entry.Message.find(text) != std::string::npos)
					found = entry.Message;
			}
			return found;
		}

		int RunCustomComponent(int argc, char** argv)
		{
			if (argc < 3)
				return 2;
			const std::filesystem::path directory = FileSystem::FromUTF8(argv[2]);

			bool duplicateTypeRefused = false;
			bool duplicateNameRefused = false;
			bool pipelineBeforeExtra = false;
			Engine::ModuleRegistrationOptions options;
			options.AssetPipeline = RegisterAssetPipeline;
			options.Extra.push_back([&]()
			{
				// The asset pipeline registers before the extra registrations, which may override its importers.
				pipelineBeforeExtra = AssetImporterRegistry::FindByExtension(".gltf") != nullptr;
				ComponentRegistry::Register<TestVehicleComponent>("TestVehicle")
					.Category("Tests")
					.Property("Speed", &TestVehicleComponent::Speed)
					.Property("Label", &TestVehicleComponent::Label);
				// The type is registered and the name is taken (ignoring case): both are refused, without aborting.
				duplicateTypeRefused = !ComponentRegistry::Register<TestVehicleComponent>("TestVehicleAgain").Property("Speed", &TestVehicleComponent::Speed);
				duplicateNameRefused = !ComponentRegistry::Register<LateComponent>("testvehicle");
			});
			Engine::RegisterBuiltinModules(options);

			if (!Engine::AreBuiltinModulesRegistered() || !ComponentRegistry::IsFrozen())
				return Fail("the modules are not registered, or the component registry is not frozen");
			const ComponentInfo* vehicleInfo = ComponentRegistry::Find("TestVehicle");
			if (!vehicleInfo || vehicleInfo != ComponentRegistry::Find<TestVehicleComponent>() || vehicleInfo->Properties.size() != 2)
				return Fail("the extra component is not registered as given");
			if (!ComponentRegistry::Find("Transform"))
				return Fail("the built-in components are missing");
			if (!pipelineBeforeExtra)
				return Fail("the asset pipeline was not registered before the extra registrations");
			if (!duplicateTypeRefused || !duplicateNameRefused || ComponentRegistry::Find("TestVehicleAgain"))
				return Fail("a component type or name was registered twice");
			std::printf("registered: TestVehicle\n");

			const std::filesystem::path path = directory / "Garage.stscene";
			{
				Scene scene("Garage");
				Entity rover = scene.CreateEntity("Rover");
				rover.AddComponent<TestVehicleComponent>(TestVehicleComponent { 12.5f, "Rover One" });
				std::string error;
				if (!SceneSerializer::SaveToFile(scene, path, &error))
					return Fail("saving the scene: " + error);
			}
			std::string error;
			const Ref<Scene> loaded = SceneSerializer::LoadFromFile(path, &error);
			if (!loaded)
				return Fail("loading the scene: " + error);
			const Entity rover = loaded->FindEntityByName("Rover");
			const TestVehicleComponent* vehicle = rover ? rover.TryGetComponent<TestVehicleComponent>() : nullptr;
			if (!vehicle)
				return Fail("the reloaded scene has no TestVehicle");
			std::printf("reloaded: Speed %.2f, Label %s\n", static_cast<double>(vehicle->Speed), vehicle->Label.c_str());
			if (vehicle->Speed != 12.5f || vehicle->Label != "Rover One")
				return Fail("the reloaded TestVehicle has other values");

			// The registry is frozen now: a registration is refused with an error, and the process goes on.
			const uint64_t before = Log::GetBuffer().GetLatestSequence();
			const bool lateRegistered = static_cast<bool>(ComponentRegistry::Register<LateComponent>("Late").Property("Value", &LateComponent::Value));
			const std::string lateError = FindLoggedError(before, "'Late'");
			if (lateRegistered || ComponentRegistry::Find("Late") || ComponentRegistry::Find<LateComponent>())
				return Fail("a component was registered after Freeze");
			if (lateError.find("frozen") == std::string::npos)
				return Fail("the refused registration logged no error");
			std::printf("late registration refused: %s\n", lateError.c_str());
			std::fflush(stdout);
			return 0;
		}

		AssertAction ExitOnVerify(const AssertInfo& info)
		{
			std::printf("verify failed: %s\n", info.Message.c_str());
			std::fflush(stdout);
			std::_Exit(c_VerifyExitCode);
		}

		int UseUnregisteredRegistry(int argc, char** argv)
		{
			if (argc < 3)
				return 2;
			SetAssertHandler(ExitOnVerify);
			const std::string_view registry = argv[2];
			if (registry == "components")
				std::printf("%zu components\n", ComponentRegistry::GetAll().size());
			else if (registry == "loaders")
				std::printf("loader: %s\n", AssetLoaderRegistry::Find(AssetType::Texture) ? "found" : "missing");
			else if (registry == "importers")
				std::printf("%zu importers\n", AssetImporterRegistry::GetAll().size());
			else if (registry == "builtin-assets")
				std::printf("factory: %s\n", BuiltinAssets::RegisterFactory(BuiltinAssets::CubeMesh, []() { return Ref<Asset>(); }) ? "registered" : "refused");
			else if (registry == "systems")
				std::printf("%zu systems\n", SceneSystemRegistry::GetAll().size());
			else
				return 2;
			// Reaching this means the registry was used without a verify failing.
			std::fflush(stdout);
			return 1;
		}

		// A shipped game's registration: every module, no asset pipeline.
		int RunWithoutAssetPipeline()
		{
			SetAssertHandler(ExitOnVerify);
			Engine::RegisterBuiltinModules();
			if (!Engine::AreBuiltinModulesRegistered() || !ComponentRegistry::Find("Transform"))
				return Fail("the modules are not registered");

			uint32_t loaders = 0;
			for (AssetType type : { AssetType::Scene, AssetType::Prefab, AssetType::Model, AssetType::Texture, AssetType::Mesh, AssetType::Material, AssetType::Font, AssetType::AudioClip })
				loaders += AssetLoaderRegistry::Find(type) ? 1 : 0;
			std::string systems;
			for (const SceneSystemDescriptor& descriptor : SceneSystemRegistry::GetAll())
				systems += " " + descriptor.Name;
			std::printf("loaders: %u, systems:%s\n", loaders, systems.c_str());
			std::fflush(stdout);

			// Never opened: the verify fails and exits.
			std::printf("%zu importers\n", AssetImporterRegistry::GetAll().size());
			std::fflush(stdout);
			return 1;
		}

	}

	std::optional<int> RunModuleRegistrationHelper(std::string_view mode, int argc, char** argv)
	{
		if (mode == "custom-component")
		{
			LogSpecification logSpecification;
			logSpecification.Level = LogLevel::Warn;
			Log::Init(logSpecification);
			const int result = RunCustomComponent(argc, argv);
			Log::Shutdown();
			return result;
		}
		if (mode == "unregistered-registry")
			return UseUnregisteredRegistry(argc, argv);
		if (mode == "no-asset-pipeline")
			return RunWithoutAssetPipeline();
		return std::nullopt;
	}

}
