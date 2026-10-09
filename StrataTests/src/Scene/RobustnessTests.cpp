#include <doctest/doctest.h>

#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/SceneSerializer.h"
#include "TestHelpers.h"

#include <clocale>
#include <cmath>

using namespace Strata;

namespace
{
	struct FixedCounter : public SceneSystem
	{
		void OnFixedUpdate(float) override { Count++; }
		static inline int Count = 0;
	};
}

// Regression tests for malformed input and numerical edge cases: hand-written or AI-written JSON reaches these
// code paths through the automation API, so none of them may crash, hang or corrupt state.
TEST_SUITE("Scene.Robustness")
{
	TEST_CASE("Float serialization is independent of the C locale")
	{
		const char* commaLocales[] = { "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "German_Germany.1252", "de-DE" };
		std::string previous = std::setlocale(LC_ALL, nullptr) ? std::setlocale(LC_ALL, nullptr) : "C";
		bool switched = false;
		for (const char* locale : commaLocales)
		{
			if (std::setlocale(LC_ALL, locale))
			{
				switched = true;
				break;
			}
		}

		CHECK(FloatToJson(1.5f).get<double>() == doctest::Approx(1.5));
		CHECK(FloatToJson(-0.1f).get<float>() == -0.1f);
		CHECK(FloatToJson(std::numeric_limits<float>::infinity()).get<double>() == 0.0);
		std::setlocale(LC_ALL, previous.c_str());
		if (!switched)
			MESSAGE("No comma-decimal locale available; checked the default locale only");
	}

	TEST_CASE("Parent cycles in entity data are broken instead of corrupting the hierarchy")
	{
		const nlohmann::json snapshot = { { "Entities", {
			{ { "ID", "00000000000000A1" }, { "Parent", "00000000000000A1" } },
			{ { "ID", "00000000000000B1" }, { "Parent", "00000000000000B2" } },
			{ { "ID", "00000000000000B2" }, { "Parent", "00000000000000B1" } },
			{ { "ID", "00000000000000C1" }, { "Parent", "00000000000000C2" } }, // Child listed before its parent
			{ { "ID", "00000000000000C2" } }
		} } };

		Scene scene;
		std::vector<std::string> warnings;
		EntityInstantiationOptions options;
		options.GenerateNewUUIDs = false;
		std::vector<Entity> roots = SceneSerializer::DeserializeEntities(scene, snapshot, options, nullptr, &warnings);
		CHECK(scene.GetEntityCount() == 5);
		CHECK(warnings.size() == 2); // The self-loop and one link of the A<->B loop

		Entity self = scene.GetEntityByUUID(UUID(0xA1));
		CHECK_FALSE(self.GetParent().IsValid());
		CHECK(scene.GetEntityByUUID(UUID(0xC1)).GetParent() == scene.GetEntityByUUID(UUID(0xC2)));

		// Everything stays reachable and destroyable.
		CHECK(scene.GetEntitiesInHierarchyOrder().size() == 5);
		for (Entity root : roots)
			scene.DestroyEntity(root);
		CHECK(scene.GetEntityCount() == 0);
	}

	TEST_CASE("Singular parent transforms never produce NaN children")
	{
		Scene scene;
		Entity parent = scene.CreateEntity("Flat");
		parent.GetTransform().Scale = { 1.0f, 0.0f, 1.0f };
		Entity child = scene.CreateEntity("Child");
		child.GetTransform().Translation = { 1.0f, 2.0f, 3.0f };

		CHECK(scene.SetParent(child, parent, true));
		const TransformComponent& transform = child.GetTransform();
		CHECK(std::isfinite(transform.Translation.x));
		CHECK(std::isfinite(transform.Rotation.w));
		CHECK(transform.Translation == glm::vec3(1.0f, 2.0f, 3.0f)); // Unchanged: the request could not be represented

		CHECK_FALSE(scene.SetWorldTransform(child, glm::mat4(1.0f)));
		CHECK_FALSE(child.GetTransform().SetTransform(glm::mat4(0.0f)));
		glm::mat4 nanMatrix(1.0f);
		nanMatrix[3][0] = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(child.GetTransform().SetTransform(nanMatrix));
		CHECK(child.GetTransform().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
	}

	TEST_CASE("Wrongly typed scene documents fail gracefully")
	{
		std::string error;
		Scene stringVersion;
		CHECK_FALSE(SceneSerializer::Deserialize(stringVersion, { { "Strata", { { "Format", "Scene" }, { "Version", "1" } } }, { "Scene", nlohmann::json::object() } }, &error));

		Scene numericFormat;
		CHECK_FALSE(SceneSerializer::Deserialize(numericFormat, {
			{ "Strata", { { "Format", 5 }, { "Version", 1 } } }, { "Scene", nlohmann::json::object() } }, &error));

		Scene badSettings;
		const nlohmann::json document = {
			{ "Strata", { { "Format", "Scene" }, { "Version", 1 } } },
			{ "Scene", { { "Name", 42 }, { "Settings", { { "FixedTimestep", "fast" }, { "Gravity", "down" }, { "MaxFixedStepsPerFrame", -3 } } } } }
		};
		std::vector<std::string> warnings;
		REQUIRE(SceneSerializer::Deserialize(badSettings, document, &error, &warnings));
		CHECK(badSettings.GetName() == "Untitled");
		CHECK(badSettings.GetSettings().Gravity.y == doctest::Approx(-9.81f));
		CHECK(warnings.size() == 1);
	}

	TEST_CASE("Saving survives invalid UTF-8 in strings")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("InvalidUtf8");
		Scene scene("Broken\xFF\xFE Name");
		scene.CreateEntity("Bad\xC3");
		std::string error;
		CHECK(SceneSerializer::SaveToFile(scene, directory / "Scene.stscene", &error));
		CHECK(SceneSerializer::LoadFromFile(directory / "Scene.stscene", &error) != nullptr);
		CHECK(JsonUtils::Dump(nlohmann::json("\xFF")).size() > 0);
	}

	TEST_CASE("Out-of-range and non-finite vectors are rejected")
	{
		const ComponentInfo* transform = ComponentRegistry::Find<TransformComponent>();
		const PropertyInfo* translation = transform->FindProperty("Translation");
		std::string error;
		CHECK_FALSE(PropertyValueFromJson(*translation, nlohmann::json::array({ 1e39, 0, 0 }), &error).has_value());

		TransformComponent component;
		CHECK_FALSE(translation->SetValue(&component, glm::vec3(std::numeric_limits<float>::infinity(), 0.0f, 0.0f), &error));
		CHECK(component.Translation == glm::vec3(0.0f));

		// Vector ranges are enforced per component.
		const PropertyInfo* anchor = ComponentRegistry::Find<TextComponent>()->FindProperty("ScreenAnchor");
		TextComponent text;
		CHECK(anchor->SetValue(&text, glm::vec2(2.0f, -1.0f)));
		CHECK(text.ScreenAnchor == glm::vec2(1.0f, 0.0f));
	}

	TEST_CASE("Deserialized entities notify systems with their final state")
	{
		struct Observer
		{
			glm::vec3 SeenTranslation = glm::vec3(0.0f);
			UUID SeenParent = UUID::Null();
			int Updates = 0;

			void OnUpdate(entt::registry& registry, entt::entity entity)
			{
				SeenTranslation = registry.get<TransformComponent>(entity).Translation;
				SeenParent = registry.get<RelationshipComponent>(entity).Parent;
				Updates++;
			}
		};

		const nlohmann::json snapshot = { { "Entities", {
			{ { "ID", "0000000000000001" } },
			{ { "ID", "0000000000000002" }, { "Parent", "0000000000000001" }, { "Components", { { "Transform", { { "Translation", { 5, 6, 7 } } } } } } }
		} } };

		Scene scene;
		Observer observer;
		scene.GetRegistry().on_update<TransformComponent>().connect<&Observer::OnUpdate>(observer);
		EntityInstantiationOptions options;
		options.GenerateNewUUIDs = false;
		SceneSerializer::DeserializeEntities(scene, snapshot, options);
		CHECK(observer.Updates == 2);
		CHECK(observer.SeenTranslation == glm::vec3(5.0f, 6.0f, 7.0f));
		CHECK(observer.SeenParent == UUID(1));
	}

	TEST_CASE("Script fields survive malformed neighbors and enum values")
	{
		const ComponentInfo* scriptInfo = ComponentRegistry::Find<ScriptComponent>();
		ScriptComponent component;
		const nlohmann::json json = { { "Scripts", {
			{ { "Class", "First" }, { "Fields", { { "Broken", { { "Type", "Float" } } }, { "Mode", { { "Type", "Enum" }, { "Value", 2 } } }, { "Ok", { { "Type", "Bool" }, { "Value", true } } } } } },
			{ { "Class", "Second" } }
		} } };
		std::string error;
		REQUIRE(ComponentAccess::Deserialize(*scriptInfo, &component, json, false, &error));
		REQUIRE(component.Scripts.size() == 2);
		REQUIRE(component.Scripts[0].Fields.size() == 2);
		CHECK(component.Scripts[0].FindField("Mode")->Type == PropertyType::Int);

		nlohmann::json written = ComponentAccess::Serialize(*scriptInfo, &component);
		CHECK(written["Scripts"][0]["Fields"]["Mode"]["Type"] == "Int");
	}

	TEST_CASE("A zero step limit still advances the simulation")
	{
		FixedCounter::Count = 0;
		SceneSystemRegistry::Register({ "TestFixedCounter", false, [](Scene&) { return CreateScope<FixedCounter>(); } });

		Scene scene;
		scene.GetSettings().MaxFixedStepsPerFrame = 0;
		scene.GetSettings().FixedTimestep = 0.01f;
		scene.OnRuntimeStart();
		scene.OnUpdateRuntime(0.05f);
		CHECK(FixedCounter::Count == 1);
		scene.OnRuntimeStop();
		SceneSystemRegistry::Unregister("TestFixedCounter");
	}
}
