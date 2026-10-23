#include <doctest/doctest.h>

#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/PropertyBuilder.h"
#include "Strata/Renderer/GPUSceneData.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/MaterialData.h"
#include "Strata/Renderer/TextureTable.h"

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace Strata;

TEST_SUITE("Renderer.Material")
{
	TEST_CASE("Materials round trip through JSON")
	{
		MaterialProperties properties;
		properties.BaseColor = { 0.25f, 0.5f, 0.75f, 0.5f };
		properties.Metallic = 1.0f;
		properties.Roughness = 0.125f;
		properties.EmissiveColor = { 1.0f, 0.5f, 0.0f };
		properties.EmissiveIntensity = 4.0f;
		properties.NormalScale = 0.5f;
		properties.OcclusionStrength = 0.75f;
		properties.AlphaMode = MaterialAlphaMode::Mask;
		properties.AlphaCutoff = 0.3f;
		properties.DoubleSided = true;
		properties.Unlit = true;
		properties.UVTiling = { 2.0f, 3.0f };
		properties.UVOffset = { 0.5f, 0.25f };
		properties.BaseColorMap = UUID(0x1001);
		properties.MetallicRoughnessMap = UUID(0x1002);
		properties.NormalMap = UUID(0x1003);
		properties.OcclusionMap = UUID(0x1004);
		properties.EmissiveMap = UUID(0x1005);
		properties.Clearcoat = 0.75f;
		properties.ClearcoatRoughness = 0.125f;
		properties.ClearcoatMap = UUID(0x1006);
		properties.ClearcoatRoughnessMap = UUID(0x1007);
		properties.ClearcoatNormalMap = UUID(0x1008);
		properties.Transmission = 0.5f;
		properties.TransmissionMap = UUID(0x1009);
		properties.IOR = 1.33f;
		properties.Thickness = 0.25f;
		properties.AttenuationColor = { 0.5f, 0.75f, 1.0f };
		properties.AttenuationDistance = 2.0f;

		Ref<Material> source = Material::Create(properties);
		const std::string text = JsonUtils::Dump(source->Serialize(), 1, '\t');

		std::string error;
		Ref<Material> loaded = Material::Deserialize(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()), &error);
		REQUIRE_MESSAGE(loaded, error);
		const MaterialProperties& result = loaded->GetProperties();
		CHECK(result.BaseColor == properties.BaseColor);
		CHECK(result.Metallic == properties.Metallic);
		CHECK(result.Roughness == properties.Roughness);
		CHECK(result.EmissiveColor == properties.EmissiveColor);
		CHECK(result.EmissiveIntensity == properties.EmissiveIntensity);
		CHECK(result.NormalScale == properties.NormalScale);
		CHECK(result.OcclusionStrength == properties.OcclusionStrength);
		CHECK(result.AlphaMode == properties.AlphaMode);
		CHECK(result.AlphaCutoff == properties.AlphaCutoff);
		CHECK(result.DoubleSided == properties.DoubleSided);
		CHECK(result.Unlit == properties.Unlit);
		CHECK(result.UVTiling == properties.UVTiling);
		CHECK(result.UVOffset == properties.UVOffset);
		CHECK(result.Clearcoat == properties.Clearcoat);
		CHECK(result.ClearcoatRoughness == properties.ClearcoatRoughness);
		CHECK(result.Transmission == properties.Transmission);
		CHECK(result.IOR == properties.IOR);
		CHECK(result.Thickness == properties.Thickness);
		CHECK(result.AttenuationColor == properties.AttenuationColor);
		CHECK(result.AttenuationDistance == properties.AttenuationDistance);
		CHECK(loaded->GetTextureHandles() == std::vector<AssetHandle> { UUID(0x1001), UUID(0x1002), UUID(0x1003), UUID(0x1004), UUID(0x1005), UUID(0x1006),
			UUID(0x1007), UUID(0x1008), UUID(0x1009) });
		CHECK(MaterialGPU::GetTextureMaps(result) == std::array<AssetHandle, MaterialGPU::c_TextureMapCount> { UUID(0x1001), UUID(0x1002), UUID(0x1003),
			UUID(0x1004), UUID(0x1005), UUID(0x1006), UUID(0x1007), UUID(0x1008), UUID(0x1009) });
		CHECK(loaded->Serialize() == source->Serialize());
	}

	TEST_CASE("Material documents are validated")
	{
		std::string error;
		CHECK_FALSE(Material::FromJson(nlohmann::json::object(), &error));
		CHECK_FALSE(error.empty());

		const nlohmann::json wrongFormat = { { "Strata", { { "Format", "Prefab" }, { "Version", 1 } } }, { "Material", nlohmann::json::object() } };
		CHECK_FALSE(Material::FromJson(wrongFormat, &error));
		const nlohmann::json futureVersion = { { "Strata", { { "Format", "Material" }, { "Version", 99 } } }, { "Material", nlohmann::json::object() } };
		CHECK_FALSE(Material::FromJson(futureVersion, &error));
		const nlohmann::json noBody = { { "Strata", { { "Format", "Material" }, { "Version", 1 } } } };
		CHECK_FALSE(Material::FromJson(noBody, &error));

		const std::string notJson = "{ \"Strata\": ";
		CHECK_FALSE(Material::Deserialize(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(notJson.data()), notJson.size()), &error));
	}

	TEST_CASE("Invalid material properties produce warnings and keep defaults")
	{
		const nlohmann::json document = {
			{ "Strata", { { "Format", "Material" }, { "Version", 1 } } },
			{ "Material", {
				{ "Roughness", 4.0f },
				{ "Metallic", "shiny" },
				{ "AlphaMode", "Blend" },
				{ "Sparkle", 1.0f },
				{ "BaseColor", { 1.0f, 0.0f } }
			} }
		};

		std::string error;
		std::vector<std::string> warnings;
		Ref<Material> material = Material::FromJson(document, &error, &warnings);
		REQUIRE(material);
		const MaterialProperties defaults;
		CHECK(material->GetProperties().Roughness == 1.0f); // Clamped to the property range
		CHECK(material->GetProperties().Metallic == defaults.Metallic);
		CHECK(material->GetProperties().AlphaMode == MaterialAlphaMode::Blend);
		CHECK(material->GetProperties().BaseColor == defaults.BaseColor);
		CHECK(warnings.size() == 3);
	}

	TEST_CASE("Materials of 0.5.0 load with the clear coat and transmission defaults")
	{
		// A material as 0.5.0 wrote it: every property it had, none of those added since.
		const char* noMap = "0000000000000000";
		const nlohmann::json document = {
			{ "Strata", { { "Format", "Material" }, { "Version", 1 } } },
			{ "Material", {
				{ "BaseColor", { 0.8f, 0.2f, 0.2f, 1.0f } }, { "Metallic", 0.0f }, { "Roughness", 0.4f }, { "EmissiveColor", { 0.0f, 0.0f, 0.0f } },
				{ "EmissiveIntensity", 1.0f }, { "NormalScale", 1.0f }, { "OcclusionStrength", 1.0f }, { "AlphaMode", "Opaque" }, { "AlphaCutoff", 0.5f },
				{ "DoubleSided", false }, { "Unlit", false }, { "UVTiling", { 1.0f, 1.0f } }, { "UVOffset", { 0.0f, 0.0f } },
				{ "BaseColorMap", noMap }, { "MetallicRoughnessMap", noMap }, { "NormalMap", noMap }, { "OcclusionMap", noMap }, { "EmissiveMap", noMap }
			} }
		};
		std::string error;
		std::vector<std::string> warnings;
		Ref<Material> material = Material::FromJson(document, &error, &warnings);
		REQUIRE_MESSAGE(material, error);
		CHECK(warnings.empty());
		const MaterialProperties& loaded = material->GetProperties();
		const MaterialProperties defaults;
		CHECK(loaded.Roughness == 0.4f);
		CHECK(loaded.Clearcoat == 0.0f);
		CHECK(loaded.ClearcoatRoughness == 0.0f);
		CHECK_FALSE(loaded.ClearcoatMap.IsValid());
		CHECK_FALSE(loaded.ClearcoatRoughnessMap.IsValid());
		CHECK_FALSE(loaded.ClearcoatNormalMap.IsValid());
		CHECK(loaded.Transmission == 0.0f);
		CHECK_FALSE(loaded.TransmissionMap.IsValid());
		CHECK(loaded.IOR == 1.5f);
		CHECK(loaded.Thickness == 0.0f);
		CHECK(loaded.AttenuationColor == glm::vec3(1.0f));
		CHECK(loaded.AttenuationDistance == 0.0f);
		CHECK(defaults.IOR == 1.5f);

		// Drawn as before: no coat, no glass, nothing absorbed.
		const RenderData::MaterialData data = MaterialGPU::Build(*material, [](AssetHandle, uint32_t missingIndex, uint32_t) { return missingIndex; });
		CHECK(data.Flags == 0u);
		CHECK(data.Clearcoat == 0.0f);
		CHECK(data.Transmission == 0.0f);
		CHECK(data.Absorption == glm::vec4(0.0f));
		CHECK((MaterialGPU::GetFlags(loaded) & RenderData::c_CullMaterialTransparent) == 0u);
	}

	TEST_CASE("The clear coat and transmission keep to their ranges")
	{
		const nlohmann::json document = {
			{ "Strata", { { "Format", "Material" }, { "Version", 1 } } },
			{ "Material", {
				{ "Clearcoat", 2.0f }, { "ClearcoatRoughness", -1.0f }, { "Transmission", 1.5f }, { "IOR", 0.5f }, { "Thickness", -2.0f },
				{ "AttenuationDistance", -1.0f }
			} }
		};
		std::vector<std::string> warnings;
		Ref<Material> material = Material::FromJson(document, nullptr, &warnings);
		REQUIRE(material);
		CHECK(warnings.empty());
		const MaterialProperties& loaded = material->GetProperties();
		CHECK(loaded.Clearcoat == 1.0f);
		CHECK(loaded.ClearcoatRoughness == 0.0f);
		CHECK(loaded.Transmission == 1.0f);
		CHECK(loaded.IOR == 1.0f);
		CHECK(loaded.Thickness == 0.0f);
		CHECK(loaded.AttenuationDistance == 0.0f);

		// Values set in code (an importer, a test) are bounded where the GPU data is built.
		MaterialProperties outside;
		outside.Clearcoat = 3.0f;
		outside.Transmission = 2.0f;
		outside.IOR = 0.25f;
		outside.Thickness = -1.0f;
		const RenderData::MaterialData data = MaterialGPU::Build(*Material::Create(outside), [](AssetHandle, uint32_t missingIndex, uint32_t) { return missingIndex; });
		CHECK(data.Clearcoat == 1.0f);
		CHECK(data.Transmission == 1.0f);
		CHECK(data.IOR == 1.0f);
		CHECK(data.Thickness == 0.0f);
	}

	TEST_CASE("The GPU data of a clear coat and of glass")
	{
		const auto missing = [](AssetHandle, uint32_t missingIndex, uint32_t) { return missingIndex; };
		MaterialProperties coated;
		coated.Clearcoat = 0.5f;
		coated.ClearcoatRoughness = 0.25f;
		RenderData::MaterialData data = MaterialGPU::Build(*Material::Create(coated), missing);
		CHECK(data.Flags == RenderData::c_MaterialClearcoat);
		CHECK(data.Clearcoat == 0.5f);
		CHECK(data.ClearcoatRoughness == 0.25f);
		CHECK(data.ClearcoatMap == TextureTable::c_White);
		CHECK(data.ClearcoatRoughnessMap == TextureTable::c_White);
		CHECK(data.ClearcoatNormalMap == TextureTable::c_FlatNormal);
		CHECK(data.TransmissionMap == TextureTable::c_White);

		MaterialProperties glass;
		glass.Transmission = 1.0f;
		glass.IOR = 1.33f;
		glass.Thickness = 0.5f;
		glass.AttenuationColor = { 0.5f, 0.25f, 1.0f };
		glass.AttenuationDistance = 2.0f;
		data = MaterialGPU::Build(*Material::Create(glass), missing);
		CHECK(data.Flags == RenderData::c_MaterialTransmissive);
		CHECK(data.Transmission == 1.0f);
		CHECK(data.IOR == doctest::Approx(1.33f));
		CHECK(data.Thickness == 0.5f);
		// Beer's law: what is left after the distance is the attenuation color.
		for (int channel = 0; channel < 3; channel++)
			CHECK(std::exp(-data.Absorption[channel] * glass.AttenuationDistance) == doctest::Approx(glass.AttenuationColor[channel]));
		CHECK(data.Absorption.w == 0.0f);

		// A channel the color removes entirely still absorbs a finite amount.
		glass.AttenuationColor = { 0.0f, 1.0f, 1.0f };
		CHECK(std::isfinite(MaterialGPU::GetAbsorption(glass).x));
		CHECK(MaterialGPU::GetAbsorption(glass).x > 0.0f);
		glass.AttenuationDistance = 0.0f;
		CHECK(MaterialGPU::GetAbsorption(glass) == glm::vec3(0.0f));

		// Unlit materials have neither, and neither does a material at 0.
		glass.Unlit = true;
		glass.Clearcoat = 1.0f;
		CHECK(MaterialGPU::Build(*Material::Create(glass), missing).Flags == RenderData::c_MaterialUnlit);
		CHECK_FALSE(MaterialGPU::IsTransmissive(glass));
		CHECK_FALSE(MaterialGPU::HasClearcoat(glass));
		CHECK_FALSE(MaterialGPU::IsTransmissive(MaterialProperties {}));
		CHECK_FALSE(MaterialGPU::HasClearcoat(MaterialProperties {}));
	}

	TEST_CASE("Material reflection describes every property")
	{
		const std::vector<PropertyInfo>& properties = Material::GetPropertyInfos();
		CHECK(properties.size() == 29);
		for (const char* name : { "ClearcoatMap", "ClearcoatRoughnessMap", "ClearcoatNormalMap", "TransmissionMap" })
		{
			const PropertyInfo* map = FindProperty(properties, name);
			REQUIRE(map);
			CHECK(map->Type == PropertyType::Asset);
			CHECK(map->AssetFilter == AssetType::Texture);
		}
		for (const char* name : { "Clearcoat", "ClearcoatRoughness", "Transmission", "IOR", "Thickness", "AttenuationColor", "AttenuationDistance" })
		{
			const PropertyInfo* property = FindProperty(properties, name);
			REQUIRE(property);
			CHECK_FALSE(property->Tooltip.empty());
		}
		const PropertyInfo* normalMap = FindProperty(properties, "NormalMap");
		REQUIRE(normalMap);
		CHECK(normalMap->Type == PropertyType::Asset);
		CHECK(normalMap->AssetFilter == AssetType::Texture);
		CHECK(FindProperty(properties, "Missing") == nullptr);
	}
}
