#include "Engine/ModuleRegistrationHelpers.h"

#include "Strata/Asset/AssetDependencies.h"
#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/MeshGeometry.h"
#include "Strata/Core/Assert.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Engine/BuiltinModules.h"
#include "Strata/Reflection/ComponentGizmo.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Renderer/DebugDraw.h"
#include "Strata/Renderer/SceneGizmos.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/SceneSystem.h"
#include "Strata/Scene/UnknownComponents.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "Strata/Scripting/ScriptSystem.h"
#include "Strata/Scripting/ScriptingComponents.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/JsonUtils.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace Strata::Tests
{

	namespace
	{

		// A component of a game module: registered through ModuleRegistrationOptions::Extra. What it carries is extra data the
		// properties do not express: the entities riding in it and the asset it transports, with their references described
		// (ComponentInfoBuilder::ExtraReferences), so that generic code remaps, compares and scans them.
		struct TestVehicleComponent
		{
			float Speed = 0.0f;
			std::string Label;
			std::vector<UUID> Passengers;
			AssetHandle Cargo = AssetHandle::Null();
		};

		constexpr uint32_t c_PassengerSlot = 0;
		constexpr uint32_t c_CargoSlot = 1;

		void SerializeVehicle(const TestVehicleComponent& vehicle, nlohmann::json& out)
		{
			nlohmann::json passengers = nlohmann::json::array();
			for (const UUID passenger : vehicle.Passengers)
				passengers.push_back(static_cast<uint64_t>(passenger));
			out["Passengers"] = passengers;
			out["Cargo"] = static_cast<uint64_t>(vehicle.Cargo);
		}

		bool DeserializeVehicle(TestVehicleComponent& vehicle, const nlohmann::json& in, std::string* outError)
		{
			const auto passengers = in.find("Passengers");
			const auto cargo = in.find("Cargo");
			if (passengers == in.end() || !passengers->is_array() || cargo == in.end() || !cargo->is_number_unsigned())
			{
				if (outError)
					*outError = "a vehicle needs its Passengers and its Cargo";
				return false;
			}
			vehicle.Passengers.clear();
			for (const nlohmann::json& passenger : *passengers)
			{
				if (passenger.is_number_unsigned())
					vehicle.Passengers.push_back(UUID(passenger.get<uint64_t>()));
			}
			vehicle.Cargo = AssetHandle(cargo->get<uint64_t>());
			return true;
		}

		void VisitVehicleReferences(const TestVehicleComponent& vehicle, const ComponentExtraReferences::Visitor& visitor)
		{
			for (uint32_t index = 0; index < vehicle.Passengers.size(); index++)
				visitor(c_PassengerSlot, index, PropertyType::Entity, vehicle.Passengers[index]);
			visitor(c_CargoSlot, 0, PropertyType::Asset, vehicle.Cargo);
		}

		void SetVehicleReference(TestVehicleComponent& vehicle, uint32_t slot, uint32_t subslot, UUID value)
		{
			if (slot == c_PassengerSlot && subslot < vehicle.Passengers.size())
				vehicle.Passengers[subslot] = value;
			else if (slot == c_CargoSlot)
				vehicle.Cargo = value;
		}

		bool VehiclesEqual(const TestVehicleComponent& a, const TestVehicleComponent& b, const ComponentExtraReferences::Remap& remap)
		{
			if (a.Passengers.size() != b.Passengers.size() || a.Cargo != b.Cargo)
				return false;
			for (size_t index = 0; index < a.Passengers.size(); index++)
			{
				if (remap(a.Passengers[index]) != b.Passengers[index])
					return false;
			}
			return true;
		}

		void VisitVehicleReferencesJson(const nlohmann::json& extra, const ComponentExtraReferences::JsonVisitor& visitor)
		{
			TestVehicleComponent vehicle;
			if (!DeserializeVehicle(vehicle, extra, nullptr))
				return;
			VisitVehicleReferences(vehicle, [&visitor](uint32_t, uint32_t, PropertyType type, UUID value) { visitor(type, value); });
		}

		// The editor draws a vehicle as a box around its origin, with the colliders (ComponentInfoBuilder::Gizmo).
		constexpr glm::vec3 c_VehicleHalfExtents(1.0f, 0.5f, 2.0f);

		bool VehicleGizmo(const entt::registry&, entt::entity, const void*, const glm::mat4& world, ComponentGizmoShape& outShape)
		{
			outShape.Type = ComponentGizmoShape::Kind::Box;
			outShape.Transform = world;
			outShape.HalfExtents = c_VehicleHalfExtents;
			return true;
		}

		// A vehicle (with Speed, Label and a cargo) whose driver, its child, rides in it.
		Entity CreateVehicleWithDriver(Scene& scene, const std::string& name)
		{
			Entity vehicle = scene.CreateEntity(name);
			Entity driver = scene.CreateEntity(name + " Driver");
			scene.SetParent(driver, vehicle);
			vehicle.AddComponent<TestVehicleComponent>(TestVehicleComponent { 12.5f, "Rover One", { driver.GetUUID() }, AssetHandle(0xCA460) });
			return vehicle;
		}

		// Whether the vehicle's only passenger is its first child (its driver).
		bool RidesWithItsChild(const Entity& vehicle)
		{
			const TestVehicleComponent* component = vehicle.TryGetComponent<TestVehicleComponent>();
			const std::vector<Entity> children = vehicle.GetChildren();
			return component && component->Passengers.size() == 1 && !children.empty() && component->Passengers[0] == children.front().GetUUID();
		}

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
					.Property("Label", &TestVehicleComponent::Label)
					.Extra(SerializeVehicle, DeserializeVehicle)
					.ExtraReferences(VisitVehicleReferences, SetVehicleReference, VehiclesEqual, VisitVehicleReferencesJson)
					.Gizmo(ComponentGizmoGroup::Collider, VehicleGizmo);
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
				CreateVehicleWithDriver(scene, "Rover");
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
			if (vehicle->Speed != 12.5f || vehicle->Label != "Rover One" || vehicle->Cargo != AssetHandle(0xCA460) || !RidesWithItsChild(rover))
				return Fail("the reloaded TestVehicle has other values");

			// The extra data's references, through ComponentInfo::ExtraReferences: copies ride with their own drivers...
			Scene& garage = *loaded;
			const Entity duplicate = garage.DuplicateEntity(rover);
			if (!duplicate || !RidesWithItsChild(duplicate) || duplicate.GetChildren().front() == rover.GetChildren().front())
				return Fail("the duplicate's passenger is not its own driver");
			const Ref<Prefab> prefab = Prefab::CreateFromEntities(garage, { rover });
			if (!prefab)
				return Fail("no prefab of the vehicle");
			prefab->Handle = AssetHandle(0x50FE);
			const std::vector<Entity> instances = prefab->Instantiate(garage);
			if (instances.empty() || !RidesWithItsChild(instances.front()))
				return Fail("the prefab instance's passenger is not its own driver");
			// ...instances compare equal to the prefab with their references mapped, and differ once a passenger changes...
			const auto vehicleDiffers = [&](const Entity& instance)
			{
				for (const PrefabInstanceEntity& entity : prefab->CompareInstance(garage, instance))
				{
					if (entity.Instance == instance)
						return std::find(entity.Differences.begin(), entity.Differences.end(), vehicleInfo) != entity.Differences.end();
				}
				return true;
			};
			if (vehicleDiffers(instances.front()))
				return Fail("the untouched prefab instance differs from its prefab");
			Entity instance = instances.front();
			instance.GetComponent<TestVehicleComponent>().Passengers[0] = instance.GetUUID();
			if (!vehicleDiffers(instance))
				return Fail("an instance whose passenger changed does not differ from its prefab");
			// ...and documents name the cargo among their assets.
			std::vector<AssetHandle> references;
			AssetDependencies::ScanEntities(SceneSerializer::SerializeEntities(garage, { rover }), references);
			if (std::find(references.begin(), references.end(), AssetHandle(0xCA460)) == references.end())
				return Fail("the vehicle's cargo is not among the assets its snapshot references");
			std::printf("extra references: remapped, compared, scanned\n");

			// The renderer draws the game's component with the colliders, without knowing its type: a box per vehicle.
			garage.UpdateWorldTransforms();
			size_t vehicles = 0;
			for ([[maybe_unused]] const auto entity : garage.GetRegistry().view<TestVehicleComponent>())
				vehicles++;
			DebugDraw boxLines;
			boxLines.Box(glm::mat4(1.0f), c_VehicleHalfExtents, glm::vec4(1.0f));
			DebugDraw gizmos;
			SceneGizmoSettings settings;
			settings.Lights = false;
			settings.Cameras = false;
			DrawSceneGizmos(gizmos, garage, settings);
			if (vehicles != 3 || gizmos.GetLineCount() != vehicles * boxLines.GetLineCount())
				return Fail(fmt::format("{} vehicles drew {} gizmo lines, not {} boxes", vehicles, gizmos.GetLineCount(), vehicles));
			settings.Colliders = false;
			DebugDraw hidden;
			DrawSceneGizmos(hidden, garage, settings);
			if (hidden.GetLineCount() != 0)
				return Fail("the vehicles' gizmos show without the colliders");
			std::printf("gizmos: a box per vehicle\n");

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
			else if (registry == "mesh-geometry")
				MeshGeometryRegistry::Register(AssetType::Mesh, [](const Asset&, MeshGeometry&) { return false; });
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
			for (AssetType type : { AssetType::Scene, AssetType::Prefab, AssetType::Model, AssetType::Texture, AssetType::Mesh, AssetType::Material, AssetType::Font, AssetType::AudioClip,
				AssetType::Skeleton, AssetType::AnimationClip, AssetType::AnimationController })
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

		std::string Join(const std::vector<std::string>& names)
		{
			std::string text;
			for (const std::string& name : names)
				text += (text.empty() ? "" : " ") + name;
			return text;
		}

		std::vector<std::string> Split(std::string_view text, char separator)
		{
			std::vector<std::string> parts;
			size_t start = 0;
			while (start <= text.size())
			{
				const size_t end = std::min(text.find(separator, start), text.size());
				if (end > start)
					parts.emplace_back(text.substr(start, end - start));
				start = end + 1;
			}
			return parts;
		}

		size_t CountLogEntries(uint64_t after, std::string_view text)
		{
			size_t count = 0;
			for (const LogEntry& entry : Log::GetBuffer().GetEntries(after))
				count += entry.Message.find(text) != std::string::npos ? 1 : 0;
			return count;
		}

		// An engine without some of its modules (ModuleRegistrationOptions::Exclude): what registers, how the systems order,
		// what becomes of a scene's components of the absent modules, and what scripts are told when they call them.
		int RunWithoutModules(int argc, char** argv)
		{
			if (argc < 4)
				return Fail("usage: register-without <module>[,<module>...] <scene file>");
			Engine::ModuleRegistrationOptions options;
			options.Exclude = Split(argv[2], ',');
			Engine::RegisterBuiltinModules(options);
			std::vector<std::string> systems;
			for (const SceneSystemDescriptor& descriptor : SceneSystemRegistry::GetAll())
				systems.push_back(descriptor.Name);
			std::printf("modules: %s\nsystems: %s\n", Join(Engine::GetRegisteredModules()).c_str(), Join(systems).c_str());

			// The scene's components of the absent modules stay as unknown components and save back unchanged.
			const std::filesystem::path scenePath = FileSystem::FromUTF8(argv[3]);
			const std::optional<std::string> text = FileSystem::ReadText(scenePath);
			const std::optional<nlohmann::json> file = text ? JsonUtils::Parse(*text) : std::nullopt;
			std::string error;
			const Ref<Scene> loaded = SceneSerializer::LoadFromFile(scenePath, &error);
			if (!file || !loaded)
				return Fail("the scene does not load: " + error);
			std::map<std::string, nlohmann::json> fileComponents; // "<entity id>/<component>" -> its JSON in the file
			for (const nlohmann::json& entity : (*file)["Scene"]["Entities"])
			{
				for (const auto& [name, component] : entity["Components"].items())
				{
					if (!ComponentRegistry::Find(name))
						fileComponents[entity["ID"].dump() + "/" + name] = component;
				}
			}
			std::set<std::string> unknownNames;
			size_t unchanged = 0;
			const nlohmann::json saved = SceneSerializer::Serialize(*loaded);
			for (const nlohmann::json& entity : saved["Scene"]["Entities"])
			{
				for (const auto& [name, component] : entity["Components"].items())
				{
					const auto it = fileComponents.find(entity["ID"].dump() + "/" + name);
					if (it == fileComponents.end())
						continue;
					unknownNames.insert(name);
					unchanged += it->second == component ? 1 : 0;
				}
			}
			std::printf("unknown components: %s\nsaved unchanged: %zu of %zu\n", Join({ unknownNames.begin(), unknownNames.end() }).c_str(), unchanged,
				fileComponents.size());

			// A scene plays without them; the API test module's physics calls report the absent module and fail.
			const Ref<ScriptEngine> engine = CreateRef<ScriptEngine>();
			if (!engine->LoadModule(Platform::GetExecutableDirectory() / STRATA_TEST_SCRIPTS_API, &error))
				return Fail("the API test module does not load: " + error);
			ScriptEngine::SetActive(engine);
			const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
			{
				Scene scene;
				Entity crate = scene.CreateEntity("Crate");
				crate.GetTransform().Translation = glm::vec3(0.0f, 5.0f, 0.0f);
				Entity tester = scene.CreateEntity("Tester");
				ScriptEntry probe;
				probe.ClassName = "PhysicsWithoutSystem";
				tester.AddComponent<ScriptComponent>().Scripts.push_back(std::move(probe));
				scene.OnRuntimeStart();
				for (int frame = 0; frame < 10; frame++)
					scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
				const ScriptSystem* scripts = scene.GetSystem<ScriptSystem>();
				const std::optional<PropertyValue> done = scripts ? scripts->GetFieldValue(tester, "PhysicsWithoutSystem", "Done") : std::nullopt;
				const std::optional<PropertyValue> failure = scripts ? scripts->GetFieldValue(tester, "PhysicsWithoutSystem", "Failure") : std::nullopt;
				const bool ran = done && std::holds_alternative<bool>(*done) && std::get<bool>(*done);
				const std::string failed = failure && std::holds_alternative<std::string>(*failure) ? std::get<std::string>(*failure) : std::string("(none)");
				std::printf("scripts ran: %s, failed check: %s, faulted: %s\n", ran ? "yes" : "no", failed.empty() ? "(none)" : failed.c_str(),
					engine->IsFaulted() ? "yes" : "no");
				scene.OnRuntimeStop();
			}
			std::printf("reported: %zu\n", CountLogEntries(logStart, "the scene plays without physics (the engine has no physics module)"));
			ScriptEngine::SetActive(nullptr);
			std::fflush(stdout);
			return 0;
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
		if (mode == "register-without")
		{
			LogSpecification logSpecification;
			logSpecification.Level = LogLevel::Warn;
			Log::Init(logSpecification);
			const int result = RunWithoutModules(argc, argv);
			Log::Shutdown();
			return result;
		}
		return std::nullopt;
	}

}
