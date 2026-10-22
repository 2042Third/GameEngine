#include <doctest/doctest.h>

#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

#if ST_WITH_ANIMATION
	#include "Strata/Animation/AnimationComponents.h"
#endif
#if ST_WITH_PHYSICS
	#include "Strata/Physics/PhysicsComponents.h"
#endif

#include <nlohmann/json.hpp>

using namespace Strata;

namespace
{
	PropertyInfo MakeProperty(PropertyType type)
	{
		PropertyInfo property;
		property.Name = "Test";
		property.Type = type;
		return property;
	}
}

TEST_SUITE("Reflection")
{
	TEST_CASE("Every built-in component is registered with a unique name")
	{
		const char* expected[] = {
			"ID", "Name", "Transform", "Relationship", "Tag", "Inactive", "PrefabInstance", "Camera", "MeshRenderer",
			"DirectionalLight", "PointLight", "SpotLight", "SkyLight", "PostProcess", "Text",
#if ST_WITH_PHYSICS
			"RigidBody", "BoxCollider", "SphereCollider", "CapsuleCollider", "MeshCollider", "CharacterController",
#endif
#if ST_WITH_AUDIO
			"AudioSource", "AudioListener",
#endif
			"Script"
		};
		for (const char* name : expected)
			CHECK_MESSAGE(ComponentRegistry::Find(name) != nullptr, name);

		std::set<std::string> names;
		for (const ComponentInfo* info : ComponentRegistry::GetAll())
			CHECK(names.insert(info->Name).second);

		CHECK(ComponentRegistry::Find("transform") == ComponentRegistry::Find<TransformComponent>());
		CHECK(ComponentRegistry::Find("DoesNotExist") == nullptr);
		CHECK(ComponentRegistry::Find<TransformComponent>()->FindProperty("translation") != nullptr);
		CHECK_FALSE(ComponentRegistry::Find<TransformComponent>()->IsRemovable());
		CHECK(ComponentRegistry::Find<IDComponent>()->IsHidden());
	}

	TEST_CASE("Display names are derived from PascalCase identifiers")
	{
		CHECK(Utils::PascalCaseToDisplayName("HalfExtents") == "Half Extents");
		CHECK(Utils::PascalCaseToDisplayName("PerspectiveFOV") == "Perspective FOV");
		CHECK(Utils::PascalCaseToDisplayName("SSAOEnabled") == "SSAO Enabled");
		CHECK(Utils::PascalCaseToDisplayName("Mass") == "Mass");
		CHECK(Utils::PascalCaseToDisplayName("AutoExposureMinEV") == "Auto Exposure Min EV");
		CHECK(ComponentRegistry::Find<MeshRendererComponent>()->DisplayName == "Mesh Renderer");
	}

	TEST_CASE("Properties read and write component members with validation")
	{
		const ComponentInfo* info = ComponentRegistry::Find<CameraComponent>();
		REQUIRE(info);
		CameraComponent camera;

		const PropertyInfo* fov = info->FindProperty("PerspectiveFOV");
		REQUIRE(fov);
		CHECK(std::get<float>(fov->GetValue(&camera)) == doctest::Approx(60.0f));
		CHECK(fov->SetValue(&camera, 90.0f));
		CHECK(camera.PerspectiveFOV == doctest::Approx(90.0f));
		CHECK(fov->SetValue(&camera, 500.0f)); // Clamped to the range
		CHECK(camera.PerspectiveFOV == doctest::Approx(179.0f));

		std::string error;
		CHECK_FALSE(fov->SetValue(&camera, std::string("wide"), &error));
		CHECK_FALSE(error.empty());
		CHECK_FALSE(fov->SetValue(&camera, std::numeric_limits<float>::quiet_NaN()));

		const PropertyInfo* projection = info->FindProperty("Projection");
		REQUIRE(projection);
		CHECK(projection->SetValue(&camera, int32_t(1)));
		CHECK(camera.Projection == ProjectionType::Orthographic);
		CHECK_FALSE(projection->SetValue(&camera, int32_t(7)));
		CHECK(camera.Projection == ProjectionType::Orthographic);

		// Post-processing's anti-aliasing method: FXAA by default, its two choices only.
		const PropertyInfo* method = ComponentRegistry::Find<PostProcessComponent>()->FindProperty("AntiAliasingMethod");
		REQUIRE(method);
		CHECK(method->Type == PropertyType::Enum);
		REQUIRE(method->EnumValues.size() == 2);
		CHECK(method->EnumValues[0].Name == "FXAA");
		CHECK(method->EnumValues[1].Name == "TAA");
		PostProcessComponent postProcess;
		CHECK(postProcess.AntiAliasingMethod == AntiAliasingMode::FXAA);
		CHECK(method->SetValue(&postProcess, int32_t(1)));
		CHECK(postProcess.AntiAliasingMethod == AntiAliasingMode::TAA);
		CHECK_FALSE(method->SetValue(&postProcess, int32_t(2)));
		CHECK(postProcess.AntiAliasingMethod == AntiAliasingMode::TAA);

		const PropertyInfo* rotation = ComponentRegistry::Find<TransformComponent>()->FindProperty("Rotation");
		TransformComponent transform;
		CHECK(rotation->SetValue(&transform, glm::quat(2.0f, 0.0f, 0.0f, 0.0f)));
		CHECK(glm::length(transform.Rotation) == doctest::Approx(1.0f)); // Normalized
		CHECK_FALSE(rotation->SetValue(&transform, glm::quat(0.0f, 0.0f, 0.0f, 0.0f)));
	}

	TEST_CASE("JSON conversion accepts canonical and convenient forms")
	{
		std::string error;

		PropertyInfo vec3 = MakeProperty(PropertyType::Vec3);
		CHECK(std::get<glm::vec3>(*PropertyValueFromJson(vec3, nlohmann::json::array({ 1, 2, 3 }))) == glm::vec3(1, 2, 3));
		CHECK(std::get<glm::vec3>(*PropertyValueFromJson(vec3, nlohmann::json { { "x", 1 }, { "y", 2 }, { "z", 3 } })) == glm::vec3(1, 2, 3));
		CHECK(std::get<glm::vec3>(*PropertyValueFromJson(vec3, 2.5)) == glm::vec3(2.5f));
		CHECK_FALSE(PropertyValueFromJson(vec3, nlohmann::json::array({ 1, 2 }), &error).has_value());
		CHECK(error.find("Test") != std::string::npos);
		CHECK_FALSE(PropertyValueFromJson(vec3, "1,2,3").has_value());

		PropertyInfo color = MakeProperty(PropertyType::Color4);
		CHECK(std::get<glm::vec4>(*PropertyValueFromJson(color, nlohmann::json::array({ 1, 0.5, 0 }))) == glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));
		CHECK(std::get<glm::vec4>(*PropertyValueFromJson(color, nlohmann::json { { "r", 1 }, { "g", 0 }, { "b", 0 } })) == glm::vec4(1, 0, 0, 1));

		PropertyInfo quat = MakeProperty(PropertyType::Quat);
		const glm::quat fromQuat = std::get<glm::quat>(*PropertyValueFromJson(quat, nlohmann::json::array({ 0, 0, 0, 1 })));
		CHECK(fromQuat == glm::quat(1, 0, 0, 0));
		const glm::quat fromEuler = std::get<glm::quat>(*PropertyValueFromJson(quat, nlohmann::json::array({ 0, 90, 0 })));
		CHECK(Math::IsNearlyEqual(fromEuler, Math::EulerDegreesToQuat({ 0, 90, 0 })));
		const glm::quat fromObject = std::get<glm::quat>(*PropertyValueFromJson(quat, nlohmann::json { { "Euler", { 0, 90, 0 } } }));
		CHECK(Math::IsNearlyEqual(fromObject, fromEuler));
		// Canonical output is [x, y, z, w] and parses back to the same rotation.
		const nlohmann::json written = PropertyValueToJson(quat, fromEuler);
		CHECK(written.size() == 4);
		CHECK(Math::IsNearlyEqual(std::get<glm::quat>(*PropertyValueFromJson(quat, written)), fromEuler));

		PropertyInfo enumProperty = MakeProperty(PropertyType::Enum);
		enumProperty.EnumValues = { { "Static", 0 }, { "Dynamic", 1 } };
		CHECK(std::get<int32_t>(*PropertyValueFromJson(enumProperty, "dynamic")) == 1);
		CHECK(std::get<int32_t>(*PropertyValueFromJson(enumProperty, 0)) == 0);
		CHECK(PropertyValueToJson(enumProperty, int32_t(1)) == "Dynamic");
		CHECK_FALSE(PropertyValueFromJson(enumProperty, "Flying", &error).has_value());
		CHECK(error.find("Static") != std::string::npos); // Lists valid options

		PropertyInfo asset = MakeProperty(PropertyType::Asset);
		CHECK(std::get<UUID>(*PropertyValueFromJson(asset, "00000000000000FF")) == UUID(255));
		CHECK(std::get<UUID>(*PropertyValueFromJson(asset, 255)) == UUID(255));
		CHECK(std::get<UUID>(*PropertyValueFromJson(asset, nullptr)) == UUID::Null());
		CHECK(PropertyValueToJson(asset, UUID(255)) == "00000000000000FF");
		CHECK_FALSE(PropertyValueFromJson(asset, "not-a-uuid").has_value());

		PropertyInfo integer = MakeProperty(PropertyType::Int);
		CHECK(std::get<int32_t>(*PropertyValueFromJson(integer, 3.0)) == 3);
		CHECK_FALSE(PropertyValueFromJson(integer, 3.5).has_value());
		CHECK_FALSE(PropertyValueFromJson(integer, 1e12).has_value());

		PropertyInfo unsignedInteger = MakeProperty(PropertyType::UInt);
		CHECK_FALSE(PropertyValueFromJson(unsignedInteger, -1).has_value());

		PropertyInfo boolean = MakeProperty(PropertyType::Bool);
		CHECK(std::get<bool>(*PropertyValueFromJson(boolean, true)));
		CHECK_FALSE(PropertyValueFromJson(boolean, "yes").has_value());
	}

