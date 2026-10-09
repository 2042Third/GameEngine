#include <doctest/doctest.h>

#include "FeatureTest/FeatureTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scripting/ScriptHostAPI.h"
#include "Strata/Scripting/ScriptTypes.h"
#include "Strata/Scripting/ScriptValue.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The property types script fields can have (derived from the engine's mapping, so new field types are included).
	std::set<PropertyType> GetScriptFieldTypes()
	{
		std::set<PropertyType> types;
		for (uint32_t valueType = 0; valueType < 256; valueType++)
		{
			if (const std::optional<PropertyType> type = ScriptFieldTypeToPropertyType(valueType))
				types.insert(*type);
		}
		return types;
	}

	// Every ScriptCallback value (derived from their names, so new callbacks are included).
	std::vector<ScriptCallback> GetScriptCallbacks()
	{
		std::vector<ScriptCallback> callbacks;
		for (uint32_t value = 0; value < 256; value++)
		{
			const ScriptCallback callback = static_cast<ScriptCallback>(value);
			if (std::string_view(ScriptCallbackToString(callback)) != "Unknown")
				callbacks.push_back(callback);
		}
		return callbacks;
	}

	std::string MissingAssetMessage(AssetType type)
	{
		return std::string("Asset type ") + AssetTypeToString(type) + ": add an asset of this type to StrataTests/FeatureTest/Assets";
	}

}

