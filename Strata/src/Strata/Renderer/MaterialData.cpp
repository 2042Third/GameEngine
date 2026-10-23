#include "stpch.h"
#include "Strata/Renderer/MaterialData.h"

#include "Strata/Renderer/GPUSceneData.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/TextureTable.h"

#include <algorithm>
#include <cmath>

namespace Strata::MaterialGPU
{

	uint32_t GetFlags(const MaterialProperties& properties)
	{
		// Glass is drawn by the transparent pass (after the copy of what lies behind it), whatever its alpha mode.
		const bool transmissive = IsTransmissive(properties);
		return RenderData::c_CullMaterialAvailable | (properties.DoubleSided ? RenderData::c_CullMaterialDoubleSided : 0u)
			| (properties.AlphaMode == MaterialAlphaMode::Mask ? RenderData::c_CullMaterialAlphaMask : 0u)
			| (properties.AlphaMode == MaterialAlphaMode::Blend || transmissive ? RenderData::c_CullMaterialTransparent : 0u)
			| (transmissive ? RenderData::c_CullMaterialTransmissive : 0u);
	}

	std::array<AssetHandle, c_TextureMapCount> GetTextureMaps(const MaterialProperties& properties)
	{
		return { properties.BaseColorMap, properties.MetallicRoughnessMap, properties.NormalMap, properties.OcclusionMap, properties.EmissiveMap,
			properties.ClearcoatMap, properties.ClearcoatRoughnessMap, properties.ClearcoatNormalMap, properties.TransmissionMap };
	}

	bool HasClearcoat(const MaterialProperties& properties)
	{
		return !properties.Unlit && properties.Clearcoat > 0.0f;
	}

	bool IsTransmissive(const MaterialProperties& properties)
	{
		return !properties.Unlit && properties.Transmission > 0.0f;
	}

	glm::vec3 GetAbsorption(const MaterialProperties& properties)
	{
		const float distance = properties.AttenuationDistance;
		if (!(distance > 0.0f) || !std::isfinite(distance))
			return glm::vec3(0.0f);
		// A channel the color removes entirely absorbs as if it kept a ten-thousandth after the distance.
		const glm::vec3 color = glm::clamp(properties.AttenuationColor, glm::vec3(1.0e-4f), glm::vec3(1.0f));
		const glm::vec3 absorption = -glm::log(color) / distance;
		return std::isfinite(absorption.x) && std::isfinite(absorption.y) && std::isfinite(absorption.z) ? absorption : glm::vec3(0.0f);
	}

	RenderData::MaterialData Build(const Material& material, const TextureIndexResolver& textureIndex)
	{
		const MaterialProperties& properties = material.GetProperties();
		RenderData::MaterialData data = {};
		data.BaseColor = properties.BaseColor;
		data.Emissive = glm::vec4(properties.EmissiveColor * properties.EmissiveIntensity, 0.0f);
		data.Metallic = properties.Metallic;
		data.Roughness = properties.Roughness;
		data.NormalScale = properties.NormalScale;
		data.OcclusionStrength = properties.OcclusionStrength;
		data.AlphaCutoff = properties.AlphaCutoff;
		data.Flags = (properties.AlphaMode == MaterialAlphaMode::Mask ? RenderData::c_MaterialAlphaMask : 0u)
			| (properties.AlphaMode == MaterialAlphaMode::Blend ? RenderData::c_MaterialAlphaBlend : 0u)
			| (properties.Unlit ? RenderData::c_MaterialUnlit : 0u)
			| (properties.DoubleSided ? RenderData::c_MaterialDoubleSided : 0u)
			| (HasClearcoat(properties) ? RenderData::c_MaterialClearcoat : 0u)
			| (IsTransmissive(properties) ? RenderData::c_MaterialTransmissive : 0u);
		data.UVTransform = glm::vec4(properties.UVTiling, properties.UVOffset);
		data.BaseColorMap = textureIndex(properties.BaseColorMap, TextureTable::c_White, TextureTable::c_White);
		data.MetallicRoughnessMap = textureIndex(properties.MetallicRoughnessMap, TextureTable::c_White, TextureTable::c_White);
		data.NormalMap = textureIndex(properties.NormalMap, TextureTable::c_FlatNormal, TextureTable::c_FlatNormal);
		data.OcclusionMap = textureIndex(properties.OcclusionMap, TextureTable::c_White, TextureTable::c_White);
		data.EmissiveMap = textureIndex(properties.EmissiveMap, TextureTable::c_White, TextureTable::c_Black);
		data.Clearcoat = HasClearcoat(properties) ? std::min(properties.Clearcoat, 1.0f) : 0.0f;
		data.ClearcoatRoughness = std::clamp(properties.ClearcoatRoughness, 0.0f, 1.0f);
		data.Transmission = IsTransmissive(properties) ? std::min(properties.Transmission, 1.0f) : 0.0f;
		data.IOR = std::max(properties.IOR, 1.0f);
		data.Thickness = std::max(properties.Thickness, 0.0f);
		data.ClearcoatMap = textureIndex(properties.ClearcoatMap, TextureTable::c_White, TextureTable::c_White);
		data.ClearcoatRoughnessMap = textureIndex(properties.ClearcoatRoughnessMap, TextureTable::c_White, TextureTable::c_White);
		data.ClearcoatNormalMap = textureIndex(properties.ClearcoatNormalMap, TextureTable::c_FlatNormal, TextureTable::c_FlatNormal);
		data.TransmissionMap = textureIndex(properties.TransmissionMap, TextureTable::c_White, TextureTable::c_White);
		data.Absorption = glm::vec4(GetAbsorption(properties), 0.0f);
		return data;
	}

}