#if ST_WITH_PHYSICS
	TEST_CASE("Property descriptions expose metadata for tooling")
	{
		const PropertyInfo* type = ComponentRegistry::Find<RigidBodyComponent>()->FindProperty("Type");
		const nlohmann::json description = DescribeProperty(*type);
		CHECK(description["Type"] == "Enum");
		CHECK(description["Options"].size() == 3);

		const PropertyInfo* mesh = ComponentRegistry::Find<MeshRendererComponent>()->FindProperty("Mesh");
		CHECK(DescribeProperty(*mesh)["AssetType"] == "Mesh");
	}
#endif

	TEST_CASE("Components added by the engine cannot be added directly")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Instance");
		const ComponentInfo& prefabInstance = *ComponentRegistry::Find<PrefabInstanceComponent>();
		CHECK_FALSE(prefabInstance.IsAddable());
		CHECK(prefabInstance.IsRemovable());
		CHECK(ComponentRegistry::Find<PointLightComponent>()->IsAddable());
		CHECK_FALSE(ComponentRegistry::Find<RelationshipComponent>()->IsAddable());

		std::string error;
		CHECK_FALSE(ComponentAccess::AddComponent(entity, prefabInstance, &error));
		CHECK(error.find("added by the engine") != std::string::npos);
		CHECK_FALSE(entity.HasComponent<PrefabInstanceComponent>());
		// Once the engine added it (prefab instantiation), adding again is a no-op that succeeds.
		entity.AddComponent<PrefabInstanceComponent>();
		CHECK(ComponentAccess::AddComponent(entity, prefabInstance, &error));
		CHECK(ComponentAccess::RemoveComponent(entity, prefabInstance, &error));
	}
}

