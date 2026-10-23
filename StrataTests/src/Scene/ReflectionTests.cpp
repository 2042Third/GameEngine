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

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

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
			"DirectionalLight", "PointLight", "SpotLight", "SkyLight", "HeightFog", "ReflectionProbe", "PostProcess", "Text",
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

		// The TAA's preset and tunables, right after the method: the enums' names and values (file and script API), the
		// tunables' ranges and tooltips, and their defaults, Sharp's table.
		const ComponentInfo* post = ComponentRegistry::Find<PostProcessComponent>();
		const PropertyInfo* preset = post->FindProperty("TAAPreset");
		REQUIRE(preset);
		CHECK(preset->Type == PropertyType::Enum);
		REQUIRE(preset->EnumValues.size() == 3);
		CHECK((preset->EnumValues[0].Name == "Sharp" && preset->EnumValues[0].Value == 0));
		CHECK((preset->EnumValues[1].Name == "Original" && preset->EnumValues[1].Value == 1));
		CHECK((preset->EnumValues[2].Name == "Custom" && preset->EnumValues[2].Value == 2));
		CHECK(PostProcessComponent().TAAPreset == TemporalAntiAliasingPreset::Sharp);
		const PropertyInfo* clamp = post->FindProperty("TAAHistoryClamp");
		REQUIRE(clamp);
		REQUIRE(clamp->EnumValues.size() == 2);
		CHECK((clamp->EnumValues[0].Name == "MinMax" && clamp->EnumValues[0].Value == 0 && clamp->EnumValues[0].DisplayName == "Range"));
		CHECK((clamp->EnumValues[1].Name == "Variance" && clamp->EnumValues[1].Value == 1 && clamp->EnumValues[1].DisplayName == "Tight (variance)"));
		const char* const order[] = { "AntiAliasingMethod", "TAAPreset", "TAAHistoryFrames", "TAAMotionResponse", "TAAMaxMotionWeight", "TAAResampleWeight",
			"TAAHistoryClamp", "TAAClipWidth", "TAATransparentWeight", "TAADisocclusionRejection", "TAADisocclusionThreshold", "TAASharpening" };
		const auto first = std::find_if(post->Properties.begin(), post->Properties.end(), [](const PropertyInfo& property) { return property.Name == "AntiAliasingMethod"; });
		REQUIRE(std::distance(first, post->Properties.end()) >= static_cast<std::ptrdiff_t>(std::size(order)));
		for (size_t index = 0; index < std::size(order); index++)
		{
			CHECK(first[index].Name == order[index]);
			CHECK_MESSAGE(!first[index].Tooltip.empty(), order[index]);
		}
		struct TunableRange
		{
			const char* Property;
			float Min;
			float Max;
		};
		for (const TunableRange& range : { TunableRange { "TAAHistoryFrames", 2.0f, 32.0f }, TunableRange { "TAAMotionResponse", 0.0f, 0.25f },
				 TunableRange { "TAAMaxMotionWeight", 0.0f, 1.0f }, TunableRange { "TAAResampleWeight", 0.0f, 1.0f }, TunableRange { "TAAClipWidth", 0.5f, 4.0f },
				 TunableRange { "TAATransparentWeight", 0.0f, 1.0f }, TunableRange { "TAADisocclusionThreshold", 0.01f, 0.5f },
				 TunableRange { "TAASharpening", 0.0f, 1.0f } })
		{
			CAPTURE(range.Property);
			const PropertyInfo* property = post->FindProperty(range.Property);
			REQUIRE(property);
			CHECK(property->Min == range.Min);
			CHECK(property->Max == range.Max);
		}
		CHECK(post->FindProperty("TAAHistoryFrames")->Type == PropertyType::UInt);
		CHECK(post->FindProperty("TAADisocclusionRejection")->Type == PropertyType::Bool);
		// The defaults equal Sharp's table, which holds Original's values within every range.
		PostProcessComponent tunables;
		tunables.TAAPreset = TemporalAntiAliasingPreset::Custom;
		CHECK(GetTemporalAntiAliasingSettings(tunables) == c_TAASharpSettings);
		post->FindProperty("TAAHistoryFrames")->SetValue(&tunables, uint32_t(40));
		CHECK(tunables.TAAHistoryFrames == 32); // Editors clamp; commands refuse

		// The directional light's cascade blend: 0.1 by default, within [0, 0.5]; commands refuse values outside, editors clamp.
		const PropertyInfo* blend = ComponentRegistry::Find<DirectionalLightComponent>()->FindProperty("CascadeBlend");
		REQUIRE(blend);
		CHECK(blend->Type == PropertyType::Float);
		CHECK(blend->Min == 0.0f);
		CHECK(blend->Max == DirectionalLightComponent::c_MaxCascadeBlend);
		CHECK(DirectionalLightComponent::c_MaxCascadeBlend == 0.5f);
		DirectionalLightComponent sun;
		CHECK(sun.CascadeBlend == doctest::Approx(0.1f));
		CHECK(blend->IsInRange(PropertyValue(0.0f)));
		CHECK(blend->IsInRange(PropertyValue(0.5f)));
		CHECK_FALSE(blend->IsInRange(PropertyValue(0.6f)));
		CHECK_FALSE(blend->IsInRange(PropertyValue(-0.1f)));
		CHECK(blend->SetValue(&sun, PropertyValue(0.25f)));
		CHECK(sun.CascadeBlend == doctest::Approx(0.25f));
		CHECK(blend->SetValue(&sun, PropertyValue(0.9f)));
		CHECK(sun.CascadeBlend == doctest::Approx(0.5f));
		CHECK_FALSE(blend->SetValue(&sun, std::numeric_limits<float>::quiet_NaN()));
		CHECK(sun.CascadeBlend == doctest::Approx(0.5f));

		// Point and spot lights' shadows: off by default; biases within [0, 10], the filter radius within [0, 8] (texels).
		for (const char* component : { "PointLight", "SpotLight" })
		{
			CAPTURE(component);
			const ComponentInfo* lightInfo = ComponentRegistry::Find(component);
			REQUIRE(lightInfo);
			const PropertyInfo* cast = lightInfo->FindProperty("CastShadows");
			REQUIRE(cast);
			CHECK(cast->Type == PropertyType::Bool);
			for (const auto& [property, maximum] : { std::pair { "ShadowBias", 10.0f }, std::pair { "ShadowNormalBias", 10.0f },
				std::pair { "ShadowFilterRadius", PointLightComponent::c_MaxShadowFilterRadius } })
			{
				CAPTURE(property);
				const PropertyInfo* found = lightInfo->FindProperty(property);
				REQUIRE(found);
				CHECK(found->Type == PropertyType::Float);
				CHECK(found->Min == 0.0f);
				CHECK(found->Max == maximum);
				CHECK_FALSE(found->Tooltip.empty());
			}
		}
		PointLightComponent lamp;
		CHECK_FALSE(lamp.CastShadows);
		CHECK(lamp.ShadowBias == 1.0f);
		CHECK(lamp.ShadowNormalBias == 1.5f);
		CHECK(lamp.ShadowFilterRadius == 1.5f);
		SpotLightComponent spot;
		CHECK_FALSE(spot.CastShadows);
		CHECK(spot.ShadowFilterRadius == 1.5f);
		const PropertyInfo* filter = ComponentRegistry::Find<PointLightComponent>()->FindProperty("ShadowFilterRadius");
		CHECK(filter->SetValue(&lamp, PropertyValue(12.0f)));
		CHECK(lamp.ShadowFilterRadius == 8.0f);
		CHECK_FALSE(filter->SetValue(&lamp, std::numeric_limits<float>::quiet_NaN()));
		CHECK(lamp.ShadowFilterRadius == 8.0f);
		CHECK_FALSE(filter->IsInRange(PropertyValue(8.5f)));

		// Height fog: its defaults, and ranges that keep it a fog (no negative density or distance, a falloff above 0,
		// opacities and scattering strengths within [0, 1]).
		const ComponentInfo* fogInfo = ComponentRegistry::Find<HeightFogComponent>();
		REQUIRE(fogInfo);
		CHECK(fogInfo->Name == "HeightFog");
		CHECK(fogInfo->DisplayName == "Height Fog");
		const HeightFogComponent defaults;
		CHECK(defaults.Density == doctest::Approx(0.02f));
		CHECK(defaults.HeightFalloff == doctest::Approx(0.2f));
		CHECK(defaults.BaseHeight == 0.0f);
		CHECK(defaults.StartDistance == 0.0f);
		CHECK(defaults.MaxOpacity == 1.0f);
		CHECK(defaults.Color == glm::vec3(0.7f, 0.8f, 0.9f));
		CHECK(defaults.ColorFromSky);
		CHECK(defaults.SunScattering == doctest::Approx(0.5f));
		const auto range = [&](const char* name) -> std::pair<float, float>
		{
			const PropertyInfo* property = fogInfo->FindProperty(name);
			REQUIRE_MESSAGE(property, name);
			CHECK(property->Type == PropertyType::Float);
			return { property->Min, property->Max };
		};
		CHECK(range("Density").first == 0.0f);
		CHECK(range("StartDistance").first == 0.0f);
		CHECK(range("HeightFalloff").first == HeightFogComponent::c_MinHeightFalloff);
		CHECK(HeightFogComponent::c_MinHeightFalloff > 0.0f);
		CHECK(range("MaxOpacity") == std::pair(0.0f, 1.0f));
		CHECK(range("SunScattering") == std::pair(0.0f, 1.0f));
		CHECK(range("BaseHeight").first < -10000.0f); // Any world height
		const PropertyInfo* fogColor = fogInfo->FindProperty("Color");
		REQUIRE(fogColor);
		CHECK(fogColor->Type == PropertyType::Color3);
		const PropertyInfo* colorFromSky = fogInfo->FindProperty("ColorFromSky");
		REQUIRE(colorFromSky);
		CHECK(colorFromSky->Type == PropertyType::Bool);
		HeightFogComponent fog;
		CHECK_FALSE(fogInfo->FindProperty("Density")->IsInRange(PropertyValue(-0.1f)));
		CHECK_FALSE(fogInfo->FindProperty("HeightFalloff")->IsInRange(PropertyValue(0.0f)));
		CHECK_FALSE(fogInfo->FindProperty("MaxOpacity")->IsInRange(PropertyValue(1.5f)));
		CHECK_FALSE(fogInfo->FindProperty("StartDistance")->IsInRange(PropertyValue(-1.0f)));
		CHECK(fogInfo->FindProperty("HeightFalloff")->SetValue(&fog, PropertyValue(0.0f))); // Editors clamp
		CHECK(fog.HeightFalloff == HeightFogComponent::c_MinHeightFalloff);
		CHECK(fogInfo->FindProperty("SunScattering")->SetValue(&fog, PropertyValue(2.0f)));
		CHECK(fog.SunScattering == 1.0f);
		CHECK_FALSE(fogInfo->FindProperty("Density")->SetValue(&fog, std::numeric_limits<float>::quiet_NaN()));

		// The atmosphere: a third sky source after the two of 0.5.0, and its settings' defaults and ranges.
		const ComponentInfo* skyInfo = ComponentRegistry::Find<SkyLightComponent>();
		REQUIRE(skyInfo);
		const PropertyInfo* source = skyInfo->FindProperty("Source");
		REQUIRE(source);
		REQUIRE(source->EnumValues.size() == 3);
		CHECK(source->EnumValues[2].Name == "Atmosphere");
		CHECK(source->EnumValues[2].Value == 2);
		CHECK(static_cast<int>(SkyLightSource::Atmosphere) == 2);
		const SkyLightComponent skyDefaults;
		CHECK(skyDefaults.Source == SkyLightSource::EnvironmentMap);
		CHECK(skyDefaults.AtmosphereDensity == 1.0f);
		CHECK(skyDefaults.AtmosphereHaze == 1.0f);
		CHECK(skyDefaults.GroundAlbedo == doctest::Approx(0.3f));
		const auto skyRange = [&](const char* name) -> std::pair<float, float>
		{
			const PropertyInfo* property = skyInfo->FindProperty(name);
			REQUIRE_MESSAGE(property, name);
			CHECK(property->Type == PropertyType::Float);
			return { property->Min, property->Max };
		};
		CHECK(skyRange("AtmosphereDensity") == std::pair(0.0f, 10.0f));
		CHECK(skyRange("AtmosphereHaze") == std::pair(0.0f, 10.0f));
		CHECK(skyRange("GroundAlbedo") == std::pair(0.0f, 1.0f));
		SkyLightComponent sky;
		CHECK_FALSE(skyInfo->FindProperty("AtmosphereDensity")->IsInRange(PropertyValue(-0.5f)));
		CHECK_FALSE(skyInfo->FindProperty("GroundAlbedo")->IsInRange(PropertyValue(1.5f)));
		CHECK(skyInfo->FindProperty("GroundAlbedo")->SetValue(&sky, PropertyValue(1.5f))); // Editors clamp
		CHECK(sky.GroundAlbedo == 1.0f);
		CHECK_FALSE(skyInfo->FindProperty("AtmosphereHaze")->SetValue(&sky, std::numeric_limits<float>::quiet_NaN()));
		CHECK(sky.AtmosphereHaze == 1.0f);
		CHECK(fog.Density == doctest::Approx(0.02f));

		// Reflection probes: a texture capture, a box of some size, no negative blend distance or intensity.
		const ComponentInfo* probeInfo = ComponentRegistry::Find<ReflectionProbeComponent>();
		REQUIRE(probeInfo);
		CHECK(probeInfo->Name == "ReflectionProbe");
		CHECK(probeInfo->DisplayName == "Reflection Probe");
		const PropertyInfo* capture = probeInfo->FindProperty("Capture");
		REQUIRE(capture);
		CHECK(capture->Type == PropertyType::Asset);
		CHECK(capture->AssetFilter == AssetType::Texture);
		const PropertyInfo* extents = probeInfo->FindProperty("Extents");
		REQUIRE(extents);
		CHECK(extents->Type == PropertyType::Vec3);
		CHECK(extents->Min == ReflectionProbeComponent::c_MinExtent);
		CHECK(ReflectionProbeComponent::c_MinExtent > 0.0f);
		CHECK_FALSE(extents->IsInRange(PropertyValue(glm::vec3(1.0f, 0.0f, 1.0f))));
		CHECK(probeInfo->FindProperty("BlendDistance")->Min == 0.0f);
		CHECK_FALSE(probeInfo->FindProperty("BlendDistance")->IsInRange(PropertyValue(-0.5f)));
		CHECK(probeInfo->FindProperty("Intensity")->Min == 0.0f);
		CHECK_FALSE(probeInfo->FindProperty("Intensity")->IsInRange(PropertyValue(-1.0f)));
		CHECK(probeInfo->FindProperty("BoxProjection")->Type == PropertyType::Bool);
		CHECK(probeInfo->FindProperty("Priority")->Type == PropertyType::Int);
		ReflectionProbeComponent probe;
		CHECK(extents->SetValue(&probe, PropertyValue(glm::vec3(-1.0f, 2.0f, 3.0f)))); // Editors clamp
		CHECK(probe.Extents == glm::vec3(ReflectionProbeComponent::c_MinExtent, 2.0f, 3.0f));
		CHECK_FALSE(probeInfo->FindProperty("Intensity")->SetValue(&probe, std::numeric_limits<float>::quiet_NaN()));
		CHECK(probe.Intensity == 1.0f);

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

		// The directional light's cascade blend while it casts no shadows.
		DirectionalLightComponent& sun = entity.AddComponent<DirectionalLightComponent>();
		CHECK(reason("DirectionalLight", "CascadeBlend").empty());
		CHECK(reason("DirectionalLight", "ShadowBias").empty());
		sun.CastShadows = false;
		for (const char* property : { "ShadowDistance", "ShadowSoftness", "ShadowBias", "ShadowNormalBias", "CascadeBlend" })
			CHECK_MESSAGE(reason("DirectionalLight", property) == "Cast Shadows is off", property);
		CHECK(reason("DirectionalLight", "Intensity").empty()); // Always active

		// Point and spot lights' shadow settings while they cast none (by default).
		PointLightComponent& lamp = entity.AddComponent<PointLightComponent>();
		SpotLightComponent& spot = entity.AddComponent<SpotLightComponent>();
		for (const char* component : { "PointLight", "SpotLight" })
		{
			for (const char* property : { "ShadowBias", "ShadowNormalBias", "ShadowFilterRadius" })
				CHECK_MESSAGE(reason(component, property) == "Cast Shadows is off", component, ".", property);
			CHECK(reason(component, "Range").empty());
			CHECK(reason(component, "CastShadows").empty());
		}
		lamp.CastShadows = true;
		spot.CastShadows = true;
		for (const char* component : { "PointLight", "SpotLight" })
		{
			for (const char* property : { "ShadowBias", "ShadowNormalBias", "ShadowFilterRadius" })
				CHECK_MESSAGE(reason(component, property).empty(), component, ".", property);
		}

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
		// The atmosphere: its settings and the sun's size; neither source's own properties, the ambient color or the blur.
		for (const char* property : { "AtmosphereDensity", "AtmosphereHaze", "GroundAlbedo" })
			CHECK_MESSAGE(reason("SkyLight", property) == "Only the atmosphere uses it (Source is Environment Map)", property);
		CHECK_FALSE(reason("SkyLight", "SunSize").empty());
		CHECK(reason("SkyLight", "BackgroundBlur").empty());
		sky.Source = SkyLightSource::Atmosphere;
		for (const char* property : { "AtmosphereDensity", "AtmosphereHaze", "GroundAlbedo", "SunSize", "Intensity", "ShowBackground" })
			CHECK_MESSAGE(reason("SkyLight", property).empty(), property);
		for (const char* property : { "EnvironmentMap", "Rotation", "ZenithColor", "HorizonColor", "GroundColor", "SunIntensity", "AmbientColor",
				 "BackgroundBlur" })
			CHECK_MESSAGE(!reason("SkyLight", property).empty(), property);
		CHECK(reason("SkyLight", "ZenithColor") == "Only the procedural sky uses it (Source is Atmosphere)");
		sky.Source = SkyLightSource::Procedural;
		CHECK(reason("SkyLight", "SunSize").empty());
		CHECK(reason("SkyLight", "AtmosphereHaze") == "Only the atmosphere uses it (Source is Procedural)");

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

		// The TAA's preset and tunables: inactive without anti-aliasing, with FXAA, and (the tunables) unless Custom, quoting the
		// preset's value or why it reads none; under Custom, the clip width only for tight clipping and the threshold only with
		// rejection.
		const char* const temporal[] = { "TAAPreset", "TAAHistoryFrames", "TAAMotionResponse", "TAAMaxMotionWeight", "TAAResampleWeight", "TAAHistoryClamp",
			"TAAClipWidth", "TAATransparentWeight", "TAADisocclusionRejection", "TAADisocclusionThreshold", "TAASharpening" };
		post.AntiAliasing = false;
		for (const char* property : temporal)
			CHECK_MESSAGE(reason("PostProcess", property) == "Anti Aliasing is off", property);
		post.AntiAliasing = true;
		for (const char* property : temporal)
			CHECK_MESSAGE(reason("PostProcess", property) == "Only TAA uses it (Anti Aliasing Method is FXAA)", property);
		post.AntiAliasingMethod = AntiAliasingMode::TAA;
		CHECK(reason("PostProcess", "TAAPreset").empty());
		post.TAAPreset = TemporalAntiAliasingPreset::Sharp;
		CHECK(reason("PostProcess", "TAAHistoryFrames") == "The Sharp preset sets it (8): choose Custom to tune it");
		CHECK(reason("PostProcess", "TAAMotionResponse") == "The Sharp preset sets it (0.0625): choose Custom to tune it");
		CHECK(reason("PostProcess", "TAAMaxMotionWeight") == "The Sharp preset sets it (0.5): choose Custom to tune it");
		CHECK(reason("PostProcess", "TAAResampleWeight") == "The Sharp preset sets it (0.25): choose Custom to tune it");
		CHECK(reason("PostProcess", "TAAHistoryClamp") == "The Sharp preset sets it (Tight): choose Custom to tune it");
		CHECK(reason("PostProcess", "TAAClipWidth") == "The Sharp preset sets it (1.25): choose Custom to tune it");
		CHECK(reason("PostProcess", "TAATransparentWeight") == "The Sharp preset sets it (0.5): choose Custom to tune it");
		CHECK(reason("PostProcess", "TAADisocclusionRejection") == "The Sharp preset sets it (on): choose Custom to tune it");
		CHECK(reason("PostProcess", "TAADisocclusionThreshold") == "The Sharp preset sets it (0.05): choose Custom to tune it");
		CHECK(reason("PostProcess", "TAASharpening") == "The Sharp preset sets it (0.5): choose Custom to tune it");
		post.TAAPreset = TemporalAntiAliasingPreset::Original;
		CHECK(reason("PostProcess", "TAAMotionResponse") == "The Original preset sets it (0.03125): choose Custom to tune it");
		CHECK(reason("PostProcess", "TAAClipWidth") == "The Original preset clamps to the colors' range: choose Custom to tune it");
		CHECK(reason("PostProcess", "TAADisocclusionThreshold") == "The Original preset does not reject uncovered surfaces: choose Custom to tune it");
		CHECK(reason("PostProcess", "TAASharpening") == "The Original preset sets it (0): choose Custom to tune it");
		post.TAAPreset = TemporalAntiAliasingPreset::Custom;
		for (const char* property : temporal)
			CHECK_MESSAGE(reason("PostProcess", property).empty(), property);
		post.TAAHistoryClamp = TemporalHistoryClamp::MinMax;
		post.TAADisocclusionRejection = false;
		CHECK(reason("PostProcess", "TAAClipWidth") == "Only Tight clipping uses it (TAA History Clamp is Range)");
		CHECK(reason("PostProcess", "TAADisocclusionThreshold") == "TAA Disocclusion Rejection is off");

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

