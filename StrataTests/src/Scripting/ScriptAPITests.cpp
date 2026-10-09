#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Input/Input.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scripting/ScriptHostAPI.h"

#include "StrataScript/ScriptABI.h"

#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Serves registered asset bytes from memory.
	class TestAssetManager final : public AssetManagerBase
	{
	public:
		~TestAssetManager() override
		{
			WaitForInFlightLoads();
		}

		void Add(const AssetMetadata& metadata, std::vector<uint8_t> data)
		{
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_Data[metadata.Handle] = std::move(data);
			}
			RegisterAsset(metadata);
		}
	protected:
		bool ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError) override
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Data.find(metadata.Handle);
			if (it == m_Data.end())
			{
				if (outError)
					*outError = "No data";
				return false;
			}
			outData = it->second;
			return true;
		}
	private:
		std::mutex m_Mutex;
		std::unordered_map<AssetHandle, std::vector<uint8_t>> m_Data;
	};

	struct ScopedAssetManager
	{
		explicit ScopedAssetManager(const Ref<AssetManagerBase>& manager)
		{
			AssetManager::SetActive(manager);
		}

		~ScopedAssetManager()
		{
			AssetManager::SetActive(nullptr);
		}
	};

	std::vector<uint8_t> ToBytes(const nlohmann::json& json)
	{
		const std::string text = JsonUtils::Dump(json);
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// The SDK value type that reads a property of the given type.
	const char* GetScriptReadType(PropertyType type)
	{
		switch (type)
		{
			case PropertyType::Bool:   return "Bool";
			case PropertyType::Int:    return "Int";
			case PropertyType::UInt:   return "Int";
			case PropertyType::Enum:   return "Int";
			case PropertyType::Float:  return "Float";
			case PropertyType::Vec2:   return "Vec2";
			case PropertyType::Vec3:   return "Vec3";
			case PropertyType::Color3: return "Vec3";
			case PropertyType::Vec4:   return "Vec4";
			case PropertyType::Color4: return "Vec4";
			case PropertyType::Quat:   return "Quat";
			case PropertyType::String: return "String";
			case PropertyType::Entity: return "Entity";
			case PropertyType::Asset:  return "Asset";
		}
		return "";
	}

	StrataScriptString ABIString(const char* text)
	{
		return StrataScriptString { text, static_cast<uint64_t>(std::strlen(text)) };
	}

}