TEST_SUITE("Scene.InactiveProperties")
{
	TEST_CASE("Properties another property turns off name why, and only while it does")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Settings");
		auto reason = [&entity](const char* component, const char* property)
		{
			const ComponentInfo* info = ComponentRegistry::Find(component);
			REQUIRE(info);
			const PropertyInfo* found = info->FindProperty(property);
			REQUIRE(found);
			return ComponentAccess::GetInactiveReason(entity, *info, *found);
		};

		// The sky: the map's properties with the procedural sky, the procedural sky's with a map, the ambient color with either.
		SkyLightComponent& sky = entity.AddComponent<SkyLightComponent>();
		sky.Source = SkyLightSource::Procedural;
		CHECK_FALSE(reason("SkyLight", "EnvironmentMap").empty());
		CHECK_FALSE(reason("SkyLight", "Rotation").empty());
		CHECK(reason("SkyLight", "ZenithColor").empty());
		CHECK(reason("SkyLight", "SunIntensity").empty());
		CHECK_FALSE(reason("SkyLight", "AmbientColor").empty());
		CHECK(reason("SkyLight", "Intensity").empty()); // Always active
		sky.Source = SkyLightSource::EnvironmentMap;
		CHECK(reason("SkyLight", "EnvironmentMap").empty());
		CHECK_FALSE(reason("SkyLight", "HorizonColor").empty());
		CHECK(reason("SkyLight", "AmbientColor").empty()); // No map set: the ambient color lights the scene
		sky.EnvironmentMap = AssetHandle(0x5C7);
		CHECK_FALSE(reason("SkyLight", "AmbientColor").empty());

		// Post-processing: each effect's settings while it is off; Exposure always.
		PostProcessComponent& post = entity.AddComponent<PostProcessComponent>();
		post.AutoExposure = false;
		post.Bloom = false;
		post.AmbientOcclusion = false;
		post.AntiAliasing = false;
		CHECK(reason("PostProcess", "AutoExposureMinEV") == "Auto Exposure is off");
		CHECK(reason("PostProcess", "AntiAliasingMethod") == "Anti Aliasing is off");
		CHECK_FALSE(reason("PostProcess", "AutoExposureSpeed").empty());
		CHECK(reason("PostProcess", "BloomThreshold") == "Bloom is off");
		CHECK(reason("PostProcess", "AmbientOcclusionRadius") == "Ambient Occlusion is off");
		CHECK(reason("PostProcess", "Exposure").empty());
		post.AutoExposure = true;
		post.Bloom = true;
		post.AmbientOcclusion = true;
		post.AntiAliasing = true;
		for (const char* property : { "AutoExposureMinEV", "AutoExposureMaxEV", "AutoExposureSpeed", "BloomIntensity", "BloomThreshold",
			"AmbientOcclusionRadius", "AmbientOcclusionIntensity", "AntiAliasingMethod" })
			CHECK_MESSAGE(reason("PostProcess", property).empty(), property);

		// The camera's other projection; the animator's clip under a controller.
		CameraComponent& camera = entity.AddComponent<CameraComponent>();
		CHECK(reason("Camera", "PerspectiveFOV").empty());
		CHECK_FALSE(reason("Camera", "OrthographicSize").empty());
		camera.Projection = ProjectionType::Orthographic;
		CHECK_FALSE(reason("Camera", "PerspectiveNear").empty());
		CHECK(reason("Camera", "OrthographicFar").empty());
#if ST_WITH_ANIMATION
		AnimatorComponent& animator = entity.AddComponent<AnimatorComponent>();
		CHECK(reason("Animator", "Clip").empty());
		animator.Controller = AssetHandle(0xC0);
		CHECK_FALSE(reason("Animator", "Clip").empty());
#endif

		// An entity without the component has nothing inactive.
		Entity plain = scene.CreateEntity("Plain");
		const ComponentInfo* info = ComponentRegistry::Find("PostProcess");
		CHECK(ComponentAccess::GetInactiveReason(plain, *info, *info->FindProperty("BloomThreshold")).empty());
	}

	TEST_CASE("Inactive reasons are set only while the registry is open, on properties it has")
	{
		// The registry is frozen once the engine's modules registered.
		REQUIRE(ComponentRegistry::IsFrozen());
		CHECK_FALSE(ComponentRegistry::SetInactiveReason<PostProcessComponent>("Bloom", [](const PostProcessComponent&) { return std::string("never"); }));
		const PropertyInfo* bloom = ComponentRegistry::Find("PostProcess")->FindProperty("Bloom");
		REQUIRE(bloom);
		CHECK_FALSE(bloom->InactiveReason);
	}
}