TEST_SUITE("FeatureTest")
{
	TEST_CASE("The feature project imports and loads without warnings")
	{
		LogCapture log;
		FeatureProject project;
		const Ref<EditorAssetManager>& manager = project.GetAssetManager();

		std::set<AssetType> types;
		for (const AssetMetadata& metadata : manager->GetAllMetadata())
		{
			if (metadata.IsBuiltin())
				continue;
			types.insert(metadata.Type);
			if (metadata.IsSubAsset())
				continue;

			INFO("Asset ", metadata.Path);
			const AssetImportInfo import = manager->GetImportInfo(metadata.Handle);
			CHECK(import.Imported);
			CHECK(import.Error.empty());
			for (const std::string& warning : import.Warnings)
				FAIL_CHECK("Import warning: ", warning);

			// The committed .meta files are exactly what the engine keeps: importing changed none of them.
			const std::filesystem::path metaPath = FileSystem::FromUTF8(metadata.Path + std::string(EditorAssetManager::c_MetaExtension));
			const std::optional<std::string> committed = FileSystem::ReadText(GetFeatureProjectSourceDirectory() / "Assets" / metaPath);
			REQUIRE(committed.has_value());
			CHECK(FileSystem::ReadText(project.GetProject()->GetAssetDirectory() / metaPath) == committed);
		}

		// The project has an asset of every type (the types are found through their names, so new ones are included).
		for (uint16_t value = 1; std::string_view(AssetTypeToString(static_cast<AssetType>(value))) != "None"; value++)
		{
			INFO(MissingAssetMessage(static_cast<AssetType>(value)));
			CHECK(types.contains(static_cast<AssetType>(value)));
		}

		LoadAllAssets(*manager);
		for (const LogEntry& entry : log.GetEntries())
		{
			if (entry.Level >= LogLevel::Warn)
				FAIL_CHECK("Unexpected ", std::string(LogLevelToString(entry.Level)), " from ", entry.Logger, ": ", entry.Message);
		}
	}

	TEST_CASE("The feature scene contains every component with non-default values")
	{
		FeatureProject project;
		const Ref<Scene> scene = project.LoadStartScene();
		entt::registry& registry = scene->GetRegistry();
		const std::vector<Entity> entities = scene->GetEntitiesInHierarchyOrder();

		for (const ComponentInfo* info : ComponentRegistry::GetAll())
		{
			INFO("Component ", info->Name, ": the feature scene (StrataTests/FeatureTest/Assets/Scenes/Feature.stscene) must contain it, and "
				"every property must have a non-default value on at least one entity");

			// A default-constructed instance to compare with.
			entt::registry defaults;
			const entt::entity defaultEntity = defaults.create();
			const void* defaultComponent = info->Add(defaults, defaultEntity);

			std::vector<entt::entity> owners;
			for (const Entity entity : entities)
			{
				if (info->Has(registry, entity.GetHandle()))
					owners.push_back(entity.GetHandle());
			}
			CHECK_FALSE(owners.empty());

			for (const PropertyInfo& property : info->Properties)
			{
				INFO("Property ", property.Name);
				const PropertyValue defaultValue = property.GetValue(defaultComponent);
				bool nonDefault = false;
				for (const entt::entity owner : owners)
				{
					const PropertyValue value = property.GetValue(info->Get(registry, owner));
					nonDefault |= value != defaultValue;

					// References resolve: assets are registered, entities exist.
					if (property.Type == PropertyType::Asset && std::get<UUID>(value).IsValid())
						CHECK(project.GetAssetManager()->IsHandleValid(std::get<UUID>(value)));
					if (property.Type == PropertyType::Entity && std::get<UUID>(value).IsValid())
						CHECK(scene->GetEntityByUUID(std::get<UUID>(value)).IsValid());
				}
				CHECK(nonDefault);
			}
		}
	}

	TEST_CASE("The feature scene overrides script fields of every type")
	{
		FeatureProject project;
		const Ref<Scene> scene = project.LoadStartScene();

		std::set<PropertyType> overridden;
		for (const Entity entity : scene->GetEntitiesInHierarchyOrder())
		{
			const ScriptComponent* scripts = entity.TryGetComponent<ScriptComponent>();
			if (!scripts)
				continue;
			for (const ScriptEntry& entry : scripts->Scripts)
			{
				for (const ScriptFieldValue& field : entry.Fields)
				{
					overridden.insert(field.Type);
					if (field.Type == PropertyType::Asset && std::get<UUID>(field.Value).IsValid())
						CHECK(project.GetAssetManager()->IsHandleValid(std::get<UUID>(field.Value)));
					if (field.Type == PropertyType::Entity && std::get<UUID>(field.Value).IsValid())
						CHECK(scene->GetEntityByUUID(std::get<UUID>(field.Value)).IsValid());
				}
			}
		}

		for (const PropertyType type : GetScriptFieldTypes())
		{
			INFO("Script field type ", PropertyTypeToString(type), ": override a field of this type in the feature scene");
			CHECK(overridden.contains(type));
		}
	}

	TEST_CASE("The feature scene plays headless with every script check passing")
	{
		FeatureProject project;
		LoadAllAssets(*project.GetAssetManager());
		const Ref<Scene> scene = project.LoadStartScene();
		ScopedScriptEngine engine(GetFeatureScriptModule());

		// Every callback the engine offers is implemented by a feature script.
		uint32_t implemented = 0;
		for (const ScriptClassInfo& info : engine->GetClasses())
			implemented |= info.Callbacks;
		for (const ScriptCallback callback : GetScriptCallbacks())
		{
			INFO("Script callback ", ScriptCallbackToString(callback), ": implement it in a feature script");
			CHECK((implemented & (1u << static_cast<uint32_t>(callback))) != 0);
		}

		ScopedScriptLogLevel scriptLogLevel;
		LogCapture log;
		ResetScriptHostCallCounts();

		scene->OnRuntimeStart();
		PlayFeatureScene(*scene, *engine, [&]()
		{
			project.GetAssetManager()->Update();
			scene->OnUpdateRuntime(c_FeatureFrameTime);
		});
		CHECK(engine->GetLoadCount() == 2);
		CheckFeatureResults(*scene, *engine);

		// Every host function of the script API was called.
		for (const ScriptHostFunctionCalls& function : GetScriptHostCallCounts())
		{
			INFO("Host function ", std::string(function.Name), " was never called: exercise it in a feature script (StrataTests/FeatureTest/Scripts)");
			CHECK(function.Calls > 0);
		}

		// Stopping destroys every instance (OnDestroy is journaled).
		scene->OnRuntimeStop();
		CheckFeatureJournal(*scene, *engine);
		CheckFeatureLog(log.GetEntries());
	}
}
