#include "stpch.h"
#include "Strata/Renderer/Material.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/PropertyBuilder.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Reflection/PropertyOptions.h"

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
				.Property("EmissiveColor", &MaterialProperties::EmissiveColor,
					PropertyOptions { .Tooltip = "Linear color of the emitted light: times Emissive Intensity (and the emissive map) it is luminance in "
						"nits (cd/m\xC2\xB2)", .Color = true })
				.Property("EmissiveIntensity", &MaterialProperties::EmissiveIntensity,
					PropertyOptions { .Tooltip = "Luminance in nits (cd/m\xC2\xB2) emitted where the emissive color is white: a monitor about 200, a lit "
						"sign about 1,000", .Min = 0.0f, .Max = 1.0e6f, .Speed = 0.1f })
				.Property("NormalScale", &MaterialProperties::NormalScale, PropertyOptions { .Min = 0.0f, .Max = 10.0f, .Speed = 0.01f })
				.Property("OcclusionStrength", &MaterialProperties::OcclusionStrength, range(0.0f, 1.0f))
				.EnumProperty("AlphaMode", &MaterialProperties::AlphaMode, { { "Opaque", 0 }, { "Mask", 1 }, { "Blend", 2 } })
				.Property("AlphaCutoff", &MaterialProperties::AlphaCutoff, range(0.0f, 1.0f))
				.Property("DoubleSided", &MaterialProperties::DoubleSided)
				.Property("Unlit", &MaterialProperties::Unlit)
				.Property("UVTiling", &MaterialProperties::UVTiling, PropertyOptions { .Speed = 0.01f })
				.Property("UVOffset", &MaterialProperties::UVOffset, PropertyOptions { .Speed = 0.01f })
				.AssetProperty("BaseColorMap", &MaterialProperties::BaseColorMap, AssetType::Texture)
				.AssetProperty("MetallicRoughnessMap", &MaterialProperties::MetallicRoughnessMap, AssetType::Texture,
					{ .DisplayName = "Metal/Rough Map", .Tooltip = "Metallic-roughness map: green = roughness, blue = metallic" })
				.AssetProperty("NormalMap", &MaterialProperties::NormalMap, AssetType::Texture)
				.AssetProperty("OcclusionMap", &MaterialProperties::OcclusionMap, AssetType::Texture)
				.AssetProperty("EmissiveMap", &MaterialProperties::EmissiveMap, AssetType::Texture)
				.Property("Clearcoat", &MaterialProperties::Clearcoat,
					WithTooltip(range(0.0f, 1.0f), "Strength of a clear coat over the base: a second specular layer, as on car paint or varnished "
						"wood; the light it reflects no longer reaches the base"))
				.Property("ClearcoatRoughness", &MaterialProperties::ClearcoatRoughness,
					WithTooltip(range(0.0f, 1.0f), "Perceptual roughness of the clear coat"))
				.AssetProperty("ClearcoatMap", &MaterialProperties::ClearcoatMap, AssetType::Texture,
					{ .Tooltip = "Clear coat strength map: red = strength (times Clearcoat)" })
				.AssetProperty("ClearcoatRoughnessMap", &MaterialProperties::ClearcoatRoughnessMap, AssetType::Texture,
					{ .DisplayName = "Clearcoat Rough Map", .Tooltip = "Clear coat roughness map: green = roughness (times Clearcoat Roughness)" })
				.AssetProperty("ClearcoatNormalMap", &MaterialProperties::ClearcoatNormalMap, AssetType::Texture,
					{ .Tooltip = "Tangent-space normal map of the clear coat (without one the coat follows the mesh's own normal)" })
				.Property("Transmission", &MaterialProperties::Transmission,
					WithTooltip(range(0.0f, 1.0f), "Share of light passing through the surface (glass, water): a transmissive material is drawn "
						"as glass, which shows what lies behind it refracted and tinted by the base color"))
				.AssetProperty("TransmissionMap", &MaterialProperties::TransmissionMap, AssetType::Texture,
					{ .Tooltip = "Transmission map: red = transmission (times Transmission)" })
				.Property("IOR", &MaterialProperties::IOR,
					PropertyOptions { .DisplayName = "IOR", .Tooltip = "Index of refraction: how much light bends entering the surface (1 none, water "
						"1.33, glass 1.5, diamond 2.42)", .Min = 1.0f, .Max = 3.0f, .Speed = 0.01f })
				.Property("Thickness", &MaterialProperties::Thickness,
					PropertyOptions { .Tooltip = "Thickness of the volume behind a transmissive surface, in the mesh's units: how far what lies "
						"behind it is displaced and how much the volume absorbs (0: a thin wall)", .Min = 0.0f, .Max = 1000.0f, .Speed = 0.01f })
				.Property("AttenuationColor", &MaterialProperties::AttenuationColor,
					PropertyOptions { .Tooltip = "Color white light takes after traveling Attenuation Distance through a transmissive volume",
						.Color = true })
				.Property("AttenuationDistance", &MaterialProperties::AttenuationDistance,
					PropertyOptions { .Tooltip = "World units after which light in a transmissive volume has the attenuation color (0: the volume "
						"absorbs nothing)", .Min = 0.0f, .Max = c_Unbounded, .Speed = 0.01f });
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

	AssetFinalizeResult Material::FinalizeOnMainThread(const AssetFinalizeContext& context)
	{
		// Textures the manager prefetched with the material are on their way (AssetManagerBase::GetDependencies); a material
		// that was requested asks for the others itself: those of managers that know no dependencies (version 1 packs), and
		// those no prefetch started (the budgets had no room for them then, or they were dropped on arrival). A material that
		// arrives only prefetched asks for nothing: its textures would load outside the room its prefetch was allowed (and
		// requests make room by evicting), for a material nothing may ever use.
		if (context.Manager && !context.Prefetched)
		{
			for (AssetHandle texture : GetTextureHandles())
			{
				if (context.Manager->GetAssetState(texture) == AssetState::Unloaded)
					context.Manager->RequestLoad(texture, AssetPriority::Normal);
			}
		}
		return AssetFinalizeResult::Done;
	}

	std::vector<AssetHandle> Material::GetTextureHandles() const
	{
		std::vector<AssetHandle> textures;
		for (AssetHandle handle : { m_Properties.BaseColorMap, m_Properties.MetallicRoughnessMap, m_Properties.NormalMap, m_Properties.OcclusionMap, m_Properties.EmissiveMap,
			m_Properties.ClearcoatMap, m_Properties.ClearcoatRoughnessMap, m_Properties.ClearcoatNormalMap, m_Properties.TransmissionMap })
		{
			if (handle.IsValid())
				textures.push_back(handle);
		}
		return textures;
	}

}
