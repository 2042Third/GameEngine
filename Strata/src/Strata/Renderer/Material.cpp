#include "stpch.h"
#include "Strata/Renderer/Material.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/PropertyBuilder.h"
#include "Strata/Reflection/PropertyJson.h"

namespace Strata
{

	const std::vector<PropertyInfo>& Material::GetPropertyInfos()
	{
		static const std::vector<PropertyInfo> s_Properties = []()
		{
			std::vector<PropertyInfo> properties;
			auto range = [](float min, float max)
			{
				PropertyOptions options;
				options.Min = min;
				options.Max = max;
				options.Flags = PropertyFlags::Slider;
				return options;
			};
			PropertyOptions color;
			color.Color = true;

			PropertyListBuilder<MaterialProperties>(properties)
				.Property("BaseColor", &MaterialProperties::BaseColor, color)
				.Property("Metallic", &MaterialProperties::Metallic, range(0.0f, 1.0f))
				.Property("Roughness", &MaterialProperties::Roughness, range(0.0f, 1.0f))
				.Property("EmissiveColor", &MaterialProperties::EmissiveColor, color)
				.Property("EmissiveIntensity", &MaterialProperties::EmissiveIntensity, PropertyOptions { .Min = 0.0f, .Max = 1.0e6f, .Speed = 0.1f })
				.Property("NormalScale", &MaterialProperties::NormalScale, PropertyOptions { .Min = 0.0f, .Max = 10.0f, .Speed = 0.01f })
				.Property("OcclusionStrength", &MaterialProperties::OcclusionStrength, range(0.0f, 1.0f))
				.EnumProperty("AlphaMode", &MaterialProperties::AlphaMode, { { "Opaque", 0 }, { "Mask", 1 }, { "Blend", 2 } })
				.Property("AlphaCutoff", &MaterialProperties::AlphaCutoff, range(0.0f, 1.0f))
				.Property("DoubleSided", &MaterialProperties::DoubleSided)
				.Property("Unlit", &MaterialProperties::Unlit)
				.Property("UVTiling", &MaterialProperties::UVTiling, PropertyOptions { .Speed = 0.01f })
				.Property("UVOffset", &MaterialProperties::UVOffset, PropertyOptions { .Speed = 0.01f })
				.AssetProperty("BaseColorMap", &MaterialProperties::BaseColorMap, AssetType::Texture)
				.AssetProperty("MetallicRoughnessMap", &MaterialProperties::MetallicRoughnessMap, AssetType::Texture, { .Tooltip = "Green = roughness, blue = metallic" })
				.AssetProperty("NormalMap", &MaterialProperties::NormalMap, AssetType::Texture)
				.AssetProperty("OcclusionMap", &MaterialProperties::OcclusionMap, AssetType::Texture)
				.AssetProperty("EmissiveMap", &MaterialProperties::EmissiveMap, AssetType::Texture);
			return properties;
		}();
		return s_Properties;
	}

	Ref<Material> Material::Create(const MaterialProperties& properties)
	{
		Ref<Material> material(new Material());
		material->m_Properties = properties;
		return material;
	}

	nlohmann::json Material::Serialize() const
	{
		nlohmann::json properties = nlohmann::json::object();
		for (const PropertyInfo& property : GetPropertyInfos())
			properties[property.Name] = PropertyValueToJson(property, property.GetValue(&m_Properties));

		nlohmann::json json;
		json["Strata"] = { { "Format", "Material" }, { "Version", c_FormatVersion } };
		json["Material"] = properties;
		return json;
	}

	Ref<Material> Material::FromJson(const nlohmann::json& json, std::string* outError, std::vector<std::string>* outWarnings)
	{
		auto fail = [outError](const std::string& message) -> Ref<Material>
		{
			if (outError)
				*outError = message;
			return nullptr;
		};

		const nlohmann::json* header = JsonUtils::Find(json, "Strata");
		if (!header || JsonUtils::GetString(*header, "Format") != "Material")
			return fail("Not a Strata material document");
		const uint64_t version = JsonUtils::GetUInt(*header, "Version", 0);
		if (version == 0 || version > c_FormatVersion)
			return fail(fmt::format("Unsupported material version {}", version));

		const nlohmann::json* properties = JsonUtils::Find(json, "Material");
		if (!properties || !properties->is_object())
			return fail("Material document has no 'Material' object");

		Ref<Material> material = Create();
		for (const auto& [key, value] : properties->items())
		{
			const PropertyInfo* property = FindProperty(GetPropertyInfos(), key);
			std::string error;
			if (!property)
				error = fmt::format("Unknown material property '{}'", key);
			else if (std::optional<PropertyValue> parsed = PropertyValueFromJson(*property, value, &error); parsed && property->SetValue(&material->m_Properties, *parsed, &error))
				continue;

			if (outWarnings)
				outWarnings->push_back(error);
		}
		return material;
	}

	Ref<Material> Material::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		std::optional<nlohmann::json> json = JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), outError);
		if (!json)
			return nullptr;

		std::vector<std::string> warnings;
		Ref<Material> material = FromJson(*json, outError, &warnings);
		for (const std::string& warning : warnings)
			ST_CORE_WARN("Material: {}", warning);
		return material;
	}

	bool Material::FinalizeOnMainThread(const AssetFinalizeContext& context)
	{
		if (context.Manager)
		{
			for (AssetHandle texture : GetTextureHandles())
				context.Manager->RequestLoad(texture, AssetPriority::Normal);
		}
		return true;
	}

	std::vector<AssetHandle> Material::GetTextureHandles() const
	{
		std::vector<AssetHandle> textures;
		for (AssetHandle handle : { m_Properties.BaseColorMap, m_Properties.MetallicRoughnessMap, m_Properties.NormalMap, m_Properties.OcclusionMap, m_Properties.EmissiveMap })
		{
			if (handle.IsValid())
				textures.push_back(handle);
		}
		return textures;
	}

}
