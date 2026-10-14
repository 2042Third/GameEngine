#include <doctest/doctest.h>

#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/TextureImporter.h"
#include "Strata/Reflection/PropertyJson.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	const AssetImporter& GetImporter(std::string_view extension)
	{
		const AssetImporter* importer = AssetImporterRegistry::FindByExtension(extension);
		REQUIRE(importer);
		return *importer;
	}

	std::vector<std::string> GetNames(const std::vector<PropertyInfo>& properties)
	{
		std::vector<std::string> names;
		for (const PropertyInfo& property : properties)
			names.push_back(property.Name);
		return names;
	}

	const PropertyInfo& Find(const std::vector<PropertyInfo>& properties, std::string_view name)
	{
		const auto it = std::find_if(properties.begin(), properties.end(), [name](const PropertyInfo& property) { return property.Name == name; });
		REQUIRE(it != properties.end());
		return *it;
	}

	std::vector<std::string> GetOptions(const PropertyInfo& property)
	{
		std::vector<std::string> options;
		for (const EnumValue& value : property.EnumValues)
			options.push_back(value.Name);
		return options;
	}

	// The schema describes exactly the settings the importer writes into new .meta files, and its getters read them back.
	void CheckDescribesDefaults(const AssetImporter& importer, const std::filesystem::path& source)
	{
		const std::vector<PropertyInfo> properties = importer.GetSettingsProperties();
		const nlohmann::json defaults = importer.GetDefaultSettings(source);
		std::set<std::string> keys;
		for (const auto& [key, value] : defaults.items())
			keys.insert(key);
		const std::vector<std::string> names = GetNames(properties);
		CHECK(std::set<std::string>(names.begin(), names.end()) == keys);
		CHECK(std::set<std::string>(names.begin(), names.end()).size() == names.size());
		for (const PropertyInfo& property : properties)
		{
			CAPTURE(property.Name);
			CHECK_FALSE(property.DisplayName.empty());
			CHECK_FALSE(property.Tooltip.empty());
			CHECK_FALSE(property.IsReadOnly());
			// The getter reads the stored value, and the setter writes what the getter reads in the form it was stored in.
			const PropertyValue value = property.GetValue(&defaults);
			CHECK(PropertyValueToJson(property, value) == defaults[property.Name]);
			nlohmann::json written = nlohmann::json::object();
			REQUIRE(property.SetValue(&written, value));
			CHECK(written[property.Name] == defaults[property.Name]);
		}
	}

}