TEST_SUITE("Scripting.API")
{
	TEST_CASE("Entity API")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity tester = scene.CreateEntity("Tester");
		AddScriptEntry(tester, "EntityAPI");
		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);

		CheckScriptChecks(system, tester, "EntityAPI", 30);
		CHECK(tester.GetName() == "Renamed");
		CHECK_FALSE(tester.HasComponent<TagComponent>());
		// Destruction requested while starting happened once the start completed.
		CHECK_FALSE(scene.GetEntityByUUID(GetField<UUID>(system, tester, "EntityAPI", "DoomedEntity")).IsValid());
		const Entity createdRoot = scene.GetEntityByUUID(GetField<UUID>(system, tester, "EntityAPI", "CreatedRoot"));
		const Entity createdChild = scene.GetEntityByUUID(GetField<UUID>(system, tester, "EntityAPI", "CreatedChild"));
		REQUIRE(createdRoot.IsValid());
		REQUIRE(createdChild.IsValid());
		CHECK(createdChild.GetParent() == createdRoot);
		CHECK(createdRoot.GetName() == "CreatedRoot");
		CHECK(createdRoot.IsActive());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Generic component access")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity entity = scene.CreateEntity("Components");
		AddScriptEntry(entity, "ComponentAPI");
		scene.OnRuntimeStart();

		CheckScriptChecks(GetScriptSystem(scene), entity, "ComponentAPI", 46);
		CHECK_FALSE(entity.HasComponent<CameraComponent>());
		REQUIRE(entity.HasComponent<TextComponent>());
		CHECK(entity.GetComponent<TextComponent>().Text == "Score: 10");
		CHECK(entity.GetComponent<TextComponent>().FontSize == 48.0f);
		CHECK(entity.GetComponent<TextComponent>().Alignment == TextAlignment::Right);
		CHECK(entity.GetComponent<RigidBodyComponent>().Layer == 5u);
		CHECK(entity.GetComponent<MeshRendererComponent>().Mesh == UUID(0x42));
		CHECK(entity.GetComponent<BoxColliderComponent>().HalfExtents == glm::vec3(1.0f, 2.0f, 3.0f));
		CHECK(entity.GetComponent<PointLightComponent>().Color == glm::vec3(1.0f, 0.5f, 0.25f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Every registered component property is reachable from scripts")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity probe = scene.CreateEntity("Probe");
		AddScriptEntry(probe, "PropertyProbe");
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);

		int32_t probes = 0;
		std::unordered_set<std::string> probedComponents;
		for (const ComponentInfo* info : ComponentRegistry::GetAll())
		{
			// Scripts cannot add components only the engine adds (the probe's AddComponent fails), but their properties
			// must still be reachable on entities that have them.
			if (HasFlag(info->Flags, ComponentFlags::EngineAdded) && !info->Has(scene.GetRegistry(), probe.GetHandle()))
				info->Add(scene.GetRegistry(), probe.GetHandle());

			for (const PropertyInfo& property : info->Properties)
			{
				INFO("Property ", info->Name, ".", property.Name);
				REQUIRE(system.SetFieldValue(probe, "PropertyProbe", "Component", info->Name));
				REQUIRE(system.SetFieldValue(probe, "PropertyProbe", "Property", property.Name));
				scene.OnUpdateRuntime(0.0f);
				probes++;
				probedComponents.insert(info->Name);
				CHECK(GetField<int32_t>(system, probe, "PropertyProbe", "Probes") == probes);
				CHECK(GetField<std::string>(system, probe, "PropertyProbe", "ReadType") == GetScriptReadType(property.Type));
				CHECK(GetField<bool>(system, probe, "PropertyProbe", "WroteBack") == !property.IsReadOnly());
			}
		}
		// The loop proves nothing over a (nearly) empty registry: every built-in component with data must have been probed.
		for (const char* name : { "Transform", "Camera", "MeshRenderer", "DirectionalLight", "PointLight", "SpotLight", "SkyLight",
				 "PostProcess", "Text", "RigidBody", "BoxCollider", "SphereCollider", "CapsuleCollider", "MeshCollider", "AudioSource",
				 "AudioListener", "PrefabInstance" })
		{
			INFO("Component ", name);
			CHECK(probedComponents.contains(name));
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("Transform API")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity entity = scene.CreateEntity("Moving");
		AddScriptEntry(entity, "TransformAPI");
		scene.OnRuntimeStart();

		CheckScriptChecks(GetScriptSystem(scene), entity, "TransformAPI", 19);
		CHECK(entity.GetParent().GetName() == "TransformParent");
		const TransformComponent& transform = entity.GetComponent<TransformComponent>();
		CHECK(glm::all(glm::epsilonEqual(transform.Translation, glm::vec3(-10.0f, 0.0f, 0.0f), 1e-4f)));
		CHECK(glm::all(glm::epsilonEqual(glm::vec3(scene.GetWorldTransform(entity)[3]), glm::vec3(-10.0f, 0.0f, 0.0f), 1e-4f)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Prefabs and models are instantiated, with their scripts available immediately")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));

		// The prefab: "Bullet" with a Spawned script and a child "Trail".
		Scene authoring;
		Entity bullet = authoring.CreateEntity("Bullet");
		AddScriptEntry(bullet, "Spawned");
		authoring.CreateChildEntity(bullet, "Trail");
		const Ref<Prefab> prefab = Prefab::CreateFromEntities(authoring, { bullet });

		Ref<TestAssetManager> manager = CreateRef<TestAssetManager>();
		AssetMetadata prefabMetadata;
		prefabMetadata.Handle = UUID(0x5000);
		prefabMetadata.Type = AssetType::Prefab;
		prefabMetadata.Path = "Prefabs/Bullet.stprefab";
		prefabMetadata.Name = "Bullet";
		manager->Add(prefabMetadata, ToBytes(prefab->Serialize())); // Not loaded until a script needs it

		AssetMetadata modelMetadata;
		modelMetadata.Handle = UUID(0x6000);
		modelMetadata.Path = "Models/Ship.gltf";
		modelMetadata.Name = "Ship";
		const AssetHandle model = manager->AddMemoryAsset(Model::CreateFromSnapshot(SceneSerializer::SerializeEntities(authoring, { bullet })), modelMetadata);
		ScopedAssetManager scopedManager(manager);

		Scene scene;
		CreateLogEntity(scene);
		Entity spawner = scene.CreateEntity("Spawner");
		ScriptEntry& entry = AddScriptEntry(spawner, "Spawner");
		AddFieldOverride(entry, "PrefabPath", PropertyType::String, std::string("Prefabs/Bullet.stprefab"));
		AddFieldOverride(entry, "Prefab", PropertyType::Asset, prefabMetadata.Handle);
		AddFieldOverride(entry, "Model", PropertyType::Asset, model);

		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		CheckScriptChecks(system, spawner, "Spawner", 14);

		const Entity first = scene.GetEntityByUUID(GetField<UUID>(system, spawner, "Spawner", "FirstSpawned"));
		REQUIRE(first.IsValid());
		CHECK(first.GetName() == "Bullet");
		CHECK(first.GetParent() == spawner);
		CHECK(first.GetComponent<PrefabInstanceComponent>().Prefab == prefabMetadata.Handle);
		CHECK(first.GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
		CHECK(GetField<int32_t>(system, first, "Spawned", "ValueSeenInCreate") == 7); // Configured before its OnCreate

		const Entity byPath = scene.GetEntityByUUID(GetField<UUID>(system, spawner, "Spawner", "PathSpawned"));
		REQUIRE(byPath.IsValid());
		CHECK_FALSE(byPath.GetParent().IsValid());
		CHECK(GetField<int32_t>(system, byPath, "Spawned", "ValueSeenInCreate") == 0);

		const Entity fromModel = scene.GetEntityByUUID(GetField<UUID>(system, spawner, "Spawner", "ModelSpawned"));
		REQUIRE(fromModel.IsValid());
		CHECK(fromModel.GetComponent<PrefabInstanceComponent>().Prefab == model);
		CHECK(system.HasInstance(fromModel, "Spawned"));
		CHECK(GetLog(scene) == "Bullet.Spawned.Create;Bullet.Spawned.Create;Bullet.Spawned.Create;");

		// Spawning during updates.
		const size_t entityCount = scene.GetEntityCount();
		REQUIRE(system.SetFieldValue(spawner, "Spawner", "SpawnPerUpdate", int32_t(3)));
		scene.OnUpdateRuntime(0.0f);
		CHECK(GetField<int32_t>(system, spawner, "Spawner", "SpawnCount") == 3);
		CHECK(scene.GetEntityCount() == entityCount + 3 * 2);
		const Entity last = scene.GetEntityByUUID(GetField<UUID>(system, spawner, "Spawner", "LastSpawned"));
		REQUIRE(last.IsValid());
		CHECK(GetField<int32_t>(system, last, "Spawned", "ValueSeenInCreate") == 102);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts call other scripts")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity talker = scene.CreateEntity("Talker");
		AddScriptEntry(talker, "Talker");
		Entity listener = scene.CreateEntity("ListenerEntity");
		AddScriptEntry(listener, "Listener");
		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);
		RunFrames(scene, 3);

		CheckScriptChecks(system, talker, "Talker", 15);
		CHECK(GetField<int32_t>(system, listener, "Listener", "Received") == 3);
		CHECK(GetField<std::string>(system, listener, "Listener", "LastMessage") == "hello 2");
		scene.OnRuntimeStop();
	}

	TEST_CASE("Input")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity entity = scene.CreateEntity("Input");
		AddScriptEntry(entity, "InputProbe");
		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);

		Input::Reset();
		Input::SetViewport({ 0.0f, 0.0f }, { 0.0f, 0.0f });
		Input::BeginFrame();
		Input::ProcessKey(Key::W, true);
		Input::ProcessMouseButton(Mouse::ButtonLeft, true);
		Input::ProcessMouseMove({ 100.0f, 50.0f });
		Input::ProcessMouseMove({ 110.0f, 45.0f });
		Input::ProcessScroll({ 0.0f, 2.0f });
		scene.OnUpdateRuntime(0.0f);
		CHECK(GetField<bool>(system, entity, "InputProbe", "WDown"));
		CHECK(GetField<bool>(system, entity, "InputProbe", "WPressed"));
		CHECK_FALSE(GetField<bool>(system, entity, "InputProbe", "WReleased"));
		CHECK(GetField<bool>(system, entity, "InputProbe", "LeftDown"));
		CHECK(GetField<bool>(system, entity, "InputProbe", "LeftPressed"));
		CHECK_FALSE(GetField<bool>(system, entity, "InputProbe", "LeftReleased"));
		CHECK_FALSE(GetField<bool>(system, entity, "InputProbe", "InvalidKeyDown"));
		CHECK_FALSE(GetField<bool>(system, entity, "InputProbe", "InvalidButtonDown"));
		CHECK(GetField<glm::vec2>(system, entity, "InputProbe", "MousePosition") == glm::vec2(110.0f, 45.0f));
		CHECK(GetField<glm::vec2>(system, entity, "InputProbe", "MouseDelta") == glm::vec2(10.0f, -5.0f));
		CHECK(GetField<glm::vec2>(system, entity, "InputProbe", "ScrollDelta") == glm::vec2(0.0f, 2.0f));

		Input::BeginFrame();
		Input::ProcessKey(Key::W, false);
		Input::ProcessMouseButton(Mouse::ButtonLeft, false);
		scene.OnUpdateRuntime(0.0f);
		CHECK_FALSE(GetField<bool>(system, entity, "InputProbe", "WDown"));
		CHECK_FALSE(GetField<bool>(system, entity, "InputProbe", "WPressed"));
		CHECK(GetField<bool>(system, entity, "InputProbe", "WReleased"));
		CHECK_FALSE(GetField<bool>(system, entity, "InputProbe", "LeftDown"));
		CHECK(GetField<bool>(system, entity, "InputProbe", "LeftReleased"));
		CHECK(GetField<glm::vec2>(system, entity, "InputProbe", "MouseDelta") == glm::vec2(0.0f));

		Input::Reset();
		scene.OnRuntimeStop();
	}

	TEST_CASE("Time")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		scene.GetSettings().FixedTimestep = 0.25f; // Binary fractions keep the step counts exact
		Entity entity = scene.CreateEntity("Time");
		AddScriptEntry(entity, "TimeProbe");
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);

		scene.OnUpdateRuntime(0.625f);
		CHECK(GetField<float>(system, entity, "TimeProbe", "UpdateArgument") == 0.625f);
		CHECK(GetField<float>(system, entity, "TimeProbe", "DeltaTime") == 0.625f);
		CHECK(GetField<float>(system, entity, "TimeProbe", "LateArgument") == 0.625f);
		CHECK(GetField<float>(system, entity, "TimeProbe", "ElapsedTime") == 0.0f);
		CHECK(GetField<int32_t>(system, entity, "TimeProbe", "FrameIndex") == 0);
		CHECK(GetField<int32_t>(system, entity, "TimeProbe", "FixedSteps") == 2);
		CHECK(GetField<float>(system, entity, "TimeProbe", "FixedArgument") == 0.25f);
		CHECK(GetField<float>(system, entity, "TimeProbe", "FixedDeltaTime") == 0.25f);
		CHECK(GetField<float>(system, entity, "TimeProbe", "TimeScale") == 1.0f);

		scene.OnUpdateRuntime(0.625f);
		CHECK(GetField<float>(system, entity, "TimeProbe", "ElapsedTime") == 0.625f);
		CHECK(GetField<int32_t>(system, entity, "TimeProbe", "FrameIndex") == 1);
		CHECK(GetField<int32_t>(system, entity, "TimeProbe", "FixedSteps") == 5);

		REQUIRE(system.SetFieldValue(entity, "TimeProbe", "SetTimeScaleTo", 0.5f));
		scene.OnUpdateRuntime(0.625f);
		CHECK(scene.GetTimeScale() == 0.5f);
		scene.OnUpdateRuntime(0.625f);
		CHECK(GetField<float>(system, entity, "TimeProbe", "UpdateArgument") == 0.3125f);
		CHECK(GetField<float>(system, entity, "TimeProbe", "TimeScale") == 0.5f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scene queries")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity secondary = scene.CreateEntity("SecondaryCamera");
		secondary.AddComponent<CameraComponent>().Primary = false;
		Entity mainCamera = scene.CreateEntity("MainCamera");
		mainCamera.AddComponent<CameraComponent>();
		Entity group = scene.CreateEntity("Enemies");
		Entity firstEnemy = scene.CreateChildEntity(group, "Grunt");
		firstEnemy.AddComponent<TagComponent>("Enemy");
		firstEnemy.GetComponent<TransformComponent>().Translation = { 4.0f, 0.0f, 0.0f };
		scene.CreateEntity("Boss").AddComponent<TagComponent>("Enemy");
		Entity probe = scene.CreateEntity("Probe");
		AddScriptEntry(probe, "SceneProbe");

		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);
		scene.OnUpdateRuntime(0.0f);
		CHECK(GetField<UUID>(system, probe, "SceneProbe", "PrimaryCamera") == mainCamera.GetUUID());
		CHECK(GetField<int32_t>(system, probe, "SceneProbe", "EnemyCount") == 2);
		CHECK(GetField<UUID>(system, probe, "SceneProbe", "FirstEnemy") == firstEnemy.GetUUID());
		CHECK(GetField<int32_t>(system, probe, "SceneProbe", "RootCount") == static_cast<int32_t>(scene.GetRootEntities().size()));
		scene.OnRuntimeStop();
	}

	TEST_CASE("The host API rejects calls from outside script callbacks")
	{
		const StrataScriptHostAPI& host = GetScriptHostAPI();
		CHECK(host.StructSize == sizeof(StrataScriptHostAPI));
		CHECK(host.ABIVersion == ST_SCRIPT_ABI_VERSION);
		// Every entry of the table is set.
		const size_t first = offsetof(StrataScriptHostAPI, Log);
		for (size_t offset = first; offset + sizeof(uintptr_t) <= sizeof(StrataScriptHostAPI); offset += sizeof(uintptr_t))
		{
			uintptr_t entry = 0;
			std::memcpy(&entry, reinterpret_cast<const char*>(&host) + offset, sizeof(entry));
			INFO("Host API entry ", (offset - first) / sizeof(uintptr_t));
			CHECK(entry != 0);
		}

		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity entity = scene.CreateEntity("Entity");
		const uint64_t id = static_cast<uint64_t>(entity.GetUUID());
		scene.OnRuntimeStart();
		StrataScriptContext* context = GetScriptSystem(scene).GetContext();
		REQUIRE(context != nullptr);

		// A valid context, but no script is running: the engine is not calling into the module.
		CHECK_FALSE(host.IsEntityValid(context, id));
		CHECK(host.CreateEntity(context, ABIString("Sneaky"), 0) == 0);
		CHECK(host.GetEntityName(context, id, nullptr, 0) == 0);
		// No context, or a pointer that is not a context.
		CHECK_FALSE(host.IsEntityValid(nullptr, id));
		CHECK_FALSE(host.IsEntityValid(reinterpret_cast<StrataScriptContext*>(&scene), id));
		float position[2] = { 1.0f, 1.0f };
		host.GetMousePosition(nullptr, position);
		CHECK(position[0] == 0.0f);
		// Another thread.
		bool otherThreadResult = true;
		std::thread([&]() { otherThreadResult = host.IsEntityValid(context, id); }).join();
		CHECK_FALSE(otherThreadResult);

		// Logging works from anywhere.
		host.Log(StrataScriptLogLevel_Trace, ABIString("Script host API test message"));
		host.Log(StrataScriptLogLevel_Info, StrataScriptString { nullptr, 0 });

		CHECK(scene.GetEntityCount() == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Every host function has a call counter")
	{
		const StrataScriptHostAPI& host = GetScriptHostAPI();
		const std::vector<ScriptHostFunctionCalls> before = GetScriptHostCallCounts();

		// One counter per function of the table, in declaration order.
		const size_t firstFunction = offsetof(StrataScriptHostAPI, ABIVersion) + sizeof(host.ABIVersion);
		REQUIRE(before.size() == (sizeof(StrataScriptHostAPI) - firstFunction) / sizeof(host.Log));
		CHECK(before.front().Name == "Log");
		CHECK(before.back().Name == "GetScrollDelta");
		std::unordered_set<std::string_view> names;
		for (const ScriptHostFunctionCalls& entry : before)
		{
			CHECK_FALSE(entry.Name.empty());
			CHECK(names.insert(entry.Name).second);
		}

		// Calls through the table count (rejected ones too); the other counters do not change.
		host.Log(StrataScriptLogLevel_Trace, ABIString("Counted host call"));
		host.Log(StrataScriptLogLevel_Trace, ABIString("Counted host call"));
		CHECK_FALSE(host.IsEntityValid(nullptr, 1));
		const std::vector<ScriptHostFunctionCalls> after = GetScriptHostCallCounts();
		REQUIRE(after.size() == before.size());
		for (size_t index = 0; index < after.size(); index++)
		{
			INFO("Host function ", std::string(after[index].Name));
			uint64_t expected = 0;
			if (after[index].Name == "Log")
				expected = 2;
			else if (after[index].Name == "IsEntityValid")
				expected = 1;
			CHECK(after[index].Calls - before[index].Calls == expected);
		}

		ResetScriptHostCallCounts();
		for (const ScriptHostFunctionCalls& entry : GetScriptHostCallCounts())
			CHECK(entry.Calls == 0);
	}
}
