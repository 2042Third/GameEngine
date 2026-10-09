#include <doctest/doctest.h>

#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/PropertyBuilder.h"
#include "Strata/Renderer/Material.h"

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
		CHECK(loaded->GetTextureHandles() == std::vector<AssetHandle> { UUID(0x1001), UUID(0x1002), UUID(0x1003), UUID(0x1004), UUID(0x1005) });
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

	TEST_CASE("Material reflection describes every property")
	{
		const std::vector<PropertyInfo>& properties = Material::GetPropertyInfos();
		CHECK(properties.size() == 18);
		const PropertyInfo* normalMap = FindProperty(properties, "NormalMap");
		REQUIRE(normalMap);
		CHECK(normalMap->Type == PropertyType::Asset);
		CHECK(normalMap->AssetFilter == AssetType::Texture);
		CHECK(FindProperty(properties, "Missing") == nullptr);
	}
}