TEST_SUITE("Asset.ImportSettings")
{
	TEST_CASE("Texture import settings are described in the order editors show them")
	{
		const AssetImporter& textures = GetImporter(".png");
		const std::vector<PropertyInfo> properties = textures.GetSettingsProperties();
		CHECK(GetNames(properties) == std::vector<std::string> { "Usage", "Compression", "Quality", "MaxSize", "Filter", "Wrap", "GenerateMips",
			"FlipVertically", "FlipGreen" });
		CHECK(GetOptions(Find(properties, "Usage")) == std::vector<std::string> { "Color", "NormalMap", "Data", "HDR" });
		CHECK(GetOptions(Find(properties, "Compression")) == std::vector<std::string> { "Auto", "Uncompressed" });
		CHECK(GetOptions(Find(properties, "Quality")) == std::vector<std::string> { "High", "Fast" });
		CHECK(GetOptions(Find(properties, "Filter")) == std::vector<std::string> { "Linear", "Nearest" });
		CHECK(GetOptions(Find(properties, "Wrap")) == std::vector<std::string> { "Repeat", "Clamp", "Mirror" });
		const PropertyInfo& maxSize = Find(properties, "MaxSize");
		CHECK(maxSize.Type == PropertyType::UInt);
		CHECK(maxSize.Min == 0.0f);
		CHECK(maxSize.Max == static_cast<float>(TextureImporter::c_MaxImageDimension));
		CHECK(maxSize.DisplayName == "Max Size");
		// Editors offer the limit as a choice: "No limit" (0) or a power of two.
		CHECK(HasFlag(maxSize.Flags, PropertyFlags::PowerOfTwoLimit));
		// Options are named for people too: "NormalMap" reads "Normal Map".
		CHECK(Find(properties, "Usage").EnumValues[1].DisplayName == "Normal Map");
		CHECK(Find(properties, "Usage").EnumValues[3].DisplayName == "HDR");
		CHECK(Find(properties, "GenerateMips").Type == PropertyType::Bool);

		// Every kind of default the importer guesses from a file name.
		for (const char* source : { "Albedo.png", "Brick_normal.png", "Rock_roughness.jpg", "Sky.hdr" })
		{
			CAPTURE(source);
			CheckDescribesDefaults(textures, source);
		}
		// What the schema writes is what the importer reads.
		nlohmann::json settings = nlohmann::json::object();
		REQUIRE(Find(properties, "Usage").SetValue(&settings, PropertyValue(int32_t(2))));
		REQUIRE(Find(properties, "MaxSize").SetValue(&settings, PropertyValue(uint32_t(64))));
		REQUIRE(Find(properties, "Wrap").SetValue(&settings, PropertyValue(int32_t(1))));
		REQUIRE(Find(properties, "Compression").SetValue(&settings, PropertyValue(int32_t(1))));
		REQUIRE(Find(properties, "Quality").SetValue(&settings, PropertyValue(int32_t(1))));
		std::vector<std::string> warnings;
		const TextureImportSettings read = TextureImportSettings::FromJson(settings, &warnings);
		CHECK(warnings.empty());
		CHECK(read.Usage == TextureUsage::Data);
		CHECK(read.MaxSize == 64);
		CHECK(read.Wrap == TextureWrap::Clamp);
		CHECK(read.Compression == TextureCompression::Uncompressed);
		CHECK(read.Quality == TextureQuality::Fast);
	}

	TEST_CASE("Model, audio and font import settings are described")
	{
		const AssetImporter& models = GetImporter(".glb");
		const std::vector<PropertyInfo> modelProperties = models.GetSettingsProperties();
		CHECK(GetNames(modelProperties) == std::vector<std::string> { "Scale", "GenerateLODs", "LODCount", "MaxTextureSize", "TextureCompression",
			"TextureQuality", "OptimizeMeshes", "ImportMaterials", "ImportCameras", "ImportLights" });
		CHECK(GetOptions(Find(modelProperties, "TextureCompression")) == std::vector<std::string> { "Auto", "Uncompressed" });
		CHECK(GetOptions(Find(modelProperties, "TextureQuality")) == std::vector<std::string> { "High", "Fast" });
		CHECK(Find(modelProperties, "Scale").Type == PropertyType::Float);
		CHECK(Find(modelProperties, "Scale").Min > 0.0f);
		CHECK(Find(modelProperties, "LODCount").Max == 8.0f);
		CHECK(HasFlag(Find(modelProperties, "MaxTextureSize").Flags, PropertyFlags::PowerOfTwoLimit));
		CHECK(Find(modelProperties, "GenerateLODs").DisplayName == "Generate LODs");
		CheckDescribesDefaults(models, "Helmet.glb");
		CheckDescribesDefaults(GetImporter(".gltf"), "Helmet.gltf");

		const AssetImporter& audio = GetImporter(".wav");
		const std::vector<PropertyInfo> audioProperties = audio.GetSettingsProperties();
		CHECK(GetNames(audioProperties) == std::vector<std::string> { "LoadMode" });
		CHECK(GetOptions(audioProperties.front()) == std::vector<std::string> { "Decompressed", "Streamed" });
		CheckDescribesDefaults(audio, "Missing.wav"); // A file that cannot be measured starts decompressed

		// Fonts and the engine's own documents are stored as they are: nothing to set.
		for (const char* extension : { ".ttf", ".otf", ".stmat", ".stscene", ".stprefab" })
		{
			CAPTURE(extension);
			CHECK(GetImporter(extension).GetSettingsProperties().empty());
			CHECK(GetImporter(extension).GetDefaultSettings("File").empty());
		}
	}

	TEST_CASE("Importers say which settings the other settings leave without effect, and why")
	{
		const AssetImporter& textures = GetImporter(".png");
		const nlohmann::json color = { { "Usage", "Color" }, { "Compression", "Auto" }, { "Quality", "High" } };
		CHECK(textures.GetInactiveReason("Compression", color).empty());
		CHECK(textures.GetInactiveReason("Quality", color).empty());
		CHECK(textures.GetInactiveReason("Usage", color).empty());
		CHECK(textures.GetInactiveReason("MaxSize", color).empty());
		// Quality chooses how block compression encodes color only.
		CHECK_FALSE(textures.GetInactiveReason("Quality", { { "Usage", "Color" }, { "Compression", "Uncompressed" } }).empty());
		CHECK_FALSE(textures.GetInactiveReason("Quality", { { "Usage", "Data" }, { "Compression", "Auto" } }).empty());
		CHECK_FALSE(textures.GetInactiveReason("Quality", { { "Usage", "NormalMap" } }).empty());
		// HDR textures are not block-compressed.
		CHECK_FALSE(textures.GetInactiveReason("Compression", { { "Usage", "HDR" } }).empty());
		CHECK_FALSE(textures.GetInactiveReason("Quality", { { "Usage", "HDR" } }).empty());
		// Missing settings read as the defaults (Color, Auto): everything has an effect.
		CHECK(textures.GetInactiveReason("Quality", nlohmann::json::object()).empty());

		const AssetImporter& models = GetImporter(".glb");
		const nlohmann::json model = models.GetDefaultSettings("Helmet.glb");
		for (const PropertyInfo& property : models.GetSettingsProperties())
		{
			CAPTURE(property.Name);
			CHECK(models.GetInactiveReason(property.Name, model).empty());
		}
		nlohmann::json withoutLODs = model;
		withoutLODs["GenerateLODs"] = false;
		CHECK_FALSE(models.GetInactiveReason("LODCount", withoutLODs).empty());
		nlohmann::json withoutMaterials = model;
		withoutMaterials["ImportMaterials"] = false;
		for (const char* setting : { "MaxTextureSize", "TextureCompression", "TextureQuality" })
			CHECK_FALSE(models.GetInactiveReason(setting, withoutMaterials).empty());
		nlohmann::json uncompressed = model;
		uncompressed["TextureCompression"] = "Uncompressed";
		CHECK_FALSE(models.GetInactiveReason("TextureQuality", uncompressed).empty());
		CHECK(models.GetInactiveReason("MaxTextureSize", uncompressed).empty());

		// Importers without such settings report none.
		CHECK(GetImporter(".wav").GetInactiveReason("LoadMode", nlohmann::json::object()).empty());
	}

	TEST_CASE("Settings to change are checked against the schema and written in canonical form")
	{
		const AssetImporter& textures = GetImporter(".png");
		nlohmann::json normalized;
		std::string error;
		REQUIRE_MESSAGE(NormalizeImportSettings(textures, { { "usage", "normalmap" }, { "MaxSize", 2048 }, { "GenerateMips", false }, { "Filter", 1 } },
			normalized, &error), error);
		CHECK(normalized == nlohmann::json { { "Usage", "NormalMap" }, { "MaxSize", 2048 }, { "GenerateMips", false }, { "Filter", "Nearest" } });
		REQUIRE(NormalizeImportSettings(textures, nlohmann::json::object(), normalized, &error));
		CHECK(normalized.empty());

		CHECK_FALSE(NormalizeImportSettings(textures, { { "Compressed", true } }, normalized, &error));
		CHECK(error.find("Unknown import setting 'Compressed'") != std::string::npos);
		CHECK(error.find("Usage, Compression, Quality, MaxSize, Filter, Wrap, GenerateMips, FlipVertically") != std::string::npos);
		CHECK_FALSE(NormalizeImportSettings(textures, { { "Usage", "Albedo" } }, normalized, &error));
		CHECK(error.find("Usage") != std::string::npos);
		CHECK(error.find("Color, NormalMap, Data, HDR") != std::string::npos);
		CHECK_FALSE(NormalizeImportSettings(textures, { { "Filter", 7 } }, normalized, &error));
		CHECK_FALSE(NormalizeImportSettings(textures, { { "MaxSize", -1 } }, normalized, &error));
		CHECK_FALSE(NormalizeImportSettings(textures, { { "MaxSize", "big" } }, normalized, &error));
		CHECK_FALSE(NormalizeImportSettings(textures, nlohmann::json::array(), normalized, &error));

		// A value outside a setting's range is refused with the range, never clamped into it: a tool asking for 99999 or a
		// scale of 0.00001 must not get another value without being told.
		CHECK_FALSE(NormalizeImportSettings(textures, { { "MaxSize", 99999 } }, normalized, &error));
		CHECK(error == "Import setting 'MaxSize': 99999 is outside its range, 0 to 16384");

		const AssetImporter& models = GetImporter(".gltf");
		REQUIRE(NormalizeImportSettings(models, { { "Scale", 0.01 }, { "LODCount", 8 } }, normalized, &error));
		CHECK(normalized["Scale"].get<float>() == doctest::Approx(0.01f));
		CHECK(normalized["LODCount"] == 8);
		CHECK_FALSE(NormalizeImportSettings(models, { { "Scale", "huge" } }, normalized, &error));
		CHECK_FALSE(NormalizeImportSettings(models, { { "LODCount", 20 } }, normalized, &error));
		CHECK(error == "Import setting 'LODCount': 20 is outside its range, 0 to 8");
		CHECK_FALSE(NormalizeImportSettings(models, { { "Scale", 0.00001 } }, normalized, &error));
		CHECK(error == "Import setting 'Scale': 1e-05 is outside its range, 0.0001 to 10000");
		CHECK_FALSE(NormalizeImportSettings(models, { { "Scale", 0 } }, normalized, &error));

		CHECK_FALSE(NormalizeImportSettings(GetImporter(".ttf"), { { "Size", 12 } }, normalized, &error));
		CHECK(error == "Font assets have no import settings ('Size' is none)");
	}

	TEST_CASE("Settings missing from a .meta, or invalid in it, read as the schema's defaults")
	{
		const std::vector<PropertyInfo> properties = GetImporter(".png").GetSettingsProperties();
		const nlohmann::json settings = { { "Usage", "Unknown" }, { "MaxSize", "large" } };
		CHECK(std::get<int32_t>(Find(properties, "Usage").GetValue(&settings)) == 0);
		CHECK(std::get<uint32_t>(Find(properties, "MaxSize").GetValue(&settings)) == 0u);
		CHECK(std::get<bool>(Find(properties, "GenerateMips").GetValue(&settings)) == true);
		const nlohmann::json notAnObject = nlohmann::json::array();
		CHECK(std::get<bool>(Find(properties, "FlipVertically").GetValue(&notAnObject)) == false);
	}
}
